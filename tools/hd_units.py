"""Gathers the Remastered HD sprites (anim/main_NNN.anim) of every unit type into one folder per unit, sorted by race,
units vs buildings. A unit's folder holds its main image and every image its iscripts make (shadow, birth, death,
remnants, overlays, turret), its construction image, and its turret subunit's images. Each anim is hard-linked
(no extra disk) and decoded: the diffuse frames as PNGs, every layer's atlas as a PNG.

usage: python3 -I tools/hd_units.py EXTRACTED_DIR OPENBW_DIR OUT_DIR [--no-png]
  EXTRACTED_DIR  casc-scratch/extracted (arr/, rez/, scripts/, anim/)
  OPENBW_DIR     openbw checkout (bwenums.h gives the image names)"""
import os, re, struct, sys, json
from concurrent.futures import ProcessPoolExecutor
import numpy as np
from PIL import Image

N_UNITS, N_FLINGY, N_SPRITES, N_IMAGES = 228, 209, 517, 999


def tbl(path, parts=1):
    """String table; with parts > 1 each entry's first parts NUL-separated pieces (stat_txt: name, subtitle)."""
    raw = open(path, 'rb').read()
    n = struct.unpack_from('<H', raw, 0)[0]
    out = []
    for o in struct.unpack_from('<%dH' % n, raw, 2):
        p = []
        for _ in range(parts):
            e = raw.find(b'\0', o)
            e = len(raw) if e < 0 else e
            p.append(raw[o:e].decode('latin1'))
            o = e + 1
        out.append(p[0] if parts == 1 else p)
    return out


def image_names(openbw):
    src = open(os.path.join(openbw, 'bwenums.h')).read()
    body = src[src.index('enum struct ImageTypes'):]
    body = body[body.index('{') + 1:body.index('};')]
    names = [m.group(1) for m in re.finditer(r'IMAGEID_(\w+)', body) if m.group(1) != 'None']
    assert len(names) == N_IMAGES, len(names)
    return names


class Data:
    def __init__(self, ex, openbw):
        u = open(os.path.join(ex, 'arr/units.dat'), 'rb').read()
        assert len(u) == 19876
        self.flingy = list(u[0:228])
        self.subunit = struct.unpack_from('<228H', u, 228)
        self.construction = struct.unpack_from('<228I', u, 1332)
        self.abilities = struct.unpack_from('<228I', u, 7032)
        self.group = list(u[16684:16684 + 228])
        self.flingy_sprite = struct.unpack_from('<209H', open(os.path.join(ex, 'arr/flingy.dat'), 'rb').read(), 0)
        self.sprite_image = struct.unpack_from('<517H', open(os.path.join(ex, 'arr/sprites.dat'), 'rb').read(), 0)
        im = open(os.path.join(ex, 'arr/images.dat'), 'rb').read()
        assert len(im) == 999 * 38
        self.image_grp = struct.unpack_from('<999I', im, 0)
        self.image_iscript = struct.unpack_from('<999I', im, 999 * 10)
        self.grp_names = tbl(os.path.join(ex, 'arr/images.tbl'))
        self.unit_names = [n if sub in ('', '*') else '%s (%s)' % (n, sub)
                           for n, sub in tbl(os.path.join(ex, 'rez/stat_txt.tbl'), 2)[:N_UNITS]]
        self.image_names = image_names(openbw)
        self.iscript_refs = self._iscript(os.path.join(ex, 'scripts/iscript.bin'))
        # images sharing a GRP share one anim file: the first image with that GRP (names ignore case, as on
        # Windows: the Lurker is zerg\Zlurker.grp, its remnants zerg\zLurker.grp, both in main_920.anim)
        first = {}
        key = [self.grp_names[g - 1].lower() for g in self.image_grp]
        for i, k in enumerate(key):
            first.setdefault(k, i)
        self.anim_of = [first[k] for k in key]

    def _iscript(self, path):
        """iscript id -> [('image', id) | ('sprite', id) | ('flingy', id) | ('next', None) | ('gas', n)], from every
        animation's code. Operands as in OpenBW's load_iscript_bin, with the u16 ones that name an image (I), sprite (S)
        or flingy (F) marked; s1 = signed byte, G = creategasoverlays, N = imgulnextid, v = count + u16s."""
        ops = {0: '2', 1: '2', 2: 's1', 3: 's1', 4: 's1s1', 5: '1', 6: '11', 7: 'j', 8: 'I11', 9: 'I11', 10: 'I', 11: 'I',
               12: '', 13: 'I11', 14: 'I11', 15: 'Ss1s1', 16: 'S11', 17: 'S11', 18: 'F', 19: 'S11', 20: 'S11', 21: 'S1',
               22: 'e', 23: '1', 24: '2', 25: 'v', 26: '22', 27: '', 28: 'v', 29: '', 30: '1b', 31: '1', 32: '1', 33: '',
               34: '1', 35: '1', 36: '1', 37: '1', 38: '', 39: '', 40: '1', 41: '1', 42: '', 43: '1', 44: '1', 45: '',
               46: '', 47: '', 48: '', 49: '1', 50: '', 51: '', 52: '1', 53: 'b', 54: '', 55: '2', 56: 'G', 57: 'b',
               58: '2b', 59: '22b', 60: '22b', 61: 'N1', 62: '', 63: 'b', 64: '2', 65: '1', 66: 'S11', 67: '', 68: ''}
        raw = open(path, 'rb').read()
        p = struct.unpack_from('<I', raw, 0)[0]
        refs = {}
        while True:
            sid, addr = struct.unpack_from('<hH', raw, p)
            p += 4
            if sid == -1:
                break
            highest = struct.unpack_from('<I', raw, addr + 4)[0]
            todo = [a for a in struct.unpack_from('<%dH' % ((highest + 2) & ~1), raw, addr + 8) if a]
            seen, out = set(), []
            while todo:
                pc = todo.pop()
                while pc not in seen:
                    seen.add(pc)
                    op = raw[pc]
                    pc += 1
                    end = False
                    for c in ops[op]:
                        if c in 'ISF2':  # u16 operand: image, sprite, flingy or other
                            v = struct.unpack_from('<H', raw, pc)[0]
                            pc += 2
                            if c != '2':
                                out.append(({'I': 'image', 'S': 'sprite', 'F': 'flingy'}[c], v))
                        elif c == '1':
                            pc += 1
                        elif c == 'G':
                            out.append(('gas', raw[pc]))
                            pc += 1
                        elif c == 'N':
                            out.append(('next', None))
                            pc += 1
                        elif c == 'v':
                            pc += 1 + 2 * raw[pc]
                        elif c == 'b':
                            todo.append(struct.unpack_from('<H', raw, pc)[0])
                            pc += 2
                        elif c == 'j':
                            pc = struct.unpack_from('<H', raw, pc)[0]
                        elif c == 'e':
                            end = True
                    if end:
                        break
            refs[sid] = out
        return refs

    def unit_image(self, u):
        return self.sprite_image[self.flingy_sprite[self.flingy[u]]]

    def image_closure(self, start):
        """start image -> {image id: how it was reached}, following iscript-made images and sprites."""
        found, todo = {}, [(start, 'main')]
        smoke1 = self.image_names.index('Vespene_Geyser_Smoke1')
        while todo:
            i, how = todo.pop()
            if i in found or i >= N_IMAGES:
                continue
            found[i] = how
            for kind, v in self.iscript_refs.get(self.image_iscript[i], []):
                if kind == 'image':
                    todo.append((v, 'iscript'))
                elif kind == 'sprite' and v < N_SPRITES:
                    todo.append((self.sprite_image[v], 'iscript'))
                elif kind == 'flingy' and v < N_FLINGY:
                    todo.append((self.sprite_image[self.flingy_sprite[v]], 'iscript'))
                elif kind == 'next':
                    todo.append((i + 1, 'iscript'))
                elif kind == 'gas':
                    todo += [(smoke1 + v, 'iscript'), (smoke1 + 5 + v, 'iscript')]
        return found


def race_of(group):
    for bit, race in ((1, 'zerg'), (2, 'terran'), (4, 'protoss')):
        if group & bit:
            return race
    return 'neutral'


def safe(s):
    return re.sub(r'[^A-Za-z0-9 ()\'-]+', '_', s).strip()


# --- anim decoding (layout as in src/fp_assets.h) ---

def _rgb565(c):
    c = c.astype(np.uint32)
    return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1)


def _colour(blocks, four_always):
    c0 = blocks[:, 0].astype(np.uint32) | (blocks[:, 1].astype(np.uint32) << 8)
    c1 = blocks[:, 2].astype(np.uint32) | (blocks[:, 3].astype(np.uint32) << 8)
    bits = blocks[:, 4:8].copy().view('<u4')[:, 0]
    p0, p1 = _rgb565(c0).astype(np.int32), _rgb565(c1).astype(np.int32)
    four = (c0 > c1) | four_always
    p2 = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 1)
    idx = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    a = np.where(~four[:, None] & (idx == 3), 0, 255).astype(np.uint8)
    return np.take_along_axis(pal, idx[:, :, None].astype(np.int64), 1).astype(np.uint8), a


def _alpha(blocks):
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
    fmt = bytes(raw[off + 84:off + 88])
    bw, bh = (w + 3) // 4, (h + 3) // 4
    if fmt == b'DXT1':
        rgb, a = _colour(np.frombuffer(raw, np.uint8, bw * bh * 8, off + 128).reshape(-1, 8), False)
    elif fmt == b'DXT5':
        b = np.frombuffer(raw, np.uint8, bw * bh * 16, off + 128).reshape(-1, 16)
        rgb, _ = _colour(b[:, 8:], True)
        a = _alpha(b[:, :8])
    else:
        raise ValueError('unsupported DDS format %r' % fmt)
    px = np.concatenate([rgb, a[:, :, None]], -1).reshape(bh, bw, 4, 4, 4)
    return px.transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)[:h, :w]


def decode_anim(args):
    """anim -> DIR/atlas_LAYER.png for every layer, DIR/frames/NNN.png (diffuse, all frames on one canvas)."""
    src, out = args
    raw = open(src, 'rb').read()
    assert raw[:4] == b'ANIM'
    nlayers, entries = struct.unpack_from('<HH', raw, 8)
    assert entries == 1
    names = [raw[12 + i * 32:44 + i * 32].split(b'\0')[0].decode() for i in range(nlayers)]
    H = 0x14c
    nf, _, box_w, box_h, ft = struct.unpack_from('<HHHHI', raw, H)
    os.makedirs(os.path.join(out, 'frames'), exist_ok=True)
    diffuse, present = None, []
    for L, name in enumerate(names):
        off, sz, tw, th = struct.unpack_from('<IIHH', raw, H + 12 + L * 12)
        if not sz:
            continue
        present.append(name)
        img = decode_dds(raw, off, tw, th)
        if name in ('diffuse', 'bright', 'teamcolor', 'emissive') or img[..., 3].min() < 255:
            Image.fromarray(img, 'RGBA').save(os.path.join(out, 'atlas_%s.png' % name), optimize=False, compress_level=6)
        else:
            Image.fromarray(img[..., :3], 'RGB').save(os.path.join(out, 'atlas_%s.png' % name), compress_level=6)
        if name == 'diffuse':
            diffuse = img
    rects = [struct.unpack_from('<HHhhHH', raw, ft + i * 16) for i in range(nf)]
    # one canvas for all frames: the sprite's box, grown where frames stick out of it (164 anims have such frames)
    left = min([0] + [r[2] for r in rects])
    top = min([0] + [r[3] for r in rects])
    cw = max([box_w] + [r[2] + r[4] for r in rects]) - left
    ch = max([box_h] + [r[3] + r[5] for r in rects]) - top
    frames, written = [], {}
    for i, (x, y, xo, yo, w, h) in enumerate(rects):
        frames.append(dict(x=x, y=y, xoff=xo, yoff=yo, w=w, h=h))
        if diffuse is None:
            continue
        path = os.path.join(out, 'frames', '%03d.png' % i)
        key = (x, y, xo, yo, w, h)
        if key in written:  # repeated frames share one rect in the atlas: hard-link the first one's PNG
            link(written[key], path)
            continue
        written[key] = path
        canvas = np.zeros((ch, cw, 4), np.uint8)
        canvas[yo - top:yo - top + h, xo - left:xo - left + w] = diffuse[y:y + h, x:x + w]
        Image.fromarray(canvas, 'RGBA').save(path, compress_level=6)
    json.dump(dict(box_w=box_w, box_h=box_h, canvas_w=cw, canvas_h=ch, box_x=-left, box_y=-top,
                   layers=present, frames=frames),
              open(os.path.join(out, 'frames.json'), 'w'), indent=0)
    return src


def contact_sheet(out, index, path, cell=128, cols=16):
    """One south-facing frame of every unit folder's main image, with its name."""
    from PIL import ImageDraw
    rows = (len(index) + cols - 1) // cols
    sheet = Image.new('RGBA', (cols * cell, rows * (cell + 12)), (50, 70, 50, 255))
    draw = ImageDraw.Draw(sheet)
    for k, e in enumerate(index):
        d = os.path.join(out, e['folder'], e['images'][0]['file'][:-5])
        n = len(json.load(open(os.path.join(d, 'frames.json')))['frames'])
        im = Image.open(os.path.join(d, 'frames', '%03d.png' % (16 if n > 16 else 0)))
        im = im.crop(im.getbbox() or (0, 0, 1, 1))
        im.thumbnail((cell - 4, cell - 4))
        x, y = (k % cols) * cell, (k // cols) * (cell + 12)
        sheet.alpha_composite(im, (x + (cell - im.width) // 2, y + (cell - im.height) // 2))
        draw.text((x + 2, y + cell), e['name'][:20], fill=(255, 255, 255, 255))
    sheet.convert('RGB').save(path)


def link(src, dst):
    if os.path.exists(dst):
        os.unlink(dst)
    os.link(src, dst)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 3:
        sys.exit(__doc__)
    ex, openbw, out = args
    png = '--no-png' not in sys.argv
    d = Data(ex, openbw)
    anim_dir = os.path.join(ex, 'anim')
    subunits = {s for u in range(N_UNITS) for s in [d.subunit[u]] if s < N_UNITS}

    # one folder per distinct main image; heroes and duplicates sharing it are listed in the index
    by_image = {}
    for u in range(N_UNITS):
        if u in subunits:
            continue
        by_image.setdefault(d.unit_image(u), []).append(u)
    pool = os.path.join(out, '_decoded')
    jobs, index = {}, []
    for img, units in sorted(by_image.items(), key=lambda kv: min(v for v in kv[1] if not d.abilities[v] & 0x40) if any(not d.abilities[v] & 0x40 for v in kv[1]) else kv[1][0]):
        units.sort(key=lambda v: (bool(d.abilities[v] & 0x40), v))  # named after a non-hero when there is one
        u = units[0]
        building = bool(d.abilities[u] & 1)
        race = race_of(d.group[u])
        kind = 'buildings' if building else 'units'
        folder = os.path.join(out, race, kind, '%03d %s' % (u, safe(d.unit_names[u])))
        found = d.image_closure(img)
        found[img] = 'main'
        if d.subunit[u] < N_UNITS:
            for i, how in d.image_closure(d.unit_image(d.subunit[u])).items():
                found.setdefault(i, 'turret' if how == 'main' else 'turret iscript')
        c = d.construction[u]
        if c:
            for i, how in d.image_closure(c).items():
                found.setdefault(i, 'construction' if how == 'main' else 'construction iscript')
        os.makedirs(folder, exist_ok=True)
        files = []
        for i in sorted(found, key=lambda i: (found[i] != 'main', i)):
            a = d.anim_of[i]
            src = os.path.join(anim_dir, 'main_%03d.anim' % a)
            if not os.path.exists(src):
                files.append(dict(image=i, name=d.image_names[i], role=found[i], missing='main_%03d.anim' % a))
                continue
            base = '%03d %s' % (i, safe(d.image_names[i]))
            link(src, os.path.join(folder, base + '.anim'))
            if png:
                jobs[a] = os.path.join(pool, 'main_%03d' % a)
            files.append(dict(image=i, name=d.image_names[i], role=found[i], grp=d.grp_names[d.image_grp[i] - 1],
                              anim='main_%03d.anim' % a, file=base + '.anim'))
        index.append(dict(unit=u, name=d.unit_names[u], race=race, kind=kind, folder=os.path.relpath(folder, out),
                          also=[dict(unit=v, name=d.unit_names[v]) for v in units[1:]],
                          turret=d.unit_names[d.subunit[u]] if d.subunit[u] < N_UNITS else None, images=files))

    if png:
        todo = [(os.path.join(anim_dir, 'main_%03d.anim' % a), o) for a, o in sorted(jobs.items())
                if not os.path.exists(os.path.join(o, 'frames.json'))]
        with ProcessPoolExecutor(max(1, (os.cpu_count() or 2) - 2)) as ex_:
            for n, s in enumerate(ex_.map(decode_anim, todo), 1):
                print('decoded %d/%d %s' % (n, len(todo), os.path.basename(s)), flush=True)
        # each unit folder gets PNG subfolders hard-linked from the pool (decoded once, no extra disk)
        for entry in index:
            folder = os.path.join(out, entry['folder'])
            for f in entry['images']:
                if 'missing' in f:
                    continue
                srcdir = jobs[int(f['anim'][5:8])]
                dst = os.path.join(folder, f['file'][:-5])
                for root, _, names in os.walk(srcdir):
                    rel = os.path.relpath(root, srcdir)
                    os.makedirs(os.path.join(dst, rel), exist_ok=True)
                    for n in names:
                        link(os.path.join(root, n), os.path.join(dst, rel, n))
        contact_sheet(out, index, os.path.join(out, 'overview.png'))

    json.dump(index, open(os.path.join(out, 'index.json'), 'w'), indent=1)
    with open(os.path.join(out, 'README.txt'), 'w') as f:
        f.write('Remastered HD sprites (anim/main_NNN.anim, 4x the classic SD size) of every StarCraft unit type,\n'
                'gathered by tools/hd_units.py from casc-scratch/extracted. Blizzard data: do not commit or upload.\n\n'
                'RACE/units|buildings/UUU Name/  one folder per unit type (UUU = units.dat id). Heroes and other\n'
                '  types drawn with the same sprite are listed under "also" in index.json instead of their own folder.\n'
                '  III Image_Name.anim   images.dat id III: the main image, and every image its iscripts create\n'
                '                        (shadow, birth, death, remnants, overlays), the turret, the construction image\n'
                '  III Image_Name/frames/NNN.png    diffuse frame NNN (the classic GRP\'s frame numbers), all on one\n'
                '                        canvas so frames line up: the sprite\'s box, grown where frames stick out of it\n'
                '                        (box_x/box_y in frames.json). Directional sprites hold each animation frame as\n'
                '                        17 frames in a row, facing north (0) clockwise to south (16); the game mirrors\n'
                '                        them for west. Frames Remastered stores once (e.g. the Dragoon\'s 0-84) repeat.\n'
                '  III Image_Name/atlas_LAYER.png  every layer\'s packed texture (diffuse, bright, teamcolor,\n'
                '                        emissive, normal, specular, ao_depth); frames.json has each frame\'s rect\n'
                '  .anim and .png files are hard links: shared images (explosions, dust) take disk space once.\n'
                '_decoded/main_NNN/     the same PNGs by anim file (a re-run skips the ones already there)\n'
                'overview.png           every folder\'s main image, south-facing\n'
                'index.json             per folder: unit id and name, units drawn the same way ("also"), turret,\n'
                '                       and per image its role (main, iscript, turret, construction), GRP and anim\n\n')
        for e in index:
            f.write('%-44s %s\n' % (e['folder'], ', '.join('%d %s' % (i['image'], i['name']) for i in e['images'])))
    print('units', len(index), 'images', len({f['image'] for e in index for f in e['images']}),
          'missing', sum('missing' in f for e in index for f in e['images']))


if __name__ == '__main__':
    main()
