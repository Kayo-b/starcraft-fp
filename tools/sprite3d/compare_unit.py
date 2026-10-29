"""Side by side for one frame group of a turning unit, 5 directions (16, 12, 8, 4, 0), 3x:
row 1 the Remastered frame (what the game stands up as a billboard today), then at eye level (0 degrees) and 10 degrees
above: GazPrash's voxelizer (gazprash_unit.py, rows 2-3) and unit_voxel.py drawn with the frames' own pixels (rows 4-5).

    compare_unit.py ANIM MODEL.npz GAZPRASH.npz OUT.png
Run with the venv python."""
import sys
import numpy as np
from PIL import Image, ImageDraw
from anim import Anim
import eyeview as ev

if __name__ == '__main__':
    a = Anim(sys.argv[1])
    model = ev.load(sys.argv[2])
    g = np.load(sys.argv[3])
    gaz = dict(P=g['P'], col=g['col'], normal=g['normal'], faces=True)
    sampler = ev.Sampler(model, sys.argv[1])
    rad = int(max(np.abs(model['P'][:, :2]).max(), np.abs(gaz['P'][:, :2]).max()) + 6)
    top = max(model['P'][:, 2].max(), gaz['P'][:, 2].max())
    rows = [[], [], [], [], []]
    for d in (16, 12, 8, 4, 0):
        fr = ev.on_black(a.frame(model['group'] * 17 + d))
        ys, _ = np.nonzero(fr.max(2) > 0)
        cx = a.box_w / 2.0 + model['axis']
        rows[0].append(fr[max(0, ys.min() - 6):ys.max() + 6, int(cx - rad):int(cx + rad)])
        for k, e in enumerate((0.0, 10.0)):
            H = int(top * np.cos(np.radians(e)) + rad * np.sin(np.radians(e)) + 12)
            gr = int(top * np.cos(np.radians(e)) + 6)
            img, _ = ev.render(gaz, d * 360.0 / 32, e, 2 * rad, H, gr)
            rows[1 + k].append(ev.on_black(img))
            img, _, _ = ev.render_vdt(model, sampler, d * 360.0 / 32, e, 2 * rad, H, gr)
            rows[3 + k].append(ev.on_black(img))
    labels = ["Remastered frame (today's billboard)", 'GazPrash voxelizer, eye level', 'GazPrash voxelizer, 10 deg',
              'calibrated voxels + frame pixels, eye level', 'calibrated voxels + frame pixels, 10 deg']
    out = []
    for lab, r in zip(labels, rows):
        h = max(t.shape[0] for t in r)
        line = np.concatenate([np.pad(t, ((h - t.shape[0], 0), (0, 4), (0, 0))) for t in r], 1)
        out.append(np.pad(line, ((14, 4), (0, 0), (0, 0))))
        img = Image.fromarray(out[-1]); ImageDraw.Draw(img).text((2, 1), lab, fill=(200, 200, 120)); out[-1] = np.asarray(img)
    w = max(o.shape[1] for o in out)
    img = np.concatenate([np.pad(o, ((0, 0), (0, w - o.shape[1]), (0, 0))) for o in out], 0)
    Image.fromarray(img).resize((img.shape[1] * 3, img.shape[0] * 3), Image.NEAREST).save(sys.argv[4])
