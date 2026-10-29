"""Novel views of an HD sprite frame with Zero-1-to-3 XL (ashawkey/zero123-xl-diffusers weights, fp16), sampled with a
local loop built from diffusers/transformers parts (the repo's own pipeline file is not used).

Conditioning as in Zero-1-to-3: CLIP image embedding + relative camera [d_polar (rad), sin d_az, cos d_az, d_radius]
-> Linear(772, 768); the input's VAE latent (unscaled) is concatenated to the noisy latent; classifier-free guidance
with zeroed conditions. d_polar > 0 lowers the camera (polar angle measured from the zenith).

usage: python zero123_views.py ANIM FRAME OUT.png [steps] [seed]"""
import sys
import numpy as np
import torch
from PIL import Image
from safetensors.torch import load_file
from diffusers import UNet2DConditionModel, AutoencoderKL, DDIMScheduler
from transformers import CLIPVisionModelWithProjection, CLIPImageProcessor
from huggingface_hub import snapshot_download
from anim import Anim

SRC_ELEV = 35.0  # the sprite camera (see relief.py)


def load(dev):
    root = snapshot_download('ashawkey/zero123-xl-diffusers', allow_patterns=['*.json', '*.safetensors'])
    dt = torch.float16
    unet = UNet2DConditionModel.from_pretrained(root, subfolder='unet', torch_dtype=dt).to(dev)
    vae = AutoencoderKL.from_pretrained(root, subfolder='vae', torch_dtype=dt).to(dev)
    enc = CLIPVisionModelWithProjection.from_pretrained(root, subfolder='image_encoder', torch_dtype=dt).to(dev)
    fe = CLIPImageProcessor.from_pretrained(root, subfolder='feature_extractor')
    sd = load_file(root + '/clip_camera_projection/diffusion_pytorch_model.safetensors')
    proj = torch.nn.Linear(772, 768).to(dev, dt)
    proj.load_state_dict({'weight': sd['proj.weight'], 'bias': sd['proj.bias']})
    sch = DDIMScheduler.from_pretrained(root, subfolder='scheduler')
    return unet, vae, enc, fe, proj, sch


def sprite_input(anim, frame, size=256, fill=0.8):
    """The frame composited on white, centred in a square with the art filling `fill` of it (Zero123's training look)."""
    rgba = anim.frame(frame, 'diffuse', boxed=False).astype(np.float32) / 255
    a = rgba[..., 3:4]
    img = rgba[..., :3] * a + (1 - a)
    h, w = img.shape[:2]
    s = int(max(h, w) / fill)
    canvas = np.ones((s, s, 3), np.float32)
    canvas[(s - h) // 2:(s - h) // 2 + h, (s - w) // 2:(s - w) // 2 + w] = img
    return Image.fromarray((canvas * 255).astype(np.uint8)).resize((size, size), Image.LANCZOS)


@torch.no_grad()
def novel_views(models, image, poses, steps=50, scale=3.0, seed=0, dev='cuda'):
    unet, vae, enc, fe, proj, sch = models
    dt = torch.float16
    clip = enc(fe(images=image, return_tensors='pt').pixel_values.to(dev, dt)).image_embeds[:, None]  # (1,1,768)
    x = torch.from_numpy(np.asarray(image).astype(np.float32) / 127.5 - 1).permute(2, 0, 1)[None].to(dev, dt)
    lat_img = vae.encode(x).latent_dist.mode()  # unscaled, as Zero123 was trained
    outs = []
    for d_polar, d_az, d_r in poses:
        cam = torch.tensor([[np.radians(d_polar), np.sin(np.radians(d_az)), np.cos(np.radians(d_az)), d_r]], device=dev, dtype=dt)[:, None]
        cond = proj(torch.cat([clip, cam], -1))
        emb = torch.cat([torch.zeros_like(cond), cond])
        g = torch.Generator(dev).manual_seed(seed)
        lat = torch.randn((1, 4, 32, 32), generator=g, device=dev, dtype=dt) * sch.init_noise_sigma
        sch.set_timesteps(steps, device=dev)
        for t in sch.timesteps:
            inp = torch.cat([torch.cat([lat, lat]), torch.cat([torch.zeros_like(lat_img), lat_img])], 1)
            inp = sch.scale_model_input(inp, t)
            n_u, n_c = unet(inp, t, encoder_hidden_states=emb).sample.chunk(2)
            lat = sch.step(n_u + scale * (n_c - n_u), t, lat).prev_sample
        img = vae.decode(lat / vae.config.scaling_factor).sample
        outs.append(((img[0].float().clamp(-1, 1) + 1) * 127.5).permute(1, 2, 0).byte().cpu().numpy())
    return outs


if __name__ == '__main__':
    path, frame, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    steps = int(sys.argv[4]) if len(sys.argv) > 4 else 50
    seed = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    models = load('cuda')
    img = sprite_input(Anim(path), frame)
    eye = SRC_ELEV - 5.0  # down to ~5 degrees above the horizon (first-person eye level)
    poses = [(0, 0, 0), (eye, 0, 0), (eye, 45, 0), (eye, 90, 0), (eye, 135, 0), (eye, 180, 0), (eye, 270, 0)]
    labels = ['same view (sanity)', 'eye level, front', 'eye, 45', 'eye, 90 (side)', 'eye, 135', 'eye, 180 (back)', 'eye, 270 (other side)']
    views = novel_views(models, img, poses, steps=steps, seed=seed)
    from PIL import ImageDraw
    tiles = [np.asarray(img)] + views
    sheet = Image.fromarray(np.concatenate(tiles, 1))
    d = ImageDraw.Draw(sheet)
    for k, lab in enumerate(['input (sprite)'] + labels):
        d.text((k * 256 + 4, 4), lab, fill=(200, 0, 0))
    sheet.save(out)
    print('saved', out, 'peak VRAM %.2f GB' % (torch.cuda.max_memory_allocated() / 1e9))
