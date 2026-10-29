"""Voxel model of one animation frame of a turning unit, from its drawn directions through the sprite camera.

A turning unit's 17 drawn directions are 17 orthographic views of one pose (11.25 degrees apart, camera tilted down about
35 degrees, see views.py). The turning axis is not always on the box's centre column (the Marine's is 6.5 HD px right of
it) and the tilt differs a little from the buildings' (the Marine fits 33 degrees best): `calibrate` finds both as the
pair whose hull agrees best with all 17 silhouettes (Marine: IoU 0.99; 0.89 on the centre column at 35 degrees).
Units were not all drawn from the same height: the SCV fits 50 degrees as clearly as the Marine fits 31. Their silhouettes carve a visual hull; each view's depth layer (ao_depth alpha, D = c - 0.6 w,
w along the ray) then empties what lies in front of the surface that view shows, which takes out the hollows a
silhouette cannot see (between the rifle and the chest, between the legs). The depth scale is fitted per unit, not
taken from the buildings (relief.BETA = -0.6): the Marine's is -0.79 (D levels per HD px along the ray), found as the
slope of the hull's upper depth envelope (the hull is never behind the true surface, and is on it where it is convex). Every surface voxel takes the pixel of the
view that sees it most squarely (no blending, so the art stays crisp); surfaces the 17 views never face (the unit's left
side) take the mirrored directions 17-31, where their depth agrees, else the nearest coloured voxel.

    unit_voxel.py ANIM GROUP OUT.npz [ELEV AXIS]      (calibrated when not given)
Units that fly or hover keep their undersides (build(..., soles=False)).
Writes the occupied voxels: grid indices, colours (RGBA, A = the teamcolor mask), normals, and the grid placement
(origin, voxel size; model frame x east, y north, z up, HD pixels, z = 0 on the lowest voxel, x = y = 0 on the turning
axis). Run with the venv python (scipy)."""
import sys
import numpy as np
from scipy import ndimage
from anim import Anim
from views import camera
from relief import median3, denoise, unclip, BETA

NDRAWN = 17


class Shot:
    """One direction of the frame group: colours, team mask, silhouette, cleaned depth and where depth can be trusted."""

    def __init__(self, a, group, d, elev, axis):
        src = d if d < NDRAWN else 32 - d
        i = group * NDRAWN + src
        rgba = a.frame(i, 'diffuse')
        tc = a.frame(i, 'teamcolor')[..., 0] if 'teamcolor' in a.layers else np.zeros(rgba.shape[:2], np.uint8)
        D = a.frame(i, 'ao_depth')[..., 3].astype(np.float64) if 'ao_depth' in a.layers else np.zeros(rgba.shape[:2])
        if d >= NDRAWN:
            rgba, tc, D = rgba[:, ::-1], tc[:, ::-1], D[:, ::-1]
        mask = rgba[..., 3] >= 128
        has_depth = D > 0  # 0: no depth there (the Medic has none at all; glowing parts of others)
        D = unclip(D, mask, elev, BETA)
        Dm = median3(np.where(mask, D, np.nan))
        inner = ndimage.binary_erosion(mask, np.ones((3, 3), bool))
        self.depth_ok = inner & np.isfinite(Dm) & (np.abs(D - Dm) < 3) & has_depth
        self.D = denoise(np.where(self.depth_ok, Dm, D), self.depth_ok)
        self.rgba, self.tc, self.mask = rgba, tc, mask
        self.heading = d * 360.0 / 32
        self.R = camera(elev, self.heading)
        self.mirrored = d >= NDRAWN
        self.ax = a.box_w / 2.0 + (-axis if self.mirrored else axis)  # column of the turning axis (mirrored: flipped)
        self.cy = a.box_h / 2.0
        self.c = self.k = None  # depth along the ray: w = c + k D

    def project(self, P, bw, bh):
        p = P @ self.R.T
        return np.floor(p[:, 0] + self.ax).astype(int), np.floor(p[:, 1] + self.cy).astype(int), p[:, 2]

    def lookup(self, img, ui, vi, fill):
        h, w = img.shape[:2]
        ok = (ui >= 0) & (vi >= 0) & (ui < w) & (vi < h)
        out = np.full(len(ui), fill, dtype=img.dtype) if img.ndim == 2 else np.zeros((len(ui),) + img.shape[2:], img.dtype)
        out[ok] = img[vi[ok], ui[ok]]
        return out, ok


def front_depth(ui, vi, w, shape):
    """Per pixel, the nearest w among the given projections (inf where none)."""
    z = np.full(shape, np.inf)
    ok = (ui >= 0) & (vi >= 0) & (ui < shape[1]) & (vi < shape[0])
    np.minimum.at(z, (vi[ok], ui[ok]), w[ok])
    return z


def cut_underside(occ, si, seen, log=print, tilt=31.0, keep=1):
    """What the carving leaves under a unit that no direction sees: from above, the space under a foot, a belly, a track
    or a turret's barrels stays inside the silhouettes of every direction (a cone under a flat bottom of half-size r,
    r tan(tilt) deep: 9 voxels under a Marine boot at 31 degrees, ~115 under a Siege Tank at 45), and at eye level it
    showed as a dark smear (painted with the fill) or a thick sole.
    In every column (x, y) of the lower part (as deep as the cone can be: the unit's half-size x tan(tilt) over its
    height plus 0.15, between 0.5 and 0.9: a Marine 0.5, a Siege Tank 0.86) the solid below the unit's underside goes, the
    underside being the lowest surface some direction sees where a column has one (a leg's or a belly's rim, seen
    from the side down to the ground) and, across columns with none low down (inside a leg, under a belly), a membrane
    spanning those rims (their heights filled in by normalized convolution). Legs keep their insides, bellies become a
    surface between their seen rims, the cones go; a cone-only rule took a Zergling's legs with its belly."""
    zs_any = np.flatnonzero(occ.any((0, 1)))
    bottom, top = zs_any[0], zs_any[-1]
    xy = np.argwhere(occ.any(2))
    half = 0.5 * max(np.ptp(xy[:, 0]), np.ptp(xy[:, 1]))
    low = float(np.clip(half * np.tan(np.radians(tilt)) / max(1, top - bottom) + 0.15, 0.5, 0.9))
    zlim = bottom + low * (top - bottom)
    s = si[seen & (si[:, 2] < zlim)]
    BIG = 1e9
    L = np.full(occ.shape[:2], BIG)
    np.minimum.at(L, (s[:, 0], s[:, 1]), s[:, 2].astype(np.float64))
    known = L < BIG
    foot = occ[:, :, :int(np.ceil(zlim))].any(2)
    if not known.any():
        return occ
    Lf = np.where(known, L, 0.0)
    fill = np.where(known, L, np.nan)
    got = known.copy()
    for sig in (2.0, 4.0, 8.0, 16.0, 32.0, 64.0):
        num = ndimage.gaussian_filter(Lf * known, sig)
        den = ndimage.gaussian_filter(known.astype(np.float64), sig)
        new = ~got & (den > 1e-4)
        fill[new] = num[new] / den[new]
        got |= new
    fill = np.where(got, fill, zlim)
    Z = np.arange(occ.shape[2])[None, None, :]
    cut = occ & (Z < np.minimum(fill, zlim)[..., None] - keep) & foot[..., None]
    occ = occ & ~cut
    log('underside: %d voxels below what the directions see cut (lower %.2f, %d columns with a seen rim)' % (cut.sum(), low, known.sum()))
    return occ


def hull_iou(masks, elev, axis, bw, bh, s=2.0):
    """Mean and worst IoU between the 17 silhouettes and the reprojection of their hull, on an s-px grid."""
    R = max(np.abs(np.nonzero(m)[1] + 0.5 - bw / 2.0 - axis).max() for m in masks) + 2
    rows = np.concatenate([np.nonzero(m)[0] for m in masks])
    se, ce = np.sin(np.radians(elev)), np.cos(np.radians(elev))
    xs = np.arange(-R, R, s) + s / 2
    zs = np.arange((bh / 2.0 - rows.max() - 1 - R * se) / ce - 2, (bh / 2.0 - rows.min() + R * se) / ce + 2, s) + s / 2
    X, Y, Z = np.meshgrid(xs, xs, zs, indexing='ij')
    P = np.stack([X.ravel(), Y.ravel(), Z.ravel()], 1)
    sm = [m[:m.shape[0] // int(s) * int(s), :m.shape[1] // int(s) * int(s)] for m in masks]
    sm = [m.reshape(m.shape[0] // int(s), int(s), m.shape[1] // int(s), int(s)).mean((1, 3)) >= 0.5 for m in sm]
    keep = np.ones(len(P), bool)
    proj = []
    for d, m in enumerate(sm):
        p = P @ camera(elev, d * 360.0 / 32).T
        ui = np.floor((p[:, 0] + bw / 2.0 + axis) / s).astype(int)
        vi = np.floor((p[:, 1] + bh / 2.0) / s).astype(int)
        ok = (ui >= 0) & (vi >= 0) & (ui < m.shape[1]) & (vi < m.shape[0])
        ins = np.zeros(len(P), bool)
        ins[ok] = m[vi[ok], ui[ok]]
        keep &= ins
        proj.append((ui, vi, ok))
    ious = []
    for m, (ui, vi, ok) in zip(sm, proj):
        img = np.zeros_like(m)
        k = ok & keep
        img[vi[k], ui[k]] = True
        ious.append((img & m).sum() / max(1, (img | m).sum()))
    return float(np.mean(ious)), float(np.min(ious))


def calibrate(a, group, log=print):
    """Camera tilt and turning-axis column offset (HD px right of the box centre) that make the drawn directions'
    silhouettes agree: axis on a 2-px grid first, then tilt in 1-degree steps, then the axis to 0.5 px."""
    masks = [a.frame(group * NDRAWN + d)[..., 3] >= 128 for d in range(NDRAWN)]
    bw, bh = a.box_w, a.box_h
    fine = 1.0 if max(bw, bh) < 400 else 2.0  # big sprites (the Siege Tank): the last steps on a 2-px grid
    f = lambda e, x, s=2.0: hull_iou(masks, e, x, bw, bh, s)[0]
    axis = max(np.arange(-16, 16.1, 2.0), key=lambda x: f(35.0, x, 4.0))
    axis = max(np.arange(axis - 2, axis + 2.1, 0.5), key=lambda x: f(35.0, x))
    elev = max(np.arange(20, 61, 1.0), key=lambda e: f(e, axis))  # units differ: Marine 31, SCV 50
    axis = max(np.arange(axis - 1, axis + 1.1, 0.5), key=lambda x: f(elev, x, fine))
    elev = max(np.arange(elev - 1, elev + 1.1, 0.5), key=lambda e: f(e, axis, fine))
    log('calibrated: tilt %.1f deg, axis %+.1f px (hull IoU mean %.3f, worst %.3f)'
        % ((elev, axis) + hull_iou(masks, elev, axis, bw, bh, fine)))
    return elev, axis


def cut_soles(occ, log=print, tilt=31.0, full=0.95):
    """Under each foot the carving leaves an upside-down cone: from `tilt` degrees above, a point below a flat bottom of
    half-size r stays inside its silhouette in every direction until it is r tan(tilt) down, so nothing removes it
    (a Marine boot: 9 layers at 31 degrees; a Siege Tank at 45 degrees: some 50). It is not boot: at eye level it
    showed as a dark smear (v1, painted with the fill) or, carried down as a block, as a thick sole (v2). Each foot (a
    part of the bottom layers) loses everything below its sole, the first layer within r tan(tilt) of its lowest point
    that is `full` x as large as the largest there; the bottom slab grows until it holds the whole cone. A lifted foot
    loses its own cone. The model is grounded again afterwards (z = 0 on the new lowest voxel), and the game stands
    the frames on their lowest pixels anyway."""
    zs_any = np.flatnonzero(occ.any((0, 1)))
    bottom, height = zs_any[0], zs_any[-1] - zs_any[0] + 1
    t = np.tan(np.radians(tilt))
    slab = 16
    while True:
        lab, n = ndimage.label(occ[:, :, bottom:bottom + slab])
        sizes = ndimage.sum_labels(np.ones_like(lab), lab, range(1, n + 1)) if n else np.zeros(0)
        boxes = ndimage.find_objects(lab)
        feet = [c for c in range(1, n + 1) if sizes[c - 1] >= 0.02 * sizes.max()]  # specks are not feet
        reaches = {}
        for c in feet:
            bx = boxes[c - 1]
            r = 0.5 * max(bx[0].stop - bx[0].start, bx[1].stop - bx[1].start) + 1
            reaches[c] = int(np.ceil(r * t)) + 2
        need = max(reaches.values()) + 4 if reaches else slab
        cap = int(0.8 * height)  # a Siege Tank's cone is over half its carved height
        if need <= slab or slab >= cap:
            break
        slab = min(need, cap)
    cut, soles = 0, []
    for c in feet:
        bx = boxes[c - 1]
        part = lab[bx] == c
        layers = part.sum((0, 1))
        z0 = int(np.flatnonzero(layers)[0])
        win = layers[z0:z0 + reaches[c]]
        zs = z0 + int(np.flatnonzero(win >= full * win.max())[0])  # in the box's own layers
        below = np.zeros_like(part); below[:, :, :zs] = part[:, :, :zs]
        sub = occ[:, :, bottom:bottom + slab][bx]
        sub[below] = False
        cut += int(below.sum())
        soles.append(zs - z0)
    log('soles: %d feet, cones of %s layers cut (%d voxels, slab %d)' % (len(feet), soles, cut, slab))
    return occ


def group_iou(a, group, elev, axis, s=2.0):
    """How well one frame group's 17 directions agree as views of one turning model (mean hull IoU): low for groups
    that are not a turntable (a Dragoon's body faces the camera in every direction; burrowing, deploying)."""
    masks = [a.frame(group * NDRAWN + d)[..., 3] >= 128 for d in range(NDRAWN)]
    return hull_iou(masks, elev, axis, a.box_w, a.box_h, s)[0]


def build(path, group, elev=None, axis=None, log=print, soles=True, floor=None, prior=None, prior_grow=2):
    a = Anim(path)
    bw, bh = a.box_w, a.box_h
    if elev is None or axis is None:
        elev, axis = calibrate(a, group, log)
    shots = [Shot(a, group, d, elev, axis) for d in range(32)]
    drawn = shots[:NDRAWN]

    # Grid bounds from the silhouettes: horizontal radius from the farthest column off the axis, height from the rows.
    R = 2 + max(np.abs(np.nonzero(s.mask)[1] + 0.5 - s.ax).max() for s in drawn)
    rows = np.concatenate([np.nonzero(s.mask)[0] for s in drawn])
    se, ce = np.sin(np.radians(elev)), np.cos(np.radians(elev))
    zhi = (bh / 2.0 - rows.min() + R * se) / ce + 2
    zlo = (bh / 2.0 - rows.max() - 1 - R * se) / ce - 2
    xs = np.arange(-R, R + 1e-9, 1.0)
    zs = np.arange(zlo, zhi + 1e-9, 1.0)
    # Where the unit is, from a coarse hull first (4-px cells against silhouettes grown by 3 px, so it holds every fine
    # voxel of the hull): the fine grid covers only that box (the Siege Tank's bounds alone are 306 x 306 x 633).
    cx, cz = np.arange(-R, R + 1e-9, 4.0), np.arange(zlo, zhi + 1e-9, 4.0)
    CX, CY, CZ = np.meshgrid(cx, cx, cz, indexing='ij')
    C = np.stack([CX.ravel(), CY.ravel(), CZ.ravel()], 1).astype(np.float32)
    del CX, CY, CZ
    ck = np.ones(len(C), bool)
    for s in drawn:
        grown = ndimage.binary_dilation(s.mask, iterations=3)
        idx = np.flatnonzero(ck)
        ui, vi, _ = s.project(C[idx], bw, bh)
        inside, _ = s.lookup(grown, ui, vi, False)
        ck[idx[~inside]] = False
    lo, hi = C[ck].min(0) - 6, C[ck].max(0) + 6
    del C, ck
    ys = xs[(xs >= lo[1]) & (xs <= hi[1])]
    xs = xs[(xs >= lo[0]) & (xs <= hi[0])]
    zs = zs[(zs >= lo[2]) & (zs <= hi[2])]
    shape = (len(xs), len(ys), len(zs))
    X, Y, Z = np.meshgrid(xs, ys, zs, indexing='ij')
    P = np.stack([X.ravel(), Y.ravel(), Z.ravel()], 1).astype(np.float32)
    del X, Y, Z
    log('grid %s (%d voxels)' % (shape, len(P)))

    # 1. Silhouette hull: inside every drawn direction's silhouette (calibrated, so they agree to a pixel).
    keep = np.ones(len(P), bool)
    for s in drawn:
        idx = np.flatnonzero(keep)
        ui, vi, _ = s.project(P[idx], bw, bh)
        inside, _ = s.lookup(s.mask, ui, vi, False)
        keep[idx[~inside]] = False
    log('hull: %d voxels' % keep.sum())
    if prior is not None:
        # a shape to stay inside (mod_unit.py: the mod's model posed and fitted to this pose), grown by prior_grow
        # voxels: where the silhouettes leave the hull loose (steep art), the model's shape decides
        pocc = np.zeros(shape, bool)
        ii = np.round((prior - [xs[0], ys[0], zs[0]])).astype(int)
        ok = np.all((ii >= 0) & (ii < shape), axis=1)
        pocc[tuple(ii[ok].T)] = True
        pocc = ndimage.binary_closing(pocc, iterations=1)
        if prior_grow:
            pocc = ndimage.binary_dilation(pocc, iterations=prior_grow)
        before = keep.sum()
        keep &= pocc.ravel()
        log('prior shape: hull %d -> %d voxels' % (before, keep.sum()))

    # Units without a depth layer (the Medic's is all 0): silhouettes only.
    depth = sum(s.depth_ok.sum() for s in drawn) > 0.2 * sum(s.mask.sum() for s in drawn)
    if not depth:
        log('no depth layer: silhouettes only')
        for s in shots:
            s.k, s.c = -1.3, 0.0
            s.depth_ok = np.zeros_like(s.depth_ok)
    # 2. Depth scale and offsets: w = c + k D. The hull lies in front of (or on) the true surface, so per D level the
    # hull's farthest depths trace the surface: k is the slope of that upper envelope (median over the drawn views),
    # then c >= w_hull - k D everywhere with equality where the hull is tight, so c is a high quantile of that bound.
    H = P[keep]
    fronts, slopes = [], []
    for s in (shots if depth else []):
        ui, vi, w = s.project(H, bw, bh)
        zf = front_depth(ui, vi, w, s.mask.shape)
        ok = s.depth_ok & np.isfinite(zf)
        fronts.append((s.D[ok], zf[ok]))
        if not s.mirrored:
            Dv, wv = fronts[-1]
            edges = np.percentile(Dv, np.linspace(0, 100, 9))
            env = [(np.median(Dv[sel]), np.percentile(wv[sel], 90))
                   for sel in ((Dv >= e0) & (Dv < e1) for e0, e1 in zip(edges[:-1], edges[1:])) if sel.sum() > 20]
            slopes.append(np.polyfit(*np.array(env).T, 1)[0])
    k = float(np.median(slopes)) if depth else -1.3
    for s, (Dv, wv) in zip(shots, fronts):
        s.k, s.c = k, float(np.percentile(wv - k * Dv, 85))
    cs = np.array([s.c for s in drawn])
    for s in shots[NDRAWN:]:  # a turntable keeps the depth reference: the mirrored views (of a mirrored model) share it
        s.c = float(np.median(cs))
    if depth:
        log('depth scale %.2f HD px per level (beta %.2f), offsets %.1f..%.1f' % (k, 1 / k, cs.min(), cs.max()))

    # 3. Carve with depth: a voxel in front of the surface a drawn view shows there (by more than tol) is empty.
    # Two views must agree, so one view's misaligned edge pixel cannot cut a thin part.
    tol = 4.0
    votes = np.zeros(len(P), np.int32)
    idx = np.flatnonzero(keep)
    for s in drawn:
        ui, vi, w = s.project(P[idx], bw, bh)
        ok, inside = s.lookup(s.depth_ok, ui, vi, False)
        ok &= inside
        Dv, _ = s.lookup(s.D, ui, vi, np.nan)
        wsurf = s.c + s.k * Dv
        votes[idx[ok & (w < wsurf - tol)]] += 1
    carved = keep & (votes >= 2)
    keep &= ~carved
    occ = keep.reshape(shape)
    # drop specks: keep the largest connected parts (anything over 2% of the biggest)
    lab, n = ndimage.label(occ)
    if n > 1:
        sizes = ndimage.sum(occ, lab, range(1, n + 1))
        occ = np.isin(lab, 1 + np.flatnonzero(sizes >= 0.02 * sizes.max()))
    log('depth carving removed %d, %d voxels left' % (carved.sum(), occ.sum()))
    if floor is not None:  # a part that sits on another (a turret on its hull): nothing below the surface it sits on
        cut = occ[:, :, zs < floor].sum()
        occ[:, :, zs < floor] = False
        log('floor at z %.1f: %d voxels cut' % (floor, cut))
    underside = floor is None and soles and prior is None  # not for flyers and hovering units (soles are not theirs),
    # nor with a prior shape (it brings its own underside)

    # 4. Surface voxels and their normals (from the smoothed occupancy).
    def surface(occ):
        solid = ndimage.binary_erosion(occ, np.ones((3, 3, 3), bool) & ndimage.generate_binary_structure(3, 1))
        surf = occ & ~solid
        sm = ndimage.gaussian_filter(occ.astype(np.float32), 1.5)
        g = np.stack(np.gradient(sm), -1)
        si = np.argwhere(surf)
        N = -g[surf]
        N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-6)
        S = np.stack([xs[si[:, 0]], ys[si[:, 1]], zs[si[:, 2]]], 1)
        return si, N, S

    # 5. Colour: the view that faces each surface voxel most squarely and sees it (nearest surface along its ray).
    def colour(si, N, S):
        best = np.full(len(S), -np.inf)
        col = np.zeros((len(S), 4), np.uint8)
        for s in shots:
            ui, vi, w = s.project(S, bw, bh)
            zf = ndimage.minimum_filter(front_depth(ui, vi, w, s.mask.shape), 3)
            zv, _ = s.lookup(zf, ui, vi, np.inf)
            inmask, _ = s.lookup(s.mask, ui, vi, False)
            seen = inmask & (w <= zv + 2.0)
            if s.mirrored:  # a mirrored view shows a mirrored model: only where its depth agrees with this voxel
                dok, _ = s.lookup(s.depth_ok, ui, vi, False)
                Dv, _ = s.lookup(s.D, ui, vi, np.nan)
                seen &= dok & (np.abs(w - (s.c + s.k * Dv)) < 4.0)
            facing = -(N @ s.R[2])
            score = np.where(seen, facing - (1.0 if s.mirrored else 0.0), -np.inf)
            better = score > best
            rgb, _ = s.lookup(s.rgba, ui[better], vi[better], 0)
            tc, _ = s.lookup(s.tc, ui[better], vi[better], 0)
            col[better, :3] = rgb[:, :3]
            col[better, 3] = tc
            best[better] = score[better]
        return col, best

    if underside:
        si, N, S = surface(occ)
        _, best = colour(si, N, S)
        occ = cut_underside(occ, si, best > 0.15, log, elev)
    si, N, S = surface(occ)
    col, best = colour(si, N, S)
    direct = (best > -0.5).sum()
    mirror = ((best > -np.inf) & (best <= -0.5)).sum()
    have = best > -np.inf
    if (~have).any():  # never seen: the nearest coloured surface voxel
        grid = np.full(shape, -1, np.int64)
        grid[tuple(si[have].T)] = np.flatnonzero(have)
        _, near = ndimage.distance_transform_edt(grid < 0, return_indices=True)
        src = grid[near[0][tuple(si[~have].T)], near[1][tuple(si[~have].T)], near[2][tuple(si[~have].T)]]
        col[~have] = col[src]
    log('surface %d: %d from drawn views, %d mirrored, %d filled' % (len(S), direct, mirror, (~have).sum()))

    origin = np.array([xs[0], ys[0], zs[0]])
    zmin = si[:, 2].min()
    return dict(idx=si.astype(np.int16), col=col, normal=N.astype(np.float32),
                origin=origin - [0, 0, zs[zmin]], voxel=1.0, zshift=zs[zmin], shape=np.array(shape),
                solid=np.argwhere(occ).astype(np.int16), elev=elev, axis=axis, group=group, c=np.array([s.c for s in shots]),
                k=k)


if __name__ == '__main__':
    elev, axis = (float(sys.argv[4]), float(sys.argv[5])) if len(sys.argv) > 5 else (None, None)
    m = build(sys.argv[1], int(sys.argv[2]), elev, axis)
    np.savez_compressed(sys.argv[3], **m)
    print('wrote', sys.argv[3])
