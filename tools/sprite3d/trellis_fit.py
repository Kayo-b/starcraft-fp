"""Combine a TRELLIS mesh (from `kaggle/trellis_kernel.py`) with the sprite's own relief: the relief keeps every pixel the
sprite shows, the mesh adds only the surfaces hidden from the sprite camera (TRELLIS's colours on the sprite's palette).

Fit: the mesh is turned (yaw, every 5 degrees), scaled and moved so that its outline seen from the calibrated sprite
camera best covers the sprite's alpha (IoU), then moved along the view ray so its front matches the relief's depth.
Parts that TRELLIS fuses with what stands behind them (the Nexus's side arches) can be swapped: the body mesh loses what
`relief.part_mask`'s rule selects around each foot (CX, CY) and gets instead either the part's own relief, extruded 30
HD px along the sprite camera's view ray (`relief:CX:CY`; the original art), or a part mesh generated on its own
(`PART.glb:CX:CY`, from `trellis_inputs.py` NAME:IMAGE_ID:FRAME:CX:CY) fitted to the part's cut-out of the relief.
The Nexus's arches are best as TRELLIS's mesh of one arch alone (seed 2 of the west cut-out) on all four discs, as one
GLB: `trellis_compose.py`.
Run with the venv python (needs trimesh):  trellis_fit.py IMAGE_ID FRAME MESH.glb RADIUS OUT.png [PART:CX:CY ...]"""
import sys
import numpy as np
import trimesh
from PIL import Image, ImageDraw
from anim import Anim
from relief import lift, part_mask, extrude, ELEV, BETA, CAMERA_BY_IMAGE
from views import camera
from preview import look_camera, splat, ground
from demo import DATA, views_for, W, H


def load_glb(path):
    """Vertices (z up, as TRELLIS generates them), faces, UVs and the RGB texture of a TRELLIS GLB (exported y up).
    Ground plates are dropped: flat pieces spanning the whole generation cube, which TRELLIS adds under the Dragoon (its
    near-black feet on TRELLIS's black input background read as dark ground), and the fragments at their height."""
    m = trimesh.load(path, force='mesh')
    V = m.vertices @ np.array([[1, 0, 0], [0, 0, 1], [0, -1, 0]], float)  # undo to_glb's z-up -> y-up turn
    F = np.asarray(m.faces)
    lab = trimesh.graph.connected_component_labels(m.face_adjacency, node_count=len(F))
    comps = [(c, V[np.unique(F[lab == c])]) for c in np.unique(lab)]
    plates = [np.median(v[:, 2]) for c, v in comps if max(np.ptp(v[:, 0]), np.ptp(v[:, 1])) > 0.9 and np.ptp(v[:, 2]) < 0.03]
    for c, v in comps:
        if any(np.abs(v[:, 2] - z).max() < 0.015 for z in plates):
            lab[lab == c] = -1
    tex = np.asarray(m.visual.material.baseColorTexture.convert('RGB'))
    return V, F[lab >= 0], np.asarray(m.visual.uv), tex


def sample_surface(V, F, uv, tex, per_area, seed=0):
    """Points spread over the triangles (per_area points per unit area, at least one each) with texture colours."""
    rng = np.random.default_rng(seed)
    a, b, c = V[F[:, 0]], V[F[:, 1]], V[F[:, 2]]
    n = np.maximum(1, np.ceil(0.5 * np.linalg.norm(np.cross(b - a, c - a), axis=1) * per_area)).astype(int)
    f = np.repeat(np.arange(len(F)), n)
    r1, r2 = rng.random(len(f)), rng.random(len(f))
    s = np.sqrt(r1)
    wa, wb, wc = 1 - s, s * (1 - r2), s * r2
    X = wa[:, None] * a[f] + wb[:, None] * b[f] + wc[:, None] * c[f]
    UV = wa[:, None] * uv[F[f, 0]] + wb[:, None] * uv[F[f, 1]] + wc[:, None] * uv[F[f, 2]]
    th, tw = tex.shape[:2]
    col = tex[np.clip(((1 - UV[:, 1]) * (th - 1)).round().astype(int), 0, th - 1), np.clip((UV[:, 0] * (tw - 1)).round().astype(int), 0, tw - 1)]
    return X, col


def rz(deg):
    c, s = np.cos(np.radians(deg)), np.sin(np.radians(deg))
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1.0]])


def fit(X, P, mask, elev, yaws=range(0, 360, 5), scale=None):
    """Yaw (one of `yaws`), scale (unless given) and offset putting the mesh samples X over the relief P as the sprite
    camera sees them. Returns (yaw, scale, t, iou); model point = scale * Rz(yaw) @ x + t."""
    R = camera(elev, 0.0)
    U, Vv = P @ R[0], P @ R[1]
    u0, v0 = U[0, 0], Vv[0, 0]  # frame pixel (0, 0): U and V step by exactly one per column and row
    h, w = mask.shape
    rows, cols = np.nonzero(mask)
    best = None
    for yaw in yaws:
        Y = X @ rz(yaw).T
        pu, pv = Y @ R[0], Y @ R[1]
        # scale and offset from the outlines' extents (1st-99th percentile, robust to stray samples)
        lo_u, hi_u, lo_v, hi_v = *np.percentile(pu, [0.5, 99.5]), *np.percentile(pv, [0.5, 99.5])
        tlo_u, thi_u, tlo_v, thi_v = *np.percentile(U[mask], [0.5, 99.5]), *np.percentile(Vv[mask], [0.5, 99.5])
        s = scale or 0.5 * ((thi_u - tlo_u) / (hi_u - lo_u) + (thi_v - tlo_v) / (hi_v - lo_v))
        cu = 0.5 * (tlo_u + thi_u) - s * 0.5 * (lo_u + hi_u)
        cv = 0.5 * (tlo_v + thi_v) - s * 0.5 * (lo_v + hi_v)
        c = np.clip(np.round(s * pu + cu - u0).astype(int), 0, w - 1)
        r = np.clip(np.round(s * pv + cv - v0).astype(int), 0, h - 1)
        occ = np.zeros_like(mask); occ[r, c] = True
        occ |= np.roll(occ, 1, 0) | np.roll(occ, 1, 1)
        iou = (occ & mask).sum() / (occ | mask).sum()
        if best is None or iou > best[3]:
            best = (yaw, s, (cu, cv), iou)
    yaw, s, (cu, cv), iou = best
    # depth: move along the view ray so the mesh's nearest surface per pixel matches the relief's depth
    Y = s * (X @ rz(yaw).T)
    pu, pv, pw = Y @ R[0] + cu, Y @ R[1] + cv, Y @ R[2]
    c = np.clip(np.round(pu - u0).astype(int), 0, w - 1); r = np.clip(np.round(pv - v0).astype(int), 0, h - 1)
    zb = np.full(mask.shape, np.inf); np.minimum.at(zb, (r, c), pw)
    Wr = P @ R[2]
    both = mask & np.isfinite(zb)
    dw = np.median(Wr[both] - zb[both])
    t = cu * R[0] + cv * R[1] + dw * R[2]
    return yaw, s, t, iou


def hidden_part(Xm, P, mask, elev, tol=12.0):
    """Mesh samples (model frame) the sprite camera does not see: inside the sprite's outline and more than `tol` HD px
    behind the relief. Samples outside the outline are dropped (the sprite would show them if they existed)."""
    R = camera(elev, 0.0)
    U, Vv, Wr = P @ R[0], P @ R[1], P @ R[2]
    u0, v0 = U[0, 0], Vv[0, 0]  # frame pixel (0, 0): U and V step by exactly one per column and row
    h, w = mask.shape
    c = np.round(Xm @ R[0] - u0).astype(int); r = np.round(Xm @ R[1] - v0).astype(int)
    inside = (c >= 0) & (c < w) & (r >= 0) & (r < h)
    keep = np.zeros(len(Xm), bool)
    ci, ri = c[inside], r[inside]
    keep[inside] = mask[ri, ci] & (Xm[inside] @ R[2] > np.where(mask[ri, ci], Wr[ri, ci], -np.inf) + tol)
    return keep


def match_palette(col, rgba, mask):
    """TRELLIS's colours mapped onto the sprite's palette: each channel's distribution matched to the sprite's pixels
    (quantile mapping). Projecting the sprite's pixels onto the mesh was tried and speckles: the fitted mesh is close to
    the sprite's shape (outline IoU ~0.84) but not pixel-exact."""
    out = np.empty(col.shape, np.float64)
    q = np.linspace(0, 100, 257)
    for ch in range(3):
        out[:, ch] = np.interp(col[:, ch], np.percentile(col[:, ch], q), np.percentile(rgba[..., ch][mask], q))
    return np.clip(out, 0, 255).astype(np.uint8)


def points_splat(img, zb, X, col, cam):
    """Splat loose points (2x2 footprint, z-buffered) through the preview camera."""
    eye, R, focal = cam
    c = (X - eye) @ R.T
    ok = c[:, 2] > 1
    c, col = c[ok], col[ok]
    u = np.floor(c[:, 0] / c[:, 2] * focal + W / 2).astype(int); v = np.floor(c[:, 1] / c[:, 2] * focal + H / 2).astype(int)
    for du, dv in ((0, 0), (1, 0), (0, 1), (1, 1)):
        uu, vv = u + du, v + dv
        ok = (uu >= 0) & (vv >= 0) & (uu < W) & (vv < H)
        key = vv[ok] * W + uu[ok]; z = c[ok, 2]; cc = col[ok]
        order = np.lexsort((z, key)); first = np.ones(len(order), bool); first[1:] = key[order][1:] != key[order][:-1]
        sel = order[first]; better = z[sel] < zb.ravel()[key[sel]]
        k = key[sel][better]; zb.ravel()[k] = z[sel][better]; img.reshape(-1, 3)[k] = cc[sel][better]


def fitted(glb, P, rgba, mask, elev, name):
    """Mesh samples (about 4 per HD px^2) in the model frame, fitted to the relief within `mask`, with TRELLIS's
    colours and the same on the sprite's palette."""
    V, F, uv, tex = load_glb(glb)
    yaw, s, t, iou = fit(sample_surface(V, F, uv, tex, per_area=4e5)[0], P, mask, elev)
    print('%s: yaw %d, scale %.0f HD px per unit, outline IoU %.2f' % (name, yaw, s, iou))
    Xd, col = sample_surface(V, F, uv, tex, per_area=4.0 * s * s, seed=1)
    return s * (Xd @ rz(yaw).T) + t, col, match_palette(col, rgba, mask)


if __name__ == '__main__':
    image_id, frame, glb, radius, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], float(sys.argv[4]), sys.argv[5]
    elev, beta = CAMERA_BY_IMAGE.get(image_id, (ELEV, BETA))
    a = Anim(DATA % image_id)
    P, rgba, mask = lift(a, frame, elev=elev, beta=beta)
    Xm, cold, matched = fitted(glb, P, rgba, mask, elev, 'body')
    parts = []  # (body samples to drop, part samples, part colours, relief layers, part mask)
    for spec in sys.argv[6:]:
        src, cx, cy = spec.rsplit(':', 2)
        cx, cy = float(cx), float(cy)
        d = np.hypot(Xm[:, 0] - cx, Xm[:, 1] - cy)
        cut = (d < 60) | ((d < 100) & (Xm[:, 2] > 90))  # relief.part_mask's rule, on the body mesh
        pm = part_mask(P, mask, cx, cy)
        if src == 'relief':
            parts.append((cut, Xm[:0], matched[:0], [P] + extrude(P, elev, 30.0, 12), pm))
        else:
            Xp, _, Cp = fitted(src, P, rgba, pm, elev, src.split('/')[-1])
            parts.append((cut, Xp, Cp, [], pm))
    keep = hidden_part(Xm, P, mask, elev)
    print('mesh samples %d, hidden from the sprite camera %d' % (len(Xm), keep.sum()))
    zmax = np.percentile(P[..., 2][mask], 95)
    rows = []
    kinds = ['relief + generated hidden side', 'generated mesh, sprite palette', 'generated mesh, TRELLIS colours']
    if parts:
        kinds = ['generated mesh + swapped parts, sprite palette', 'generated mesh, sprite palette']
        body = ~np.any([p[0] for p in parts], axis=0)
        Xa = np.concatenate([Xm[body]] + [p[1] for p in parts]); Ca = np.concatenate([matched[body]] + [p[2] for p in parts])
    for kind in kinds:
        tiles = []
        for name, eye, tgt in views_for(radius, zmax * 0.45):
            cam = look_camera(eye, tgt, 66, W, H)
            img = np.zeros((H, W, 3), np.uint8); zb = np.full((H, W), np.inf)
            ground(img, zb, cam, W, H)
            if kind.startswith('generated mesh + swapped'):
                points_splat(img, zb, Xa, Ca, cam)
                for _, _, _, layers, pm in parts:
                    for L in layers:
                        splat(img, zb, L, rgba, cam, W, H, mask=pm)
            elif kind.startswith('relief'):
                splat(img, zb, P, rgba, cam, W, H, mask=mask)
                points_splat(img, zb, Xm[keep], matched[keep], cam)
            elif kind.endswith('palette'):
                points_splat(img, zb, Xm, matched, cam)
            else:
                points_splat(img, zb, Xm, cold, cam)
            ti = Image.fromarray(img); d = ImageDraw.Draw(ti); d.text((4, 4), name, fill=(255, 255, 0)); d.text((4, H - 14), kind, fill=(255, 255, 255))
            tiles.append(np.asarray(ti))
        rows.append(np.concatenate(tiles, 1))
    Image.fromarray(np.concatenate(rows, 0)).save(out)
