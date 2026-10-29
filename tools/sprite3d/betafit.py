"""Depth scale from flat roofs and south walls (camera elevation e): along screen v a roof's depth changes by
dw/dv = -cot(e), a south wall's by tan(e); the depth layer changes by beta times that."""
import sys
import numpy as np
from anim import Anim
from views import box_blur

e = np.radians(float(sys.argv[1]))
for spec in sys.argv[2:]:
    path, f = spec.split(':')
    a = Anim(path)
    rgba = a.frame(int(f), 'diffuse', boxed=False); n = a.frame(int(f), 'normal', boxed=False)
    D = a.frame(int(f), 'ao_depth', boxed=False)[..., 3].astype(float)
    m = rgba[..., 3] >= 250
    for _ in range(3):
        m = m & np.roll(m, 1, 0) & np.roll(m, -1, 0) & np.roll(m, 1, 1) & np.roll(m, -1, 1)
    nx = n[..., 0] / 127.5 - 1; nd = -(n[..., 3] / 127.5 - 1); nz = np.sqrt(np.clip(1 - nx ** 2 - nd ** 2, 0, 1))
    ang = np.degrees(np.arctan2(nd, nz))
    Db = box_blur(D, 3)
    dDv = (np.roll(Db, -3, 0) - np.roll(Db, 3, 0)) / 6
    roof = m & (np.abs(nx) < 0.08) & (np.abs(ang + (90 - np.degrees(e))) < 3)
    wall = m & (np.abs(nx) < 0.08) & (np.abs(ang - np.degrees(e)) < 3)
    # require the neighbours used by the gradient to be the same surface class
    roof &= np.roll(roof, 3, 0) & np.roll(roof, -3, 0); wall &= np.roll(wall, 3, 0) & np.roll(wall, -3, 0)
    br = np.median(dDv[roof]) / (-1 / np.tan(e)) if roof.sum() > 50 else np.nan
    bw = np.median(dDv[wall]) / np.tan(e) if wall.sum() > 50 else np.nan
    print('%s: roof px %5d beta %+.3f | wall px %5d beta %+.3f' % (path.split('/')[-1], roof.sum(), br, wall.sum(), bw))
