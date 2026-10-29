"""Draw a unit voxel model (unit_voxel.py .npz) orthographically from any heading and elevation, as sprite frames.

Each surface voxel is splatted as a square covering its cube's footprint on a K-times finer grid, nearest wins, then
box-filtered down to HD pixels (anti-aliased edges, as the original frames have). Placement: the turning axis at
column W/2, the ground (z = 0) at row GROUND, so frames line up as one sprite's frames do.

    eyeview.py check MODEL.npz ANIM OUT.png        model through the sprite camera (35 deg) beside the 17 drawn frames,
                                                   with silhouette IoU and mean colour error per direction
    eyeview.py sheet MODEL.npz OUT.png [ELEV...]   17 directions per row, one row per elevation (default 0 and 35)
    eyeview.py zoom MODEL.npz ANIM OUT.png         5 directions, 3x: the original frame; at 0 and 10 degrees the voxel
                                                   colours, then the frames' own pixels (view-dependent)
Run with the venv python."""
import sys
import numpy as np
from PIL import Image
from views import camera


def load(path):
    m = np.load(path)
    P = m['origin'] + m['idx'].astype(np.float64) * float(m['voxel'])
    return dict(P=P, col=m['col'], normal=m['normal'], elev=float(m['elev']), zshift=float(m['zshift']),
                group=int(m['group']), axis=float(m['axis']),
                c=m['c'] if 'c' in m else None, k=float(m['k']) if 'k' in m else None)


def render(model, heading, elev, W, H, ground, K=4, foot=1.25, axis_col=None):
    """RGBA (H,W,4) uint8 with A = coverage, and the teamcolor mask (H,W) uint8."""
    R = camera(elev, heading)
    P, colours = model['P'], model['col']
    if model.get('faces'):  # one point per voxel face (gazprash_unit.py): only faces turned toward the camera
        front = model['normal'] @ R[2] < 0
        P, colours = P[front], colours[front]
    p = P @ R.T
    u = (p[:, 0] + (W / 2.0 if axis_col is None else axis_col)) * K
    v = (p[:, 1] + ground) * K  # p[:,1] is screen-down from the ground point's row
    w = p[:, 2]
    # facing culling: voxels whose normal points away are behind something nearer anyway; keep all for thin parts
    s = int(np.ceil(foot * K))
    o = np.arange(s) - (s - 1) / 2.0
    Wk, Hk = W * K, H * K
    zb = np.full(Hk * Wk, np.inf)
    keys = []
    for dy in o:
        for dx in o:
            ui = np.floor(u + dx).astype(np.int64)
            vi = np.floor(v + dy).astype(np.int64)
            ok = (ui >= 0) & (vi >= 0) & (ui < Wk) & (vi < Hk)
            lin = vi[ok] * Wk + ui[ok]
            np.minimum.at(zb, lin, w[ok])
            keys.append((lin, np.flatnonzero(ok)))
    owner = np.full(Hk * Wk, -1, np.int64)
    for lin, which in keys:
        hit = w[which] <= zb[lin]
        owner[lin[hit]] = which[hit]
    has = owner >= 0
    col = np.zeros((Hk * Wk, 4), np.float64)
    col[has, :3] = colours[owner[has], :3]
    col[has, 3] = 255.0
    tcm = np.zeros(Hk * Wk)
    tcm[has] = colours[owner[has], 3]
    col = col.reshape(Hk, Wk, 4)
    # box filter K x K; colour averaged over covered subpixels only
    a = col[..., 3].reshape(H, K, W, K).mean((1, 3))
    rgb = (col[..., :3] * (col[..., 3:] / 255.0)).reshape(H, K, W, K, 3).sum((1, 3))
    n = (col[..., 3] / 255.0).reshape(H, K, W, K).sum((1, 3))
    rgb = rgb / np.maximum(n, 1e-6)[..., None]
    tc = (tcm.reshape(Hk, Wk)).reshape(H, K, W, K).sum((1, 3)) / np.maximum(n, 1e-6)
    out = np.concatenate([rgb, a[..., None]], -1)
    return np.clip(out, 0, 255).astype(np.uint8), np.clip(tc, 0, 255).astype(np.uint8)


def _cubic(t):
    """Catmull-Rom weights for the 4 taps at offsets -1, 0, 1, 2 from floor(x), t = x - floor(x)."""
    t2, t3 = t * t, t * t * t
    return np.stack([-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1, -1.5 * t3 + 2 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2], -1)


def interpolate(img, x, y, how):
    """img (h,w,c) at continuous pixel-centre coordinates x, y -> (n,c)."""
    h, w = img.shape[:2]
    if how == 'nearest':
        return img[np.clip(np.floor(y + 0.5).astype(int), 0, h - 1), np.clip(np.floor(x + 0.5).astype(int), 0, w - 1)].astype(np.float64)
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    if how == 'bilinear':
        taps, wx, wy = (0, 1), np.stack([1 - fx, fx], -1), np.stack([1 - fy, fy], -1)
    else:
        taps, wx, wy = (-1, 0, 1, 2), _cubic(fx), _cubic(fy)
    out = np.zeros((len(x), img.shape[2]))
    for j, dy in enumerate(taps):
        yi = np.clip(y0 + dy, 0, h - 1)
        for i, dx in enumerate(taps):
            out += (wx[:, i] * wy[:, j])[:, None] * img[yi, np.clip(x0 + dx, 0, w - 1)]
    if how == 'cubic':  # no overshoot past the 4 nearest pixels (bright specks where dark meets light)
        near = np.stack([img[np.clip(y0 + dy, 0, h - 1), np.clip(x0 + dx, 0, w - 1)] for dy in (0, 1) for dx in (0, 1)])
        out = np.clip(out, near.min(0), near.max(0))
    return out


def _excluded(exclude):
    if exclude is None:
        return set()
    return set(exclude) if hasattr(exclude, '__iter__') else {exclude}


class Sampler:
    """View-dependent texturing from the unit's own frames: the 17 drawn directions of the model's frame group, with
    the model's front depth per direction (what each direction can see)."""

    def __init__(self, model, anim_path, mirrored=False, depth_tol=None):
        from anim import Anim
        from unit_voxel import Shot, front_depth
        from scipy import ndimage
        a = Anim(anim_path)
        self.bw, self.bh = a.box_w, a.box_h
        self.depth_tol = depth_tol or None  # None or 0: no depth agreement test (as before)
        n = 32 if mirrored else 17
        self.shots = [Shot(a, model['group'], d, model['elev'], model['axis']) for d in range(n)]
        # each frame's own depth along the ray (w = c + k D) where the frame's depth layer can be trusted, else nan
        for d, s in enumerate(self.shots):
            s.wsurf = None
            if model.get('c') is not None and model.get('k') is not None and s.depth_ok.any():
                s.wsurf = np.where(s.depth_ok, float(model['c'][d]) + model['k'] * s.D, np.nan)
        self.lift = np.array([0.0, 0.0, model['zshift']])  # npz frame -> the frame the shots project
        S = model['P'] + self.lift
        self.front = []
        for s in self.shots:
            ui, vi, w = s.project(S, self.bw, self.bh)
            self.front.append(ndimage.minimum_filter(front_depth(ui, vi, w, s.mask.shape), 3))
            rgba = s.rgba.astype(np.float32)
            s.pre = np.concatenate([rgba[..., :3] * rgba[..., 3:] / 255.0, rgba[..., 3:], s.tc[..., None].astype(np.float32)
                                    * rgba[..., 3:] / 255.0], -1)  # premultiplied RGB, A, premultiplied team mask

    def agrees(self, s, ui, vi, ws, ok):
        """Where the frame's own depth is known, does the point lie on the surface the frame shows there? (A point the
        model puts a few px off the sprite's real surface otherwise takes the pixel of whatever is in front or behind
        in that frame: red body on a silver shoulder pad.) True where the frame has no depth."""
        out = np.ones(len(ws), bool)
        if s.wsurf is None or self.depth_tol is None:
            return out
        wf = np.full(len(ws), np.nan)
        wf[ok] = s.wsurf[vi[ok], ui[ok]]
        bad = np.isfinite(wf) & (np.abs(ws - wf) > self.depth_tol)
        return out & ~bad

    def primary(self, P, eye_fwd, exclude=None, tol=2.5):
        """The direction nearest eye_fwd, and whether it sees each point."""
        excl = _excluded(exclude)
        angs = [np.degrees(np.arccos(np.clip(eye_fwd @ s.R[2], -1, 1))) if k not in excl else np.inf
                for k, s in enumerate(self.shots)]
        k0 = int(np.argmin(angs))
        s, zf = self.shots[k0], self.front[k0]
        p = (P + self.lift) @ s.R.T
        ui, vi = np.floor(p[:, 0] + s.ax).astype(int), np.floor(p[:, 1] + s.cy).astype(int)
        h, w = zf.shape
        ok = (ui >= 0) & (vi >= 0) & (ui < w) & (vi < h)
        vis = np.zeros(len(P), bool)
        vis[ok] = (p[ok, 2] <= zf[vi[ok], ui[ok]] + tol) & s.mask[vi[ok], ui[ok]]
        vis &= self.agrees(s, ui, vi, p[:, 2], ok)
        return k0, vis

    def sample(self, P, N, eye_fwd, sigma=2.0, interp='cubic', exclude=None, tol=2.5, force=None, k0=None):
        """Colour (n,3), alpha-weight sum and team mask for model points P with normals N seen along eye_fwd.
        interp: 'bilinear', 'cubic' (Catmull-Rom: keeps edges crisp) or 'nearest'; exclude: a direction to leave out
        (to measure how well the others stand in for it; or several). force (with k0, the nearest direction): per point, True
        takes that direction alone (as if it sees the point), False leaves it out (render_vdt decides it per area)."""
        Q = P + self.lift
        acc = np.zeros((len(P), 5))
        wsum = np.zeros(len(P))
        # view weights fall off with the angle beyond the nearest direction's (relative, so they cannot underflow)
        angs = [np.degrees(np.arccos(np.clip(eye_fwd @ s.R[2], -1, 1))) for s in self.shots]
        excl = _excluded(exclude)
        amin = min(a for k, a in enumerate(angs) if k not in excl)
        for k, (s, zf) in enumerate(zip(self.shots, self.front)):
            if k in excl:
                continue
            p = Q @ s.R.T
            us, vs, ws = p[:, 0] + s.ax - 0.5, p[:, 1] + s.cy - 0.5, p[:, 2]  # pixel-centre coordinates
            ui, vi = np.floor(us + 0.5).astype(int), np.floor(vs + 0.5).astype(int)
            h, w = zf.shape
            ok = (ui >= 0) & (vi >= 0) & (ui < w) & (vi < h)
            vis = np.zeros(len(P), bool)
            vis[ok] = (ws[ok] <= zf[vi[ok], ui[ok]] + tol) & s.mask[vi[ok], ui[ok]]
            vis &= self.agrees(s, ui, vi, ws, ok)
            if force is not None:
                vis = force.copy() if k == k0 else vis & ~force
            facing = np.clip(-(N @ s.R[2]), 0, 1)
            wt = np.where(vis, np.exp(-0.5 * ((angs[k] - amin) / sigma) ** 2) * (0.05 + facing), 0.0)
            if s.mirrored:
                wt *= 0.1
            if not wt.any():
                continue
            smp = interpolate(s.pre, us, vs, interp)
            acc += wt[:, None] * smp
            wsum += wt
        a = acc[:, 3]
        rgb = acc[:, :3] / np.maximum(a / 255.0, 1e-6)[:, None]
        tc = acc[:, 4] / np.maximum(a, 1e-6) * 255.0
        return rgb, (a > 1e-6) & (wsum > 0), tc


def render_vdt(model, sampler, heading, elev, W, H, ground, K=4, foot=1.25, axis_col=None, sigma=2.0, interp='cubic',
               centre=True, darken=0.85, exclude=None, relax=16.0, coherent=True, info=None):
    """As `render`, but colours come from the unit's frames (Sampler) at the surface point, found from the splatted
    voxels' depth (smoothed within each surface so the points do not step voxel by voxel).
    centre=False: every subpixel is sampled and the K x K are averaged (softer); centre=True: one sample per output
    pixel, at the centre of its front-most covered subpixels, coverage still from the K x K (sharper).
    Defaults (v2): centre, Catmull-Rom, sigma 2 (the nearest direction's frame wherever it sees the point): through the
    sprite camera 98% of a frame's sharpness and 32.9 dB, 73% with that frame left out (`vdtcheck`). v1 was
    centre=False, interp='bilinear', sigma=6, darken=0.8: 52%, 26.5 dB.
    relax=16 (since the unit batch): a point no frame sees, but at most 16 px behind what one shows, takes that frame's
    pixel instead of the blur (the SCV, drawn from 49 degrees: 53% -> 22% of its eye-level pixels unseen).
    Returns RGBA, the team mask and the share of covered pixels no frame shows (filled from around them).
    info (a dict, centre mode): gets 'filled' (pixels no frame shows, filled from around them) and 'relaxed' (pixels
    taken from a frame that sees a surface just in front), H x W bool, for the clean-up model (refine.py)."""
    from scipy import ndimage
    R = camera(elev, heading)
    ax = W / 2.0 if axis_col is None else axis_col
    p = model['P'] @ R.T
    u, v, w = (p[:, 0] + ax) * K, (p[:, 1] + ground) * K, p[:, 2]
    s = int(np.ceil(foot * K))
    o = np.arange(s) - (s - 1) / 2.0
    Wk, Hk = W * K, H * K
    zb = np.full(Hk * Wk, np.inf)
    keys = []
    for dy in o:
        for dx in o:
            ui = np.floor(u + dx).astype(np.int64)
            vi = np.floor(v + dy).astype(np.int64)
            ok = (ui >= 0) & (vi >= 0) & (ui < Wk) & (vi < Hk)
            lin = vi[ok] * Wk + ui[ok]
            np.minimum.at(zb, lin, w[ok])
            keys.append((lin, np.flatnonzero(ok)))
    owner = np.full(Hk * Wk, -1, np.int64)
    for lin, which in keys:
        hit = w[which] <= zb[lin]
        owner[lin[hit]] = which[hit]
    has = owner >= 0
    Z = zb.reshape(Hk, Wk)
    # edge-aware smoothing of the depth over about one voxel
    r = K
    Zp = np.pad(np.where(has.reshape(Hk, Wk), Z, np.nan), r, constant_values=np.nan)
    acc = np.zeros_like(Z); n = np.zeros_like(Z)
    for i in range(0, 2 * r + 1, 2):
        for j in range(0, 2 * r + 1, 2):
            sh = Zp[i:i + Hk, j:j + Wk]
            ok = np.abs(sh - Z) < 2.5
            acc += np.where(ok, sh, 0); n += ok
    Zs = np.where(n > 0, acc / np.maximum(n, 1), Z)
    cov = has.reshape(H, K, W, K).transpose(0, 2, 1, 3).reshape(H, W, K * K)
    nsum = cov.sum(-1)
    if centre:
        # per output pixel: the subpixels on its front-most surface, their centroid and mean depth
        Zq = np.where(has.reshape(Hk, Wk), Zs, np.inf).reshape(H, K, W, K).transpose(0, 2, 1, 3).reshape(H, W, K * K)
        front = cov & (Zq <= Zq.min(-1, keepdims=True) + 2.5)
        sy, sx = np.divmod(np.arange(K * K), K)
        nf = np.maximum(front.sum(-1), 1)
        cy = (front * (sy + 0.5)).sum(-1) / nf / K
        cx = (front * (sx + 0.5)).sum(-1) / nf / K
        cz = np.where(front, Zq, 0).sum(-1) / nf
        own = owner.reshape(H, K, W, K).transpose(0, 2, 1, 3).reshape(H, W, K * K)
        first = np.argmax(front, -1)
        pix = np.flatnonzero(nsum.ravel() > 0)
        yy, xx = np.divmod(pix, W)
        pc = np.stack([xx + cx.ravel()[pix] - ax, yy + cy.ravel()[pix] - ground, cz.ravel()[pix]], 1)
        vox = own.reshape(-1, K * K)[pix, first.ravel()[pix]]
        shape_out = (H, W)
    else:
        pix = np.flatnonzero(has)
        vv, uu = np.divmod(pix, Wk)
        pc = np.stack([(uu + 0.5) / K - ax, (vv + 0.5) / K - ground, Zs.ravel()[pix]], 1)
        vox = owner[pix]
        shape_out = (Hk, Wk)
    Pw = pc @ R  # back to the model frame
    force = k0 = None
    if coherent and centre:
        # Where the nearest direction sees the surface is decided per area (a 5 x 5 majority over the covered pixels),
        # not pixel by pixel: at the edges of what it sees, pixels flipping between it and its neighbours (lit a little
        # differently, registered a pixel off) made a mosaic (the Firebat's chest, specks on the SCV and the Medic).
        k0, vis0 = sampler.primary(Pw, R[2], exclude)
        vm = np.zeros(shape_out); vm.ravel()[pix] = vis0
        cv = np.zeros(shape_out); cv.ravel()[pix] = 1.0
        maj = ndimage.uniform_filter(vm, 5) > 0.5 * ndimage.uniform_filter(cv, 5)
        force = maj.ravel()[pix]
    rgb, okc, tc = sampler.sample(Pw, model['normal'][vox], R[2], sigma=sigma, interp=interp, exclude=exclude, force=force, k0=k0)
    if relax > 0 and (~okc).any():
        # a point no frame sees, but only just behind what the nearest direction shows there (filler under an arm, a
        # hollow the carving could not reach): that direction's pixel, rather than the blur of the surroundings
        miss = np.flatnonzero(~okc)
        r2, ok2, t2 = sampler.sample(Pw[miss], model['normal'][vox[miss]], R[2], sigma=sigma, interp=interp, exclude=exclude, tol=relax)
        rgb[miss[ok2]], tc[miss[ok2]], okc[miss[ok2]] = r2[ok2], t2[ok2], True
        if info is not None:
            rl = np.zeros(shape_out[0] * shape_out[1], bool); rl[pix[miss[ok2]]] = True
            info['relaxed'] = rl.reshape(shape_out)
    npx = shape_out[0] * shape_out[1]
    col = np.zeros((npx, 3)); tcm = np.zeros(npx)
    fallback = ~okc
    col[pix] = rgb; tcm[pix] = tc
    if fallback.any():
        # Surfaces no frame shows (hidden from above: under the shoulder pads, behind the rifle; or hull filler in
        # hollows no view can carve): filled from the seen pixels around them in this picture (normalized
        # convolution at growing radii, the background never counts), a little darker, as a recess in shadow.
        known = np.zeros(npx); known[pix[okc]] = 1.0
        todo = np.zeros(npx, bool); todo[pix[fallback]] = True
        img = np.concatenate([col, tcm[:, None]], 1).reshape(shape_out + (4,))
        kn = known.reshape(shape_out)
        fill = np.zeros(shape_out + (4,)); got = np.zeros(shape_out, bool)
        scale = 1 if centre else K
        for sig in (1.0, 2.0, 4.0, 8.0, 16.0, 32.0):
            sg = sig * scale / 2
            num = np.stack([ndimage.gaussian_filter(img[..., c] * kn, sg) for c in range(4)], -1)
            den = ndimage.gaussian_filter(kn, sg)
            new = (den > 1e-3) & ~got
            fill[new] = num[new] / den[new][:, None]
            got |= new
        f = fill.reshape(-1, 4)
        col[todo] = f[todo, :3] * darken
        tcm[todo] = f[todo, 3]
    if centre:
        out_rgb, out_tc = col.reshape(H, W, 3), tcm.reshape(H, W)
    else:
        covk = has.astype(np.float64).reshape(H, K, W, K)
        ns = covk.sum((1, 3))
        out_rgb = col.reshape(H, K, W, K, 3).sum((1, 3)) / np.maximum(ns, 1e-6)[..., None]
        out_tc = tcm.reshape(H, K, W, K).sum((1, 3)) / np.maximum(ns, 1e-6)
    out = np.concatenate([out_rgb, (nsum / (K * K) * 255.0)[..., None]], -1)
    if info is not None and centre:
        fl = np.zeros(npx, bool); fl[pix[fallback]] = True
        info['filled'] = fl.reshape(shape_out)
        info.setdefault('relaxed', np.zeros(shape_out, bool))
    return np.clip(out, 0, 255).astype(np.uint8), np.clip(out_tc, 0, 255).astype(np.uint8), fallback.mean()


def sharpness(rgb, mask):
    """Mean absolute Laplacian of the luminance inside the mask (eroded by a pixel)."""
    from scipy import ndimage
    L = rgb[..., :3].astype(np.float64) @ [0.3, 0.59, 0.11]
    m = ndimage.binary_erosion(mask, np.ones((3, 3), bool))
    return np.abs(ndimage.laplace(L))[m].mean()


def on_black(rgba):
    a = rgba[..., 3:4] / 255.0
    return (rgba[..., :3] * a).astype(np.uint8)


if __name__ == '__main__':
    cmd = sys.argv[1]
    model = load(sys.argv[2])
    if cmd == 'check':
        from anim import Anim
        from unit_voxel import Shot
        a = Anim(sys.argv[3])
        rows, stats = [], []
        shots = [Shot(a, model['group'], d, model['elev'], model['axis']) for d in range(17)]
        # the box centre is the model point (0, 0, -zshift): place it there
        gr = a.box_h / 2.0 - np.cos(np.radians(model['elev'])) * model['zshift']
        for s in shots:
            img, _ = render(model, s.heading, model['elev'], a.box_w, a.box_h, gr, axis_col=s.ax)
            m1, m2 = img[..., 3] >= 128, s.mask
            iou = (m1 & m2).sum() / max(1, (m1 | m2).sum())
            both = m1 & m2
            err = np.abs(img[..., :3].astype(int) - s.rgba[..., :3].astype(int))[both].mean()
            stats.append((iou, err))
            ys, xs = np.nonzero(m2)
            y0, y1, x0, x1 = max(0, ys.min() - 4), ys.max() + 5, max(0, xs.min() - 4), xs.max() + 5
            rows.append(np.concatenate([on_black(s.rgba)[y0:y1, x0:x1], on_black(img)[y0:y1, x0:x1]], 0))
        h = max(r.shape[0] for r in rows)
        rows = [np.pad(r, ((0, h - r.shape[0]), (0, 2), (0, 0))) for r in rows]
        Image.fromarray(np.concatenate(rows, 1)).save(sys.argv[4])
        print('ground row %d' % gr)
        print('IoU  ' + ' '.join('%.2f' % s[0] for s in stats))
        print('err  ' + ' '.join('%4.1f' % s[1] for s in stats))
        print('mean IoU %.3f, mean colour error %.1f' % (np.mean([s[0] for s in stats]), np.mean([s[1] for s in stats])))
    elif cmd == 'vdtcheck':
        # the model drawn through the sprite camera against the frames themselves: with every frame available (what
        # the pipeline loses by itself) and with that direction's frame left out (closer to a new view)
        from anim import Anim
        a = Anim(sys.argv[3])
        sampler = Sampler(model, sys.argv[3])
        gr = a.box_h / 2.0 - np.cos(np.radians(model['elev'])) * model['zshift']
        v1 = dict(centre=False, interp='bilinear', sigma=6.0)
        configs = [v1, dict(v1, sigma=2.0), dict(v1, interp='cubic'), dict(v1, centre=True), dict(v1, centre=True, interp='cubic'),
                   dict(centre=True, interp='cubic', sigma=2.0), dict(centre=True, interp='nearest', sigma=2.0)]
        for cfg in configs:
            for loo in (False, True):
                ps, sh = [], []
                for d in range(0, 17, 2):
                    s = sampler.shots[d]
                    img, _, _ = render_vdt(model, sampler, s.heading, model['elev'], a.box_w, a.box_h, gr, axis_col=s.ax,
                                           exclude=d if loo else None, **cfg)
                    m = (img[..., 3] >= 128) & s.mask
                    err = (img[..., :3].astype(float) - s.rgba[..., :3])[m]
                    ps.append(10 * np.log10(255 ** 2 / max(1e-6, (err ** 2).mean())))
                    sh.append(sharpness(img, m) / sharpness(s.rgba, m))
                print('%-55s %s PSNR %.1f dB, sharpness %.2f of the frame' % (cfg,
                      'without its frame' if loo else 'with its frame   ', np.mean(ps), np.mean(sh)), flush=True)
    elif cmd == 'zoom':
        from anim import Anim
        a = Anim(sys.argv[3])
        P = model['P']
        rad = int(np.abs(P[:, :2]).max() + 6)
        top = P[:, 2].max()
        cols = []
        sampler = Sampler(model, sys.argv[3])
        for d in (16, 12, 8, 4, 0):
            fr = on_black(a.frame(model['group'] * 17 + d))
            ys, xs = np.nonzero(fr.max(2) > 0)
            cx = a.box_w / 2.0 + model['axis']
            orig = fr[max(0, ys.min() - 6):ys.max() + 6, int(cx - rad):int(cx + rad)]
            tiles = [orig]
            for e in (0.0, 10.0):
                H = int(top * np.cos(np.radians(e)) + rad * np.sin(np.radians(e)) + 12)
                img, _ = render(model, d * 360.0 / 32, e, 2 * rad, H, int(top * np.cos(np.radians(e)) + 6))
                tiles.append(on_black(img))
                img, _, fb = render_vdt(model, sampler, d * 360.0 / 32, e, 2 * rad, H, int(top * np.cos(np.radians(e)) + 6))
                print('dir %d elev %.0f: %.1f%% of pixels unseen by the frames' % (d, e, 100 * fb))
                tiles.append(on_black(img))
            h = max(t.shape[0] for t in tiles)
            tiles = [np.pad(t, ((h - t.shape[0], 0), (0, 0), (0, 0))) for t in tiles]
            cols.append(np.pad(np.concatenate(tiles, 0), ((0, 0), (0, 4), (0, 0))))
        h = max(c.shape[0] for c in cols)
        img = np.concatenate([np.pad(c, ((0, h - c.shape[0]), (0, 0), (0, 0))) for c in cols], 1)
        Image.fromarray(img).resize((img.shape[1] * 3, img.shape[0] * 3), Image.NEAREST).save(sys.argv[4])
    elif cmd == 'sheet':
        elevs = [float(e) for e in sys.argv[4:]] or [0.0, 35.0]
        P = model['P']
        rad = np.abs(P[:, :2]).max() + 4
        top = P[:, 2].max() + 4
        W = int(2 * rad)
        H = int(top + rad + 8)
        rows = []
        for e in elevs:
            row = []
            for d in range(17):
                img, _ = render(model, d * 360.0 / 32, e, W, H, int(top * np.cos(np.radians(e)) + 4))
                row.append(np.pad(on_black(img), ((0, 0), (0, 2), (0, 0))))
            rows.append(np.concatenate(row, 1))
        Image.fromarray(np.concatenate(rows, 0)).save(sys.argv[3])
