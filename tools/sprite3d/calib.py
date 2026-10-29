"""Camera elevation from the silhouettes of a turning unit: carve the visual hull of one animation frame's drawn directions
for each candidate elevation and measure how much of each silhouette the hull's reprojection fills (mean/min per line). Works on a
`step`-times-smaller grid (masks shrunk by majority), so every voxel covers one mask pixel."""
import sys
import numpy as np
from anim import Anim
from views import frame_views, project


def shrink(mask, s):
    h, w = mask.shape[0] // s * s, mask.shape[1] // s * s
    return mask[:h, :w].reshape(h // s, s, w // s, s).mean((1, 3)) >= 0.5


def carve(masks, headings, elev, bw, bh, s, extent):
    ax = np.arange(-extent, extent, s) + s / 2
    zs = np.arange(-extent * 0.3, extent * 1.3, s) + s / 2
    X, Y, Z = np.meshgrid(ax, ax, zs, indexing='ij')
    P = np.stack([X.ravel(), Y.ravel(), Z.ravel()], 1)
    keep = np.ones(len(P), bool)
    for m, h in zip(masks, headings):
        u, v, _ = project(P[keep], elev, h, bw, bh)
        ui, vi = np.floor(u / s).astype(int), np.floor(v / s).astype(int)
        inside = (ui >= 0) & (vi >= 0) & (ui < m.shape[1]) & (vi < m.shape[0])
        ok = np.zeros(len(ui), bool)
        ok[inside] = m[vi[inside], ui[inside]]
        idx = np.flatnonzero(keep)
        keep[idx[~ok]] = False
    return P[keep]


def fill(masks, headings, P, elev, bw, bh, s):
    out = []
    for m, h in zip(masks, headings):
        u, v, _ = project(P, elev, h, bw, bh)
        img = np.zeros(m.shape, bool)
        ui, vi = np.floor(u / s).astype(int), np.floor(v / s).astype(int)
        ok = (ui >= 0) & (vi >= 0) & (ui < m.shape[1]) & (vi < m.shape[0])
        img[vi[ok], ui[ok]] = True
        out.append((img & m).sum() / max(1, m.sum()))
    return float(np.mean(out)), float(np.min(out))


if __name__ == '__main__':
    # Only the 17 drawn directions: the other 15 are mirror images, i.e. views of a mirrored model (an asymmetric unit,
    # such as a Marine holding its rifle in one hand, does not agree with them). usage: calib.py ANIM:GROUP ...
    s = 2
    for spec in sys.argv[1:]:
        path, group = spec.split(':')
        a = Anim(path)
        views = frame_views(a, int(group))[:17]
        masks = [shrink(v.mask, s) for v in views]
        hs = [v.heading for v in views]
        extent = max(a.box_w, a.box_h) * 0.5
        res = []
        for e in range(25, 66, 5):
            P = carve(masks, hs, e, a.box_w, a.box_h, s, extent)
            m, mn = fill(masks, hs, P, e, a.box_w, a.box_h, s)
            res.append((e, m, mn))
        print(path.split('/')[-1], group, ' '.join('%d:%.3f/%.3f' % r for r in res), flush=True)
