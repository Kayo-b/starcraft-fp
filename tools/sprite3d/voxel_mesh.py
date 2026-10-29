"""A TRELLIS structure (NAME_coords.npy: voxel indices in its 64^3 grid, as `sc2_structure.py` writes) as a GLB of
cubes, to look at with `blender_views.py ... shaded`. Grid units scaled to SIZE game px across (default 140); YAW is
the turn `sc2_structure.py` was given, undone so the sprite camera sees it as the sprite.

    voxel_mesh.py NAME_coords.npy OUT.glb [SIZE [YAW]]
Run with the venv python (needs trimesh)."""
import sys
import numpy as np
import trimesh
from trellis_fit import rz

if __name__ == '__main__':
    G = np.load(sys.argv[1]).astype(np.float64)
    size = float(sys.argv[3]) if len(sys.argv) > 3 else 140.0
    yaw = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
    occ = {tuple(v) for v in G.astype(int)}
    corners = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0], [0, 0, 1], [1, 0, 1], [1, 1, 1], [0, 1, 1]], float)
    faces = {(-1, 0, 0): [0, 4, 7, 3], (1, 0, 0): [1, 2, 6, 5], (0, -1, 0): [0, 1, 5, 4], (0, 1, 0): [3, 7, 6, 2],
             (0, 0, -1): [0, 3, 2, 1], (0, 0, 1): [4, 5, 6, 7]}
    V, F = [], []
    for v in G.astype(int):
        for d, q in faces.items():
            if (v[0] + d[0], v[1] + d[1], v[2] + d[2]) in occ:
                continue  # inner face
            n = len(V)
            V.extend(v + corners[q])
            F.extend([[n, n + 1, n + 2], [n, n + 2, n + 3]])
    V = np.array(V)
    V -= [V[:, 0].mean(), V[:, 1].mean(), V[:, 2].min()]
    V = V @ rz(-yaw).T
    V *= size / np.ptp(V[:, :2], 0).max()
    V = V @ np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]], float)  # z up -> y up (glTF)
    trimesh.Trimesh(V, F, process=False).export(sys.argv[2])
    print('%s: %d voxels, %d faces' % (sys.argv[2], len(G), len(F)))
