"""Render a GLB (from `trellis_compose.py`: game pixels, y up) with Blender, unlit (the art has its lighting baked in):
views around it at first-person eye height, one from above, and one through the sprite camera (orthographic, from the
south, ELEV degrees down) to compare with the sprite. Also saves it as a .blend (File > Open; a .glb needs
File > Import > glTF 2.0).

    blender -b -P blender_views.py -- IN.glb OUT_PREFIX [ELEV] [shaded]   -> OUT_PREFIX.png (grid), OUT_PREFIX.blend
With `shaded`, studio light on plain grey instead (shapes without textures, as `voxel_mesh.py` writes)."""
import math
import os
import sys
import bpy
import numpy as np
from mathutils import Vector

EYE = 12.0  # game px, as the first-person camera
TILE = (640, 480)

argv = sys.argv[sys.argv.index('--') + 1:]
glb, out = argv[0], argv[1]
elev = float(argv[2]) if len(argv) > 2 else 35.0
shaded = 'shaded' in argv[3:]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.context.preferences.filepaths.save_version = 0  # no OUT.blend1 backups
bpy.ops.import_scene.gltf(filepath=glb)  # glTF y up -> Blender z up: x east, y north, z up, as the model frame
objs = [o for o in bpy.data.objects if o.type == 'MESH']
pts = np.array([list(o.matrix_world @ Vector(c)) for o in objs for c in o.bound_box])
lo, hi = pts.min(0), pts.max(0)
size = float(max(hi[:2] - lo[:2]))
frame = max(size, 1.4 * float(hi[2]))  # tall buildings (the Hive's necks) need the camera further back
ctr = Vector(((lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, hi[2] * 0.35))

scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.light = 'STUDIO' if shaded else 'FLAT'
scene.display.shading.color_type = 'SINGLE' if shaded else 'TEXTURE'
scene.render.resolution_x, scene.render.resolution_y = TILE
scene.render.film_transparent = False
scene.world = bpy.data.worlds.new('sky')
scene.world.color = (0.47, 0.55, 0.65)
bpy.ops.mesh.primitive_plane_add(size=size * 8, location=(ctr.x, ctr.y, 0))
ground = bpy.context.object
ground.name = 'ground'
ground.data.materials.append(bpy.data.materials.new('ground'))
ground.data.materials[0].diffuse_color = (0.28, 0.26, 0.22, 1)
ground.color = (0.28, 0.26, 0.22, 1)
cam = bpy.data.objects.new('camera', bpy.data.cameras.new('camera'))
scene.collection.objects.link(cam)
scene.camera = cam
cam.data.clip_end = size * 40

views = [('south', 270, None), ('south-east', 315, None), ('east', 0, None), ('north-east', 45, None), ('north', 90, None),
         ('west', 180, None), ('from above, south-west', 225, 40.0), ('sprite camera (compare with the sprite)', 270, elev)]
tiles = []
for name, az, el in views:
    a = math.radians(az)
    if el is None or name.startswith('sprite'):
        d = frame * 1.35
    else:
        d = frame * 1.6
    if el is None:  # eye level
        loc = Vector((ctr.x + d * math.cos(a), ctr.y + d * math.sin(a), EYE))
        cam.data.type, cam.data.angle = 'PERSP', math.radians(60)
    else:
        e = math.radians(el)
        loc = ctr + d * Vector((math.cos(e) * math.cos(a), math.cos(e) * math.sin(a), math.sin(e)))
        cam.data.type = 'ORTHO' if name.startswith('sprite') else 'PERSP'
        tall = (hi[1] - lo[1]) * math.sin(e) + hi[2] * math.cos(e)  # the picture's height through this camera
        cam.data.ortho_scale, cam.data.angle = max(size, tall * TILE[0] / TILE[1]) * 1.15, math.radians(50)
    cam.location = loc
    cam.rotation_euler = (ctr - loc).to_track_quat('-Z', 'Y').to_euler()
    scene.render.filepath = '%s_%d.png' % (out, len(tiles))
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(scene.render.filepath)
    px = np.empty(TILE[0] * TILE[1] * 4, np.float32)
    img.pixels.foreach_get(px)
    tiles.append(px.reshape(TILE[1], TILE[0], 4))  # bottom row first
    bpy.data.images.remove(img)
    os.remove(scene.render.filepath)

cols = 4
rows = [np.concatenate(tiles[r * cols:(r + 1) * cols], 1) for r in range(len(tiles) // cols)]
grid = np.concatenate(rows[::-1], 0)  # Blender images start at the bottom
g = bpy.data.images.new('grid', grid.shape[1], grid.shape[0])
g.pixels.foreach_set(grid.ravel())
g.filepath_raw, g.file_format = out + '.png', 'PNG'
g.save()
bpy.data.images.remove(g)
bpy.data.objects.remove(cam)
bpy.ops.wm.save_as_mainfile(filepath=out + '.blend')
print('wrote %s.png, %s.blend (views: %s)' % (out, out, ', '.join(v[0] for v in views)))
