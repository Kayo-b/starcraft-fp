"""Preview sheet: rows = today's billboard, the lifted relief, relief + mirrored back; columns = viewer azimuths around
the model at eye height (and one from high ground)."""
import sys
import numpy as np
from PIL import Image, ImageDraw
from anim import Anim
from relief import lift, mirror_back, ELEV, BETA, CAMERA_BY_IMAGE
from preview import look_camera, splat, billboard, ground

DATA = '/home/kxyx/projects/starcraft-data/casc-scratch/extracted/anim/main_%03d.anim'
W, H = 320, 240
EYE = 48.0  # 12 game px

def views_for(radius, height_center):
    out = []
    for name, az, h in [('south (sprite side)', 270, EYE), ('south-east', 315, EYE), ('east', 0, EYE), ('north-east', 45, EYE),
                        ('north', 90, EYE), ('from high ground, SW', 225, 448 + EYE)]:
        a = np.radians(az)
        eye = np.array([radius * np.cos(a), radius * np.sin(a), h])
        out.append((name, eye, np.array([0, 0, height_center])))
    return out


def sheet(image_id, frame, radius, out_path, mirror=True, facing=False, cone=40, smooth=True):
    a = Anim(DATA % image_id)
    elev, beta = CAMERA_BY_IMAGE.get(image_id, (ELEV, BETA))
    P, rgba, mask = lift(a, frame, elev=elev, beta=beta, smooth=smooth)
    ctr = np.zeros(3)
    zmax = np.percentile(P[..., 2][mask], 95)
    Q = mirror_back(P, rgba, mask)[0] if mirror else None
    x, y, xo, yo, w, h = a.frames[frame]
    ground_row = float(np.median([f[3] + f[5] for f in a.frames])) - yo  # the engine stands sprites on the median frame bottom
    rows = []
    vs = views_for(radius, zmax * 0.45)
    kinds = ['billboard (today)', 'relief, fixed + mirrored back', 'relief, facing viewer' if facing else 'relief, turns (art shown within +-%d deg)' % cone]
    for kind in kinds:
        tiles = []
        for name, eye, tgt in vs:
            eye = eye + ctr * [1, 1, 0]; tgt = tgt + ctr * [1, 1, 0]
            cam = look_camera(eye, tgt, 66, W, H)
            img = np.zeros((H, W, 3), np.uint8); zb = np.full((H, W), np.inf)
            ground(img, zb, cam, W, H)
            if kind.startswith('billboard'):
                billboard(img, zb, rgba, ground_row, 1.0, cam, W, H, ctr)
            elif kind.endswith('back'):
                splat(img, zb, P, rgba, cam, W, H, mask=mask)
                splat(img, zb, Q, rgba, cam, W, H, mask=mask)
            else:
                # viewer azimuth relative to the drawn side (south = 0), the model turns by the excess over the cone
                rel = (np.degrees(np.arctan2(eye[1], eye[0])) + 90 + 180) % 360 - 180
                yaw = rel if facing else rel - cone * np.sin(np.radians(rel))  # the drawn side swings +-cone, continuously
                c, s_ = np.cos(np.radians(yaw)), np.sin(np.radians(yaw))
                Rz = np.array([[c, -s_, 0], [s_, c, 0], [0, 0, 1]])
                splat(img, zb, P @ Rz.T, rgba, cam, W, H, mask=mask)
            t = Image.fromarray(img); d = ImageDraw.Draw(t); d.text((4, 4), name, fill=(255, 255, 0)); d.text((4, H - 14), kind, fill=(255, 255, 255))
            tiles.append(np.asarray(t))
        rows.append(np.concatenate(tiles, 1))
    Image.fromarray(np.concatenate(rows, 0)).save(out_path)


if __name__ == '__main__':
    opts = sys.argv[5:]
    sheet(int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3]), sys.argv[4], facing='facing' in opts, smooth='raw' not in opts)
