"""One frame redrawn from eye level by its own depth (no turning model): for frame groups whose directions do not agree
as views of one turning model (a Dragoon's body faces the camera in every direction; burrowing, deploying).

Seen in the same direction from ELEV instead of TILT degrees above, the camera only turns about the screen's
horizontal axis, so every pixel column stays a column: a pixel at row y with depth w (w = k D, D the ao_depth level)
goes to row y cos(T - E) + w sin(T - E). Each column is warped on its own, neighbouring pixels whose depths are within
JOIN px of each other are joined (so surfaces that stretch do not open gaps), nearest wins. With TIDY, a frame that was
one piece comes out in one piece (tidy()). The depth origin is the
unit's median depth, so the frame turns about the unit's middle; the game stands frames on their lowest pixels anyway.

    relief_eye.py ANIM FRAME OUT.png [TILT [ELEV [K]]]     (preview: the frame beside its eye-level version)"""
import sys
import numpy as np
from relief import median3, denoise, unclip, BETA
from scipy import ndimage

JOIN = 10.0  # 4 left the Overlord's tentacles in pieces, 20 drips streaks
STRETCH = 2  # a source pixel pair drawn over more rows than this is a stretched surface (unsure, for repaint.py)
TIDY = False  # frames in one piece in the original stay in one piece, without drips or notches (tidy(); the Lurker)
HOLE = 1500   # tidy() fills holes up to this many px (the hollow under the Lurker's shell; clean() stops at 400)


def warp(rgba, tc, D, mask, tilt=31.0, elev=5.0, k=-1.31):
    """rgba (h,w,4), tc (h,w), D (h,w) depth levels, mask -> eye-level rgba, tc on the same canvas (rows may move
    outside: the caller sizes the canvas). Returns (rgba, tc, row_shift, stretched) where row_shift is how far the box
    grew up and stretched marks the pixels drawn from a pair spread over more than STRETCH rows."""
    h, w = mask.shape
    dl = np.radians(tilt - elev)
    c, s = np.cos(dl), np.sin(dl)
    W = k * D
    w0 = np.median(W[mask]) if mask.any() else 0.0
    W = W - w0
    ys = np.arange(h, dtype=np.float64)[:, None] - h / 2.0
    vt = ys * c + W * s                      # target row (relative to the box centre row)
    wt = -ys * s + W * c                     # target depth
    lo = int(np.floor(vt[mask].min())) if mask.any() else 0
    hi = int(np.ceil(vt[mask].max())) if mask.any() else 0
    top = min(-h // 2, lo - 1)
    H = max(h // 2, hi + 2) - top
    out = np.zeros((H, w, 4)); tco = np.zeros((H, w)); zb = np.full((H, w), np.inf); st = np.zeros((H, w), bool)
    pre = rgba.astype(np.float64)
    for x in range(w):
        rows = np.flatnonzero(mask[:, x])
        if len(rows) == 0:
            continue
        for a in rows:
            ya, za = vt[a, x] - top, wt[a, x]
            b = a + 1
            if b < h and mask[b, x] and abs(W[a, x] - W[b, x]) < JOIN:
                yb, zb_ = vt[b, x] - top, wt[b, x]
            else:
                yb, zb_ = ya + 1.0, za  # a lone pixel covers one row
            r0, r1 = int(np.floor(min(ya, yb))), int(np.ceil(max(ya, yb)))
            wide = r1 - r0 > STRETCH
            for r in range(max(r0, 0), min(r1, H)):
                t = 0.0 if yb == ya else min(1.0, max(0.0, (r + 0.5 - ya) / (yb - ya)))
                z = za + t * (zb_ - za)
                if z < zb[r, x]:
                    zb[r, x] = z
                    st[r, x] = wide
                    src = pre[a, x] * (1 - t) + pre[min(b, h - 1), x] * t if b < h and mask[b, x] else pre[a, x]
                    out[r, x] = src
                    tco[r, x] = tc[a, x] * (1 - t) + (tc[min(b, h - 1), x] * t if b < h else 0)
    return np.clip(out, 0, 255).astype(np.uint8), np.clip(tco, 0, 255).astype(np.uint8), -h // 2 - top, st


def frame_eye(anim, i, tilt=31.0, elev=5.0, k=-1.31, unsure=False):
    """Frame i of an anim (boxed) at eye level: rgba, team mask, and how many rows the box grew upward; with unsure,
    also where the frame is unsure (stretched surfaces and filled holes, for repaint.py)."""
    rgba = anim.frame(i, 'diffuse')
    tc = anim.frame(i, 'teamcolor')[..., 0] if 'teamcolor' in anim.layers else np.zeros(rgba.shape[:2], np.uint8)
    D = anim.frame(i, 'ao_depth')[..., 3].astype(np.float64)
    mask = rgba[..., 3] > 0
    solid = rgba[..., 3] >= 128
    D = unclip(D, solid, tilt, BETA)
    Dm = median3(np.where(solid, D, np.nan))
    D = np.where(np.isfinite(Dm), Dm, D)
    D = denoise(D, solid)
    # edge pixels (alpha < 128) take their nearest solid neighbour's depth
    if solid.any():
        _, (iy, ix) = ndimage.distance_transform_edt(~solid, return_indices=True)
        D = D[iy, ix]
    out, tco, up, st = warp(rgba, tc, D, mask, tilt, elev, k)
    rgba, tc, st = clean(out, tco, st)
    if TIDY and whole(mask):
        rgba, tc, st = tidy(rgba, tc, st)
    return (rgba, tc, up, st) if unsure else (rgba, tc, up)


def whole(mask, speck=30):
    """Whether a frame is one piece (no second part of `speck` px or more): a walking unit, not a death's splatter."""
    lab, n = ndimage.label(mask, np.ones((3, 3)))
    if n < 2:
        return n == 1
    s = np.sort(ndimage.sum_labels(np.ones_like(lab), lab, range(1, n + 1)))
    return s[-2] < speck


def tidy(rgba, tc, unsure):
    """A frame that was one piece kept in one piece: drips and teeth narrower than 3 px off (opening), every part not
    joined to the body dropped (a claw whose leg the warp lost), notches narrower than 3 px and the holes `clean` left
    closed (up to HOLE px). Added pixels take the nearest colour and join the unsure mask (repaint.py draws them)."""
    sq = np.ones((3, 3), bool)
    m = rgba[..., 3] > 0
    o = ndimage.binary_opening(np.pad(m, 2), sq)  # padded: the closing below would erode pixels on the canvas edge
    lab, n = ndimage.label(o, sq)
    if n > 1:
        s = ndimage.sum_labels(np.ones_like(lab), lab, range(1, n + 1))
        o = lab == 1 + int(np.argmax(s))
    t = ndimage.binary_closing(o, sq)
    holes, nh = ndimage.label(ndimage.binary_fill_holes(t) & ~t)
    if nh:
        hs = ndimage.sum_labels(np.ones_like(holes), holes, range(1, nh + 1))
        t |= np.isin(holes, 1 + np.flatnonzero(hs <= HOLE))
    t = t[2:-2, 2:-2]
    add = t & ~m
    rgba = rgba.copy(); tc = tc.copy()
    if add.any():
        _, (iy, ix) = ndimage.distance_transform_edt(~(m & t), return_indices=True)
        rgba[add] = rgba[iy[add], ix[add]]
        tc[add] = tc[iy[add], ix[add]]
    rgba[~t] = 0
    tc[~t] = 0
    return rgba, tc, (unsure & t) | add


def clean(rgba, tc, unsure=None, speck=30, hole=400):
    """Drop the spray the warp leaves where a surface ends (pixels with fewer than 3 opaque neighbours, specks) and
    fill the holes it opens inside the unit (up to `hole` px). With an unsure mask, returns it too, the holes added."""
    m = rgba[..., 3] > 0
    n = ndimage.convolve(m.astype(np.int32), np.ones((3, 3), np.int32), mode='constant') - m
    m &= n >= 3
    lab, k = ndimage.label(m)
    if k:
        sizes = ndimage.sum_labels(np.ones_like(lab), lab, range(1, k + 1))
        m = np.isin(lab, 1 + np.flatnonzero(sizes >= speck))
    rgba = rgba.copy(); rgba[~m] = 0
    tc = np.where(m, tc, 0).astype(tc.dtype)
    # holes the warp opens inside the unit (where a part was hidden behind another in the original: the Lurker's
    # body) take the nearest colour around them, if small enough to be holes rather than gaps between legs
    filled = ndimage.binary_fill_holes(m)
    holes, nh = ndimage.label(filled & ~m)
    small = np.zeros_like(m)
    if nh:
        hs = ndimage.sum_labels(np.ones_like(holes), holes, range(1, nh + 1))
        small = np.isin(holes, 1 + np.flatnonzero(hs <= hole))
        if small.any():
            _, (iy, ix) = ndimage.distance_transform_edt(~m, return_indices=True)
            rgba[small] = rgba[iy[small], ix[small]]
            tc[small] = tc[iy[small], ix[small]]
    if unsure is None:
        return rgba, tc
    return rgba, tc, (unsure & m) | small


if __name__ == '__main__':
    from anim import Anim
    from PIL import Image
    a = Anim(sys.argv[1])
    i = int(sys.argv[2])
    tilt = float(sys.argv[4]) if len(sys.argv) > 4 else 31.0
    elev = float(sys.argv[5]) if len(sys.argv) > 5 else 5.0
    k = float(sys.argv[6]) if len(sys.argv) > 6 else -1.31
    o = a.frame(i)
    e, _, up = frame_eye(a, i, tilt, elev, k)
    H = max(o.shape[0] + up, e.shape[0])
    left = np.zeros((H, o.shape[1], 4), np.uint8); left[up:up + o.shape[0]] = o
    right = np.zeros((H, e.shape[1], 4), np.uint8); right[:e.shape[0]] = e
    img = np.concatenate([left, right], 1)
    a_ = img[..., 3:4] / 255.0
    Image.fromarray((img[..., :3] * a_).astype(np.uint8)).save(sys.argv[3])
