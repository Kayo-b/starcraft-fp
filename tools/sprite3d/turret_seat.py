"""Where a turret sits on its hull, for eyelevel_anim.py --floor: the hull roof under the turret point, in the turret's
own frame (HD px above its box centre point).

The game composes a turret on its hull by the box centres plus OpenBW's turret offset for the hull's frame (lo index 2);
seen by the original art's camera that offset is the projection of one point fixed on the hull, A (right, forward, up),
which the game fits from the offsets' 17 directions (fp_render.h turret_point). The turret's box centre point is A away
from the hull's, so the hull roof at A's place, minus A's height, is where the turret's own bottom must be: below it the
carving leaves a cone the hull roof hides in the original art.

    turret_seat.py HULL_MODEL.npz AX AY AZ          (A in game px, e.g. the Siege Tank's 0 -12.7 14.1)
Run with the venv python."""
import sys
import numpy as np

if __name__ == '__main__':
    m = np.load(sys.argv[1])
    ax, ay, az = (4 * float(v) for v in sys.argv[2:5])  # game px -> HD px
    P = m['origin'] + m['solid'].astype(np.float64) * float(m['voxel'])
    near = np.hypot(P[:, 0] - ax, P[:, 1] - ay) <= 8
    roof = P[near, 2].max() + float(m['zshift'])  # hull frame with its box centre point at 0
    print('%.1f' % (roof - az))
