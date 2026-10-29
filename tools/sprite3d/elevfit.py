"""Camera elevation from a building's normal map: normals with no sideways part (|nx| small) cluster at the roofs,
tilted -(90 - e) from the view axis toward screen-up, and at south-facing walls, tilted +e toward screen-down."""
import sys
import numpy as np
from anim import Anim

for spec in sys.argv[1:]:
    path, f = spec.split(':')
    a = Anim(path)
    rgba = a.frame(int(f), 'diffuse', boxed=False); n = a.frame(int(f), 'normal', boxed=False)
    m = rgba[..., 3] >= 200
    nx = n[..., 0] / 127.5 - 1
    nd = -(n[..., 3] / 127.5 - 1)
    nz = np.sqrt(np.clip(1 - nx ** 2 - nd ** 2, 0, 1))
    sel = m & (np.abs(nx) < 0.12)
    ang = np.degrees(np.arctan2(nd[sel], nz[sel]))
    h, edges = np.histogram(ang, bins=np.arange(-90, 91, 2))
    top = np.argsort(h)[::-1][:6]
    peaks = sorted((edges[i] + 1, h[i]) for i in top)
    print(path.split('/')[-1], 'pixels', sel.sum(), 'peaks (angle deg, count):', ', '.join('%+.0f:%d' % p for p in peaks))
    # print coarse histogram
    hc, ec = np.histogram(ang, bins=np.arange(-90, 91, 10))
    print('   ', ' '.join('%+d:%d' % (e + 5, c) for e, c in zip(ec[:-1], hc)))
