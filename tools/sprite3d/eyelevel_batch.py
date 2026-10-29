"""Eye-level sprites for many units at once (eyelevel_anim.py per unit), with a side-by-side sheet per unit.

    eyelevel_batch.py OUT_ROOT terran|zerg|protoss|NAME...
Per unit: OUT_ROOT/NAME/ (the anim, sheet.png, models/, log.txt) and OUT_ROOT/NAME/compare.png: the original frames
against the eye-level ones read back from the written anim (what the game draws), directions 16, 12, 8, 4, 0, for the
first pose and one from the middle. Run with the venv python. ANIM_DIR: the extracted anim files."""
import os
import subprocess
import sys
import numpy as np
from PIL import Image, ImageDraw
from anim import Anim
import eyeview as ev

ANIM_DIR = os.environ.get('ANIM_DIR', '/home/kxyx/projects/starcraft-data/casc-scratch/extracted/anim')
HERE = os.path.dirname(os.path.abspath(__file__))
# name, anim file, hovers or flies (keeps its underside), race. The anim file is the one the game loads for the unit's
# image (the Lurker, image 921, is drawn from main_920). The Siege Tank's turret is its own image, drawn on the hull.
# Groups that are not a turning model (the Vulture's directions do not agree as one) are redrawn by their own depth
# (--relief) rather than kept.
UNITS = [
    ('marine', 'main_239.anim', False, 'terran'),
    ('scv', 'main_247.anim', False, 'terran'),
    ('firebat', 'main_226.anim', False, 'terran'),
    ('medic', 'main_944.anim', False, 'terran'),
    ('tank', 'main_250.anim', False, 'terran'),
    # the turret sits on the hull roof: cut there (turret_seat.py tank/models/g00.npz 0.99 -12.70 13.65, the turret point
    # the game fits from OpenBW's offsets), not by the cone rule (a turret is wider at its barrels than at its base ring)
    ('tank_turret', 'main_251.anim', False, 'terran', ['--floor=-25.0', '--calib', '44.5,-1.0']),
    ('vulture', 'main_256.anim', True, 'terran', ['--relief-join', '4']),
    ('zergling', 'main_054.anim', False, 'zerg'),
    # the carving cannot part the Overlord's dangling tentacles (they fill in as one curtain): every frame by its own depth
    ('overlord', 'main_042.anim', True, 'zerg', ['--gate', '1.01']),
    # thin spread wings: carved, their under-cones dipped into the floor (kept) or hung as streaks (cut); by relief they lie flat
    ('drone', 'main_017.anim', True, 'zerg', ['--gate', '1.01']),
    ('hydralisk', 'main_029.anim', False, 'zerg'),
    # the hollow under its shell opens holes, and claws whose legs the warp loses float: one-piece frames kept whole
    ('lurker', 'main_920.anim', False, 'zerg', ['--relief-tidy']),
    ('probe', 'main_137.anim', True, 'protoss'),
    ('zealot', 'main_151.anim', False, 'protoss'),
    ('dragoon', 'main_122.anim', False, 'protoss'),
]


def compare(orig_path, eye_path, out, zoom=3):
    a, e = Anim(orig_path), Anim(eye_path)
    groups = sorted({0, (len(a.frames) // 17) // 2})
    tiles = []
    for g in groups:
        for d in (16, 12, 8, 4, 0):
            i = g * 17 + d
            if i >= len(a.frames):
                continue
            o, n = ev.on_black(a.frame(i)), ev.on_black(e.frame(i))
            # the same window around both box centres, just big enough for what either draws
            hw = max(a.box_w, e.box_w) // 2 + 2
            hh = max(a.box_h, e.box_h) // 2 + 2
            def centred(img, bw, bh):
                c = np.zeros((2 * hh, 2 * hw, 3), np.uint8)
                y0, x0 = hh - bh // 2, hw - bw // 2
                c[y0:y0 + bh, x0:x0 + bw] = img
                return c
            o, n = centred(o, a.box_w, a.box_h), centred(n, e.box_w, e.box_h)
            m = (o.max(2) > 8) | (n.max(2) > 8)
            ys, xs = np.nonzero(m)
            if len(ys) == 0:
                continue
            sl = (slice(max(0, ys.min() - 3), ys.max() + 4), slice(max(0, xs.min() - 3), xs.max() + 4))
            tiles.append(np.concatenate([o[sl], np.full((3, o[sl].shape[1], 3), 50, np.uint8), n[sl]], 0))
    h = max(t.shape[0] for t in tiles)
    img = np.concatenate([np.pad(t, ((0, h - t.shape[0]), (0, 4), (0, 0))) for t in tiles], 1)
    im = Image.fromarray(img).resize((img.shape[1] * zoom, img.shape[0] * zoom), Image.NEAREST)
    ImageDraw.Draw(im).text((4, 2), 'original (top) / eye level (below); directions 16 12 8 4 0 per pose', fill=(220, 220, 120))
    im.save(out)


if __name__ == '__main__':
    root, picks = sys.argv[1], sys.argv[2:]
    todo = [u for u in UNITS if u[0] in picks or u[3] in picks]
    for name, anim, hover, race, *extra in todo:
        out = os.path.join(root, name)
        src = os.path.join(ANIM_DIR, anim)
        big = max(Anim(src).box_w, Anim(src).box_h) >= 450
        # processes by free memory: a Siege Tank pose peaks near 2 GB, a Marine's near 0.5 GB
        avail = int(open('/proc/meminfo').read().split('MemAvailable:')[1].split()[0]) / 1048576
        jobs = max(1, min(6, int(avail / (3.0 if big else 1.0))))
        cmd = [sys.executable, os.path.join(HERE, 'eyelevel_anim.py'), src, out, '--jobs', str(jobs), '--relief']
        if hover:
            cmd.append('--no-soles')
        if extra:
            cmd += extra[0]
        print('==', name, flush=True)
        subprocess.run(cmd, check=True)
        compare(src, os.path.join(out, anim), os.path.join(out, 'compare.png'))
