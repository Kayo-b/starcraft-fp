"""A building as one mesh from TRELLIS parts: the body mesh (TRELLIS on the whole sprite) loses the parts it got wrong
and gets instead a part mesh (TRELLIS on the part's cut-out, `trellis_inputs.py` NAME:IMAGE_ID:FRAME:CX:CY), fitted to
the part's cut-out of the lifted sprite. With N, the same part also replaces N - 1 more, turned by 360/N degrees each
around a vertical axis through (X0, Y0): each copy keeps the part's size and that turn (within 20 degrees) and is placed
over its own cut-out, at the foot the turn gives.

The Nexus needs this: TRELLIS fuses each of its four arches with what stands behind it in the picture (a pillar with a
bulb), while the arch alone comes out right. Its arches are one design turned by 90 degrees: the east and west ones show
their curved side to the sprite camera, the south and north ones their edges. Fitted freely, the west arch's mesh
matches the south and east cut-outs at turns of +105 and +190 degrees (outline IoU 0.81, 0.70); turned rigidly around
the midpoint of the east and west feet it misses them by ~30 HD px (IoU 0.22, 0.44), so each copy is placed on its own.

The pyramid has one face with the stairs and three alike. From the sprite camera the tops of the two back faces show as
green-tiled bands beside the crystal, and TRELLIS makes them eaves standing out from the two ridges beside the stair
face (the other ridges come out clean). `wedge:X0:Y0:A0:A1:TURN` replaces the body's faces in a wedge around the axis
through (X0, Y0) (degrees counter-clockwise from east, A0..A1) by the faces TURN degrees before it, turned into place:
the strips along those ridges become copies of the north-east face's ridge strips.

    trellis_compose.py IMAGE_ID FRAME BODY.glb|BODY.obj RADIUS OUT [PART.glb:CX:CY[:N:X0:Y0] | wedge:X0:Y0:A0:A1:TURN |
                       yaws:LO:HI | squash:Z0:K | size:K | sprite-texture | recolour | gold:GAIN:DARKEN] ...
    -> OUT.glb (game pixels, y up; one textured mesh per piece) and OUT.png (previews: TRELLIS's colours, the sprite's
       palette)
BODY.obj is an SC2 model (`m3_obj`, `m3_pose.py`). yaws:LO:HI limits the body's turn (degrees, 5-degree steps): the
mod's Hatchery, Lair and Hive are one model, so all three share one turn, and the Lair alone fits best at a wrong one.
`recolour` (BODY.obj only): the mod's own textures and UVs, recoloured to the sprite's palette (`colour_map`), instead
of the sprite's pixels; where the mod's shape differs from the sprite's,
painting with the sprite's pixels puts them on the wrong parts, recolouring cannot. gold:GAIN:DARKEN deepens the
recoloured gold (`colour_map`; the Protoss: gold:1.6:6). squash:Z0:K (BODY.obj only) shrinks the model above height Z0
(model units) by K before fitting: the mod's Creep Colony stands its tentacle up twice as tall as the sprite's bulb
(squash:0.5:0.5 puts the sprite's ribbed bulb on it, outline IoU 0.71 -> 0.75). size:K scales the finished model by K
around the unit's position on the ground (OUT.glb only; the fit and OUT.png are the sprite's size): in first person the
Nexus's pyramid read small beside the Command Center's body, the user asked for 1.4, then 1.4 again (1.96); the Pylon
0.85 (the user: 2/3 the size of a Gateway).
Run with the venv python (needs trimesh)."""
import sys
import numpy as np
import cv2
import trimesh
import xatlas
from scipy import ndimage
from scipy.spatial import cKDTree
from PIL import Image, ImageDraw
from anim import Anim
from relief import lift, part_mask, ELEV, BETA, CAMERA_BY_IMAGE
from preview import look_camera, ground
from views import camera
from demo import DATA, views_for, W, H
from trellis_fit import load_glb, sample_surface, rz, fit, match_palette, points_splat
from sc2_structure import load_obj, surface_points

HD_PER_GAME_PX = 4.0


def cut(X, cx, cy):
    """relief.part_mask's rule (within 60 HD px of the foot, or 100 px above 90 px), on model-frame points."""
    d = np.hypot(X[:, 0] - cx, X[:, 1] - cy)
    return (d < 60) | ((d < 100) & (X[:, 2] > 90))


def texel_samples(V, F, uv, size, per_texel, seed=0):
    """Random points on every triangle, about `per_texel` per texel of its area in a size x size texture: face index,
    barycentric weights (n, 3), model-frame points, UVs."""
    ua, ub, uc = uv[F[:, 0]], uv[F[:, 1]], uv[F[:, 2]]
    e1, e2 = ub - ua, uc - ua
    n = np.maximum(1, np.ceil(0.5 * np.abs(e1[:, 0] * e2[:, 1] - e1[:, 1] * e2[:, 0]) * size * size * per_texel)).astype(int)
    f = np.repeat(np.arange(len(F)), n)
    rng = np.random.default_rng(seed)
    r1, r2 = rng.random(len(f)), rng.random(len(f))
    s = np.sqrt(r1)
    w = np.stack([1 - s, s * (1 - r2), s * r2], 1)
    return f, w, (w[:, :, None] * V[F[f]]).sum(1), (w[:, :, None] * uv[F[f]]).sum(1)


def texel(UV, size):
    """Texel (row, column) of UVs (v up, as trimesh and glTF after its flip)."""
    return (np.clip(((1 - UV[:, 1]) * size).astype(int), 0, size - 1), np.clip((UV[:, 0] * size).astype(int), 0, size - 1))


def to_texture(UV, col, size, background=None, gap=3.0):
    """A size x size RGB texture: each texel the mean colour of the samples in it. Texels without samples take the
    nearest texel that has some (random samples leave ~5% of texels empty at 3 per texel); beyond `gap` texels from
    any, `background` when given (so filtering at the islands' edges stays clean either way)."""
    tr, tc = texel(UV, size)
    acc = np.zeros((size, size, 3)); cnt = np.zeros((size, size))
    np.add.at(acc, (tr, tc), col); np.add.at(cnt, (tr, tc), 1)
    hit = cnt > 0
    dist, idx = ndimage.distance_transform_edt(~hit, return_indices=True)
    tex = (acc / np.maximum(cnt, 1)[..., None])[idx[0], idx[1]]
    if background is not None:
        tex[dist > gap] = background[dist > gap]
    return np.clip(tex, 0, 255).astype(np.uint8)


def alike_features(Y, N, fold, k_normal=60.0, k_turn=60.0):
    """Features (HD px) to match points around a vertical axis through the origin: height, distance from the axis,
    the normal's radial, tangential (unsigned: mirroring flips it) and vertical parts, and the direction from the axis;
    with `fold`, the direction folded onto the south half (y < 0), where the sprite shows the sides."""
    th = np.arctan2(Y[:, 1], Y[:, 0])
    cs, sn = np.cos(th), np.sin(th)
    nr, nt = N[:, 0] * cs + N[:, 1] * sn, np.abs(N[:, 1] * cs - N[:, 0] * sn)
    if fold:
        sn = -np.abs(sn)
    return np.stack([Y[:, 2], np.hypot(Y[:, 0], Y[:, 1]), k_normal * nr, k_normal * nt, k_normal * N[:, 2],
                     k_turn * cs, k_turn * sn], 1)


def surface_axes(N):
    """Two unit vectors along surfaces with normals N: horizontal (up x N; east where N is vertical), then up the surface."""
    t1 = np.cross([0.0, 0.0, 1.0], N)
    flat = np.linalg.norm(t1, axis=1) < 1e-3
    t1[flat] = [1.0, 0.0, 0.0]
    t1 /= np.linalg.norm(t1, axis=1, keepdims=True)
    return t1, np.cross(N, t1)


def bilinear(img, mask, u, v):
    """img (h, w, 3) at pixel coordinates (u = column, v = row), from pixels inside `mask` only."""
    h, w = mask.shape
    x0, y0 = np.clip(np.floor(u).astype(int), 0, w - 2), np.clip(np.floor(v).astype(int), 0, h - 2)
    fx, fy = np.clip(u - x0, 0, 1)[:, None], np.clip(v - y0, 0, 1)[:, None]
    m = mask.astype(np.float64)[..., None]
    corners = [((1 - fx) * (1 - fy), y0, x0), (fx * (1 - fy), y0, x0 + 1), ((1 - fx) * fy, y0 + 1, x0), (fx * fy, y0 + 1, x0 + 1)]
    num = sum(wt * img[yy, xx] * m[yy, xx] for wt, yy, xx in corners)
    den = sum(wt * m[yy, xx] for wt, yy, xx in corners)
    return num / np.maximum(den, 1e-6)


def colour_map(own, sprite, iters=12, sample=200000, gold=None):
    """The sprite's palette for the model's own colours, as a function on RGB arrays: iterative distribution transfer
    (Pitie et al. 2005) in Lab: the model's colours (where the sprite shows the surface) are turned by a random
    rotation, each axis quantile-matched to the sprite's colours turned the same way, `iters` times, until the whole
    3D colour cloud matches. Ranks, not pairs: the mod's most yellow stripe becomes the sprite's yellow, its peach
    Protoss trim the sprite's gold, though the two put them in different places (a mapping learnt per point, from the
    sprite's colour at the same place, turned the stripes grey: most such pairs are grey, the rest off by a few px).
    The colour cloud matches, but the Protoss gold still reads washed out: the mod's broad panels are its lightest
    colour, so they take the sprite's highlights (the sprite's lighting is baked in, the mod's is not). gold (GAIN,
    DARKEN): afterwards, the chroma of yellow-orange colours (Lab hue 55-105 degrees, fading out by 40/120) times GAIN
    and their lights darkened by up to DARKEN (more darkening turns a light yellow olive)."""
    lab = lambda c: cv2.cvtColor(np.asarray(c, np.float32).reshape(-1, 1, 3) / 255, cv2.COLOR_RGB2Lab).reshape(-1, 3)
    rng = np.random.default_rng(0)
    A = lab(own[rng.permutation(len(own))[:sample]]).astype(np.float64)
    B = lab(sprite[rng.permutation(len(sprite))[:sample]]).astype(np.float64)
    q = np.linspace(0, 100, 513)
    steps = []
    for R in (np.linalg.qr(rng.normal(size=(3, 3)))[0] for _ in range(iters)):
        a, b = A @ R, B @ R
        maps = [(np.percentile(a[:, c], q), np.percentile(b[:, c], q)) for c in range(3)]
        steps.append((R, maps))
        A = np.stack([np.interp(a[:, c], *maps[c]) for c in range(3)], 1) @ R.T

    def apply(rgb):
        rgb = np.asarray(rgb)[..., :3]
        key, inv = np.unique(rgb.reshape(-1, 3) // 2, axis=0, return_inverse=True)  # each colour once (7 bits)
        C = lab(key * 2 + 1).astype(np.float64)
        for R, maps in steps:
            c = C @ R
            C = np.stack([np.interp(c[:, k], *maps[k]) for k in range(3)], 1) @ R.T
        if gold:
            hue, chroma = np.degrees(np.arctan2(C[:, 2], C[:, 1])), np.hypot(C[:, 1], C[:, 2])
            w = np.clip(1 - (np.abs(hue - 80) - 25) / 15, 0, 1) * np.clip((chroma - 4) / 10, 0, 1)
            C[:, 1:] *= 1 + (gold[0] - 1) * w[:, None]
            C[:, 0] -= gold[1] * w * np.clip((C[:, 0] - 25) / 45, 0, 1)
        out = C[inv.ravel()].astype(np.float32).reshape(-1, 1, 3)
        return np.clip(cv2.cvtColor(out, cv2.COLOR_Lab2RGB).reshape(rgb.shape) * 255 + 0.5, 0, 255).astype(np.uint8)
    return apply


class Piece:
    """A TRELLIS mesh placed in the model frame (HD px, z up): vertices, faces, UVs, texture."""
    def __init__(self, glb, P, mask, elev, name, yaws=range(0, 360, 5), scale=None):
        self.V0, self.F, self.uv, self.tex = load_glb(glb)
        self.yaw, self.scale, t, iou = fit(sample_surface(self.V0, self.F, self.uv, self.tex, per_area=4e5)[0], P, mask, elev, yaws, scale)
        print('%s: yaw %d, scale %.0f HD px per unit, outline IoU %.2f' % (name, self.yaw, self.scale, iou))
        self.V = self.scale * (self.V0 @ rz(self.yaw).T) + t

    def with_wedges(self, specs, r_min=50.0):
        """Faces in each wedge (x0, y0, a0, a1, turn) replaced: the wedge spans a0..a1 degrees, counter-clockwise from
        east, around the vertical axis through (x0, y0), from `r_min` HD px out (the Nexus's crystal and its platform
        stay whole); its new faces are the original's from a0 - turn .. a1 - turn, turned by `turn` around that axis.
        The triangles are first cut along every wedge's sides (TRELLIS's are large: picking whole ones by their centres
        left spiky seams)."""
        if not specs:
            return self
        V, F, uv = self.V, self.F, self.uv
        for x0, y0, deg in {(x0, y0, (a - t) % 360) for x0, y0, a0, a1, turn in specs for a in (a0, a1) for t in (0, turn)}:
            n = np.array([-np.sin(np.radians(deg)), np.cos(np.radians(deg)), 0.0])
            sides = [trimesh.intersections.slice_faces_plane(V, F, m, np.array([x0, y0, 0.0]), uv=uv) for m in (n, -n)]
            V = np.concatenate([sides[0][0], sides[1][0]])
            F = np.concatenate([sides[0][1], sides[1][1] + len(sides[0][0])])
            uv = np.concatenate([sides[0][2], sides[1][2]])
        C = V[F].mean(1)
        def inside(x0, y0, a0, a1, off):
            ang = np.degrees(np.arctan2(C[:, 1] - y0, C[:, 0] - x0))
            return ((ang - (a0 - off)) % 360 < a1 - a0) & (np.hypot(C[:, 0] - x0, C[:, 1] - y0) > r_min)
        keep = ~np.any([inside(*w[:4], 0) for w in specs] + [np.zeros(len(C), bool)], axis=0)
        Vs, uvs, Fs = [V], [uv], [F[keep]]
        for k, (x0, y0, a0, a1, turn) in enumerate(specs):
            c = np.array([x0, y0, 0.0])
            Vs.append((V - c) @ rz(turn).T + c); uvs.append(uv)
            Fs.append(F[inside(x0, y0, a0, a1, turn)] + (k + 1) * len(V))
        q = Piece.__new__(Piece)
        q.V, q.uv, q.F, q.tex, q.scale = np.concatenate(Vs), np.concatenate(uvs), np.concatenate(Fs), self.tex, self.scale
        return q

    def without(self, drop, speck=15.0):
        """The piece minus faces whose centre `drop` selects, and minus loose bits smaller than `speck` HD px (TRELLIS
        leaves dozens of them floating around each mesh; vertices split at UV seams are joined to find them). Not for an
        SC2 model: its small parts are real, and faces joined only by edges three or more faces share (which
        trimesh's adjacency skips) looked loose too (148 faces of the Assimilator's dome, 4% of the Missile Turret)."""
        q = Piece.__new__(Piece)
        q.V, q.uv, q.tex, q.scale = self.V, self.uv, self.tex, self.scale
        kept = np.nonzero(~drop(self.V[self.F].mean(1)))[0]
        if not hasattr(self, 'model'):
            F = self.F[kept]
            same = np.unique(np.round(self.V, 2), axis=0, return_inverse=True)[1].ravel()
            joined = trimesh.Trimesh(np.zeros((same.max() + 1, 3)), same[F], process=False)
            lab = trimesh.graph.connected_component_labels(joined.face_adjacency, node_count=len(F))
            small = [c for c in np.unique(lab) if np.ptp(self.V[np.unique(F[lab == c])], 0).max() < speck]
            kept = kept[~np.isin(lab, small)]
        q.F = self.F[kept]
        if hasattr(self, 'model'):  # an SC2 model: its own mesh, and each face's texture (`recoloured`)
            q.model, q.fmat = self.model, self.fmat[kept]
        return q

    @staticmethod
    def from_model(prefix, P, mask, elev, name, size=2048, yaws=range(0, 360, 5), squash=None):
        """An SC2 model (`m3_obj` output) placed like a TRELLIS mesh, with a fresh UV atlas (the mod's UVs tile and share
        texture space between parts, so the sprite could not be projected into them) holding its own texture.
        squash (Z0, K): heights above Z0 (model units) shrunk by K, for a part the mod made taller than the sprite's."""
        V, T, F, mat, tex = load_obj(prefix)
        if squash:
            z0, k = squash
            V[:, 2] = np.where(V[:, 2] > z0, z0 + (V[:, 2] - z0) * k, V[:, 2])
        # parts entirely below the ground are never seen (the Sunken's buried tentacle, construction pieces that rise
        # out of the ground) and would throw the fit off
        keep = V[F, 2].max(1) > -0.05
        used, F = np.unique(F[keep], return_inverse=True)
        V, T, F, mat = V[used], T[used], F.reshape(-1, 3), mat[keep]
        q = Piece.__new__(Piece)
        q.yaw, q.scale, t, iou = fit(surface_points(V, F, 0.01 * np.ptp(V, 0).max()), P, mask, elev, yaws)
        print('%s: yaw %d, scale %.0f HD px per unit, outline IoU %.2f' % (name, q.yaw, q.scale, iou))
        vmap, q.F, q.uv = xatlas.parametrize(V.astype(np.float32), F.astype(np.uint32))
        q.V = q.scale * (V[vmap] @ rz(q.yaw).T) + t
        q.model = (q.scale * (V @ rz(q.yaw).T) + t, T, F, mat, tex)  # placed, with the mod's own UVs (`recoloured`)
        q.fmat = mat  # each face's texture (xatlas keeps the faces in order)
        f, w, _, UV = texel_samples(q.V, q.F, q.uv, size, 2.0)
        old = (w[:, :, None] * T[vmap][q.F[f]]).sum(1)  # the mod's UV of each sample (v down its image)
        col = np.zeros((len(f), 3))
        for m in np.unique(mat):
            sel = mat[f] == m
            if not 0 <= m < len(tex):  # a texture from the base game, which the mod does not ship: plain grey
                col[sel] = 128; continue
            th, tw = tex[m].shape[:2]
            col[sel] = tex[m][((old[sel, 1] % 1) * th).astype(int) % th, ((old[sel, 0] % 1) * tw).astype(int) % tw]
        q.tex = to_texture(UV, col, size)
        return q

    def sprite_textured(self, P, rgba, mask, elev, size=2048, tol=4.0, facing=(0.15, 0.4), per_texel=3.0, max_flow=8.0,
                        transfer=True, moved_tol=8.0, centre=None, similar=(30.0, 90.0), pairs=False):
        """The piece with a new texture: the Remastered sprite's own pixels wherever the sprite camera sees the surface,
        the piece's own colours elsewhere, mapped onto the sprite's colours (per-channel quantiles fitted on the texels
        that have both). Seen: the point is the piece's nearest surface at its pixel (within `tol` HD px along the ray)
        and the sprite has a pixel there. Surfaces turned from the camera blend from the sprite's pixels (cosine above
        facing[1]) to the fallback (below facing[0]), so grazing walls do not get a few pixels smeared across them.
        The piece only matches the sprite to a few pixels, so the sprite is first aligned to the piece: optical flow
        (DIS, at most `max_flow` px) from the piece drawn in its own colours through the sprite camera to the sprite.
        With `transfer`, a surface the sprite does not show is mirrored across the east-west plane through the piece's
        centre, or turned about its vertical axis (30-degree steps, smallest first, then mirrored and turned); the first
        of these that lands on a shown surface (the one the sprite shows there within `moved_tol` HD px, its normal
        within ~37 degrees) gives its pixel. So the back of a round hull gets the front's pixels where the shapes agree,
        and a feature only on the front (a ramp) is not painted onto the back's plain wall.
        With `similar`, what is still unpainted takes, per triangle, the sprite around the most alike point the sprite
        shows well (`alike_features` of the triangle's centre: height, distance from the centre, normal against the
        radial direction, and closeness to the point folded onto the south half), laid out along the surface the same
        way around both (`surface_axes`), so a triangle gets a coherent patch, not pixels from scattered places; fully
        below similar[0] in feature distance, not at all above similar[1]. Surfaces facing down are darkened (the hull's shadow); the sprite never
        shows one. Only beyond that the piece's own colours remain.
        Returns (piece, share of the surface textured from the sprite directly, from moved points, from alike points);
        with `pairs`, only (the piece's own colours, the sprite's) where the sprite shows the surface (`colour_map`)."""
        f, w, X, UV = texel_samples(self.V, self.F, self.uv, size, per_texel)
        old = np.asarray(Image.fromarray(self.tex).resize((size, size), Image.BICUBIC)).astype(np.float64)
        tr, tc = texel(UV, size)
        own = old[tr, tc]
        a, b, c = (self.V[self.F[:, k]] for k in range(3))
        nrm = np.cross(b - a, c - a); nrm /= np.linalg.norm(nrm, axis=1, keepdims=True) + 1e-12
        R = camera(elev, 0.0)
        u0, v0 = P[0, 0] @ R[0], P[0, 0] @ R[1]
        h, wd = mask.shape
        cu, rv, depth = X @ R[0] - u0, X @ R[1] - v0, X @ R[2]
        ci, ri = np.round(cu).astype(int), np.round(rv).astype(int)
        inside = (ci >= 0) & (ci < wd) & (ri >= 0) & (ri < h)
        ci, ri = np.clip(ci, 0, wd - 1), np.clip(ri, 0, h - 1)
        # the piece through the sprite camera: nearest sample per pixel, its depth and its own colour
        key = ri * wd + ci
        order = np.lexsort((depth, key)); order = order[inside[order]]
        first = order[np.r_[True, key[order][1:] != key[order][:-1]]]
        zb = np.full(h * wd, np.inf); zb[key[first]] = depth[first]
        drawn = np.zeros((h * wd, 3)); drawn[key[first]] = own[first]
        has = np.isfinite(zb).reshape(h, wd)
        # optical flow from the piece's picture to the sprite (backgrounds the same grey, so outlines pull too)
        grey = lambda im, m: np.where(m, (im[..., :3] @ [0.299, 0.587, 0.114]), 128).astype(np.uint8)
        dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
        flow = dis.calc(grey(drawn.reshape(h, wd, 3), has), grey(rgba.astype(np.float64), mask), None)
        mag = np.linalg.norm(flow, axis=2, keepdims=True)
        flow = flow * np.minimum(1, max_flow / np.maximum(mag, 1e-6))
        nb = np.zeros((h * wd, 3)); nb[key[first]] = nrm[f][first]
        image = rgba[..., :3].astype(np.float64)

        def look(Y, N, moved):
            """Weight and sprite colour for points Y with normals N. Unmoved: Y is the nearest surface at its pixel.
            Moved (`transfer`): the surface the sprite shows there lies within `moved_tol` of Y and turns the same way."""
            cu, rv, d = Y @ R[0] - u0, Y @ R[1] - v0, Y @ R[2]
            ci, ri = np.round(cu).astype(int), np.round(rv).astype(int)
            ok = (ci >= 0) & (ci < wd) & (ri >= 0) & (ri < h)
            ci, ri = np.clip(ci, 0, wd - 1), np.clip(ri, 0, h - 1)
            k = ri * wd + ci
            su, sv = cu + flow[ri, ci, 0], rv + flow[ri, ci, 1]
            cos = -(N @ R[2])
            ok &= mask[np.clip(np.round(sv).astype(int), 0, h - 1), np.clip(np.round(su).astype(int), 0, wd - 1)] & (cos > facing[0])
            ok &= ((np.abs(d - zb[k]) < moved_tol) & ((N * nb[k]).sum(1) > 0.8)) if moved else (d < zb[k] + tol)
            return ok * np.clip((cos - facing[0]) / (facing[1] - facing[0]), 0, 1), su, sv

        wt, su, sv = look(X, nrm[f], False)
        sprite = bilinear(image, mask, su, sv)
        if pairs:  # only where the mod has a texture: the sprite's colours of the other parts would be spread over it
            textured = (wt > 0.5) & (self.fmat[f] >= 0) & (self.fmat[f] < len(self.model[4]))
            return own[textured], sprite[textured]
        q = np.linspace(0, 100, 257)
        pair = wt > 0.5
        mapped_tex, mapped = np.empty_like(old), np.empty_like(own)
        for ch in range(3):  # the piece's colours -> the sprite's, from the texels that have both
            src, dst = np.percentile(own[pair, ch], q), np.percentile(sprite[pair, ch], q)
            mapped_tex[..., ch] = np.interp(old[..., ch], src, dst); mapped[:, ch] = np.interp(own[:, ch], src, dst)
        col, left = wt[:, None] * sprite, 1 - wt
        moved, alike = np.zeros(len(X)), np.zeros(len(X))
        centre = np.r_[0.5 * (self.V[:, :2].min(0) + self.V[:, :2].max(0)) if centre is None else centre, 0.0]
        if transfer:  # surfaces the sprite does not show take the pixels of the shown ones they mirror or turn onto
            mirror = np.diag([1.0, -1.0, 1.0])
            turns = [mirror] + [rz(s * t) for t in range(30, 181, 30) for s in (1, -1)]
            turns += [mirror @ rz(s * t) for t in range(30, 181, 30) for s in (1, -1)]
            for T in turns:
                i = np.nonzero(left > 0.01)[0]
                w_t, su, sv = look((X[i] - centre) @ T.T + centre, nrm[f[i]] @ T.T, True)
                keep = w_t > 0
                j, share = i[keep], left[i[keep]] * w_t[keep]
                col[j] += share[:, None] * bilinear(image, mask, su[keep], sv[keep])
                moved[j] += share; left[j] -= share
        if similar:  # the rest: per triangle, the patch of the sprite around the most alike shown point
            i = np.nonzero(left > 0.01)[0]
            src = np.nonzero(wt > 0.9)[0]
            src = src[np.random.default_rng(0).permutation(len(src))[:400000]]
            tree = cKDTree(alike_features(X[src] - centre, nrm[f[src]], False))
            tri, inv = np.unique(f[i], return_inverse=True)
            mid = (a[tri] + b[tri] + c[tri]) / 3
            d, j = tree.query(alike_features(mid - centre, nrm[tri], True), workers=-1)
            j = src[j]
            # each sample's offset from its triangle's centre, along the surface, laid out the same way around the
            # source point (horizontal along the surface, then up it) and looked up in the sprite
            t1, t2 = surface_axes(nrm[tri])
            s1, s2 = surface_axes(nrm[f[j]])
            D = X[i] - mid[inv]
            Y = X[j][inv] + (D * t1[inv]).sum(1, keepdims=True) * s1[inv] + (D * t2[inv]).sum(1, keepdims=True) * s2[inv]
            pu, pv = Y @ R[0] - u0, Y @ R[1] - v0
            pc, pr = np.clip(np.round(pu).astype(int), 0, wd - 1), np.clip(np.round(pv).astype(int), 0, h - 1)
            pu, pv = pu + flow[pr, pc, 0], pv + flow[pr, pc, 1]
            on = mask[np.clip(np.round(pv).astype(int), 0, h - 1), np.clip(np.round(pu).astype(int), 0, wd - 1)]
            patch = np.where(on[:, None], bilinear(image, mask, pu, pv), sprite[j][inv])  # off the sprite: the point's own
            close = np.clip((similar[1] - d) / (similar[1] - similar[0]), 0, 1)[inv]
            shade = 0.45 + 0.55 * np.clip(1 + nrm[f[i], 2], 0, 1)  # facing down: in the hull's shadow
            share = left[i] * close
            col[i] += (share * shade)[:, None] * patch
            alike[i] += share; left[i] -= share
        col += left[:, None] * mapped
        tex = to_texture(UV, col, size, background=mapped_tex)
        out = Piece.__new__(Piece)
        out.V, out.F, out.uv, out.scale, out.tex = self.V, self.F, self.uv, self.scale, tex
        if hasattr(self, 'fmat'):
            out.fmat = self.fmat
        area = 0.5 * np.linalg.norm(np.cross(b - a, c - a), axis=1)
        per = area / np.bincount(f, minlength=len(self.F))
        return out, *(float((s * per[f]).sum() / area.sum()) for s in (wt, moved, alike))

    def recoloured(self, cmap, fallback, size=1.0):
        """The mod's own model (`from_model`) with its own UVs and textures, each texture recoloured by `cmap`
        (`colour_map`): one mesh per texture. Faces without a texture (base-game ones, `m3_fx.py`'s added parts) take
        `fallback`'s (this piece sprite-textured: same faces, atlas UVs)."""
        V, T, F, mat, tex = self.model
        to_gl = lambda X: size * X / HD_PER_GAME_PX @ np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]], float)  # z up -> y up
        meshes = {}
        for m in np.unique(mat):
            sel = mat == m
            if 0 <= m < len(tex):
                img, uv, Vm, Fm = cmap(tex[m]), np.c_[T[:, 0], 1 - T[:, 1]], V, F[sel]  # T: v down its image
            else:
                img, uv, Vm, Fm = fallback.tex, fallback.uv, fallback.V, fallback.F[fallback.fmat == m]
            mat_ = trimesh.visual.material.PBRMaterial(baseColorTexture=Image.fromarray(img), metallicFactor=0.0, roughnessFactor=1.0)
            mesh = trimesh.Trimesh(to_gl(Vm), Fm, visual=trimesh.visual.TextureVisuals(uv=uv, material=mat_), process=False)
            mesh.remove_unreferenced_vertices()
            meshes['body_t%d' % m if m >= 0 else 'body_sprite'] = mesh
        return meshes

    def samples(self):
        return sample_surface(self.V, self.F, self.uv, self.tex, per_area=4.0, seed=1)

    def mesh(self, size=1.0):
        V = size * self.V / HD_PER_GAME_PX @ np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]], float)  # z up -> y up (glTF)
        mat = trimesh.visual.material.PBRMaterial(baseColorTexture=Image.fromarray(self.tex), metallicFactor=0.0, roughnessFactor=1.0)
        m = trimesh.Trimesh(V, self.F, visual=trimesh.visual.TextureVisuals(uv=self.uv, material=mat), process=False)
        m.remove_unreferenced_vertices()
        return m


if __name__ == '__main__':
    image_id, frame, glb, radius, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], float(sys.argv[4]), sys.argv[5]
    elev, beta = CAMERA_BY_IMAGE.get(image_id, (ELEV, BETA))
    P, rgba, mask = lift(Anim(DATA % image_id), frame, elev=elev, beta=beta)
    yaws = next(([int(v) % 360 for v in range(int(a), int(b) + 1, 5)] for a, b in
                 (spec.split(':')[1:3] for spec in sys.argv[6:] if spec.startswith('yaws:'))), range(0, 360, 5))
    squash = next((tuple(float(v) for v in spec.split(':')[1:3]) for spec in sys.argv[6:] if spec.startswith('squash:')), None)
    size = next((float(spec.split(':')[1]) for spec in sys.argv[6:] if spec.startswith('size:')), 1.0)
    body = (Piece.from_model(glb[:-4], P, mask, elev, 'body', yaws=yaws, squash=squash) if glb.endswith('.obj')
            else Piece(glb, P, mask, elev, 'body', yaws))
    parts, feet, wedges = [], [], []
    sprite_texture = 'sprite-texture' in sys.argv[6:] or 'recolour' in sys.argv[6:]
    recolour = 'recolour' in sys.argv[6:]
    for spec in sys.argv[6:]:
        src, *nums = spec.split(':')
        if src in ('sprite-texture', 'yaws', 'recolour', 'squash', 'gold', 'size'):
            continue
        if src == 'wedge':
            wedges.append(tuple(float(v) for v in nums))
            continue
        cx, cy = float(nums[0]), float(nums[1])
        n, x0, y0 = (int(nums[2]), float(nums[3]), float(nums[4])) if len(nums) > 2 else (1, cx, cy)
        part = Piece(src, P, part_mask(P, mask, cx, cy), elev, '%s at (%.0f, %.0f)' % (src.split('/')[-1], cx, cy))
        stem = src.split('/')[-1].rsplit('.', 1)[0]
        part.name = stem
        parts.append((part, part_mask(P, mask, cx, cy))); feet.append((cx, cy))
        for k in range(1, n):
            fx, fy = (np.array([cx, cy]) - [x0, y0]) @ rz(360.0 * k / n)[:2, :2].T + [x0, y0]
            pm = part_mask(P, mask, fx, fy)
            turn = round(part.yaw + 360.0 * k / n)
            q = Piece(src, P, pm, elev, '  turned %d at (%.0f, %.0f)' % (360 * k // n, fx, fy), range(turn - 20, turn + 21, 5), part.scale)
            q.name = '%s_turned%d' % (stem, 360 * k // n)
            parts.append((q, pm)); feet.append((fx, fy))
    body = body.with_wedges(wedges).without(lambda X: np.any([cut(X, fx, fy) for fx, fy in feet] + [np.zeros(len(X), bool)], axis=0))
    pieces = [(body, mask)] + [(q.without(lambda X: np.zeros(len(X), bool)), pm) for q, pm in parts]
    names = ['body'] + [q.name for q, _ in parts]  # object names in Blender
    if sprite_texture:
        for i, (p, m) in enumerate(pieces):
            tp, share, moved, alike = p.sprite_textured(P, rgba, mask, elev)
            print('%s: surface textured from the sprite %.0f%% where it shows it, %.0f%% from mirrored or turned points, '
                  '%.0f%% from alike points' % (names[i], 100 * share, 100 * moved, 100 * alike))
            if recolour and i == 0:
                gold = next((tuple(float(v) for v in s.split(':')[1:3]) for s in sys.argv[6:] if s.startswith('gold:')), None)
                cmap = colour_map(*p.sprite_textured(P, rgba, mask, elev, pairs=True), gold=gold)
                recoloured = p.recoloured(cmap, tp, size)
            pieces[i] = (tp, m)
    if recolour:  # the mod's own model and textures, recoloured (sprite pixels only where it has no texture)
        trimesh.Scene(recoloured).export(out + '.glb')
    else:
        trimesh.Scene({n: p.mesh(size) for n, (p, _) in zip(names, pieces)}).export(out + '.glb')
    S = [p.samples() for p, _ in pieces]
    X = np.concatenate([s[0] for s in S])
    cols = [np.concatenate([s[1] for s in S]), np.concatenate([match_palette(s[1], rgba, m) for s, (_, m) in zip(S, pieces)])]
    zmax = np.percentile(P[..., 2][mask], 95)
    rows = []
    for col, kind in zip(cols, ['TRELLIS body + swapped parts, TRELLIS colours', 'the same on the sprite palette']):
        tiles = []
        for name, eye, tgt in views_for(radius, zmax * 0.45):
            cam = look_camera(eye, tgt, 66, W, H)
            img = np.zeros((H, W, 3), np.uint8); zb = np.full((H, W), np.inf)
            ground(img, zb, cam, W, H)
            points_splat(img, zb, X, col, cam)
            ti = Image.fromarray(img); d = ImageDraw.Draw(ti); d.text((4, 4), name, fill=(255, 255, 0)); d.text((4, H - 14), kind, fill=(255, 255, 255))
            tiles.append(np.asarray(ti))
        rows.append(np.concatenate(tiles, 1))
    Image.fromarray(np.concatenate(rows, 0)).save(out + '.png')
    print('wrote %s.glb, %s.png' % (out, out))
