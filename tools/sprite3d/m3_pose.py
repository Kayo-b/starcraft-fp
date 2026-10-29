"""An SC2 .m3 model posed by one of its animations (`m3_obj` writes the rest pose only). The mod's Hatchery, Lair and
Hive are one model (HatcherySCBW.m3, each form with its own bones and texture); its "Stand", "Stand B" and "Stand C"
animations show one form each, the other forms' bones scaled to nothing. This evaluates the animation's bones (first
key of every track; rest values where the animation has none), skins the vertices (4 weighted bones each) and writes
the OBJ `m3_obj` wrote with the posed positions, dropping the triangles that collapse. Struct layouts from m3addon's
structures.xml (MODL v23+, BONE v1, STC_ v4, SEQS v1/2, REGN v3+).

    m3_obj ASSET_ROOT MODEL.m3 OUT/sc2/NAME             (rest pose: OBJ, faces, UVs, textures)
    m3_pose.py ASSET_ROOT/MODEL.m3 OUT/sc2/NAME "SEQUENCE" OUT/sc2/NAME_POSE [--only N]   [--list: the sequence names]
With --only N, only the triangles drawn with texture N (`m3_obj`'s tN) are kept: one form of the Hatchery model
(t2 Hatchery in "Stand", t1 Lair in "Stand B", t0 Hive in "Stand C"; the others stay full size, hidden in game).
Run with the venv python (numpy)."""
import os
import shutil
import struct
import sys
import numpy as np


class M3:
    def __init__(self, path):
        self.d = d = open(path, 'rb').read()
        assert d[:4] == b'43DM', 'not an MD34 file'
        ioff, icnt = struct.unpack_from('<II', d, 4)
        self.E = [(d[o:o + 4][::-1].decode('latin1'), *struct.unpack_from('<III', d, o + 4))
                  for o in range(ioff, ioff + 16 * icnt, 16)]  # (tag, offset, count, version)
        self.m = self.ref(12)[1]

    def u16(self, o): return struct.unpack_from('<H', self.d, o)[0]
    def i16(self, o): return struct.unpack_from('<h', self.d, o)[0]
    def u32(self, o): return struct.unpack_from('<I', self.d, o)[0]
    def f32(self, o, n=1): return np.array(struct.unpack_from('<%df' % n, self.d, o))

    def ref(self, o):
        """The index entry a Reference at o points to, or None when empty."""
        n, i = struct.unpack_from('<II', self.d, o)
        return self.E[i] if n and i < len(self.E) else None

    def array(self, o, fmt):
        e = self.ref(o)
        return np.zeros(0) if e is None else np.array(struct.unpack_from('<%d%s' % (e[2], fmt), self.d, e[1]))

    def text(self, o):
        e = self.ref(o)
        return '' if e is None else self.d[e[1]:e[1] + e[2]].split(b'\0')[0].decode('latin1')


def quat_matrix(q):
    x, y, z, w = q / np.linalg.norm(q)
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def sequences(f):
    _, so, sn, sv = f.ref(f.m + 16)
    size = 96 if sv == 1 else 92
    return [f.text(so + k * size + 8) for k in range(sn)]


def pose(f, seq):
    """Absolute bone matrices (4x4) in the animation's first keys (seq None: the bones' own values), and the bones'
    inverse rest matrices."""
    m = f.m
    _, stc_o, _, _ = f.ref(m + 28)
    _, stg_o, _, _ = f.ref(m + 40)
    _, bo, bn, _ = f.ref(m + 80)
    tracks = {}  # animId -> first key, from the group's collections (earlier collections win)
    for c in [] if seq is None else f.array(stg_o + seq * 24 + 12, 'I').astype(int):
        co = stc_o + c * 204
        ids, refs = f.array(co + 20, 'I').astype(int), f.array(co + 32, 'I').astype(int)
        for aid, r in zip(ids, refs):
            kind, idx = r >> 16, r & 0xFFFF  # high half: which list (2 SD3V, 3 SD4Q), low half: the entry
            if aid in tracks or kind not in (2, 3):
                continue
            _, to, tn, _ = f.ref(co + (72 if kind == 2 else 84))
            if idx >= tn:
                continue
            keys = f.ref(to + idx * 32 + 20)
            if keys is not None:
                tracks[aid] = f.f32(keys[1], 3 if kind == 2 else 4)
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


def skinned(f, A, I):
    """Posed vertex positions, in the vertex order `m3_obj` writes."""
    m = f.m
    _, vo, vbytes, _ = f.ref(m + 100)
    _, dv, _, _ = f.ref(m + 112)
    _, ro, rn, rv = f.ref(dv + 12)
    rsize = 48 if rv >= 5 else 40 if rv == 4 else 36
    lookup = f.array(m + 124, 'H').astype(int)
    regions = [struct.unpack_from('<IIIIHH', f.d, ro + k * rsize + 8) for k in range(rn)]
    nverts = max(fv + nv for fv, nv, *_ in regions)
    vs = vbytes // nverts
    raw = np.frombuffer(f.d, np.uint8, nverts * vs, vo).reshape(nverts, vs)
    P = np.c_[raw[:, :12].copy().view('<f4').reshape(nverts, 3), np.ones(nverts)]
    W = raw[:, 12:16].astype(np.float64)
    W /= np.maximum(W.sum(1, keepdims=True), 1)
    out = np.zeros((nverts, 3))
    M = A @ I  # rest -> posed, per bone
    for fv, nv, ff, nf, nb, first in regions:
        sl = slice(fv, fv + nv)
        bones = lookup[first + raw[sl, 16:20].astype(int)]
        for k in range(4):
            out[sl] += W[sl, k:k + 1] * np.einsum('nij,nj->ni', M[bones[:, k]], P[sl])[:, :3]
    return out


if __name__ == '__main__':
    f = M3(sys.argv[1])
    names = sequences(f)
    if '--list' in sys.argv:
        A, I = pose(f, None)
        print('bones %d; bone values x inverse rest matrices, largest difference from identity %.4f' % (
            len(A), np.abs(A @ I - np.eye(4)).max()))
        print('\n'.join('%3d %s' % kv for kv in enumerate(names)))
        sys.exit()
    base, seq, out = sys.argv[2], sys.argv[3], sys.argv[4]
    A, I = pose(f, names.index(seq))
    V = skinned(f, A, I)
    only = 't' + sys.argv[sys.argv.index('--only') + 1] if '--only' in sys.argv else None
    lines, vi, kept, dropped, cur = [], 0, 0, 0, None
    for line in open(base + '.obj'):
        if line.startswith('usemtl'):
            cur = line.split()[1]
        if line.startswith('f ') and only and cur != only:
            continue
        if line.startswith('v '):
            lines.append('v %.6f %.6f %.6f\n' % tuple(V[vi])); vi += 1
        elif line.startswith('f '):
            a, b, c = (V[int(t.split('/')[0]) - 1] for t in line.split()[1:4])
            if np.linalg.norm(np.cross(b - a, c - a)) < 1e-8:
                dropped += 1
                continue
            lines.append(line); kept += 1
        else:
            lines.append(line)
    assert vi == len(V), 'the OBJ is not the same model (%d vertices, the model has %d)' % (vi, len(V))
    open(out + '.obj', 'w').writelines(lines)
    k = 0
    while os.path.exists('%s_t%d.ppm' % (base, k)):
        shutil.copyfile('%s_t%d.ppm' % (base, k), '%s_t%d.ppm' % (out, k)); k += 1
    print('%s: "%s", %d triangles (%d collapsed, dropped), x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f' % (
        out, seq, kept, dropped, *np.ravel(np.stack([V.min(0), V.max(0)], 1))))
