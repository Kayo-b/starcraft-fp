"""Eye-level voxel models of a unit with the SC Evo mod's 3D model as its shape (for units drawn from high up, where the
sprite's own silhouettes leave the shape loose: the SCV and Firebat, drawn from 44-49 degrees).

For every directional pose of the sprite, the mod's model is posed by its animations at the time that matches that
pose best (Stand, Walk and Attack sampled through their lengths, scored by how well the posed model's outline covers
the 17 drawn directions' silhouettes), fitted once in turn, size and place to the sprite, and voxelized. The pose's
voxel model is then the sprite's own hull (and depth carving) inside the posed model (grown by GROW voxels):
unit_voxel.build(prior=...). Colours still come from the sprite's frames (eyeview.render_vdt).

    mod_unit.py ANIM MODEL.m3 OUT_DIR [TILT AXIS]
Writes OUT_DIR/models/gNN.npz for eyelevel_anim.py (run it with --calib TILT,AXIS and the same OUT_DIR), and
OUT_DIR/mod_fit.txt (the transform, the pose picked for each group and its outline IoU).
Needs build/m3_obj (the rest-pose OBJ: faces). Run with the venv python (numpy, scipy, trimesh)."""
import os
import struct
import subprocess
import sys
from multiprocessing import Pool
import numpy as np
from scipy import ndimage
from anim import Anim
from views import camera
import unit_voxel
from m3_pose import M3, sequences, quat_matrix, skinned

HERE = os.path.dirname(os.path.abspath(__file__))
M3_OBJ = os.path.join(HERE, '..', '..', 'build', 'm3_obj')
SEQS = ('Stand', 'Walk', 'Attack', 'Stand Ready', 'Attack 01')
GROW = int(os.environ.get("MOD_GROW", "2"))


# ---- the model's animation at any time ----

def seq_range(f, k):
    _, so, sn, sv = f.ref(f.m + 16)
    size = 96 if sv == 1 else 92
    return f.u32(so + k * size + 20), f.u32(so + k * size + 24)


def tracks_at(f, seq, t):
    """animId -> value at time t (ms) of the sequence's tracks: vectors lerped, quaternions nlerped."""
    m = f.m
    _, stc_o, _, _ = f.ref(m + 28)
    _, stg_o, _, _ = f.ref(m + 40)
    out = {}
    for c in f.array(stg_o + seq * 24 + 12, 'I').astype(int):
        co = stc_o + c * 204
        ids, refs = f.array(co + 20, 'I').astype(int), f.array(co + 32, 'I').astype(int)
        for aid, r in zip(ids, refs):
            kind, idx = r >> 16, r & 0xFFFF
            if aid in out or kind not in (2, 3):
                continue
            _, to, tn, _ = f.ref(co + (72 if kind == 2 else 84))
            if idx >= tn:
                continue
            fr, keys = f.ref(to + idx * 32), f.ref(to + idx * 32 + 20)
            if keys is None:
                continue
            n = 3 if kind == 2 else 4
            vals = f.f32(keys[1], n * keys[2]).reshape(-1, n)
            times = np.array(struct.unpack_from('<%di' % fr[2], f.d, fr[1])) if fr else np.zeros(1)
            j = int(np.searchsorted(times, t, side='right')) - 1
            if j < 0:
                v = vals[0]
            elif j >= len(times) - 1:
                v = vals[min(len(vals) - 1, len(times) - 1)]
            else:
                a = (t - times[j]) / max(1, times[j + 1] - times[j])
                v0, v1 = vals[j], vals[j + 1]
                if n == 4 and np.dot(v0, v1) < 0:
                    v1 = -v1
                v = v0 * (1 - a) + v1 * a
            out[aid] = v
    return out


def pose_at(f, seq, t):
    """Absolute bone matrices at time t of a sequence (as m3_pose.pose, with interpolated keys) and inverse rests."""
    tracks = tracks_at(f, seq, t)
    _, bo, bn, _ = f.ref(f.m + 80)
    A = np.zeros((bn, 4, 4))
    for b in range(bn):
        o = bo + b * 160
        parent = f.i16(o + 20)
        loc = tracks.get(f.u32(o + 28), f.f32(o + 32, 3))
        rot = tracks.get(f.u32(o + 64), f.f32(o + 68, 4))
        scl = tracks.get(f.u32(o + 108), f.f32(o + 112, 3))
        L = np.eye(4)
        L[:3, :3] = quat_matrix(rot) * scl
        L[:3, 3] = loc
        A[b] = A[parent] @ L if 0 <= parent < b else L
    iref = next(e for e in f.E if e[0] == 'IREF')
    I = np.array(struct.unpack_from('<%df' % (16 * iref[2]), f.d, iref[1])).reshape(-1, 4, 4).transpose(0, 2, 1)
    return A, I


def faces(obj):
    F = []
    for line in open(obj):
        if line.startswith('f '):
            F.append([int(t.split('/')[0]) - 1 for t in line.split()[1:4]])
    return np.array(F)


# ---- placing it on the sprite ----

def solid_points(V, F, pitch):
    """Points (N,3) on a pitch grid inside the mesh (surface voxels, then holes filled slice by slice in 3D)."""
    import trimesh
    mesh = trimesh.Trimesh(V, F, process=False)
    vox = mesh.voxelized(pitch)
    occ = ndimage.binary_fill_holes(ndimage.binary_closing(vox.matrix, iterations=1))
    idx = np.argwhere(occ)
    return trimesh.transformations.transform_points(idx.astype(np.float64), vox.transform)


def place(P, yaw, s, t):
    c, sn = np.cos(np.radians(yaw)), np.sin(np.radians(yaw))
    R = np.array([[c, -sn, 0], [sn, c, 0], [0, 0, 1.0]])
    return (P * s) @ R.T + t


def outline_iou(Pw, masks, elev, axis, bw, bh, step=2):
    """Mean IoU between the 17 silhouettes and the projected points (splatted step x step)."""
    ious = []
    for d, m in enumerate(masks):
        p = Pw @ camera(elev, d * 360.0 / 32).T
        ui = np.floor(p[:, 0] + bw / 2.0 + axis).astype(int)
        vi = np.floor(p[:, 1] + bh / 2.0).astype(int)
        img = np.zeros_like(m)
        for dy in range(step):
            for dx in range(step):
                u, v = ui + dx, vi + dy
                ok = (u >= 0) & (v >= 0) & (u < m.shape[1]) & (v < m.shape[0])
                img[v[ok], u[ok]] = True
        ious.append((img & m).sum() / max(1, (img | m).sum()))
    return float(np.mean(ious))


def fit(Pm, masks, elev, axis, bw, bh, log):
    """Turn, size and place of the model (points Pm, model units, z up) so its outline covers the silhouettes.
    Start: size from heights, place from the hull's box; then coordinate search."""
    # the silhouettes' hull, to start from its extent
    hull_lo, hull_hi = hull_box(masks, elev, axis, bw, bh)
    h_model = np.ptp(Pm[:, 2])
    s0 = (hull_hi[2] - hull_lo[2]) / max(h_model, 1e-6)
    best = None
    for yaw in range(0, 360, 45):
        P = place(Pm, yaw, s0, 0)
        t = (hull_lo + hull_hi) / 2 - (P.min(0) + P.max(0)) / 2
        t[2] = hull_lo[2] - P[:, 2].min()
        v = outline_iou(P + t, masks, elev, axis, bw, bh, 3)
        if best is None or v > best[0]:
            best = (v, yaw, s0, t)
    v, yaw, s, t = best
    log('fit: start yaw %d, scale %.3f, IoU %.3f' % (yaw, s, v))
    steps = [('yaw', 10.0), ('s', 0.05 * s), ('x', 4.0), ('y', 4.0), ('z', 4.0)]
    for rnd in range(4):
        for name, h in steps:
            hh = h / (2 ** rnd)
            for sign in (-1, 1):
                while True:
                    cand = [yaw, s, t.copy()]
                    if name == 'yaw':
                        cand[0] += sign * hh
                    elif name == 's':
                        cand[1] += sign * hh
                    else:
                        cand[2]['xyz'.index(name)] += sign * hh
                    vc = outline_iou(place(Pm, cand[0], cand[1], cand[2]), masks, elev, axis, bw, bh, 2)
                    if vc > v + 1e-4:
                        v, yaw, s, t = vc, cand[0], cand[1], cand[2]
                    else:
                        break
    log('fit: yaw %.1f, scale %.3f, t (%.1f, %.1f, %.1f), IoU %.3f' % (yaw, s, *t, v))
    return yaw, s, t, v


def hull_box(masks, elev, axis, bw, bh, s=2.0):
    R = max(np.abs(np.nonzero(m)[1] + 0.5 - bw / 2.0 - axis).max() for m in masks) + 2
    rows = np.concatenate([np.nonzero(m)[0] for m in masks])
    se, ce = np.sin(np.radians(elev)), np.cos(np.radians(elev))
    xs = np.arange(-R, R, s) + s / 2
    zs = np.arange((bh / 2.0 - rows.max() - 1 - R * se) / ce - 2, (bh / 2.0 - rows.min() + R * se) / ce + 2, s) + s / 2
    X, Y, Z = np.meshgrid(xs, xs, zs, indexing='ij')
    P = np.stack([X.ravel(), Y.ravel(), Z.ravel()], 1)
    keep = np.ones(len(P), bool)
    for d, m in enumerate(masks):
        p = P @ camera(elev, d * 360.0 / 32).T
        ui = np.floor(p[:, 0] + bw / 2.0 + axis).astype(int)
        vi = np.floor(p[:, 1] + bh / 2.0).astype(int)
        ok = (ui >= 0) & (vi >= 0) & (ui < m.shape[1]) & (vi < m.shape[0])
        ins = np.zeros(len(P), bool)
        ins[ok] = m[vi[ok], ui[ok]]
        keep &= ins
    H = P[keep]
    return H.min(0), H.max(0)


# ---- per group ----

def candidates(f):
    names = sequences(f)
    out = []
    for nm in SEQS:
        if nm not in names:
            continue
        k = names.index(nm)
        a, b = seq_range(f, k)
        n = 1 if nm.startswith('Stand') else 24 if nm == 'Walk' else 12
        out += [(nm, k, a + (b - a) * j / n) for j in range(n)]
    return out


def group_job(job):
    path, m3, obj, g, elev, axis, yaw, s, t, cands, out = job
    a = Anim(path)
    masks = [a.frame(g * 17 + d)[..., 3] >= 128 for d in range(17)]
    f = M3(m3)
    F = faces(obj)
    best = None
    for nm, k, tm in cands:  # the pose that fits this group best (on a 2 px grid)
        V = skinned(f, *pose_at(f, k, tm))
        P = solid_points(V, F, 2.0 / s)
        Pw = place(P, yaw, s, t)
        # small re-centring: the model's walk may sway
        v = outline_iou(Pw, masks, elev, axis, a.box_w, a.box_h, 2)
        if best is None or v > best[0]:
            best = (v, nm, tm, V)
    v, nm, tm, V = best
    Pw = place(solid_points(V, F, 1.0 / s), yaw, s, t)  # 1 voxel per HD px
    m = unit_voxel.build(path, g, elev, axis, log=lambda *x: None, prior=Pw, prior_grow=GROW)
    np.savez_compressed(os.path.join(out, 'g%02d.npz' % g), **m)
    return g, nm, tm, v


if __name__ == '__main__':
    path, m3, out = sys.argv[1], sys.argv[2], sys.argv[3]
    a = Anim(path)
    if len(sys.argv) > 5:
        elev, axis = float(sys.argv[4]), float(sys.argv[5])
    else:
        elev, axis = unit_voxel.calibrate(a, 0)
    os.makedirs(os.path.join(out, 'models'), exist_ok=True)
    name = os.path.splitext(os.path.basename(m3))[0]
    prefix = os.path.join(out, name)
    if not os.path.exists(prefix + '.obj'):
        root = m3[:m3.index('/Assets/')]  # the asset root (Base.SC2Assets), the model relative to it
        subprocess.run([M3_OBJ, root, m3[len(root) + 1:], prefix], check=True, stdout=subprocess.DEVNULL)
    f = M3(m3)
    F = faces(prefix + '.obj')
    logf = open(os.path.join(out, 'mod_fit.txt'), 'w')
    def log(*x):
        line = ' '.join(str(v) for v in x); print(line, flush=True); logf.write(line + '\n'); logf.flush()
    log('%s on %s: tilt %.1f, axis %+.1f' % (name, os.path.basename(path), elev, axis))
    # one transform for every pose, fitted on the first group in the model's standing pose
    names = sequences(f)
    V0 = skinned(f, *pose_at(f, names.index('Stand') if 'Stand' in names else 0, 0))
    h = np.ptp(V0[:, 2])
    masks = [a.frame(d)[..., 3] >= 128 for d in range(17)]
    yaw, s, t, v = fit(solid_points(V0, F, h / 60), masks, elev, axis, a.box_w, a.box_h, log)
    ngroups = len(a.frames) // 17
    cands = candidates(f)
    jobs = [(path, m3, prefix + '.obj', g, elev, axis, yaw, s, t, cands, os.path.join(out, 'models')) for g in range(ngroups)]
    with Pool(min(6, ngroups)) as p:
        for g, nm, tm, v in p.imap(group_job, jobs):
            log('group %2d: %s at %4.0f ms, outline IoU %.3f' % (g, nm, tm, v))
