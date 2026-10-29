"""First-person previews of a lifted sprite: a perspective camera at eye height orbiting the model, drawn by splatting
each pixel cell of the lifted grid (subdivided, z-buffered). Also draws today's billboard for comparison."""
import numpy as np


def look_camera(eye, target, fov_deg, W, H):
    f = target - eye; f /= np.linalg.norm(f)
    r = np.cross(f, [0, 0, 1.0]); r /= np.linalg.norm(r)
    d = np.cross(f, r)  # screen down
    focal = (W / 2) / np.tan(np.radians(fov_deg) / 2)
    return eye, np.stack([r, d, f]), focal


def cells(P, valid, jump=6.0, ratio=1.6, longest=12.0):
    """Quads of the grid to fill, and their longest edge. An edge between two neighbouring pixels is cut at a step in
    depth: longer than `jump` and `ratio` times longer than the edges before and after it in the same direction. A steady
    slope seen at a grazing angle (a foot's sole, a railing) keeps its long edges; nothing longer than `longest` is kept."""
    def kept(E, axis):
        n = E.shape[axis]
        pad = np.full_like(np.take(E, [0], axis), np.nan)
        prev = np.concatenate([pad, np.take(E, range(n - 1), axis)], axis)
        nxt = np.concatenate([np.take(E, range(1, n), axis), pad], axis)
        nb = np.fmax(prev, nxt)
        return (E <= jump) | ((E <= ratio * nb) & (E <= longest))
    Ev = np.where(valid[1:] & valid[:-1], np.linalg.norm(P[1:] - P[:-1], axis=-1), np.nan)
    Eu = np.where(valid[:, 1:] & valid[:, :-1], np.linalg.norm(P[:, 1:] - P[:, :-1], axis=-1), np.nan)
    Kv, Ku = kept(Ev, 0), kept(Eu, 1)
    diag = np.linalg.norm(P[1:, 1:] - P[:-1, :-1], axis=-1)
    q = Kv[:, :-1] & Kv[:, 1:] & Ku[:-1] & Ku[1:] & (diag <= longest)
    span = np.fmax.reduce([Ev[:, :-1], Ev[:, 1:], Eu[:-1], Eu[1:], diag])
    return q, np.where(q, span, 0.0)


def splat(img, zbuf, P, col, cam, W, H, sub=3, mask=None):
    """P (h,w,3) grid of points with colours col (h,w,4); quads between valid neighbours (`cells`) are sampled
    bilinearly, sub x sub or more for long quads. Pixels that belong to no quad are dropped (they would float)."""
    eye, R, focal = cam
    valid = col[..., 3] >= 128 if mask is None else mask
    pts, cols = [], []
    q, span = cells(P, valid)
    need = np.clip(np.ceil(span / 1.5), sub, 32).astype(int)
    for n in np.unique(need[q]):
        qn = q & (need == n)
        for a in range(n):
            for b in range(n):
                s, t = (a + 0.5) / n, (b + 0.5) / n
                X = ((1 - s) * (1 - t)) * P[:-1, :-1] + (s * (1 - t)) * P[1:, :-1] + ((1 - s) * t) * P[:-1, 1:] + (s * t) * P[1:, 1:]
                C = ((1 - s) * (1 - t)) * col[:-1, :-1, :3] + (s * (1 - t)) * col[1:, :-1, :3] + ((1 - s) * t) * col[:-1, 1:, :3] + (s * t) * col[1:, 1:, :3]
                pts.append(X[qn]); cols.append(C[qn])
    X = np.concatenate(pts); C = np.concatenate(cols)
    c = (X - eye) @ R.T
    front = c[:, 2] > 1
    c, C = c[front], C[front]
    u = np.floor(c[:, 0] / c[:, 2] * focal + W / 2).astype(int)
    v = np.floor(c[:, 1] / c[:, 2] * focal + H / 2).astype(int)
    ok = (u >= 0) & (v >= 0) & (u < W) & (v < H)
    u, v, z, C = u[ok], v[ok], c[ok, 2], C[ok]
    # point size: one pixel, plus a second for close points (avoid pinholes)
    for du, dv in ((0, 0), (1, 0), (0, 1), (1, 1)):
        uu, vv = np.clip(u + du, 0, W - 1), np.clip(v + dv, 0, H - 1)
        key = vv * W + uu
        order = np.lexsort((z, key))
        first = np.ones(len(order), bool); first[1:] = key[order][1:] != key[order][:-1]
        sel = order[first]
        zk = z[sel] + (0 if du == dv == 0 else 0.5)
        better = zk < zbuf.ravel()[key[sel]]
        k = key[sel][better]
        zbuf.ravel()[k] = zk[better]
        img.reshape(-1, 3)[k] = np.clip(C[sel][better], 0, 255).astype(np.uint8)


def billboard(img, zbuf, rgba, ground_row, width_px, cam, W, H, center):
    """Today's look: the sprite frame as an upright quad facing the camera, its ground line on the floor at `center`.
    rgba: tight frame; ground_row: frame row standing on the floor; width_px: HD px per frame column."""
    eye, R, focal = cam
    h, w = rgba.shape[:2]
    right = R[0].copy(); right[2] = 0; right /= np.linalg.norm(right)
    vv, uu = np.mgrid[0:h, 0:w].astype(float)
    X = center + (uu - w / 2)[..., None] * right * width_px + (ground_row - vv)[..., None] * np.array([0, 0, 1.0]) * width_px
    splat(img, zbuf, X, rgba, cam, W, H, sub=2)


def ground(img, zbuf, cam, W, H, colour=(70, 64, 52), grid=128.0):
    """A flat floor with a faint grid (1 game tile = 128 HD px) so the scale reads."""
    eye, R, focal = cam
    vv, uu = np.mgrid[0:H, 0:W].astype(float)
    dirs = ((uu - W / 2) / focal)[..., None] * R[0] + ((vv - H / 2) / focal)[..., None] * R[1] + R[2]
    t = -eye[2] / np.where(dirs[..., 2] < -1e-6, dirs[..., 2], -1e-6)
    hit = dirs[..., 2] < -1e-6
    X = eye + t[..., None] * dirs
    depth = t * (dirs @ R[2])
    lines = (np.abs(((X[..., 0] / grid) % 1) - 0.5) > 0.49) | (np.abs(((X[..., 1] / grid) % 1) - 0.5) > 0.49)
    base = np.array(colour, float)
    c = np.where(lines[..., None], base * 1.25, base)
    fog = np.clip(depth / 6000, 0, 1)[..., None]
    sky = np.array([120, 140, 165.0])
    out = np.where(hit[..., None], c * (1 - fog) + sky * fog, sky)
    img[:] = out.astype(np.uint8)
    zbuf[:] = np.where(hit, depth, np.inf)
