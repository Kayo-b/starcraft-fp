"""Is the depth layer distance along the view ray, or height above the ground? Fit dD/du = a0 + a1*nx/nz and
dD/dv = b0 + b1*ny/nz (normal map: R = x, A = y up, so ny(down) = -(A/127.5 - 1)) over every drawn direction.
View depth predicts a0 = b0 = 0 and a1 = b1; height above the ground predicts b0 = -gamma*cos(e), far from 0.
Measured: offsets ~0, slopes equal -> view depth. The slopes themselves are NOT the depth scale (the normal map carries
bump detail the depth layer does not, which inflates |slope|); betafit.py measures the scale on flat roofs and walls.
usage: depthkind.py ANIM:GROUP ..."""
import sys
import numpy as np
from anim import Anim
from views import frame_views
from views import box_blur

for spec in sys.argv[1:]:
    path, g = spec.split(':')
    a = Anim(path)
    views = frame_views(a, int(g))[:17]
    X, Y, DU, DV = [], [], [], []
    for v in views:
        m = v.mask.copy()
        for _ in range(3):
            m = m & np.roll(m, 1, 0) & np.roll(m, -1, 0) & np.roll(m, 1, 1) & np.roll(m, -1, 1)
        D = box_blur(v.depth, 2)
        dDu = (np.roll(D, -1, 1) - np.roll(D, 1, 1)) / 2
        dDv = (np.roll(D, -1, 0) - np.roll(D, 1, 0)) / 2
        nx = box_blur(v.normal[..., 0].astype(float), 2) / 127.5 - 1
        ny = -(box_blur(v.normal[..., 3].astype(float), 2) / 127.5 - 1)
        nz = np.sqrt(np.clip(1 - nx ** 2 - ny ** 2, 0.05, 1))
        ok = m & (nz > 0.5) & (np.abs(dDu) < 8) & (np.abs(dDv) < 8)
        X.append((nx / nz)[ok]); Y.append((ny / nz)[ok]); DU.append(dDu[ok]); DV.append(dDv[ok])
    x, y, du, dv = map(np.concatenate, (X, Y, DU, DV))
    A = np.stack([np.ones_like(x), x], 1); a0, a1 = np.linalg.lstsq(A, du, rcond=None)[0]
    B = np.stack([np.ones_like(y), y], 1); b0, b1 = np.linalg.lstsq(B, dv, rcond=None)[0]
    print('%s g%s: dD/du = %+.3f %+.3f nx/nz (r %.2f) | dD/dv = %+.3f %+.3f ny/nz (r %.2f)'
          % (path.split('/')[-1], g, a0, a1, np.corrcoef(x, du)[0, 1], b0, b1, np.corrcoef(y, dv)[0, 1]))
