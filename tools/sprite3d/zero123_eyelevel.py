"""Zero-1-to-3 XL's take on eye level, for comparison with eyelevel_anim.py: each direction's frame lowered by
(TILT - ELEV) degrees at the same azimuth, i.e. the same direction seen from ELEV above the horizon. Pretrained only.

    zero123_eyelevel.py ANIM GROUP OUT.png [TILT [ELEV]]     (directions 16, 12, 8, 4, 0; venv python, GPU)"""
import sys
import numpy as np
from PIL import Image
from anim import Anim
from zero123_views import load, sprite_input, novel_views

if __name__ == '__main__':
    a = Anim(sys.argv[1])
    group = int(sys.argv[2])
    tilt = float(sys.argv[4]) if len(sys.argv) > 4 else 31.0
    elev = float(sys.argv[5]) if len(sys.argv) > 5 else 5.0
    models = load('cuda')
    tiles = []
    for d in (16, 12, 8, 4, 0):
        img = sprite_input(a, group * 17 + d)
        out = novel_views(models, img, [(tilt - elev, 0, 0)], seed=0)[0]
        tiles.append(np.concatenate([np.asarray(img), out], 0))
    Image.fromarray(np.concatenate(tiles, 1)).save(sys.argv[3])
    print('saved', sys.argv[3])
