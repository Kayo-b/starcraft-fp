"""A turning unit through GazPrash's 2d-to-3d-voxelizer (headless build, `cmd/headless` in the clone), for comparison
with unit_voxel.py. The voxelizer extrudes a FRONT picture and trims it by LEFT/RIGHT side pictures (quad mode,
[LEFT, FRONT, RIGHT, BACK] side by side, each centred), i.e. it expects level side views; a sprite's directions are
seen from 31 degrees above, so this is the tool used as it is meant to be, on pictures it was not meant for.

    gazprash_unit.py ANIM GROUP AXIS OUTDIR
Sheet: LEFT = direction 8 (the unit's right side, facing east), FRONT = 16 (facing the camera), RIGHT = direction 8
mirrored (its left side), BACK = direction 0 mirrored (the tool reads BACK column for column with FRONT). Writes
OUTDIR/gazprash_sheet.png, gazprash.obj/.mtl and gazprash.npz (face centres, normals, colours) for eyeview.render.
Env VOX2D3D: the binary (default: the clone in diablo-hellfire-fp/research)."""
import os
import subprocess
import sys
import numpy as np
from PIL import Image
from anim import Anim

VOX = os.environ.get('VOX2D3D', '/home/kxyx/projects/diablo-hellfire-fp/research/2d-to-3d-voxelizer/vox2d3d')


def sheet(a, group, axis):
    tiles = []
    for d, flip in ((8, False), (16, False), (8, True), (0, True)):
        f = a.frame(group * 17 + d)
        ax = a.box_w / 2.0 + axis
        if flip:
            f, ax = f[:, ::-1], a.box_w - ax
        tiles.append((f, ax))
    cols = np.nonzero(np.concatenate([f[..., 3] for f, _ in tiles], 0).max(0) >= 128)[0]
    R = int(max(max(abs(np.nonzero(f[..., 3].max(0) >= 128)[0] + 0.5 - ax)) for f, ax in tiles)) + 2
    rows = np.nonzero(np.concatenate([f[..., 3] for f, _ in tiles], 1).max(1) >= 128)[0]
    y0, y1 = rows.min() - 1, rows.max() + 2
    out = []
    for f, ax in tiles:
        c0 = int(round(ax)) - R
        t = np.zeros((y1 - y0, 2 * R, 4), np.uint8)
        src = f[y0:y1, max(0, c0):c0 + 2 * R]
        t[:, max(0, -c0):max(0, -c0) + src.shape[1]] = src
        t[..., 3] = np.where(t[..., 3] >= 128, 255, 0)  # the tool treats any alpha as solid
        out.append(t)
    return np.concatenate(out, 1), R


def load_obj(path):
    """Quads of the voxelizer's OBJ -> face centres, normals, colours (Kd of each face's material)."""
    mtl = {}
    cur = None
    for line in open(os.path.splitext(path)[0] + '.mtl'):
        if line.startswith('newmtl'):
            cur = line.split()[1]
        elif line.startswith('Kd') and cur:
            mtl[cur] = [float(x) * 255 for x in line.split()[1:4]]
    V, C, N, col = [], [], [], []
    cur = None
    for line in open(path):
        if line.startswith('v '):
            V.append([float(x) for x in line.split()[1:4]])
        elif line.startswith('usemtl'):
            cur = mtl[line.split()[1]]
        elif line.startswith('f '):
            q = np.array([V[int(t.split('/')[0]) - 1] for t in line.split()[1:5]])
            n = np.cross(q[1] - q[0], q[2] - q[0])
            C.append(q.mean(0)); N.append(n / max(np.linalg.norm(n), 1e-9)); col.append(cur)
    return np.array(C), np.array(N), np.array(col)


if __name__ == '__main__':
    a = Anim(sys.argv[1])
    group, axis, out = int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
    img, R = sheet(a, group, axis)
    Image.fromarray(img).save(os.path.join(out, 'gazprash_sheet.png'))
    obj = os.path.join(out, 'gazprash.obj')
    subprocess.run([VOX, '-in', os.path.join(out, 'gazprash_sheet.png'), '-out', obj, '-mode', 'quad',
                    '-depth-scale', '1'], check=True)
    C, N, col = load_obj(obj)
    # tool frame (x right, y up, z toward the viewer of FRONT, where the unit faces) -> model frame facing north
    M = np.array([[-1.0, 0, 0], [0, 0, 1.0], [0, 1.0, 0]])
    C = (C - [R, 0, 0]) @ M.T
    N = N @ M.T
    # The tool's faces sit half a voxel out; the face centres pulled back in are its voxels, one colour per face.
    C[:, 2] -= C[:, 2].min()
    np.savez_compressed(os.path.join(out, 'gazprash.npz'), P=C, normal=N.astype(np.float32),
                        col=np.concatenate([col, np.zeros((len(col), 1))], 1).astype(np.uint8))
    print('%d faces' % len(C))
