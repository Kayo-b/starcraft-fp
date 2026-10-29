"""The relief (a sprite frame lifted with its own depth layer, `relief.lift`) as a mesh to open and edit in Blender: one
vertex per sprite pixel, the quads `preview.cells` keeps (cut at depth steps, lone pixels dropped), textured with the
Remastered sprite frame itself (each vertex at its pixel's centre in the texture; alpha masked). With `back`, the
mirrored back (`relief.mirror_back`) is a second object. GLB in game pixels, y up, as `trellis_compose.py` writes;
render it and save a .blend with `blender_views.py`.

    relief_mesh.py IMAGE_ID FRAME OUT.glb [back]
Run with the venv python (needs trimesh)."""
import sys
import numpy as np
import trimesh
from PIL import Image
from anim import Anim
from relief import lift, mirror_back, ELEV, BETA, CAMERA_BY_IMAGE
from preview import cells
from views import camera
from demo import DATA

HD_PER_GAME_PX = 4.0


def grid_mesh(P, rgba, mask, elev, mirrored=False):
    """Triangles over the kept quads of the pixel grid, wound to face the sprite camera (a mirrored copy the other way)."""
    h, w = mask.shape
    q, _ = cells(P, mask)
    r, c = np.nonzero(q)
    a, b, d, e = r * w + c, r * w + c + 1, (r + 1) * w + c + 1, (r + 1) * w + c
    F = np.concatenate([np.stack([a, e, b], 1), np.stack([b, e, d], 1)])
    V = P.reshape(-1, 3)
    n = np.cross(V[F[:, 1]] - V[F[:, 0]], V[F[:, 2]] - V[F[:, 0]])
    if (n @ camera(elev, 0.0)[2]).sum() > 0 or mirrored:  # want normals towards the camera: n . view < 0
        F = F[:, ::-1]
    vv, uu = np.mgrid[0:h, 0:w]
    uv = np.stack([(uu.ravel() + 0.5) / w, 1 - (vv.ravel() + 0.5) / h], 1)
    Vg = np.nan_to_num(V) / HD_PER_GAME_PX @ np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]], float)  # z up -> y up (glTF)
    mat = trimesh.visual.material.PBRMaterial(baseColorTexture=Image.fromarray(rgba), alphaMode='MASK', alphaCutoff=0.5,
                                              doubleSided=True, metallicFactor=0.0, roughnessFactor=1.0)
    m = trimesh.Trimesh(Vg, F, visual=trimesh.visual.TextureVisuals(uv=uv, material=mat), process=False)
    m.remove_unreferenced_vertices()
    return m


if __name__ == '__main__':
    image_id, frame, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    elev, beta = CAMERA_BY_IMAGE.get(image_id, (ELEV, BETA))
    P, rgba, mask = lift(Anim(DATA % image_id), frame, elev=elev, beta=beta)
    meshes = {'relief': grid_mesh(P, rgba, mask, elev)}
    if 'back' in sys.argv[4:]:
        meshes['relief_mirrored_back'] = grid_mesh(mirror_back(P, rgba, mask)[0], rgba, mask, elev, mirrored=True)
    trimesh.Scene(meshes).export(out)
    print('%s: %s' % (out, ', '.join('%s %d triangles' % (k, len(m.faces)) for k, m in meshes.items())))
