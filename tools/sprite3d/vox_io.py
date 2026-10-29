"""MagicaVoxel .vox round trip for unit_voxel.py models, so a pose can be fixed by hand in a voxel editor (MagicaVoxel,
Goxel, Blender with a .vox add-on) and drawn again by eyelevel_anim.py.

    vox_io.py export MODEL.npz OUT.vox            every solid voxel; surface voxels in their colours (255-colour palette),
                                                  the inside dark grey
    vox_io.py import EDITED.vox MODEL.npz OUT.npz the edited shape with MODEL's placement and camera calibration;
                                                  surface colours from the palette (the game frames take theirs from
                                                  the Remastered frames anyway, eyeview.Sampler; these only fill gaps)
To use an edited pose: import it over OUT_DIR/models/gNN.npz and run eyelevel_anim.py again (it keeps existing models).
Grid: x east, y north, z up (MagicaVoxel's own axes), one voxel per HD pixel, the unit facing north (+y). The .vox holds
the occupied box plus MARGIN voxels of room on every side (keep the canvas size when editing)."""
import struct
import sys
import numpy as np
from PIL import Image
from scipy import ndimage


MARGIN = 8


def window(m):
    """Low corner and size of the exported box: the solid voxels' bounds plus MARGIN, inside the model's grid."""
    solid = m['solid'].astype(int)
    shape = np.array([int(v) for v in m['shape']])
    lo = np.maximum(solid.min(0) - MARGIN, 0)
    hi = np.minimum(solid.max(0) + MARGIN + 1, shape)
    return lo, hi - lo


def chunk(cid, content, children=b''):
    return cid + struct.pack('<ii', len(content), len(children)) + content + children


def export(npz, out):
    m = np.load(npz)
    lo, size = window(m)
    shape = tuple(int(v) for v in size)
    if max(shape) > 256:
        raise SystemExit('occupied box %s is over MagicaVoxel\'s 256 limit' % (shape,))
    solid = m['solid'].astype(int) - lo
    surf = m['idx'].astype(int) - lo
    col = m['col'][:, :3]
    q = Image.fromarray(col[None]).quantize(255, method=Image.Quantize.MEDIANCUT)
    pal = np.array(q.getpalette()[:255 * 3], np.uint8).reshape(-1, 3)
    pal = np.concatenate([pal, np.zeros((255 - len(pal), 3), np.uint8)]) if len(pal) < 255 else pal
    sidx = np.asarray(q)[0].astype(int) + 1  # 1..255
    grid = np.zeros(shape, np.int16)
    grid[tuple(solid.T)] = 256  # inside: the grey entry below
    grid[tuple(surf.T)] = sidx
    # the inside takes the palette entry nearest dark grey
    grey = 1 + int(np.argmin(np.abs(pal.astype(int) - 40).sum(1)))
    grid[grid == 256] = grey
    v = np.argwhere(grid > 0)
    xyzi = struct.pack('<i', len(v)) + np.concatenate([v, grid[tuple(v.T)][:, None]], 1).astype(np.uint8).tobytes()
    rgba = np.concatenate([np.concatenate([pal, np.full((255, 1), 255, np.uint8)], 1).ravel(), np.zeros(4, np.uint8)])
    body = chunk(b'SIZE', struct.pack('<iii', *shape)) + chunk(b'XYZI', xyzi) + chunk(b'RGBA', rgba.tobytes())
    with open(out, 'wb') as f:
        f.write(b'VOX ' + struct.pack('<i', 150) + chunk(b'MAIN', b'', body))
    print('%s: %d voxels, grid %s' % (out, len(v), shape))


def read_vox(path):
    raw = open(path, 'rb').read()
    assert raw[:4] == b'VOX ', 'not a .vox file'
    pos, size, vox, pal = 8, None, None, None
    pos += 12  # MAIN header; its children follow
    while pos < len(raw):
        cid = raw[pos:pos + 4]
        n, _ = struct.unpack_from('<ii', raw, pos + 4)
        body = raw[pos + 12:pos + 12 + n]
        if cid == b'SIZE' and size is None:
            size = struct.unpack('<iii', body[:12])
        elif cid == b'XYZI' and vox is None:
            k = struct.unpack('<i', body[:4])[0]
            vox = np.frombuffer(body[4:4 + 4 * k], np.uint8).reshape(-1, 4)
        elif cid == b'RGBA':
            pal = np.frombuffer(body[:1024], np.uint8).reshape(256, 4)
        pos += 12 + n  # children of nTRN/nGRP/nSHP are siblings here: only the first model is read
    return size, vox, pal


def import_(path, base_npz, out):
    size, vox, pal = read_vox(path)
    base = dict(np.load(base_npz))
    shape = tuple(int(v) for v in base['shape'])
    lo, wsize = window(base)
    if tuple(size) != tuple(int(v) for v in wsize):
        raise SystemExit('edited canvas %s differs from the exported %s (keep the canvas size)' % (size, tuple(wsize)))
    v = vox[:, :3].astype(int) + lo
    occ = np.zeros(shape, bool)
    occ[v[:, 0], v[:, 1], v[:, 2]] = True
    cidx = np.zeros(shape, np.int32)
    cidx[v[:, 0], v[:, 1], v[:, 2]] = vox[:, 3]
    solid = ndimage.binary_erosion(occ, ndimage.generate_binary_structure(3, 1))
    surf = occ & ~solid
    si = np.argwhere(surf)
    g = np.stack(np.gradient(ndimage.gaussian_filter(occ.astype(np.float32), 1.5)), -1)
    N = -g[surf]
    N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-6)
    p = pal[np.clip(cidx[surf] - 1, 0, 255), :3]
    # team mask: the old surface voxel nearest each new one
    old = base['idx'].astype(int)
    tgrid = np.zeros(shape, np.int64) - 1
    tgrid[tuple(old.T)] = np.arange(len(old))
    _, near = ndimage.distance_transform_edt(tgrid < 0, return_indices=True)
    tc = base['col'][tgrid[near[0][surf], near[1][surf], near[2][surf]], 3]
    base.update(idx=si.astype(np.int16), col=np.concatenate([p, tc[:, None]], 1).astype(np.uint8),
                normal=N.astype(np.float32), solid=np.argwhere(occ).astype(np.int16))
    np.savez_compressed(out, **base)
    print('%s: %d voxels, %d on the surface' % (out, occ.sum(), len(si)))


if __name__ == '__main__':
    if sys.argv[1] == 'export':
        export(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == 'import':
        import_(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        raise SystemExit(__doc__)
