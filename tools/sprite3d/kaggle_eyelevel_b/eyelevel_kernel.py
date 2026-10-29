"""Kaggle kernel (free T4, 4 CPUs): the eye-level sprite jobs that the private dataset kayo01/sc-eyelevel-inputs lists in
its todo.json, so the local machine stays free. The dataset holds the sprite3d tools (tools/*.py), the units' anim
files (anim/, Blizzard data: private only) and the clean-up model's files (refine/).
Jobs: {"kind": "run", "pip": [...], "cmd": "SCRIPT ARGS", "needs": [...]}: any tool ({IN}/{OUT} in cmd expanded);
{"kind": "unit", "name", "anim", "args": [...], "refine": "MODEL.pt"?, "models": "UNIT"?, "models_from": "NAME"?}:
eyelevel_anim.py into out/NAME (anim, compare.png, sheet.png, log.txt, models/), reusing voxel models from the
dataset's models/UNIT or this run's out/NAME (their calibration is passed on, so they are not rebuilt); {"kind": "train", "pairs", "out", "iters"}: refine.py train into out/refine;
{"kind": "pairs", "out", "units": [...]}: refine_data.py over out/'s units.
A job with "kernel": "a" or "b" runs only on that kernel (kayo01/sc-eyelevel is a, kayo01/sc-eyelevel-b is b; both
mount the same dataset, so one todo.json can keep both busy); a job without it runs on either.
Push: kaggle datasets version -p DATASET_DIR -m MSG --dir-mode zip; kaggle kernels push -p tools/sprite3d/kaggle_eyelevel.
Fetch: kaggle kernels output kayo01/sc-eyelevel -p DIR."""
import glob
import io
import json
import os
import shutil
import subprocess
import sys
import time

T0 = time.time()
W = '/kaggle/working'
TOOLS, OUT = W + '/tools', W + '/out'


def sh(cmd, check=True):
    print('[%5.0fs] $ %s' % (time.time() - T0, cmd), flush=True)
    r = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(r.stdout[-6000:], flush=True)
    if check and r.returncode:
        print('FAILED: %s' % cmd, flush=True)
    return r.returncode == 0


todo = glob.glob('/kaggle/input/**/todo.json', recursive=True)
IN = os.path.dirname(todo[0])
KERNEL = 'b'  # a copy of kaggle_eyelevel/eyelevel_kernel.py
JOBS = [j for j in json.load(open(todo[0])) if j.get('kernel', KERNEL) == KERNEL]
print('input', IN, os.listdir(IN), flush=True)
# a stale dataset version (the kernel pushed while a new one was still processing) shows as missing files: stop early
missing = [j.get('pairs') for j in JOBS if j['kind'] == 'train' and not os.path.exists(os.path.join(IN, 'refine', j['pairs']))]
missing += [p for j in JOBS for p in j.get('needs', []) if not os.path.exists(os.path.join(IN, p))]
if missing:
    raise SystemExit('dataset is missing %s: an older version is mounted' % missing)
sh('nvidia-smi --query-gpu=name,memory.total --format=csv; python -V; nproc; free -g | head -2', check=False)
try:  # writing DXT5/DXT1 DDS needs Pillow 11.2+
    from PIL import Image
    Image.new('RGBA', (4, 4)).save(io.BytesIO(), 'DDS', pixel_format='DXT5')
except Exception as e:
    print('Pillow cannot write DXT DDS (%s): upgrading' % e, flush=True)
    sh('pip install -q -U "pillow>=12"')
shutil.copytree(IN + '/tools', TOOLS, dirs_exist_ok=True)
os.makedirs(OUT, exist_ok=True)
os.environ['ANIM_DIR'] = IN + '/anim'
os.environ['EYELEVEL_OUT'] = OUT
py = sys.executable


def reuse_models(src, out):
    """Copy voxel models into out/models; returns ['--calib', 'TILT,AXIS'] from them."""
    import numpy as np
    files = sorted(glob.glob(os.path.join(src, '*.npz')))
    if not files:
        return []
    os.makedirs(os.path.join(out, 'models'), exist_ok=True)
    for f in files:
        shutil.copy(f, os.path.join(out, 'models'))
    m = np.load(files[0])
    return ['--calib', '%r,%r' % (float(m['elev']), float(m['axis']))]

for job in JOBS:
    print('=' * 20, job, flush=True)
    if job['kind'] == 'unit':
        out = os.path.join(OUT, job['name'])
        anim = os.path.join(IN, 'anim', job['anim'])
        extra = []
        if job.get('models'):
            extra = reuse_models(os.path.join(IN, 'models', job['models']), out)
        elif job.get('models_from'):
            extra = reuse_models(os.path.join(OUT, job['models_from'], 'models'), out)
        cmd = '%s %s/eyelevel_anim.py %s %s --jobs 4 %s %s' % (py, TOOLS, anim, out, ' '.join(job.get('args', [])), ' '.join(extra))
        if job.get('refine'):  # a model trained earlier in this run (out/refine) or one from the dataset
            mine = os.path.join(OUT, 'refine', job['refine'])
            cmd += ' --refine %s' % (mine if os.path.exists(mine) else os.path.join(IN, 'refine', job['refine']))
        if sh('cd %s && %s' % (TOOLS, cmd)):
            sh('cd %s && %s -c "import sys; sys.argv=[\'x\']; import eyelevel_batch as b; b.compare(\'%s\', \'%s/%s\', \'%s/compare.png\')"'
               % (TOOLS, py, anim, out, job['anim'], out))
    elif job['kind'] == 'train':
        os.makedirs(OUT + '/refine', exist_ok=True)
        sh('cd %s && %s refine.py train %s %s/refine/%s %d %s' % (TOOLS, py, os.path.join(IN, 'refine', job['pairs']), OUT,
                                                                 job['out'], job.get('iters', 20000), '--gan' if job.get('gan') else ''))
        sh('cd %s && %s refine.py preview %s/refine/%s %s %s/refine/%s_preview.png' % (TOOLS, py, OUT, job['out'],
                                                                                      os.path.join(IN, 'refine', job['pairs']), OUT, job['out']))
    elif job['kind'] == 'run':  # any tool: {"kind": "run", "pip": [...], "cmd": "SCRIPT ARGS" ({IN}, {OUT} expanded), "needs": [...]}
        for c in job.get('pre', []):  # e.g. pip uninstall -y torchao (the image's old one breaks new diffusers)
            sh(c, check=False)
        if job.get('pip'):
            sh('pip install -q ' + ' '.join(job['pip']))
        sh('cd %s && %s %s' % (TOOLS, py, job['cmd'].format(IN=IN, OUT=OUT)))
    elif job['kind'] == 'pairs':
        os.makedirs(OUT + '/refine', exist_ok=True)
        sh('cd %s && %s refine_data.py %s/refine/%s %s' % (TOOLS, py, OUT, job['out'], ' '.join(job.get('units', []))))
print('done in %.0f s' % (time.time() - T0), flush=True)
