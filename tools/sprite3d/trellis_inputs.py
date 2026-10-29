"""Write sprite frames as RGBA PNGs for the TRELLIS run on Kaggle (`kaggle/trellis_kernel.py`), plus the metadata of the
private Kaggle dataset that carries them. Game art: the folder is under out/ (not in git) and the dataset stays private.

    python3 trellis_inputs.py OUT_DIR NAME:IMAGE_ID:FRAME[:CX:CY] ...
    kaggle datasets create -p OUT_DIR        (first time; private by default)
    kaggle datasets version -p OUT_DIR -m "..."   (later)

With CX:CY (HD px, model frame of `relief.lift`) only the free-standing part whose foot is there is written
(`relief.part_mask`), for parts TRELLIS fuses with what stands behind them (the Nexus's side arches). The names written
go to todo.txt, which the kernel processes (the other PNGs of the dataset are kept but skipped)."""
import json
import os
import sys
import numpy as np
from PIL import Image
from anim import Anim
from relief import lift, part_mask, ELEV, BETA, CAMERA_BY_IMAGE

DATA = '/home/kxyx/projects/starcraft-data/casc-scratch/extracted/anim/main_%03d.anim'
DATASET = 'kayo01/sc-sprite3d-inputs'

if __name__ == '__main__':
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    names = []
    for spec in sys.argv[2:]:
        name, image_id, frame, *foot = spec.split(':')
        a = Anim(DATA % int(image_id))
        rgba = a.frame(int(frame), 'diffuse', boxed=False)
        if foot:
            elev, beta = CAMERA_BY_IMAGE.get(int(image_id), (ELEV, BETA))
            P, _, mask = lift(a, int(frame), elev=elev, beta=beta)
            keep = part_mask(P, mask, float(foot[0]), float(foot[1]))
            rgba = np.where(keep[..., None], rgba, 0).astype(np.uint8)
            rows, cols = np.nonzero(keep)
            rgba = rgba[rows.min():rows.max() + 1, cols.min():cols.max() + 1]
        Image.fromarray(rgba).save(os.path.join(out, '%s.png' % name))
        names.append(name)
        print(name, rgba.shape)
    with open(os.path.join(out, 'todo.txt'), 'w') as f:
        f.write('\n'.join(names) + '\n')
    with open(os.path.join(out, 'dataset-metadata.json'), 'w') as f:
        json.dump({'title': 'sc-sprite3d-inputs', 'id': DATASET, 'licenses': [{'name': 'other'}]}, f, indent=2)
