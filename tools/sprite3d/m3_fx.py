"""The parts of an SC2 .m3 model that `m3_obj` leaves out: batches with non-opaque materials (glows, energy, effects;
the game draws only opaque ones). Some are the building itself in the sprite, e.g. the Fleet Beacon's energy sphere, so
they can be added back as plain geometry (texture -1: the sprite's pixels paint them, `trellis_compose.py ...
sprite-texture`). The OBJ from `m3_obj` (or `m3_pose.py`) holds every vertex of the model, so only faces are added.
Struct layouts from m3addon's structures.xml, as `fp_m3.h` reads them (MODL v23+, DIV_, REGN v3+, BAT_, MATM, MAT_).

    m3_fx.py MODEL.m3 OBJ                    list the left-out batches (blend mode, texture, size in OBJ's pose)
    m3_fx.py MODEL.m3 OBJ OUT.obj [K ...] [drop:R ...]
                                             OBJ plus left-out batches K (numbers from the list), without the triangles
                                             of region R, written to OUT.obj (with OBJ's textures)
Some models hold two versions of a part in the same place, shown in turn in game: the Assimilator's dome full (region 5)
and depleted (region 2), both opaque, so both drawn they fight (jagged holes). drop:2 keeps the full one.
Run with the venv python (numpy)."""
import glob
import shutil
import sys
import numpy as np
from m3_pose import M3

BLEND = ['opaque', 'blend', 'add', 'alpha add', 'mod', 'mod 2x']


def left_out(f):
    """[(region, faces as global vertex indices (n, 3), material description)] for every non-opaque batch."""
    m = f.m
    _, dv, _, _ = f.ref(m + 112)
    U = f.array(dv, 'H').astype(int)
    _, ro, rn, rver = f.ref(dv + 12)
    _, bo, bn, _ = f.ref(dv + 24)
    matm, mat = f.ref(m + 300), f.ref(m + 312)
    rsize = 48 if rver >= 5 else 40 if rver == 4 else 36
    msize = 352 if mat[3] >= 20 else 340 if mat[3] == 19 else 280 if mat[3] >= 16 else 268
    out = []
    for k in range(bn):
        ri, mi = f.u16(bo + k * 14 + 4), f.u16(bo + k * 14 + 10)
        kind, si = f.u32(matm[1] + mi * 8), f.u32(matm[1] + mi * 8 + 4)
        if kind != 1:
            desc = 'material kind %d' % kind
            if kind == 3:  # composite: drawn when one of its sections is opaque (as fp_m3.h does)
                continue
        else:
            mo = mat[1] + si * msize
            blend = f.u32(mo + 20)
            if blend == 0:
                continue
            layers = mo + (64 if mat[3] >= 20 else 52)
            lay = f.ref(layers)
            desc = '%s, %s' % (BLEND[blend] if blend < len(BLEND) else blend, f.text(lay[1] + 4) if lay else '')
        r = ro + ri * rsize
        fv, ff, nf = f.u32(r + 8), f.u32(r + 16), f.u32(r + 20)
        out.append((ri, fv + U[ff:ff + nf].reshape(-1, 3), desc))
    return out


def region_vertices(f, r):
    """The vertex index range [first, end) of region r."""
    _, dv, _, _ = f.ref(f.m + 112)
    _, ro, _, rver = f.ref(dv + 12)
    o = ro + r * (48 if rver >= 5 else 40 if rver == 4 else 36)
    return f.u32(o + 8), f.u32(o + 8) + f.u32(o + 12)


def obj_vertices(path):
    return np.array([[float(x) for x in l.split()[1:4]] for l in open(path) if l.startswith('v ')])


if __name__ == '__main__':
    model, obj = sys.argv[1], sys.argv[2]
    f = M3(model)
    parts, V = left_out(f), obj_vertices(obj)
    if len(sys.argv) == 3:
        for k, (ri, F, desc) in enumerate(parts):
            P = V[F.ravel()]
            print('%3d region %3d  %5d triangles  %s\n      x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f'
                  % (k, ri, len(F), desc, *np.ravel(np.stack([P.min(0), P.max(0)]).T)))
    else:
        out, add = sys.argv[3], [int(k) for k in sys.argv[4:] if not k.startswith('drop:')]
        drop = [region_vertices(f, int(k[5:])) for k in sys.argv[4:] if k.startswith('drop:')]
        lines, dropped = [], 0
        for line in open(obj):
            if line.startswith('f ') and any(a <= int(line.split()[1].split('/')[0]) - 1 < b for a, b in drop):
                dropped += 1
                continue
            lines.append(line)
        for t in glob.glob(obj[:-4] + '_t*.ppm'):  # the textures go with the OBJ's name
            shutil.copyfile(t, out[:-4] + t[len(obj) - 4:])
        with open(out, 'w') as o:
            o.writelines(lines)
            o.write('usemtl t-1\n')
            for k in add:
                for a, b, c in parts[k][1] + 1:
                    o.write('f %d/%d %d/%d %d/%d\n' % (a, a, b, b, c, c))
        print('%s: added %d triangles, dropped %d' % (out, sum(len(parts[k][1]) for k in add), dropped))
