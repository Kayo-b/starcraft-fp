"""Clean-up model for eye-level frames: a small U-Net that mends a render of a unit's voxel model in the unit's own art.

Trained on refine_data.py's pairs (renders through the original camera made without the frame they stand in for,
against that frame). Input: the render's RGB, coverage, and where no frame showed the surface (filled from around it)
or a frame showed one just in front (borrowed); output: a correction added to the RGB (the outline stays the render's).
Loss: L1 and L1 of the image gradients against the frame, inside both outlines, filled and borrowed pixels weighted
double. 128-px patches (so it mends locally, not by recalling whole poses); directions 3 and 11 of every unit held
out for validation.

    refine.py train PAIRS.npz OUT.pt [ITERS] [--gan]
--gan (v2): plus a conditional PatchGAN critic (hinge loss, weight 0.03 after 2000 steps), so where the answer is
uncertain (big unseen areas) it paints texture in the art's style rather than the average (v1: L1 alone, which blurs).
    refine.py preview OUT.pt PAIRS.npz PREVIEW.png       held-out pairs: render | cleaned | frame
Use from code: Refiner(OUT.pt)(rgba, filled, relaxed) -> rgba. Run with the venv python (torch, CUDA)."""
import sys
import time
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

HOLDOUT = {3, 11}


def block(i, o):
    return nn.Sequential(nn.Conv2d(i, o, 3, padding=1), nn.GroupNorm(8, o), nn.SiLU(),
                         nn.Conv2d(o, o, 3, padding=1), nn.GroupNorm(8, o), nn.SiLU())


class UNet(nn.Module):
    def __init__(self, cin=6, base=32):
        super().__init__()
        ch = [base, base * 2, base * 4, base * 8]
        self.enc = nn.ModuleList([block(cin, ch[0])] + [block(ch[k], ch[k + 1]) for k in range(3)])
        self.mid = block(ch[3], ch[3])
        self.up = nn.ModuleList([nn.ConvTranspose2d(ch[k + 1], ch[k], 2, stride=2) for k in range(3)][::-1])
        self.dec = nn.ModuleList([block(ch[k] * 2, ch[k]) for k in range(3)][::-1])
        self.out = nn.Conv2d(ch[0], 3, 1)
        nn.init.zeros_(self.out.weight); nn.init.zeros_(self.out.bias)  # starts as "leave the render as it is"

    def forward(self, x):
        skips = []
        h = x
        for k, e in enumerate(self.enc):
            h = e(h)
            if k < 3:
                skips.append(h)
                h = F.max_pool2d(h, 2)
        h = self.mid(h)
        for up, dec in zip(self.up, self.dec):
            h = up(h)
            h = dec(torch.cat([h, skips.pop()], 1))
        return (x[:, :3] + self.out(h)).clamp(0, 1)


class Critic(nn.Module):
    """Conditional PatchGAN: input channels (6) + RGB (3) -> a realness score per ~34-px patch."""
    def __init__(self, base=48):
        super().__init__()
        sn = nn.utils.spectral_norm
        self.net = nn.Sequential(
            sn(nn.Conv2d(9, base, 4, 2, 1)), nn.LeakyReLU(0.2),
            sn(nn.Conv2d(base, base * 2, 4, 2, 1)), nn.LeakyReLU(0.2),
            sn(nn.Conv2d(base * 2, base * 4, 4, 2, 1)), nn.LeakyReLU(0.2),
            sn(nn.Conv2d(base * 4, base * 4, 3, 1, 1)), nn.LeakyReLU(0.2),
            sn(nn.Conv2d(base * 4, 1, 3, 1, 1)))

    def forward(self, x, rgb):
        return self.net(torch.cat([x, rgb * x[:, 3:4]], 1))


def to_tensor(inp):
    """inp (h,w,6) uint8 -> (6,h,w) float: RGB premultiplied by coverage, coverage, filled, borrowed."""
    x = inp.astype(np.float32) / 255.0
    x[..., :3] *= x[..., 3:4]
    return torch.from_numpy(x).permute(2, 0, 1)


def patches(samples, n, size, rng):
    """n random size x size crops (padded where a sample is smaller), weighted toward bigger samples."""
    w = np.array([s['inp'].shape[0] * s['inp'].shape[1] for s in samples], np.float64)
    pick = rng.choice(len(samples), n, p=w / w.sum())
    X = np.zeros((n, 6, size, size), np.float32); Y = np.zeros((n, 4, size, size), np.float32)
    for j, i in enumerate(pick):
        s = samples[i]
        h, wd = s['inp'].shape[:2]
        for _ in range(8):  # a crop that holds some of the unit
            y0 = rng.integers(0, max(1, h - size + 1)); x0 = rng.integers(0, max(1, wd - size + 1))
            t = s['tgt'][y0:y0 + size, x0:x0 + size]
            if (t[..., 3] > 127).mean() > 0.15:
                break
        a = s['inp'][y0:y0 + size, x0:x0 + size]
        X[j, :, :a.shape[0], :a.shape[1]] = to_tensor(a).numpy()
        tt = t.astype(np.float32) / 255.0
        tt[..., :3] *= tt[..., 3:4]
        Y[j, :, :tt.shape[0], :tt.shape[1]] = tt.transpose(2, 0, 1)
    return torch.from_numpy(X), torch.from_numpy(Y)


def loss_fn(out, x, y):
    m = ((x[:, 3:4] > 0.5) & (y[:, 3:4] > 0.5)).float()
    w = m * (1 + x[:, 4:5] + x[:, 5:6])  # filled and borrowed pixels count double
    l1 = (w * (out - y[:, :3]).abs()).sum() / (3 * w.sum() + 1e-6)
    def grad(t):
        return t[..., 1:, :] - t[..., :-1, :], t[..., :, 1:] - t[..., :, :-1]
    (oy, ox), (ty, tx) = grad(out), grad(y[:, :3])
    gy = (w[..., 1:, :] * (oy - ty).abs()).sum() / (3 * w[..., 1:, :].sum() + 1e-6)
    gx = (w[..., :, 1:] * (ox - tx).abs()).sum() / (3 * w[..., :, 1:].sum() + 1e-6)
    return l1 + 0.5 * (gx + gy)


def psnr(out, y, m):
    e = ((out - y) ** 2 * m).sum() / (3 * m.sum() + 1e-6)
    return float(10 * torch.log10(1 / (e + 1e-12)))


def load(path):
    samples = list(np.load(path, allow_pickle=True)['samples'])
    tr = [s for s in samples if s['d'] not in HOLDOUT]
    va = [s for s in samples if s['d'] in HOLDOUT]
    return tr, va


def train(pairs, out, iters=20000, size=128, batch=12, gan=False):
    dev = 'cuda'
    tr, va = load(pairs)
    print('%d training pairs, %d held out (directions %s)' % (len(tr), len(va), sorted(HOLDOUT)), flush=True)
    rng = np.random.default_rng(0)
    vx, vy = patches(va, 96, size, np.random.default_rng(1))
    net = UNet().to(dev)
    print('%.1fM parameters' % (sum(p.numel() for p in net.parameters()) / 1e6), flush=True)
    opt = torch.optim.AdamW(net.parameters(), 2e-4, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, 2e-4, total_steps=iters, pct_start=0.05)
    scaler = torch.amp.GradScaler()
    if gan:
        crit = Critic().to(dev)
        optc = torch.optim.AdamW(crit.parameters(), 2e-4, betas=(0.5, 0.99))
    best, t0 = 1e9, time.time()
    for it in range(1, iters + 1):
        x, y = patches(tr, batch, size, rng)
        x, y = x.to(dev), y.to(dev)
        with torch.autocast('cuda', dtype=torch.float16):
            o = net(x)
        loss = loss_fn(o.float(), x, y)
        if gan and it > 2000:
            # critic: real frames vs cleaned renders, hinge loss (fp32: spectral norm and fp16 do not mix well)
            lc = F.relu(1 - crit(x, y[:, :3])).mean() + F.relu(1 + crit(x, o.detach().float())).mean()
            optc.zero_grad(set_to_none=True); lc.backward(); optc.step()
            loss = loss + 0.03 * (-crit(x, o.float()).mean())
        opt.zero_grad(set_to_none=True)
        scaler.scale(loss).backward()
        scaler.step(opt); scaler.update(); sched.step()
        if it % 1000 == 0 or it == iters:
            net.eval()
            with torch.no_grad():
                vl, pr, pn = 0.0, 0.0, 0.0
                for k in range(0, len(vx), 16):
                    xx, yy = vx[k:k + 16].to(dev), vy[k:k + 16].to(dev)
                    oo = net(xx)
                    m = ((xx[:, 3:4] > 0.5) & (yy[:, 3:4] > 0.5)).float()
                    vl += float(loss_fn(oo, xx, yy)); pr += psnr(xx[:, :3], yy[:, :3], m); pn += psnr(oo, yy[:, :3], m)
                n = (len(vx) + 15) // 16
            net.train()
            print('it %d  train %.4f  held-out loss %.4f  PSNR render %.2f -> cleaned %.2f dB  (%.0f s)'
                  % (it, float(loss), vl / n, pr / n, pn / n, time.time() - t0), flush=True)
            if gan:  # the adversarial model trades a little L1 for texture: keep the last, not the lowest loss
                torch.save(net.state_dict(), out)
            elif vl / n < best:
                best = vl / n
                torch.save(net.state_dict(), out)
    print('best held-out loss %.4f -> %s' % (best, out))


class Refiner:
    def __init__(self, path, dev=None):
        import os
        dev = dev or os.environ.get('REFINE_DEVICE') or ('cuda' if torch.cuda.is_available() else 'cpu')
        self.dev = dev
        self.net = UNet().to(dev)
        self.net.load_state_dict(torch.load(path, map_location=dev))
        self.net.eval()

    @torch.no_grad()
    def __call__(self, rgba, filled, relaxed):
        """rgba (h,w,4) uint8 render, filled/relaxed (h,w) bool -> rgba with the RGB cleaned (coverage unchanged)."""
        h, w = rgba.shape[:2]
        inp = np.concatenate([rgba, filled[..., None].astype(np.uint8) * 255, relaxed[..., None].astype(np.uint8) * 255], -1)
        H, W = (h + 15) // 16 * 16, (w + 15) // 16 * 16
        x = torch.zeros((1, 6, H, W))
        x[0, :, :h, :w] = to_tensor(inp)
        o = self.net(x.to(self.dev))[0, :, :h, :w].permute(1, 2, 0).cpu().numpy()
        a = rgba[..., 3:4].astype(np.float32) / 255.0
        rgb = np.where(a > 0, o / np.maximum(a, 1e-6), 0) * 255.0  # back from premultiplied
        return np.concatenate([np.clip(rgb, 0, 255).astype(np.uint8), rgba[..., 3:4]], -1)


def preview(model, pairs, out, n=12):
    from PIL import Image
    _, va = load(pairs)
    rng = np.random.default_rng(2)
    r = Refiner(model)
    rows = []
    for i in rng.choice(len(va), min(n, len(va)), replace=False):
        s = va[i]
        rgba = s['inp'][..., :4]
        c = r(rgba, s['inp'][..., 4] > 127, s['inp'][..., 5] > 127)
        tiles = []
        for im in (rgba, c, s['tgt']):
            al = im[..., 3:4] / 255.0
            tiles.append((im[..., :3] * al).astype(np.uint8))
        rows.append(np.concatenate([np.pad(t, ((0, 2), (0, 2), (0, 0))) for t in tiles], 1))
    w = max(x.shape[1] for x in rows)
    img = np.concatenate([np.pad(x, ((0, 0), (0, w - x.shape[1]), (0, 0))) for x in rows], 0)
    Image.fromarray(img).save(out)


if __name__ == '__main__':
    if sys.argv[1] == 'train':
        args = [a for a in sys.argv[2:] if not a.startswith('--')]
        train(args[0], args[1], int(args[2]) if len(args) > 2 else 20000, gan='--gan' in sys.argv)
    elif sys.argv[1] == 'preview':
        preview(sys.argv[2], sys.argv[3], sys.argv[4])
