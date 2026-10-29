"""Lift one HD sprite frame into 3D with its own depth layer.

Camera that drew the Remastered sprites (fitted here, see README of this directory): orthographic, looking north, tilted
down 35 degrees; depth layer (alpha of `ao_depth`) D = alpha + BETA * w, w = distance along the view ray in HD pixels,
BETA = -0.6 (nearer is brighter), one scale for every sprite. Model frame: x east, y north, z up, HD pixels; the unit's
position is the ground point drawn at the centre of the sprite box."""
import numpy as np
from views import camera

ELEV = 35.0
BETA = -0.6
# Sprites drawn with another camera, by image id: (elevation, beta). Nexus: its four base discs (S, E, W, N) stand level
# only with beta = -0.565 * tan(elev) / tan(45); at 45 degrees they also form a square around the crystal, as its
# four-fold design needs (at 35 degrees it is 1.27 times deeper than wide, and beta would be -0.40).
CAMERA_BY_IMAGE = {179: (45.0, -0.565)}


def median3(a):
    p = np.pad(a, 1, mode='edge')
    st = np.stack([p[i:i + a.shape[0], j:j + a.shape[1]] for i in range(3) for j in range(3)])
    return np.median(st, 0)


def clip_level(D, mask, share=0.005):
    """The depth layer's near limit for this sprite, or None: the Nexus saturates at 255, the Gateway at 204 (its nearest
    pixels pile up on the maximum value)."""
    d = D[mask]
    top = d.max()
    return top if (d >= top).mean() > share else None


def unclip(D, mask, elev=ELEV, beta=BETA, top=None):
    """The nearest parts of some sprites (the Nexus's spires, the Gateway's front) saturate the depth layer. Continue each
    clipped run of a column upward as a vertical surface from the pixel below it (D grows by -beta*tan(elev) per row up);
    a run that starts over a farther part has no depth to continue and starts at the limit."""
    if top is None:
        top = clip_level(D, mask)
        if top is None:
            return D
    slope = -beta * np.tan(np.radians(elev))
    D = D.copy()
    clip = mask & (D >= top)
    for c in np.nonzero(clip.any(0))[0]:
        rows = np.nonzero(clip[:, c])[0]
        for run in np.split(rows, np.nonzero(np.diff(rows) > 1)[0] + 1):
            below = run[-1] + 1
            start = top
            if below < D.shape[0] and mask[below, c] and D[below, c] >= top - 6:
                start = D[below, c]
            D[run, c] = np.maximum(top, start + slope * (below - run))
    return D


def denoise(D, mask, radius=3, tol=4.0, iters=2):
    """Edge-aware smoothing of the 8-bit, DXT5-compressed depth (it steps by 1-2 levels from pixel to pixel, which
    scatters thin parts like the Dragoon's legs when seen from the side): average each pixel with the neighbours in a
    (2r+1)^2 window that lie on the same surface (|dD| < tol); jumps between parts stay sharp."""
    h, w = D.shape
    for _ in range(iters):
        Dp = np.pad(np.where(mask, D, np.nan), radius, constant_values=np.nan)
        acc = np.zeros_like(D); n = np.zeros_like(D)
        for i in range(2 * radius + 1):
            for j in range(2 * radius + 1):
                s = Dp[i:i + h, j:j + w]
                ok = np.abs(s - D) < tol
                acc += np.where(ok, s, 0.0); n += ok
        D = np.where(mask & (n > 0), acc / np.maximum(n, 1), D)
    return D


def lift(anim, frame, elev=ELEV, beta=BETA, min_alpha=128, smooth=True):
    """Points (H,W,3) in the model frame, colours (H,W,4) and the kept mask, on the frame's own pixel grid.
    Clipped depth is continued (`unclip`), depth is median-filtered, 'flying' pixels (edge pixels whose depth blends
    foreground and background) are dropped and the compression noise is smoothed (`denoise`).
    The model is moved along the view ray so its lowest points stand on z = 0 (the picture does not change), then
    centred horizontally on the unit's position."""
    x, y, xo, yo, w, h = anim.frames[frame]
    rgba = anim.frame(frame, 'diffuse', boxed=False)
    D = anim.frame(frame, 'ao_depth', boxed=False)[..., 3].astype(np.float64)
    mask = rgba[..., 3] >= min_alpha
    D = unclip(D, mask, elev, beta)
    inner = mask & np.roll(mask, 1, 0) & np.roll(mask, -1, 0) & np.roll(mask, 1, 1) & np.roll(mask, -1, 1)
    Dm = median3(np.where(mask, D, np.nan))
    mask = mask & (inner | (np.abs(D - Dm) < 3)) & np.isfinite(Dm)
    D = np.where(mask, Dm, D)
    if smooth:
        D = denoise(D, mask)
    R = camera(elev, 0.0)
    vv, uu = np.mgrid[0:h, 0:w].astype(np.float64)
    u = uu + xo - anim.box_w / 2.0 + 0.5
    v = vv + yo - anim.box_h / 2.0 + 0.5
    P = u[..., None] * R[0] + v[..., None] * R[1] + (D / beta)[..., None] * R[2]
    zb = np.percentile(P[..., 2][mask], 0.5)
    P = P + (zb / -R[2][2]) * R[2]
    lo, hi = np.percentile(P[mask], 1, axis=0), np.percentile(P[mask], 99, axis=0)
    P[..., 0] -= 0.5 * (lo[0] + hi[0]); P[..., 1] -= 0.5 * (lo[1] + hi[1])
    return P, rgba, mask


def mirror_back(P, rgba, mask, plane_y=None):
    """Symmetric completion of the hidden north half: reflect the visible surface across an east-west plane through the
    middle of its footprint. Returns the extra points/colours (same grid layout)."""
    if plane_y is None:
        ys = P[..., 1][mask]
        plane_y = 0.5 * (np.percentile(ys, 1) + np.percentile(ys, 99))
    Q = P.copy()
    Q[..., 1] = 2 * plane_y - P[..., 1]
    return Q, rgba, mask, plane_y


def part_mask(P, mask, cx, cy, r_base=60.0, r_top=100.0, z_top=90.0):
    """Pixels of one free-standing part (the Nexus's side arches) cut out of the lifted sprite in 3D: everything within
    r_base of its foot (cx, cy) on the ground plane, plus whatever rises above z_top within r_top (an arch's tip curls
    inward). The low parts around it (the legs it stands in front of in the picture) stay out."""
    d = np.hypot(P[..., 0] - cx, P[..., 1] - cy)
    return mask & ((d < r_base) | ((d < r_top) & (P[..., 2] > z_top)))


def extrude(P, elev, thickness, layers=6):
    """A drawn surface made solid by pushing it back along the sprite camera's view ray: copies of P at depths
    thickness * i / layers (i = 1..layers). The outline from the sprite camera is unchanged; from the side a blade-like
    part (the Nexus's arches) shows `thickness` HD px of its own art instead of a paper-thin shell."""
    fwd = camera(elev, 0.0)[2]
    return [P + (thickness * i / layers) * fwd for i in range(1, layers + 1)]
