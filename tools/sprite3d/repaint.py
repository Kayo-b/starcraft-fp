"""Generative repaint of eye-level frames (GPU; written for a Kaggle T4): Stable Diffusion 1.5 with a LoRA trained on
the unit's own Remastered frames, run img2img with ControlNet-tile on each eye-level render (eyelevel_anim.py), so the
render's structure stays and the model repaints its texture in the unit's look. The result goes back to sprite size
inside the render's own outline (coverage and team mask unchanged).

    repaint.py pilot ORIG.anim EYE.anim OUT_DIR --token sc_firebat --desc "..."    a few frames x settings -> pilot.png
    repaint.py full ORIG.anim EYE.anim OUT_DIR --token ... --strength S            every rebuilt frame -> OUT_DIR/NAME.anim
Options: --steps N (LoRA training steps, default 1200), --lora DIR (reuse trained weights), --groups G,G (full: only these),
--masks NAME.masks.npz (eyelevel_anim.py: where the render is unsure): two passes, the whole frame at --strength, then
the unsure areas (grown, soft-edged) inpainted again at --mask-strength. With --strength 0 only the unsure areas are
inpainted (from the render) and pasted back over it: every other pixel stays the render's, frames without any unsure
pixel are left as they are; the masks are made solid first (solid()).
Needs torch, diffusers, transformers, peft (pip). Model weights from the Hugging Face hub."""
import argparse
import os
import sys
import time
import numpy as np
from PIL import Image

T0 = time.time()
BASE = 'stable-diffusion-v1-5/stable-diffusion-v1-5'
TILE = 'lllyasviel/control_v11f1e_sd15_tile'
SIZE = 512


def log(*x):
    print('[%5.0fs]' % (time.time() - T0), *x, flush=True)


# ---- frames <-> 512 canvases ----

def to_canvas(rgba, fill=0.86):
    """RGBA frame -> 512x512 RGB on black (premultiplied), and the transform back."""
    a = rgba[..., 3]
    ys, xs = np.nonzero(a > 0)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    crop = rgba[y0:y1, x0:x1].astype(np.float32)
    rgb = crop[..., :3] * crop[..., 3:] / 255.0
    h, w = rgb.shape[:2]
    s = SIZE * fill / max(h, w)
    W, H = max(1, round(w * s)), max(1, round(h * s))
    im = Image.fromarray(rgb.astype(np.uint8)).resize((W, H), Image.LANCZOS)
    canvas = Image.new('RGB', (SIZE, SIZE))
    ox, oy = (SIZE - W) // 2, (SIZE - H) // 2
    canvas.paste(im, (ox, oy))
    return canvas, (y0, y1, x0, x1, ox, oy, W, H)


def mask_canvas(mask, tf, grow=2, soft=6):
    """A frame-crop mask -> 512 canvas mask (PIL L), grown by `grow` sprite px and soft-edged."""
    from scipy import ndimage
    y0, y1, x0, x1, ox, oy, W, H = tf
    m = ndimage.binary_dilation(mask, iterations=grow) if grow else mask
    m = m[y0:y1, x0:x1].astype(np.uint8) * 255
    im = Image.fromarray(m).resize((W, H), Image.BILINEAR)
    canvas = Image.new('L', (SIZE, SIZE))
    canvas.paste(im, (ox, oy))
    from PIL import ImageFilter
    return canvas.filter(ImageFilter.GaussianBlur(soft))


def solid(masks):
    """Unsure masks as solid regions (closed then opened, 3x3): a relief frame's stretch mask comes in row stripes."""
    from scipy import ndimage
    k = np.ones((3, 3), bool)
    return {f: ndimage.binary_opening(ndimage.binary_closing(masks[f], k), k) for f in masks.files}


def paste(new, rgba, mask):
    """Mask-only repaint: the repainted frame only on the unsure pixels and a 1 px soft ring around them, the render's
    own pixels everywhere else (the round trip through the 512 canvas would soften them)."""
    from scipy import ndimage
    w = np.clip(ndimage.gaussian_filter(ndimage.binary_dilation(mask, iterations=1).astype(np.float32), 0.7) * 1.5, 0, 1)
    out = new.copy()
    out[..., :3] = (new[..., :3] * w[..., None] + rgba[..., :3] * (1 - w[..., None])).round().astype(np.uint8)
    return out


def from_canvas(canvas, tf, rgba):
    """Repainted canvas -> the frame's RGB at its own size; alpha (coverage) stays the render's."""
    y0, y1, x0, x1, ox, oy, W, H = tf
    region = canvas.crop((ox, oy, ox + W, oy + H)).resize((x1 - x0, y1 - y0), Image.LANCZOS)
    out = rgba.copy()
    a = rgba[y0:y1, x0:x1, 3:4].astype(np.float32) / 255.0
    rgb = np.asarray(region).astype(np.float32)
    # back from premultiplied on black (edge pixels with partial coverage)
    out[y0:y1, x0:x1, :3] = np.where(a > 0.02, np.clip(rgb / np.maximum(a, 0.02), 0, 255), out[y0:y1, x0:x1, :3]).astype(np.uint8)
    return out


# ---- LoRA on the unit's frames ----

def train_lora(frames, prompt, steps, out_dir, dev='cuda'):
    import torch
    import torch.nn.functional as F
    from diffusers import AutoencoderKL, UNet2DConditionModel, DDPMScheduler
    from transformers import CLIPTextModel, CLIPTokenizer
    from peft import LoraConfig
    from peft.utils import get_peft_model_state_dict
    tok = CLIPTokenizer.from_pretrained(BASE, subfolder='tokenizer')
    te = CLIPTextModel.from_pretrained(BASE, subfolder='text_encoder', torch_dtype=torch.float16).to(dev)
    vae = AutoencoderKL.from_pretrained(BASE, subfolder='vae', torch_dtype=torch.float32).to(dev)
    unet = UNet2DConditionModel.from_pretrained(BASE, subfolder='unet', torch_dtype=torch.float16).to(dev)
    sched = DDPMScheduler.from_pretrained(BASE, subfolder='scheduler')
    for m in (te, vae, unet):
        m.requires_grad_(False)
    unet.add_adapter(LoraConfig(r=16, lora_alpha=16, init_lora_weights='gaussian',
                                target_modules=['to_k', 'to_q', 'to_v', 'to_out.0']))
    params = [p for p in unet.parameters() if p.requires_grad]
    for p in params:
        p.data = p.data.float()
    unet.enable_gradient_checkpointing()
    with torch.no_grad():
        ids = tok([prompt], padding='max_length', max_length=tok.model_max_length, truncation=True, return_tensors='pt').input_ids.to(dev)
        emb = te(ids)[0]
        lat = []
        for k in range(0, len(frames), 8):
            x = torch.from_numpy(np.stack([np.asarray(f) for f in frames[k:k + 8]])).permute(0, 3, 1, 2).float().to(dev) / 127.5 - 1
            lat.append(vae.encode(x).latent_dist.sample() * vae.config.scaling_factor)
        lat = torch.cat(lat)
    del vae
    log('LoRA: %d training images, %d steps' % (len(lat), steps))
    opt = torch.optim.AdamW(params, lr=1e-4, weight_decay=1e-2)
    scaler = torch.amp.GradScaler()
    g = torch.Generator(dev).manual_seed(0)
    bs = 4
    for step in range(1, steps + 1):
        idx = torch.randint(0, len(lat), (bs,), generator=g, device=dev)
        x0 = lat[idx]
        noise = torch.randn(x0.shape, generator=g, device=dev)
        t = torch.randint(0, sched.config.num_train_timesteps, (bs,), generator=g, device=dev)
        xt = sched.add_noise(x0, noise, t)
        with torch.autocast('cuda', dtype=torch.float16):
            pred = unet(xt.half(), t, emb.expand(bs, -1, -1)).sample
        loss = F.mse_loss(pred.float(), noise)
        opt.zero_grad(set_to_none=True)
        scaler.scale(loss).backward()
        scaler.step(opt)
        scaler.update()
        if step % 200 == 0:
            log('LoRA step %d loss %.4f' % (step, float(loss)))
    os.makedirs(out_dir, exist_ok=True)
    torch.save(get_peft_model_state_dict(unet), os.path.join(out_dir, 'lora_unet.pt'))
    return unet, te, tok


def pipeline(unet=None, te=None, tok=None, lora_dir=None, dev='cuda'):
    import torch
    from diffusers import StableDiffusionControlNetImg2ImgPipeline, ControlNetModel, UniPCMultistepScheduler
    cn = ControlNetModel.from_pretrained(TILE, torch_dtype=torch.float16)
    kw = {}
    if unet is not None:
        kw = dict(unet=unet, text_encoder=te, tokenizer=tok)
    pipe = StableDiffusionControlNetImg2ImgPipeline.from_pretrained(BASE, controlnet=cn, torch_dtype=torch.float16,
                                                                    safety_checker=None, **kw).to(dev)
    if unet is None and lora_dir:
        from peft import LoraConfig, set_peft_model_state_dict
        pipe.unet.add_adapter(LoraConfig(r=16, lora_alpha=16, target_modules=['to_k', 'to_q', 'to_v', 'to_out.0']))
        set_peft_model_state_dict(pipe.unet, torch.load(os.path.join(lora_dir, 'lora_unet.pt')))
    pipe.unet.half()
    pipe.scheduler = UniPCMultistepScheduler.from_config(pipe.scheduler.config)
    pipe.set_progress_bar_config(disable=True)
    return pipe


def repaint_masked(pipe, canvas, mask, prompt, strength, mask_strength, seed=0, cn_scale=1.0, mask_cn=0.6, steps=30):
    """The whole frame at `strength`, then the masked (unsure) areas inpainted from that at `mask_strength`, with the
    first pass as the tile control (the render's blur there would hold the second pass back). Strength 0: no first
    pass (the caller pastes the result back with paste())."""
    import torch
    from diffusers import StableDiffusionControlNetInpaintPipeline
    first = repaint(pipe, canvas, prompt, strength, seed=seed, cn_scale=cn_scale, steps=steps) if strength > 0 else canvas
    if not hasattr(pipe, '_inpaint'):
        pipe._inpaint = StableDiffusionControlNetInpaintPipeline(**pipe.components)
        pipe._inpaint.set_progress_bar_config(disable=True)
    g = torch.Generator('cuda').manual_seed(seed + 1)
    return pipe._inpaint(prompt=prompt, negative_prompt='blurry, noisy, low quality, deformed, extra limbs, text, watermark',
                         image=first, mask_image=mask, control_image=first, strength=mask_strength,
                         num_inference_steps=steps, guidance_scale=6.0, controlnet_conditioning_scale=mask_cn,
                         generator=g).images[0]


def repaint(pipe, canvas, prompt, strength, seed=0, cn_scale=1.0, lora_scale=1.0, steps=30):
    import torch
    g = torch.Generator('cuda').manual_seed(seed)
    if lora_scale == 0:  # the base model alone (a scale of 0 would divide by zero when the scale is undone)
        pipe.unet.disable_adapters()
    try:
        return pipe(prompt=prompt, negative_prompt='blurry, noisy, low quality, deformed, extra limbs, text, watermark',
                    image=canvas, control_image=canvas, strength=strength, num_inference_steps=steps, guidance_scale=6.0,
                    controlnet_conditioning_scale=cn_scale, generator=g).images[0]
    finally:
        if lora_scale == 0:
            pipe.unet.enable_adapters()


def training_frames(orig):
    """Every frame of the original sprite on a canvas, plus its mirror image (the game mirrors directions too)."""
    out = []
    for i in range(len(orig.frames)):
        rgba = orig.frame(i)
        if (rgba[..., 3] > 0).sum() < 50:
            continue
        c, _ = to_canvas(rgba)
        out += [c, c.transpose(Image.FLIP_LEFT_RIGHT)]
    return out


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from anim import Anim
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=['pilot', 'full'])
    ap.add_argument('orig')
    ap.add_argument('eye')
    ap.add_argument('out')
    ap.add_argument('--token', required=True)
    ap.add_argument('--desc', default='')
    ap.add_argument('--steps', type=int, default=1200)
    ap.add_argument('--lora')
    ap.add_argument('--strength', type=float, default=0.4)
    ap.add_argument('--cn', type=float, default=1.0)
    ap.add_argument('--groups')
    ap.add_argument('--masks')
    ap.add_argument('--mask-strength', type=float, default=0.75)
    o = ap.parse_args()
    os.makedirs(o.out, exist_ok=True)
    orig, eye = Anim(o.orig), Anim(o.eye)
    prompt = '%s, %s, starcraft remastered unit sprite render, black background' % (o.token, o.desc)
    if o.lora:
        pipe = pipeline(lora_dir=o.lora)
    else:
        unet, te, tok = train_lora(training_frames(orig), prompt, o.steps, os.path.join(o.out, 'lora'))
        pipe = pipeline(unet, te, tok)
    log('pipeline ready')
    if o.mode == 'pilot':
        from PIL import ImageDraw
        mid = (len(orig.frames) // 17) // 2
        picks = [(g * 17 + d) for g in (0, mid) for d in (16, 12, 8, 4)] if not o.masks else \
                [(mid * 17 + d) for d in (16, 10, 9, 8, 7, 6, 0)] + [12, 8]
        masks = np.load(o.masks) if o.masks else None
        masks = solid(masks) if masks is not None and o.strength <= 0 else masks
        if masks is None:
            settings = [('render', None), ('s0.3', dict(strength=0.3)), ('s0.45', dict(strength=0.45)),
                        ('s0.6', dict(strength=0.6)), ('s0.45 cn0.6', dict(strength=0.45, cn_scale=0.6)),
                        ('s0.45 no LoRA', dict(strength=0.45, lora_scale=0.0))]
        elif o.strength <= 0:
            settings = [('render', None), ('unsure (mask)', 'mask')] + \
                       [('mask only %g' % v, dict(strength=0, mask_strength=v)) for v in (0.6, 0.75, 0.9)]
        else:
            settings = [('render', None), ('unsure (mask)', 'mask'), ('s0.4', dict(strength=0.4)),
                        ('s0.3 + mask 0.75', dict(strength=0.3, mask_strength=0.75)),
                        ('s0.3 + mask 0.9', dict(strength=0.3, mask_strength=0.9)),
                        ('s0.4 + mask 0.75', dict(strength=0.4, mask_strength=0.75))]
        rows = []
        for i in picks:
            rgba = eye.frame(i, 'diffuse', boxed=False)
            canvas, tf = to_canvas(rgba)
            mk = mask_canvas(masks['f%d' % i], tf) if masks is not None and 'f%d' % i in masks else None
            tiles = [to_canvas(orig.frame(i))[0].resize((256, 256))]
            for name, kw in settings:
                if kw == 'mask':
                    img = Image.composite(Image.new('RGB', canvas.size, (255, 0, 255)), canvas, mk) if mk else canvas
                elif kw is None:
                    img = canvas
                elif 'mask_strength' in kw and mk is not None:
                    img = repaint_masked(pipe, canvas, mk, prompt, kw['strength'], kw['mask_strength'], seed=i)
                elif kw.get('strength') == 0:  # mask only, and this frame has no mask
                    img = canvas
                else:
                    img = repaint(pipe, canvas, prompt, seed=i, **{k: v for k, v in kw.items() if k != 'mask_strength'})
                back = from_canvas(img, tf, rgba)
                if isinstance(kw, dict) and kw.get('strength') == 0 and mk is not None:
                    back = paste(back, rgba, masks['f%d' % i])
                a = back[..., 3:4] / 255.0
                tiles.append(Image.fromarray((back[..., :3] * a).astype(np.uint8)).resize(
                    (back.shape[1] * 2, back.shape[0] * 2), Image.NEAREST))
            h = max(t.height for t in tiles)
            row = Image.new('RGB', (sum(t.width for t in tiles) + 4 * len(tiles), h))
            x = 0
            for t in tiles:
                row.paste(t, (x, 0)); x += t.width + 4
            rows.append(row)
            log('pilot frame %d' % i)
        W = max(r.width for r in rows)
        sheet = Image.new('RGB', (W, sum(r.height for r in rows) + 20))
        y = 20
        for r in rows:
            sheet.paste(r, (0, y)); y += r.height
        ImageDraw.Draw(sheet).text((4, 4), 'original (31-49 deg art) | ' + ' | '.join(n for n, _ in settings), fill=(255, 255, 0))
        sheet.save(os.path.join(o.out, 'pilot.png'))
    else:
        from eyelevel_anim import write_anim
        ngroups = len(eye.frames) // 17
        groups = [int(v) for v in o.groups.split(',')] if o.groups else range(ngroups)
        masks = np.load(o.masks) if o.masks else None
        masks = solid(masks) if masks is not None and o.strength <= 0 else masks
        frames = []
        tcl = 'teamcolor' in eye.layers
        for i in range(len(eye.frames)):
            x, y, xo, yo, w, h = eye.frames[i]
            rgba = eye.frame(i, 'diffuse', boxed=False)
            tc = eye.frame(i, 'teamcolor', boxed=False)[..., 0] if tcl else np.zeros((h, w), np.uint8)
            unsure = masks is not None and 'f%d' % i in masks and masks['f%d' % i].any()
            if i // 17 in groups and i < ngroups * 17 and (rgba[..., 3] > 0).sum() > 50 and (o.strength > 0 or unsure):
                canvas, tf = to_canvas(rgba)
                if unsure:
                    img = repaint_masked(pipe, canvas, mask_canvas(masks['f%d' % i], tf), prompt, o.strength,
                                         o.mask_strength, seed=1000 + i % 17, cn_scale=o.cn)
                else:
                    img = repaint(pipe, canvas, prompt, o.strength, seed=1000 + i % 17, cn_scale=o.cn)
                new = from_canvas(img, tf, rgba)
                rgba = paste(new, rgba, masks['f%d' % i]) if o.strength <= 0 else new
            frames.append((xo, yo, rgba, tc))
            if i % 17 == 16:
                log('group %d done' % (i // 17))
        write_anim(os.path.join(o.out, os.path.basename(o.eye)), eye.box_w, eye.box_h, frames, eye.tilt10 / 10.0)
        log('wrote', os.path.join(o.out, os.path.basename(o.eye)))
