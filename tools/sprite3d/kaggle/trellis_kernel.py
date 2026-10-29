"""Kaggle kernel (free T4, 16 GB): TRELLIS-image-large on the sprite PNGs of the private input dataset that its todo.txt
names (all of them without one), a few seeds each. A NAME_coords.npy next to NAME.png (`sc2_structure.py`) is the
coarse structure to use (voxel indices in the 64^3 grid): only TRELLIS's second stage (details, colours) runs.
Writes to /kaggle/working/out: NAME_sSEED.glb (textured mesh), NAME_sSEED.npz (raw mesh), NAME_sSEED.png (previews:
row 1 at 15 degrees up, row 2 at 40 degrees, 8 yaws each) and NAME_input.png (the crop TRELLIS saw).
Push with `kaggle kernels push -p tools/sprite3d/kaggle`; fetch with `kaggle kernels output kayo01/sc-sprite3d-trellis`."""
import glob
import os
import subprocess
import sys
import time
import types

T0 = time.time()
SEEDS = [0, 1, 2]
TRELLIS_COMMIT = '442aa1e'


def sh(cmd, check=True):
    print('[%5.0fs] $ %s' % (time.time() - T0, cmd), flush=True)
    r = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(r.stdout[-3000:], flush=True)
    if check and r.returncode:
        raise SystemExit('failed: %s' % cmd)
    return r.returncode == 0


sh('nvidia-smi --query-gpu=name,memory.total --format=csv; python -V; nvcc --version | tail -1', check=False)
import torch
TV = torch.__version__.split('+')[0]
print('torch', torch.__version__, 'cuda', torch.version.cuda, flush=True)
os.environ['TORCH_CUDA_ARCH_LIST'] = '7.5'  # T4
os.environ['MAX_JOBS'] = '4'

# every step runs and reports, so one run shows every install problem (the image is Python 3.13, torch 2.11, CUDA 12.8)
sh('pip install -q plyfile easydict imageio imageio-ffmpeg trimesh xatlas pyvista pymeshfix igraph ninja', check=False)
sh('pip install -q git+https://github.com/EasternJournalist/utils3d.git@9a4eb15e4021b67b12c460c7057d642626897ec8', check=False)
sh('git clone -q https://github.com/NVlabs/nvdiffrast.git /tmp/nvdiffrast && pip install -q --no-build-isolation /tmp/nvdiffrast', check=False)
sh('git clone -q --recursive https://github.com/autonomousvision/mip-splatting.git /tmp/mip && '
   "sed -i '1i #include <cstdint>' /tmp/mip/submodules/diff-gaussian-rasterization/cuda_rasterizer/rasterizer_impl.h && "
   'pip install -q --no-build-isolation /tmp/mip/submodules/diff-gaussian-rasterization/', check=False)
sh('git clone -q https://github.com/microsoft/TRELLIS.git /tmp/TRELLIS && git -C /tmp/TRELLIS checkout -q %s && '
   'git -C /tmp/TRELLIS submodule update -q --init' % TRELLIS_COMMIT)  # FlexiCubes is a submodule

# xformers built for this torch if PyPI has one; otherwise a stand-in on torch's own attention (TRELLIS only needs
# memory_efficient_attention and BlockDiagonalMask.from_seqlens)
have_xformers = sh('pip install -q --only-binary=:all: xformers "torch==%s"' % TV, check=False) and \
    sh('python -c "import torch, xformers.ops as x; q = torch.randn(1, 64, 4, 32, device=\'cuda\', dtype=torch.half); '
       'print(x.memory_efficient_attention(q, q, q).shape)"', check=False)
if not have_xformers:
    print('xformers unavailable: using the scaled_dot_product_attention stand-in', flush=True)
    import torch.nn.functional as F

    class BlockDiagonalMask:
        def __init__(self, q, kv):
            self.q, self.kv = q, kv

        @classmethod
        def from_seqlens(cls, q_seqlen, kv_seqlen=None):
            return cls(list(q_seqlen), list(kv_seqlen if kv_seqlen is not None else q_seqlen))

    def memory_efficient_attention(q, k, v, attn_bias=None):  # [B, N, H, C]
        att = lambda q, k, v: F.scaled_dot_product_attention(q.transpose(1, 2), k.transpose(1, 2), v.transpose(1, 2)).transpose(1, 2)
        if attn_bias is None:
            return att(q, k, v)
        out, qs, ks = [], 0, 0
        for lq, lk in zip(attn_bias.q, attn_bias.kv):
            out.append(att(q[:, qs:qs + lq], k[:, ks:ks + lk], v[:, ks:ks + lk]))
            qs, ks = qs + lq, ks + lk
        return torch.cat(out, 1)

    xf = types.ModuleType('xformers'); ops = types.ModuleType('xformers.ops')
    ops.memory_efficient_attention = memory_efficient_attention
    ops.fmha = types.SimpleNamespace(BlockDiagonalMask=BlockDiagonalMask)
    xf.ops = ops
    sys.modules['xformers'] = xf; sys.modules['xformers.ops'] = ops

# spconv: spconv-cu126's cumm dependency rebuilds itself on import here and fails (Python 3.13), so TRELLIS gets a
# stand-in. TRELLIS only runs submanifold convolutions (stride 1, kernel 3 or 1) at inference; the stand-in gathers the
# 27 neighbours by sorted voxel keys and sums one matrix product per offset, with spconv's weight layout
# (out, kx, ky, kz, in) and the cross-correlation of a dense Conv3d (checked against F.conv3d below).
import torch.nn as nn
import torch.nn.functional as F


class SparseConvTensor:
    def __init__(self, features, indices, spatial_shape, batch_size, grid=None, voxel_num=None, indice_dict=None,
                 benchmark=False):
        self._features, self.indices, self.spatial_shape, self.batch_size = features, indices, spatial_shape, batch_size
        self.grid, self.voxel_num = grid, voxel_num
        self.indice_dict = {} if indice_dict is None else indice_dict
        self.benchmark, self.benchmark_record, self.thrust_allocator = benchmark, {}, None
        self._timer, self.force_algo, self.int8_scale = None, None, None

    @property
    def features(self):
        return self._features

    @features.setter
    def features(self, value):
        self._features = value

    def replace_feature(self, feature):
        new = SparseConvTensor(feature, self.indices, self.spatial_shape, self.batch_size, self.grid, self.voxel_num, self.indice_dict)
        return new

    def dense(self):
        raise NotImplementedError('spconv stand-in: dense() is not used by TRELLIS inference')


def neighbours(indices, ksize, dilation):
    """For each kernel offset (kx, ky, kz in weight order), the row of the voxel at coords + offset, or -1."""
    idx = indices.long()
    r = (ksize // 2) * dilation
    lo = idx.min(0).values
    c = idx - lo
    c[:, 1:] += r
    ext = c.max(0).values + 1
    ext[1:] += r
    key = lambda c: ((c[:, 0] * ext[1] + c[:, 1]) * ext[2] + c[:, 2]) * ext[3] + c[:, 3]
    k = key(c)
    sk, order = k.sort()
    out = []
    rng = range(-(ksize // 2), ksize // 2 + 1)
    for a in rng:
        for b in rng:
            for d in rng:
                q = key(c + torch.tensor([0, a * dilation, b * dilation, d * dilation], device=c.device))
                pos = torch.searchsorted(sk, q).clamp(max=len(sk) - 1)
                out.append(torch.where(sk[pos] == q, order[pos], -1))
    return out


class SubMConv3d(nn.Module):
    def __init__(self, in_channels, out_channels, kernel_size, stride=1, padding=0, dilation=1, groups=1, bias=True,
                 indice_key=None, algo=None):
        super().__init__()
        assert stride == 1 and groups == 1
        self.in_channels, self.out_channels, self.ksize, self.dilation = in_channels, out_channels, kernel_size, dilation
        self.weight = nn.Parameter(torch.zeros(out_channels, kernel_size, kernel_size, kernel_size, in_channels))
        self.bias = nn.Parameter(torch.zeros(out_channels)) if bias else None

    def forward(self, x):
        cache_key = ('stand-in', self.ksize, self.dilation)
        hit = x.indice_dict.get(cache_key)
        if hit is None or hit[0] is not x.indices:
            hit = (x.indices, neighbours(x.indices, self.ksize, self.dilation))
            x.indice_dict[cache_key] = hit
        f = x.features
        W = self.weight.to(f.dtype).reshape(self.out_channels, -1, self.in_channels)
        out = torch.zeros(f.shape[0], self.out_channels, dtype=f.dtype, device=f.device)
        for kk, nb in enumerate(hit[1]):
            sel = (nb >= 0).nonzero().squeeze(1)
            if len(sel):
                out.index_add_(0, sel, f[nb[sel]] @ W[:, kk].t())
        if self.bias is not None:
            out += self.bias.to(f.dtype)
        return x.replace_feature(out)


def not_used(*a, **k):
    raise NotImplementedError('spconv stand-in: only submanifold convolution is implemented')


spconv_pytorch = types.ModuleType('spconv.pytorch')
spconv_pytorch.SparseConvTensor, spconv_pytorch.SubMConv3d = SparseConvTensor, SubMConv3d
spconv_pytorch.SparseConv3d = spconv_pytorch.SparseInverseConv3d = not_used
spconv_pytorch.ConvAlgo = types.SimpleNamespace(Native='native', MaskImplicitGemm='implicit_gemm')
spconv_mod = types.ModuleType('spconv'); spconv_mod.pytorch = spconv_pytorch
sys.modules['spconv'] = spconv_mod; sys.modules['spconv.pytorch'] = spconv_pytorch

# self-test against a dense convolution: random voxels in a 12^3 grid, batch of 2
g = torch.Generator().manual_seed(0)
dense_in = torch.zeros(2, 5, 12, 12, 12)
occ = torch.rand(2, 12, 12, 12, generator=g) < 0.3
dense_in.permute(0, 2, 3, 4, 1)[occ] = torch.randn(int(occ.sum()), 5, generator=g)
coords = occ.nonzero().int().cuda()
conv = SubMConv3d(5, 7, 3).cuda()
with torch.no_grad():
    conv.weight.normal_(); conv.bias.normal_()
    got = conv(SparseConvTensor(dense_in.permute(0, 2, 3, 4, 1)[occ].cuda(), coords, [12, 12, 12], 2)).features
    ref = F.conv3d(dense_in.cuda(), conv.weight.permute(0, 4, 1, 2, 3), conv.bias, padding=1).permute(0, 2, 3, 4, 1)[occ.cuda()]
print('spconv stand-in vs dense conv3d: max abs diff %.2e' % (got - ref).abs().max().item(), flush=True)

missing = [m for m in ['nvdiffrast.torch', 'diff_gaussian_rasterization', 'utils3d', 'xatlas', 'pyvista',
                        'pymeshfix', 'igraph', 'trimesh', 'easydict', 'plyfile']
           if not sh('python -c "import %s"' % m, check=False)]
if missing:
    raise SystemExit('missing: %s' % missing)

os.environ['ATTN_BACKEND'] = 'xformers'
os.environ['SPARSE_ATTN_BACKEND'] = 'xformers'
os.environ['SPCONV_ALGO'] = 'native'
sys.modules.setdefault('rembg', types.ModuleType('rembg'))    # inputs carry alpha: background removal is never used
o3d = types.ModuleType('open3d'); o3d.geometry = types.SimpleNamespace(TriangleMesh=object)
sys.modules.setdefault('open3d', o3d)  # only the text pipeline uses it (and names o3d.geometry.TriangleMesh in a signature)
for m in ['kaolin', 'kaolin.utils', 'kaolin.utils.testing']:  # FlexiCubes only uses kaolin's shape assertion
    sys.modules[m] = types.ModuleType(m)
sys.modules['kaolin.utils.testing'].check_tensor = lambda *a, **k: True
sys.path.insert(0, '/tmp/TRELLIS')

import numpy as np
from PIL import Image
from trellis.pipelines import TrellisImageTo3DPipeline
from trellis.utils import render_utils, postprocessing_utils

pipe = TrellisImageTo3DPipeline.from_pretrained('microsoft/TRELLIS-image-large')
pipe.cuda()
print('[%5.0fs] pipeline loaded' % (time.time() - T0), flush=True)

OUT = '/kaggle/working/out'
os.makedirs(OUT, exist_ok=True)
yaws = [i * np.pi / 4 for i in range(8)]
todo = glob.glob('/kaggle/input/**/todo.txt', recursive=True)  # names to process; without it, every PNG
todo = set(open(todo[0]).read().split()) if todo else None
for path in sorted(glob.glob('/kaggle/input/**/*.png', recursive=True)):
    name = os.path.splitext(os.path.basename(path))[0]
    if todo is not None and name not in todo:
        continue
    image = Image.open(path).convert('RGBA')
    pipe.preprocess_image(image).save('%s/%s_input.png' % (OUT, name))
    structure = path[:-4] + '_coords.npy'  # a given shape (`sc2_structure.py`): only the second stage runs, on its voxels
    structure = torch.from_numpy(np.load(structure)).int().cuda() if os.path.exists(structure) else None
    for seed in SEEDS:
        t = time.time()
        if structure is None:
            res = pipe.run(image, seed=seed, formats=['gaussian', 'mesh'])
        else:
            with torch.no_grad():  # as pipe.run does; without it the mesh decoder keeps its activations (out of memory)
                cond = pipe.get_cond([pipe.preprocess_image(image)])
                torch.manual_seed(seed)
                coords = torch.cat([torch.zeros_like(structure[:, :1]), structure], 1)  # batch index, x, y, z
                res = pipe.decode_slat(pipe.sample_slat(cond, coords), ['gaussian', 'mesh'])
        g, m = res['gaussian'][0], res['mesh'][0]
        rows = []
        for pitch in (np.radians(15), np.radians(40)):
            ext, intr = render_utils.yaw_pitch_r_fov_to_extrinsics_intrinsics(yaws, [pitch] * 8, 2, 40)
            frames = render_utils.render_frames(g, ext, intr, {'resolution': 384, 'bg_color': (0.47, 0.55, 0.65)}, verbose=False)['color']
            rows.append(np.concatenate(frames, 1))
        Image.fromarray(np.concatenate(rows, 0)).save('%s/%s_s%d.png' % (OUT, name, seed))
        np.savez_compressed('%s/%s_s%d.npz' % (OUT, name, seed), vertices=m.vertices.cpu().numpy(), faces=m.faces.cpu().numpy())
        try:
            glb = postprocessing_utils.to_glb(g, m, simplify=0.9, texture_size=1024, verbose=False)
        except RuntimeError as e:  # hole filling rasterises the whole mesh; on a big one (Hatchery) it ran out of memory
            print('  %s seed %d: %s; again without hole filling' % (name, seed, str(e).splitlines()[0]), flush=True)
            torch.cuda.empty_cache()
            try:
                glb = postprocessing_utils.to_glb(g, m, simplify=0.9, fill_holes=False, texture_size=1024, verbose=False)
            except RuntimeError as e:
                print('  %s seed %d: no GLB (%s)' % (name, seed, str(e).splitlines()[0]), flush=True)
                glb = None
        if glb is not None:
            glb.export('%s/%s_s%d.glb' % (OUT, name, seed))
        print('[%5.0fs] %s seed %d done in %.0fs, peak %.1f GB' % (time.time() - T0, name, seed, time.time() - t,
              torch.cuda.max_memory_allocated() / 2 ** 30), flush=True)
        del res, g, m
        torch.cuda.empty_cache()
print('[%5.0fs] all done' % (time.time() - T0), flush=True)
