"""Reader for Remastered HD sprites (anim/main_NNN.anim): header, frame table and every DXT layer, decoded with numpy.
Layout as in src/fp_assets.h (verified there on real files)."""
import struct
import numpy as np


def _rgb565(c):
    c = c.astype(np.uint32)
    return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1)


def _colour_block(blocks, four_colour_always):
    """blocks: (N,8) uint8 colour part of DXT1/DXT5 blocks -> (N,16,3) uint8 texels in block raster order."""
    c0 = blocks[:, 0].astype(np.uint32) | (blocks[:, 1].astype(np.uint32) << 8)
    c1 = blocks[:, 2].astype(np.uint32) | (blocks[:, 3].astype(np.uint32) << 8)
    bits = blocks[:, 4:8].copy().view('<u4')[:, 0]
    p0, p1 = _rgb565(c0).astype(np.int32), _rgb565(c1).astype(np.int32)
    four = (c0 > c1) | four_colour_always
    p2 = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 1)  # (N,4,3)
    idx = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3  # (N,16)
    return np.take_along_axis(pal, idx[:, :, None].astype(np.int64), 1).astype(np.uint8)


def _alpha_block(blocks):
    """blocks: (N,8) uint8 DXT5 alpha part -> (N,16) uint8."""
    a0, a1 = blocks[:, 0].astype(np.int32), blocks[:, 1].astype(np.int32)
    ab = np.zeros(len(blocks), np.uint64)
    for i in range(6):
        ab |= blocks[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    idx = ((ab[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)).astype(np.int32)
    pal = np.zeros((len(blocks), 8), np.int32)
    pal[:, 0], pal[:, 1] = a0, a1
    eight = a0 > a1
    for i in range(2, 8):
        pal[:, i] = np.where(eight, ((8 - i) * a0 + (i - 1) * a1) // 7, 0)
    for i in range(2, 6):
        pal[:, i] = np.where(eight, pal[:, i], ((6 - i) * a0 + (i - 1) * a1) // 5)
    pal[:, 6] = np.where(eight, pal[:, 6], 0)
    pal[:, 7] = np.where(eight, pal[:, 7], 255)
    return np.take_along_axis(pal, idx, 1).astype(np.uint8)


def decode_dds(raw, off, w, h):
    """DDS at raw[off:] (128-byte header) -> (h,w,4) uint8 RGBA."""
    fmt = bytes(raw[off + 84:off + 88])
    bw, bh = w // 4, h // 4
    if fmt == b'DXT1':
        b = np.frombuffer(raw, np.uint8, bw * bh * 8, off + 128).reshape(-1, 8)
        rgb = _colour_block(b, False)
        a = np.full(rgb.shape[:2], 255, np.uint8)
    elif fmt == b'DXT5':
        b = np.frombuffer(raw, np.uint8, bw * bh * 16, off + 128).reshape(-1, 16)
        a = _alpha_block(b[:, :8])
        rgb = _colour_block(b[:, 8:], True)
    else:
        raise ValueError('unsupported DDS format %r' % fmt)
    px = np.concatenate([rgb, a[:, :, None]], -1).reshape(bh, bw, 4, 4, 4)
    return px.transpose(0, 2, 1, 3, 4).reshape(h, w, 4)


class Anim:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        if raw[:4] != b'ANIM':
            raise ValueError('not an ANIM file')
        self.raw = raw
        nlayers, entries = struct.unpack_from('<HH', raw, 8)
        if entries != 1:
            raise ValueError('expected one sprite record (HD file)')
        self.layer_names = [raw[12 + i * 32:44 + i * 32].split(b'\0')[0].decode() for i in range(nlayers)]
        H = 0x14c
        nf, self.tilt10, self.box_w, self.box_h, ft = struct.unpack_from('<HHHHI', raw, H)  # tilt10: eye-level files' art tilt
        self.layers = {}
        for L, name in enumerate(self.layer_names):
            off, sz, tw, th = struct.unpack_from('<IIHH', raw, H + 12 + L * 12)
            if sz:
                self.layers[name] = (off, tw, th)
        self.frames = [struct.unpack_from('<HHhhHH', raw, ft + i * 16) for i in range(nf)]  # x, y, xoff, yoff, w, h
        # Some frames stick out of the box (164 anims; the Firebat's): the box grows evenly until every frame fits, so
        # its centre (the unit's position) stays where it was. orig_box: the box as stored.
        self.orig_box = (self.box_w, self.box_h)
        px = max([0] + [-f[2] for f in self.frames] + [f[2] + f[4] - self.box_w for f in self.frames])
        py = max([0] + [-f[3] for f in self.frames] + [f[3] + f[5] - self.box_h for f in self.frames])
        if px or py:
            self.box_w += 2 * px
            self.box_h += 2 * py
            self.frames = [(x, y, xo + px, yo + py, w, h) for x, y, xo, yo, w, h in self.frames]
        self._atlas = {}

    def atlas(self, layer):
        if layer not in self._atlas:
            off, tw, th = self.layers[layer]
            self._atlas[layer] = decode_dds(self.raw, off, tw, th)
        return self._atlas[layer]

    def frame(self, i, layer='diffuse', boxed=True):
        """Frame i of a layer, placed in the sprite's box (box_h, box_w, 4) when boxed, else its tight rect."""
        x, y, xo, yo, w, h = self.frames[i]
        img = self.atlas(layer)[y:y + h, x:x + w]
        if not boxed:
            return img
        out = np.zeros((self.box_h, self.box_w, 4), np.uint8)
        out[yo:yo + h, xo:xo + w] = img
        return out
