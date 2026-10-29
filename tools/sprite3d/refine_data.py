"""Training pairs for the clean-up model (refine.py): renders of a unit's voxel model through the original camera, made
without the frame they stand in for, against that frame.

There are no eye-level frames to learn from, but an eye-level render's faults are the faults of a render made from
other directions: surfaces no frame shows (filled from around them), pixels borrowed from a surface just in front, a
neighbouring direction's lighting and a pixel of misregistration, stretching. Drawing direction d through the
original camera with frame d left out (and, harder, d and both neighbours) gives renders with those faults whose right
answer is known: frame d itself. Input and answer share the viewpoint, so the model learns to mend the art, not to
change the view.

    refine_data.py OUT.npz [UNIT...]      (default: every unit with voxel models in out/sprite3d/eyelevel)
v2 adds harder variants closer to eye level, where 20-30% of a steep unit is surface no frame shows: 'loo5' (d and two
neighbours each side left out) and 'blob' (the loo1 render with random blobs, 20-35% of the unit, replaced by the
renderer's fill from around them and marked filled), so the model learns to paint big unseen areas, not only seams.

Per sample: inp (h,w,6) uint8 = render RGB, coverage, filled mask, borrowed mask; tgt (h,w,4) = the frame's RGBA;
cropped around the unit; plus unit, group, direction, variant. Run with the venv python."""
import os
import sys
from multiprocessing import Pool
import numpy as np
from anim import Anim
import eyeview as ev
from eyelevel_batch import UNITS, ANIM_DIR

ROOT = os.environ.get('EYELEVEL_OUT', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'out', 'sprite3d', 'eyelevel'))


def fill_from_around(rgb, known, darken=0.85):
    """The renderer's fill (eyeview.render_vdt): normalized convolution of the known pixels at growing radii."""
    from scipy import ndimage
    out = rgb.astype(np.float64).copy()
    kn = known.astype(np.float64)
    got = known.copy()
    for sig in (0.5, 1.0, 2.0, 4.0, 8.0, 16.0):
        num = np.stack([ndimage.gaussian_filter(rgb[..., c] * kn, sig) for c in range(3)], -1)
        den = ndimage.gaussian_filter(kn, sig)
        new = (den > 1e-3) & ~got
        out[new] = num[new] / den[new][:, None] * darken
        got |= new
    return np.clip(out, 0, 255).astype(np.uint8)


def blobs(cov, rng, share):
    """Random smooth blobs covering about `share` of the covered pixels."""
    from scipy import ndimage
    noise = ndimage.gaussian_filter(rng.standard_normal(cov.shape), rng.uniform(3, 9))
    v = noise[cov]
    if len(v) == 0:
        return np.zeros_like(cov)
    return cov & (noise > np.quantile(v, 1 - share))


def pairs(job):
    name, anim_file, npz = job
    path = os.path.join(ANIM_DIR, anim_file)
    a = Anim(path)
    model = ev.load(npz)
    sampler = ev.Sampler(model, path)
    g = model['group']
    ground = a.box_h / 2.0 - np.cos(np.radians(model['elev'])) * model['zshift']
    out = []
    for d in range(17):
        s = sampler.shots[d]
        tgt = a.frame(g * 17 + d)
        for variant, excl in (('loo1', {d}), ('loo3', {d - 1, d, d + 1} & set(range(17))),
                              ('loo5', set(range(d - 2, d + 3)) & set(range(17)))):
            info = {}
            rgba, _, _ = ev.render_vdt(model, sampler, s.heading, model['elev'], a.box_w, a.box_h, ground,
                                       axis_col=s.ax, exclude=excl, info=info)
            inp = np.concatenate([rgba, info['filled'][..., None].astype(np.uint8) * 255,
                                  info['relaxed'][..., None].astype(np.uint8) * 255], -1)
            m = (rgba[..., 3] > 0) | (tgt[..., 3] > 0)
            ys, xs = np.nonzero(m)
            if len(ys) == 0:
                continue
            y0, y1 = max(0, ys.min() - 4), min(a.box_h, ys.max() + 5)
            x0, x1 = max(0, xs.min() - 4), min(a.box_w, xs.max() + 5)
            out.append(dict(inp=inp[y0:y1, x0:x1], tgt=tgt[y0:y1, x0:x1], unit=name, group=g, d=d, variant=variant))
            if variant == 'loo1':  # the same render with big unseen blobs, filled as the renderer fills
                rng = np.random.default_rng(1000 * g + d + hash(name) % 997)
                cov = rgba[..., 3] > 127
                b = blobs(cov, rng, rng.uniform(0.2, 0.35))
                known = cov & ~b & ~info['filled']
                rgb2 = fill_from_around(rgba[..., :3], known)
                bl = rgba.copy(); bl[..., :3] = np.where((b | info['filled'])[..., None], rgb2, rgba[..., :3])
                inp2 = np.concatenate([bl, ((b | info['filled']).astype(np.uint8) * 255)[..., None],
                                       info['relaxed'][..., None].astype(np.uint8) * 255], -1)
                out.append(dict(inp=inp2[y0:y1, x0:x1], tgt=tgt[y0:y1, x0:x1], unit=name, group=g, d=d, variant='blob'))
    print('%s g%02d: %d pairs' % (name, g, len(out)), flush=True)
    return out


if __name__ == '__main__':
    want = set(sys.argv[2:])
    jobs = []
    for name, anim_file, *_ in UNITS:
        mdir = os.path.join(ROOT, name, 'models')
        if (want and name not in want) or not os.path.isdir(mdir):
            continue
        jobs += [(name, anim_file, os.path.join(mdir, f)) for f in sorted(os.listdir(mdir)) if f.endswith('.npz')]
    with Pool(6) as p:
        res = [s for r in p.map(pairs, jobs) for s in r]
    np.savez_compressed(sys.argv[1], samples=np.array(res, dtype=object))
    print('%s: %d pairs from %d poses' % (sys.argv[1], len(res), len(jobs)))
