"""An SC2 model's shape as TRELLIS's coarse structure: TRELLIS then only details and paints it from the sprite.

TRELLIS works in two stages: a 64^3 occupancy grid (where the object is), then the details and colours on those voxels,
both conditioned on the image. On the Dragoon the first stage goes wrong: it stands the walker on a ground plate that
spans the whole grid (its near-black feet on the black input background), so the walker itself gets a third of the grid
(about 20 voxels across) and comes out lumpy. The SC Evo Complete mod has a Dragoon modelled after the original, which
matches the sprite's outline from the sprite camera about as well as TRELLIS's own (`trellis_fit.fit`: IoU 0.68 / 0.72
on frames 0 / 68, against 0.66-0.71). This writes that model's surface voxels, filling the grid as TRELLIS's training
objects do (centred, longest side = the grid), as NAME_coords.npy next to the sprite's NAME.png (`trellis_inputs.py`);
the kernel then runs only the second stage on them.

YAW turns the model from the sprite's frame (x east, y north, z up; SC2 units face -y, the sprite camera) into the frame
TRELLIS generates this picture in: `trellis_fit.fit` puts all six TRELLIS Dragoons at yaw 180, so YAW = 180.

With IMAGE_ID FRAME, the structure is the sprite's own wherever the sprite shows the object (`sprite_and_model`: the
relief from the HD depth layer, the space in front of it and outside the outline empty) and the model only behind
that, where no sprite angle tells: for buildings whose hidden side TRELLIS invents (the Command Center's back came out
a smooth sloping skirt). YAW then turns the sprite frame into TRELLIS's: minus the yaw `fit` found for a TRELLIS mesh
of the same picture (Command Center seed 1: 235, so YAW = 125).

    m3_obj ASSET_ROOT Assets/Units/Protoss/DragoonSCBW/DragoonSCBW.m3 OUT/sc2/dragoon
    sc2_structure.py OUT/sc2/dragoon YAW OUT/kaggle_in/NAME_coords.npy [IMAGE_ID FRAME]
    voxel_mesh.py OUT/kaggle_in/NAME_coords.npy CHECK.glb 140 YAW   (cubes to look at in Blender)

Run with the venv python (needs trimesh)."""
import os
import sys
import numpy as np
from PIL import Image
from scipy import ndimage
from anim import Anim
from relief import lift, ELEV, BETA, CAMERA_BY_IMAGE
from views import camera
from demo import DATA
from trellis_fit import sample_surface, rz, fit

RES = 64
FILL = 0.98  # longest side, in grid widths (TRELLIS's training objects: 1)


def load_obj(prefix):
    """The OBJ and PPM textures `m3_obj` writes: vertices, UVs (v down the image), faces, texture per face, textures.
    A texture with a glow ("# glow tN MULT") is drawn as the texture plus the glow times MULT (at most 2), as the game
    draws it unlit: such pairs are added after the PPMs as textures of their own. A glow that is the texture itself is
    left out."""
    V, T, F, mat, lit, cur, glow, pairs = [], [], [], [], [], 0, None, []
    for line in open(prefix + '.obj'):
        p = line.split()
        if not p:
            continue
        if p[0] == 'v':
            V.append([float(x) for x in p[1:4]])
        elif p[0] == 'vt':
            T.append([float(x) for x in p[1:3]])
        elif p[0] == 'usemtl':
            cur, glow = int(p[1][1:]), None
        elif p[:2] == ['#', 'glow'] and int(p[2][1:]) != cur:  # the Pylon's crystal glows with its own diffuse map
            glow = (cur, int(p[2][1:]), min(2.0, max(0.0, float(p[3]))))  # (x1.7): added, it blows out to flat cyan
        elif p[0] == 'f':
            F.append([int(x.split('/')[0]) - 1 for x in p[1:4]]); mat.append(cur)
            if glow and cur >= 0 and glow not in pairs:
                pairs.append(glow)
            lit.append(pairs.index(glow) if glow and cur >= 0 else -1)
    tex = []
    while os.path.exists('%s_t%d.ppm' % (prefix, len(tex))):
        tex.append(np.asarray(Image.open('%s_t%d.ppm' % (prefix, len(tex))).convert('RGB')))
    lit = np.array(lit)
    mat = np.where(lit >= 0, len(tex) + lit, mat)
    for d, g, k in pairs:
        h, w = tex[d].shape[:2]
        add = np.asarray(Image.fromarray(tex[g]).resize((w, h), Image.BILINEAR)).astype(np.float64)
        tex.append(np.clip(tex[d] + k * add, 0, 255).astype(np.uint8))
    return np.array(V), np.array(T), np.array(F), mat, tex


def surface_points(V, F, step):
    """Points on every triangle, about `step` apart."""
    return sample_surface(V, F, np.zeros((len(V), 2)), np.zeros((1, 1, 3), np.uint8), per_area=4.0 / step ** 2)[0]


def in_grid(V, yaw):
    """V turned by `yaw` degrees, centred and scaled into TRELLIS's cube [-0.5, 0.5]^3 (longest side FILL)."""
    V = V @ rz(yaw).T
    lo, hi = V.min(0), V.max(0)
    return (V - 0.5 * (lo + hi)) * (FILL / np.ptp(V, 0).max())


def voxels(X):
    """Unique voxel indices (x, y, z) of points X in TRELLIS's RES^3 grid over [-0.5, 0.5]^3."""
    return np.unique(np.clip(np.floor((X + 0.5) * RES), 0, RES - 1).astype(np.int32), axis=0)


def sprite_and_model(V, F, image_id, frame, cell=4.0, tol=12.0):
    """Surface points (sprite frame, HD px) of a solid that is the sprite's own where the sprite shows the object and
    the model's where it cannot: the model is fitted to the sprite (`trellis_fit.fit`) and filled (voxels of `cell`
    HD px); along each pixel's ray the space in front of the relief (the sprite's depth layer, `relief.lift`) is
    empty, a skin of `tol` behind it is solid, and further back the model's solid is kept; outside the outline
    everything is empty. Enclosed holes are filled, so a model smaller than the sprite leaves no inner surface."""
    elev, beta = CAMERA_BY_IMAGE.get(image_id, (ELEV, BETA))
    P, rgba, mask = lift(Anim(DATA % image_id), frame, elev=elev, beta=beta)
    yaw, s, t, iou = fit(surface_points(V, F, np.ptp(V, 0).max() / 300), P, mask, elev)
    print('model: yaw %d, scale %.1f HD px per unit, outline IoU %.2f' % (yaw, s, iou))
    Vm = s * (V @ rz(yaw).T) + t
    R = camera(elev, 0.0)
    U, Vv, Wr = P @ R[0], P @ R[1], P @ R[2]
    u0, v0 = U[0, 0], Vv[0, 0]
    h, w = mask.shape
    Pm = P[mask]
    lo = np.minimum(Vm.min(0), Pm.min(0)) - 3 * cell
    hi = np.maximum(Vm.max(0), Pm.max(0)) + 3 * cell
    lo[2] = 0.0  # nothing below the ground
    shape = np.ceil((hi - lo) / cell).astype(int)
    idx = lambda X: np.clip(np.floor((X - lo) / cell).astype(int), 0, shape - 1)
    model = np.zeros(shape, bool)
    model[tuple(idx(surface_points(Vm, F, cell / 3)).T)] = True
    model = ndimage.binary_fill_holes(ndimage.binary_closing(model, iterations=2))
    g = np.stack(np.meshgrid(*[lo[k] + cell * (np.arange(shape[k]) + 0.5) for k in range(3)], indexing='ij'), -1)
    c = np.round(g @ R[0] - u0).astype(int); r = np.round(g @ R[1] - v0).astype(int)
    inside = (c >= 0) & (c < w) & (r >= 0) & (r < h)
    c, r = np.clip(c, 0, w - 1), np.clip(r, 0, h - 1)
    shown = inside & mask[r, c]
    behind = np.where(shown, g @ R[2] - np.nan_to_num(Wr)[r, c], -np.inf)  # depth behind the relief, along the ray
    solid = ((behind > -0.5 * cell) & (behind < tol)) | (model & (behind >= tol))
    solid = ndimage.binary_fill_holes(solid)
    surface = solid & ~ndimage.binary_erosion(solid)
    print('solid: %d cells of %d HD px (sprite skin %d, model behind it %d), surface %d' % (
        solid.sum(), cell, ((behind > -0.5 * cell) & (behind < tol)).sum(), (model & (behind >= tol)).sum(), surface.sum()))
    return g[surface]


if __name__ == '__main__':
    prefix, yaw, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    V, T, F, mat, tex = load_obj(prefix)
    if len(sys.argv) > 5:
        X = sprite_and_model(V, F, int(sys.argv[4]), int(sys.argv[5]))
        G = voxels(in_grid(X, yaw))
    else:
        G = voxels(surface_points(in_grid(V, yaw), F, 0.25 / RES))
    np.save(out, G)
    print('%s: %d voxels, spanning %s of %d' % (out, len(G), np.ptp(G, 0) + 1, RES))
