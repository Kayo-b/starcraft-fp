"""An eye-level version of a turning unit's HD sprite, as a drop-in anim file for the game (same frame numbers).

Every directional frame group (17 drawn directions of one pose) whose directions agree as views of one turning model
(unit_voxel.group_iou >= --gate) becomes a voxel model (unit_voxel.py), drawn from ELEV degrees above in the same 17
directions with the frames' own pixels (eyeview.render_vdt). Groups that do not agree (burrowing, deploying, a Dragoon's
body) are copied as they are, or with --relief redrawn from eye level by their own depth (relief_eye.py: each frame on
its own, no turning model); frames that are not directional (deaths) are always copied.
Placement as in the original: the box centre is the same point of the unit (the original box centre's point of the
model), the turning axis as far off the centre column as in the original art, so turrets composed on a hull by the
box centres (the game's renderer) and copied frames line up. The box grows evenly when the eye-level poses need room.
Output: an anim with "diffuse" (DXT5) and "teamcolor" (DXT1) layers, a contact sheet, the voxel models
(OUT_DIR/models/gNN.npz, reused when present) and log.txt.

    eyelevel_anim.py ANIM OUT_DIR [--elev E] [--calib TILT,AXIS] [--no-soles] [--gate G] [--relief [--relief-tidy]] [--jobs J]
--no-soles: for units that fly or hover (their undersides are not soles: unit_voxel.cut_soles).
--floor Z: for a part sitting on another (a turret on its hull, turret_seat.py gives Z): cut below that height instead.
--refine MODEL: clean the rebuilt frames with refine.py's model (trained on renders made without the frame they stand in for).
Run with the venv python. Writes OUT_DIR/main_NNN.anim (same name as the input) and OUT_DIR/sheet.png."""
import argparse
import os
import struct
from multiprocessing import Pool
import numpy as np
from PIL import Image
from anim import Anim
import unit_voxel
import eyeview as ev


def build_group(job):
    path, g, tilt, axis, mdir, soles, floor = job
    npz = os.path.join(mdir, 'g%02d.npz' % g)
    if os.path.exists(npz):  # reused only when built with this calibration
        m = np.load(npz)
        if abs(float(m['elev']) - tilt) > 1e-6 or abs(float(m['axis']) - axis) > 1e-6:
            os.remove(npz)
    if not os.path.exists(npz):
        m = unit_voxel.build(path, g, tilt, axis, log=lambda *a: None, soles=soles, floor=floor)
        np.savez_compressed(npz, **m)
    return npz


def render_group(job):
    path, npz, elev, W, H, axis, depth_tol = job
    model = ev.load(npz)
    sampler = ev.Sampler(model, path, depth_tol=depth_tol)
    ground = H / 2.0 - np.cos(np.radians(elev)) * model['zshift']  # the original box centre's point on the centre row
    out = []
    for d in range(17):
        info = {}
        rgba, tc, _ = ev.render_vdt(model, sampler, d * 360.0 / 32, elev, W, H, ground, axis_col=W / 2.0 + axis, info=info)
        out.append((rgba, tc, info['filled'], info['relaxed']))
    return out


def relief_frame(job):
    path, i, tilt, elev, k, join, tidy = job
    import relief_eye
    relief_eye.JOIN = join
    relief_eye.TIDY = tidy
    from anim import Anim as _A
    a = _A(path)
    rgba, tc, up, un = relief_eye.frame_eye(a, i, tilt, elev, k, unsure=True)
    x0, y0, r, t = crop(rgba, tc)
    # relative to the box centre; the unsure mask in the frame's own crop
    return (x0 - a.box_w / 2.0, y0 - up - a.box_h / 2.0, r, t), un[y0:y0 + r.shape[0], x0:x0 + r.shape[1]]


def extent(npz, elev, axis):
    """Half width and half height the model needs around the box centre, drawn at elev in any direction."""
    m = ev.load(npz)
    P = m['P']
    r = np.hypot(P[:, 0], P[:, 1]).max()
    se, ce = np.sin(np.radians(elev)), np.cos(np.radians(elev))
    v = ce * (P[:, 2] + m['zshift'])
    return r + abs(axis) + 3, max(np.abs(v).max() + r * se, 0) + 3


def crop(rgba, tc):
    ys, xs = np.nonzero(rgba[..., 3] > 0)
    if len(ys) == 0:
        return 0, 0, np.zeros((4, 4, 4), np.uint8), np.zeros((4, 4), np.uint8)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    return x0, y0, rgba[y0:y1, x0:x1], tc[y0:y1, x0:x1]


def pack(tiles, width=2048):
    """Shelf packing on a 4-px grid (DXT blocks); returns positions and the atlas height."""
    pos, x, y, shelf = [], 0, 0, 0
    for t in tiles:
        h, w = t.shape[:2]
        w4, h4 = (w + 3) // 4 * 4 + 4, (h + 3) // 4 * 4 + 4
        if x + w4 > width:
            x, y, shelf = 0, y + shelf, 0
        pos.append((x, y))
        x += w4
        shelf = max(shelf, h4)
    return pos, (y + shelf + 3) // 4 * 4


def bleed(img):
    """Transparent texels take their 4x4 block's mean visible colour, so a sprite's edge blocks spend their two DXT
    endpoints on colours that show (not on the black around the sprite)."""
    h, w = img.shape[:2]
    b = img.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(h // 4, w // 4, 16, 4).astype(np.float32)
    vis = b[..., 3:] > 0
    mean = (b[..., :3] * vis).sum(2, keepdims=True) / np.maximum(vis.sum(2, keepdims=True), 1)
    b[..., :3] = np.where(vis, b[..., :3], mean)
    return b.reshape(h // 4, w // 4, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(h, w, 4).round().astype(np.uint8)


def dds(img, fmt):
    """Atlas -> DDS (128-byte header + DXT5/DXT1 blocks), encoded by quicktex at its best level. Pillow's DXT encoder
    (used before) picks poor block endpoints: 22 dB against quicktex's 29 dB on repainted frames, its errors showing
    as stray coloured 4x4 squares (the 'smeared' pixels)."""
    import quicktex
    from quicktex.s3tc import bc1, bc3
    h, w = img.shape[:2]
    if fmt == 'DXT5':
        img, enc = bleed(img), bc3.BC3Encoder(18)
    else:
        img, enc = np.dstack([img, np.full((h, w), 255, np.uint8)]), bc1.BC1Encoder(18, bc1.BC1Encoder.ColorMode.FourColor)
    blocks = bytes(enc.encode(quicktex.RawTexture.frombytes(np.ascontiguousarray(img).tobytes(), w, h)))
    head = struct.pack('<4s7I44x', b'DDS ', 124, 0x81007, h, w, len(blocks), 0, 0)  # caps|height|width|pixelformat|linearsize
    head += struct.pack('<2I4s5I', 32, 4, fmt.encode(), 0, 0, 0, 0, 0)  # pixel format: FourCC
    head += struct.pack('<4I4x', 0x1000, 0, 0, 0)  # a plain texture
    return head + blocks


def write_anim(path, box_w, box_h, frames, tilt=0.0):
    """frames: list of (xoff, yoff, rgba, teamcolor) in box coordinates. tilt: the original art's camera tilt
    (degrees), kept in the sprite record's second u16 (unused in Blizzard's files) as tenths of a degree: the game
    scales turret offsets by cos(eye level) / cos(tilt)."""
    tiles = [f[2] for f in frames]
    pos, ah = pack(tiles)
    aw = 2048
    atlas = np.zeros((ah, aw, 4), np.uint8)
    team = np.zeros((ah, aw, 3), np.uint8)
    table = b''
    for (xo, yo, rgba, tc), (x, y) in zip(frames, pos):
        h, w = rgba.shape[:2]
        atlas[y:y + h, x:x + w] = rgba
        team[y:y + h, x:x + w] = tc[..., None]
        table += struct.pack('<HHhhHHI', x, y, xo, yo, w, h, 0)  # 16-byte records
    blobs = [dds(atlas, 'DXT5'), dds(team, 'DXT1')]
    names = [b'diffuse', b'teamcolor']
    head = bytearray(b'ANIM') + struct.pack('<HHHH', 0x0202, 0, len(names), 1)
    for n in names:
        head += n.ljust(32, b'\0')
    head += b'\0' * (0x14c - len(head))
    rec_len = 12 + 12 * len(names)
    ft = 0x14c + rec_len
    off = ft + len(table)
    rec = struct.pack('<HHHHI', len(frames), int(round(tilt * 10)), box_w, box_h, ft)
    for blob, (w, h) in zip(blobs, ((aw, ah), (aw, ah))):
        rec += struct.pack('<IIHH', off, len(blob), w, h)
        off += len(blob)
    with open(path, 'wb') as f:
        f.write(bytes(head) + rec + table + b''.join(blobs))


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('anim')
    ap.add_argument('out')
    ap.add_argument('--elev', type=float, default=5.0)
    ap.add_argument('--calib')
    ap.add_argument('--no-soles', action='store_true')
    ap.add_argument('--gate', type=float, default=0.9)
    ap.add_argument('--relief', action='store_true')
    ap.add_argument('--depth-tol', type=float, default=6.0,
                    help="a frame's pixel only for points within this many px of the surface its depth layer shows (0: off)")
    ap.add_argument('--refine', help='clean-up model (refine.py) applied to the rebuilt frames')
    ap.add_argument('--relief-join', type=float, default=10.0, help='relief_eye.JOIN (the Vulture: 4, its red underside drips at 10)')
    ap.add_argument('--relief-tidy', action='store_true', help='relief_eye.TIDY: one-piece frames stay one piece (the Lurker)')
    ap.add_argument('--floor', type=float, help='cut below this height (HD px above the box centre point): a turret on its hull')
    ap.add_argument('--jobs', type=int, default=6)
    o = ap.parse_args()
    a = Anim(o.anim)
    os.makedirs(os.path.join(o.out, 'models'), exist_ok=True)
    logf = open(os.path.join(o.out, 'log.txt'), 'w')
    def log(*x):
        line = ' '.join(str(v) for v in x)
        print(line, flush=True); logf.write(line + '\n'); logf.flush()
    if o.calib:
        tilt, axis = map(float, o.calib.split(','))
    else:
        tilt, axis = unit_voxel.calibrate(a, 0, log)
    ngroups = len(a.frames) // 17
    ious = [unit_voxel.group_iou(a, g, tilt, axis) for g in range(ngroups)]
    rebuilt = [g for g in range(ngroups) if ious[g] >= o.gate]
    log('%s: %d frames, %d directional groups, tilt %.1f, axis %+.1f' % (os.path.basename(o.anim), len(a.frames), ngroups, tilt, axis))
    log('group hull IoU: ' + ' '.join('%d:%.2f%s' % (g, v, '' if v >= o.gate else '(kept)') for g, v in enumerate(ious)))
    with Pool(o.jobs) as p:
        npzs = dict(zip(rebuilt, p.map(build_group, [(o.anim, g, tilt, axis, os.path.join(o.out, 'models'), not o.no_soles, o.floor) for g in rebuilt])))
    # the render canvas: the original box, grown evenly where the eye-level poses need more room
    hw, hh = a.box_w / 2.0, a.box_h / 2.0
    for g in rebuilt:
        ew, eh = extent(npzs[g], o.elev, axis)
        hw, hh = max(hw, ew), max(hh, eh)
    padw, padh = int(np.ceil(hw - a.box_w / 2.0)), int(np.ceil(hh - a.box_h / 2.0))
    W, H = a.box_w + 2 * padw, a.box_h + 2 * padh
    with Pool(o.jobs) as p:
        rendered = dict(zip(rebuilt, p.map(render_group, [(o.anim, npzs[g], o.elev, W, H, axis, o.depth_tol) for g in rebuilt])))
    if o.refine and rendered:  # the clean-up model, on the GPU in this process
        from refine import Refiner
        net = Refiner(o.refine)
        rendered = {g: [(net(rgba, fl, rl), tc, fl, rl) for rgba, tc, fl, rl in fr] for g, fr in rendered.items()}
        log('cleaned with %s' % o.refine)
    tcl = 'teamcolor' in a.layers
    # every frame placed relative to the box centre (x, y of its top-left), the box sized to fit them all at the end
    def original(i):
        x, y, xo, yo, w, h = a.frames[i]
        rgba = a.frame(i, 'diffuse', boxed=False)
        tc = a.frame(i, 'teamcolor', boxed=False)[..., 0] if tcl else np.zeros((h, w), np.uint8)
        return (xo - a.box_w / 2.0, yo - a.box_h / 2.0, rgba, tc)
    others = [i for i in range(ngroups * 17) if i // 17 not in rendered]  # kept directional groups (deaths stay as they are)
    relief = {}
    if o.relief and others:
        ks = [float(np.load(npzs[g])['k']) for g in rebuilt]
        k = float(np.median(ks)) if ks else -1.3
        with Pool(o.jobs) as p:
            relief = dict(zip(others, p.map(relief_frame, [(o.anim, i, tilt, o.elev, k, o.relief_join, o.relief_tidy) for i in others])))
        log('%d frames redrawn by their own depth (k %.2f)' % (len(others), k))
    placed, masks = [], {}
    for i in range(len(a.frames)):
        g, d = divmod(i, 17)
        if g in rendered:
            rgba, tc, fl, rl = rendered[g][d]
            x0, y0, r, t = crop(rgba, tc)
            placed.append((x0 - W / 2.0, y0 - H / 2.0, r, t))
            # where the frame is unsure (no frame shows the surface, or a pixel borrowed from one just in front), in the
            # frame's own crop: for repaint.py, which repaints those areas more freely
            masks['f%d' % i] = (fl | rl)[y0:y0 + r.shape[0], x0:x0 + r.shape[1]]
        elif i in relief:
            placed.append(relief[i][0])
            masks['f%d' % i] = relief[i][1]  # stretched surfaces and filled holes
        else:
            placed.append(original(i))
    # the box keeps the original's parity, so the centre stays on the same half-pixel
    ex = max(max(-x, x + r.shape[1]) for x, y, r, t in placed)
    ey = max(max(-y, y + r.shape[0]) for x, y, r, t in placed)
    BW = a.box_w + 2 * int(np.ceil(max(0.0, ex - a.box_w / 2.0)))
    BH = a.box_h + 2 * int(np.ceil(max(0.0, ey - a.box_h / 2.0)))
    frames = [(int(round(x + BW / 2.0)), int(round(y + BH / 2.0)), r, t) for x, y, r, t in placed]
    W, H = BW, BH
    name = os.path.basename(o.anim)
    write_anim(os.path.join(o.out, name), W, H, frames, tilt)
    if masks:
        np.savez_compressed(os.path.join(o.out, os.path.splitext(name)[0] + '.masks.npz'), **masks)
    check = Anim(os.path.join(o.out, name))
    # contact sheet: every directional group's 17 directions as written (rebuilt, relief or kept), cropped to what is drawn
    if ngroups:
        imgs = [[ev.on_black(check.frame(g * 17 + d)) for d in range(17)] for g in range(ngroups)]
        alpha = np.zeros(imgs[0][0].shape[:2], bool)
        for row in imgs:
            for im in row:
                alpha |= im.max(2) > 0
        ys, xs = np.nonzero(alpha)
        rows = [np.concatenate([im[ys.min():ys.max() + 1, xs.min():xs.max() + 1] for im in row], 1) for row in imgs]
        Image.fromarray(np.concatenate(rows, 0)).save(os.path.join(o.out, 'sheet.png'))
    log('%s: %d frames (%d groups rebuilt, %d %s, %d single frames), box %dx%d (original %dx%d), eye-level at %.0f deg'
        % (name, len(check.frames), len(rebuilt), ngroups - len(rebuilt), 'by their own depth' if relief else 'kept',
           len(a.frames) - 17 * ngroups, check.box_w, check.box_h, a.box_w, a.box_h, o.elev))
