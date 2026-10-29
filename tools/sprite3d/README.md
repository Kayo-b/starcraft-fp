# sprite3d — HD sprites seen from first person, in their own art

Research tools (Python, numpy + Pillow; `zero123_views.py` also needs torch/diffusers) for turning Remastered HD sprites,
which are drawn from a fixed camera above the battlefield, into something that reads correctly from a first-person
camera at eye level and from every side. Focus: the units that look wrong in first person (the Dragoon) and buildings.
Run from this directory: `python3 demo.py IMAGE_ID FRAME RADIUS OUT.png [facing]`.

## What the HD sprites carry (measured on real files, 2026-10-20)
Every `anim/main_NNN.anim` layer is the same atlas layout as `diffuse` (see `anim.py`, the numpy reader of all layers).
- `normal` (DXT5): screen-space normal, **R = x (right), A = y (up)**, z = sqrt(1 - x² - y²) toward the camera.
  Check: on Terran buildings, roofs and south walls form two clusters 91-92° apart (`elevfit.py`).
- `ao_depth` (DXT5): **G = ambient occlusion, A = depth along the view ray**, nearer = larger, one absolute scale for every
  sprite (Marine spans 24-76, Nexus 17-255; the Nexus agrees only with its own camera, below). Not height above the ground: depth gradients follow the normals with zero
  offset on both axes (`depthkind.py`).
- The sprite camera: **orthographic, looking north, tilted down 35°** (roofs at -55..-57°, walls at +35..+37°:
  `elevfit.py`; the silhouette hull of a turning Marine agrees, 30-40°: `calib.py`). **Depth scale: D = alpha - 0.6 * w**
  (w in HD pixels along the ray; roof and wall slopes give 0.49-0.68 across ten buildings: `betafit.py`).
- Units: 17 drawn directions (0 = facing north, 16 = south); directions 17-31 are mirror images, i.e. a mirrored model,
  so an asymmetric unit (the Marine's rifle hand) only agrees with the 17. **The Dragoon does not turn**: in every
  direction its body faces the camera and only the legs change (true of the original game too).
- Buildings are one frame: one view, from the south and above. Their east, west and north faces are never drawn. Lifted
  at 35 degrees they come out deeper than their footprint (Supply Depot: 80 px deep on a 64 px footprint).
- The depth layer is 8-bit and DXT5-compressed: it steps by 1-2 levels from pixel to pixel (`denoise` smooths it within
  each part). Some sprites saturate at their nearest parts, at a per-sprite limit: Nexus 255 (2.8% of its pixels, the
  front spire), Gateway 204 (2.7%); `unclip` continues those runs as vertical surfaces, which the normals there confirm.
- **The Nexus does not fit the 35-degree camera.** Its four base discs (S, E, W, N) stand level only with
  beta = -0.565 * tan(elev) / tan(45): at 35 degrees that is beta = -0.40 and the building is 1.27 times deeper than wide;
  at 45 degrees beta is the usual -0.565 and the discs form a square around the crystal, as its four-fold design needs.
  `CAMERA_BY_IMAGE` in `relief.py` holds it. Open question: 45 degrees would also explain the Supply Depot's extra depth
  (sin 45 / sin 35 = 1.23), but then the Terran roofs and walls would be tilted 10 degrees, so the other buildings keep 35.

## Approaches tried
| | Result | Art style | Hidden sides | Cost |
|---|---|---|---|---|
| **Relief from the sprite's own depth** (`relief.py`) | Exact geometry of everything the sprite shows; every frame and animation | the original pixels | not drawn (see below) | no ML; per frame, cheap |
| Zero-1-to-3 XL (`zero123_views.py`, user's suggestion) | Dragoon: plausible eye-level and back views. Buildings: often garbled; the Nexus barely leaves the top-down view | close, but invented and blurrier (256x256) | invented, and views disagree with each other | 2.7 GB VRAM, ~3 s/view on the RTX 3060 |
| **TRELLIS-image-large on a free Kaggle T4** (`kaggle/`, `trellis_fit.py`) | Nexus and Gateway: complete, coherent buildings from every side, fixed in the world; the three seeds agree | close (textures follow the sprite), TRELLIS's own colours (redder Nexus horns) mapped onto the sprite's palette | generated, consistent | ~65 s and 10.2 GB per mesh on the T4; 13 min for 6 meshes including setup |
| pixelNeRF (not run) | Needs a prior learned per category (ShapeNet/DTU) and perspective cameras; blurry where unseen. The relief gets exact geometry without it | - | blurry | training data would have to come from the turning units |

Previews (`demo.py`; eye 12 game px, FOV 66°, and one view from high ground): `out/sprite3d/*_relief.png` (rows: today's
billboard, relief fixed in the world with a mirrored back, relief as recommended below) and `out/sprite3d/*_zero123.png`;
`out/sprite3d/v1/` holds the first sheets (before `denoise`, `unclip`, the Nexus camera and the edge rule below).

Drawing the relief (`preview.cells`): neighbouring pixels are joined into a surface unless the edge between them is a
step, longer than 6 HD px and 1.6 times longer than the edges before and after it (nothing over 12 px is joined); pixels
in no surface are dropped. Cutting every edge over 6 px instead left lone pixels floating ("sparkle") and cut surfaces
seen at a grazing angle (the Dragoon's soles and leg edges, the Nexus's railings) into combs.

Also tried: the Nexus rebuilt from four copies of its south quarter. The spires dominate and the arms break up; the
mirrored back and the ±40° swing both look better.

## Generated sides: TRELLIS on Kaggle (2026-10-20)
- `trellis_inputs.py` writes sprite frames as RGBA PNGs into `out/sprite3d/kaggle_in/` and the metadata of the private
  Kaggle dataset `kayo01/sc-sprite3d-inputs` (`kaggle datasets create|version -p ...`). Game art: never in git, never public.
- `kaggle/trellis_kernel.py` (private kernel `kayo01/sc-sprite3d-trellis`, T4): `kaggle kernels push -p kaggle`, then
  `kaggle kernels output kayo01/sc-sprite3d-trellis -p DIR` for `NAME_sSEED.glb`, `.npz` and 8-view previews. The image is
  Python 3.13 / torch 2.11 / CUDA 12.8: xformers 0.0.35, nvdiffrast and mip-splatting's Gaussian rasterizer install (the
  last needs `#include <cstdint>`); spconv does not (its cumm dependency rebuilds itself on import and fails), so the
  kernel carries a plain-PyTorch submanifold convolution in spconv's place (TRELLIS only uses those at inference; weights
  are (out, kx, ky, kz, in); checked against `F.conv3d`, max difference 6e-6). TRELLIS must be cloned with its
  FlexiCubes submodule; kaolin (one shape assertion), rembg and open3d are stubbed.
- `trellis_fit.py IMAGE_ID FRAME MESH.glb RADIUS OUT.png` (venv python, needs trimesh): turns, scales and moves the mesh
  until its outline from the sprite camera covers the sprite (IoU 0.84 for both), matches its depth to the relief, then
  draws: the relief plus the mesh's hidden side; the mesh alone on the sprite's palette (per-channel quantile mapping);
  the mesh in TRELLIS's colours. Projecting the sprite's pixels onto the mesh speckles (the fit is not pixel-exact).
- Results: `out/sprite3d/nexus_trellis.png`, `gateway_trellis.png`; meshes and TRELLIS's own previews in
  `out/sprite3d/trellis/`.
- Nexus arches: TRELLIS fuses each arch with the leg that stands behind it in the picture (a pillar with a bulb).
  `relief.part_mask` cuts a free-standing part out of the lifted sprite in 3D (within 60 HD px of its foot, plus what
  rises above 90 px within 100 px); TRELLIS on the west arch's cut-out, seed 2, makes the arch right (a curved horn with a
  hooked tip; outline IoU 0.85 against the cut-out). The four arches are one design turned by 90 degrees (east and west
  show their curved side to the sprite camera, south and north their edges): fitted freely, the west arch's mesh covers
  the south and east cut-outs at turns of +105 and +190 degrees. `trellis_compose.py` cuts the body's four fused pillars
  away and puts that arch on each disc (same size, turn within 20 degrees of 90 k, placed over its own cut-out: IoU
  0.85 / 0.77 / 0.68 / 0.58 for W / S / E / N, the last half hidden behind the crystal), drops TRELLIS's floating
  specks, and writes one GLB.
- Nexus pyramid: it has one face with the stairs (south-west) and three alike (white and green stripes). From the sprite
  camera the tops of the two back faces show as green-tiled bands beside the crystal; TRELLIS makes them eaves standing
  out over the stair face's two ridges (the other ridges come out clean). `wedge:X0:Y0:A0:A1:TURN` rebuilds the 20-degree
  strips on both sides of those ridges from the clean north-east face's ridge strips, turned into place. The axis the
  body repeats around every 90 degrees is (1, 11) HD px (median height mismatch 4.8 px; 17 px around the crystal's
  centre, which left dark slivers); the triangles are cut along each strip's sides first (picked whole by their
  centres, TRELLIS's large triangles left spiky seams).
  `trellis_compose.py 179 0 nexus_s2.glb 950 OUT nexus_arch_w_s2.glb:-235:-11:4:5.5:-10.5 wedge:1:11:160:180:90
  wedge:1:11:180:200:180 wedge:1:11:250:270:180 wedge:1:11:270:290:-90` -> `out/sprite3d/nexus_composed.glb`.
- `blender -b -P blender_views.py -- IN.glb OUT [ELEV]` renders a GLB unlit in Blender (rows: south, south-east, east,
  north-east / north, west, from above south-west, through the sprite camera) and saves OUT.blend; a .glb opens in
  Blender with File > Import > glTF 2.0, not File > Open. -> `out/sprite3d/nexus_composed.png`, `nexus_composed.blend`.
  Turned rigidly around the midpoint of the east and west feet instead, the copies miss by ~30 HD px. Before the arch
  meshes were compared seed by seed, each arch's own relief extruded 30 px was swapped in (`trellis_fit.py ...
  relief:CX:CY`, `nexus_trellis_arches.png`): the original art, but a flat panel from the side.
- Dragoon, TRELLIS alone (standing frame 0, mid-stride frame 68, 3 seeds each): it stands the walker on a square ground
  plate spanning the whole 64^3 grid (its near-black lower legs on TRELLIS's black input background), so the walker gets
  only ~0.3 of the grid (about 20 voxels across) and comes out lumpy, with stub legs and the face plate smeared.
  `trellis_fit.load_glb` drops the plate and its fragments.
- Dragoon on the SC Evo Complete mod's shape (`sc2_structure.py`; user's idea): the mod (MIT, local copy in
  `starcraft-data/SCEvoComplete`) has a Dragoon modelled after the original, which matches the HD sprite's outline from
  the sprite camera as well as TRELLIS's own (IoU 0.68). `m3_obj.cpp` (built against `src/fp_m3.h`) writes it as OBJ;
  its surface voxels, filling the grid and turned 180 degrees as TRELLIS turns this picture, go to the kernel as
  `NAME_coords.npy`, and only TRELLIS's second stage runs (details and colours from the sprite; ~56 s per seed, 10.4 GB
  peak; it must run under `torch.no_grad()`, as `pipe.run` does, or the mesh decoder runs out of memory). Result: the
  mod's walker in the sprite's own look (gold shells, silver frame, the emblem plate), solid from every side; through
  the sprite camera it reads as the sprite. `out/sprite3d/dragoon_sc2_s0-2.glb/.png/.blend`, `dragoon_compare.png`
  (sprite; the mod's model as is; TRELLIS alone; the three seeds). One pose: the mod's rest pose, no animation. From eye
  level the undersides are dark, as the sprite's lower legs are (lighting baked into the art).
- Training TRELLIS on the mod's models instead (asked): not done. Its flow models do not fine-tune on a 16 GB T4, renders
  of the mod's models carry SC2's look rather than the original's, and for a unit the mod has, giving TRELLIS that
  shape is what a fine-tune would at best learn.

## Models to open in Blender; Remastered pixels as texture (2026-10-20)
Best versions so far are copied to `starcraft-data/sprite3d-models/v1/` (README.txt there; the user edits them, the tools
never write there). Rule from the user: the look must be Remastered's own art, its pixels the texture; the mod's models
only fill shape gaps the sprite's angle cannot show.
- `relief_mesh.py IMAGE_ID FRAME OUT.glb [back]`: the relief as a mesh, one vertex per sprite pixel, quads as
  `preview.cells` keeps them, textured with the sprite frame itself (UV = pixel centre; exact Remastered pixels), the
  mirrored back as a second object. Supply Depot: 121k triangles per side.
- `trellis_compose.py ... sprite-texture`: rebakes every piece's texture (2048^2): the sprite's pixels wherever the sprite
  camera sees the piece (its own nearest surface at that pixel, within 4 HD px along the ray; blended out on surfaces
  turned away, cosine 0.4 -> 0.15, so grazing walls are not smeared), the piece's own colours elsewhere mapped onto the
  sprite's (per-channel quantiles fitted on the texels that have both). The piece matches the sprite only to a few
  pixels, so the sprite is first aligned to it by optical flow (OpenCV DIS, at most 8 px) from the piece drawn in its
  own colours through the sprite camera. Testing visibility against the sprite's depth instead textured only 11% of the
  Gateway (TRELLIS's surfaces sit tens of px off it along the ray); the mesh's own depth buffer gives 24%. Nexus:
  `nexus_trellis_remastered`, 18-24% per piece.
- `trellis_compose.py` also takes the mod's model as BODY (`out/sprite3d/sc2/NAME.obj`, from `m3_obj`): it gets a fresh
  UV atlas (xatlas; the mod's UVs tile and share texture space) holding its own texture. The mod's Nexus fits the sprite's
  outline poorly (IoU 0.56, TRELLIS's 0.85): flatter, other arches. `out/sprite3d/nexus_compare_textures.png`.
- Remastered HD sprites also carry `teamcolor` (the player-colour areas: white in the diffuse, tinted in game),
  `emissive`, `bright` and `specular` layers.
- The Command Center is `main_063.anim` (image 275 shares `control.grp` with image 63; anim files are numbered by the
  first image using a graphic). `trellis_inputs.py command_center:63:0`.
- Hidden sides from the sprite too (`sprite-texture`, on by default): a surface the sprite does not show is mirrored
  across the east-west plane through the piece's centre or turned about its vertical axis (30-degree steps); the first
  of these that lands on a shown surface (within 8 HD px of what the sprite shows there, normal within ~37 degrees)
  gives its pixel, so a ramp only on the front is not painted onto the back's plain wall. What is still unpainted
  takes the pixel of the most alike point the sprite shows well (`alike_features`: height, distance from the centre,
  normal against the radial direction, and closeness to the point folded onto the south half, which keeps neighbours
  on neighbouring source pixels); surfaces facing down are darkened. Only beyond that do the piece's own colours stay
  (TRELLIS paints hidden sides near-black). Command Center: 14% direct, 10% moved, 54% alike.
- Command Center (2026-10-20). The relief alone is a sheet that splits from the east (a round hull: nothing behind
  the front). TRELLIS seeds 1 and 2 fit the outline well (IoU 0.85/0.86; seed 0 is a generic symmetric hull), seed 1
  with the sprite's stance (hull raised on the domed feet), but the sprite shows only 21% of its surface and TRELLIS
  made the rest a smooth sloping skirt, near-black in its own colours; mirroring finds a matching shown surface for
  only 12% more. The mod's Command Center fits as well (IoU 0.84-0.87) and is complete all round (ring hull, strut
  legs), but through the sprite camera it is less the sprite (its own tower; 11% textured directly). So the structure
  is now both (`sc2_structure.py ... 63 0`): the sprite's relief where it shows the building, the mod's solid behind
  it, then TRELLIS stage 2 on that (kaggle_in/command_center_mix, 14.8k voxels; `voxel_mesh.py` to look at it).
  Result `command_center_mix_s0_remastered` (outline IoU 0.87, TRELLIS put it at yaw 235 as asked): a hull on legs all
  round, the sprite exactly through its camera, the mod's design behind. The three seeds differ little. Side by side
  with the relief, TRELLIS alone and the mod: `command_center_compare.png`. The user chose the mod's model with the
  Remastered texture (`command_center_mod_remastered`, now in v1).
- Hatchery, Lair, Hive (anims `main_070`, `main_074`, `main_072`): the mod has all three in one model,
  `HatcherySCBW.m3` (the Lair and Hive actors point at it too), each form with its own bones and texture (t2 Hatchery,
  t1 Lair, t0 Hive). The rest pose is not the building (the Hive's necks lie folded); `m3_pose.py` poses it by an
  animation ("Stand", "Stand B", "Stand C": bones from the first keys, 4-bone skinning, IREF stored column-major) and
  keeps one form's triangles (`--only N`); the other forms are hidden by something other than bone scale.
  Posed, each form reads as its Remastered sprite. Fitted (`trellis_compose.py ... BODY.obj ... sprite-texture`,
  one turn for all three, `yaws:`) and painted with the sprite's pixels: outline IoU Hatchery 0.69, Lair 0.59 (it
  alone fits best turned 135 degrees, IoU 0.64, which the other two contradict), Hive 0.69. TRELLIS on the sprites
  (seed 0) fits closer: 0.75, 0.78, 0.79. Side by side: `zerg_hive_compare.png`. Kaggle: hole filling in TRELLIS's GLB
  export ran out of memory on the Hatchery's seed 2; the kernel now retries without it and skips a failing seed.
  The user picked the mod's three forms, and the mod's model with the Remastered texture for every building from now
  on.
- `mod_batch.py`: every building the mod has a model for (image ids from the game data via openbw, see its docstring),
  posed by "Stand", fitted, painted with the sprite's pixels and rendered, into `out/sprite3d/mod/`. Not in the mod:
  Starport, Physics Lab, Armory, Hydralisk Den, Spire, Greater Spire, Ultralisk Cavern, Nydus Canal.
  Two texture fixes found on the Supply Depot: the alike fill now copies a patch of sprite per triangle (laid out
  along the surface, `surface_axes`) instead of a pixel per sample, which scattered pixels from all over; and texels
  no random sample landed in (~5% at 3 per texel) take their nearest painted neighbour, not the model's own colours
  (which showed as white specks).
  Found in the first full run (all 38, outline IoU 0.13-0.90, `all_buildings.png` beside the sprites):
  - Parts entirely below the ground are dropped before fitting: several models keep pieces there in "Stand" (the
    Sunken Colony's buried tentacle down to -5 units, half the Barracks' triangles as construction pieces that rise
    out of the ground, the Stargate, Forge, Photon Cannon). Sunken 0.13 -> 0.80, Barracks 0.67 -> 0.85, Stargate
    0.66 -> 0.80.
  - Add-ons have "Stand" and "Stand Attached" (attached to their building). Only the Control Tower reads as its sprite
    attached (0.64 -> 0.73, with its lattice tower; `SEQUENCE`); the Comsat Station, Nuclear Silo, Covert Ops and
    Machine Shop look better in "Stand" (attached versions kept in `out/sprite3d/mod/attached/`).
  - `m3_fx.py`: `m3_obj` (as the game) keeps only opaque materials, so blended parts are missing: the Fleet Beacon's
    energy sphere, the Gateway's crystal, the Robotics Facility's glass dome. They are added back as plain geometry
    for the sprite's pixels to paint (`SOLID_FX`); the additive glows and ground decals stay out.
  - A texture from the base game (the Science Facility's diffuse map, not in the mod) is plain grey; it only shows
    where nothing in the sprite fits.
  - The Photon Cannon (image 183) rises out of a flat ring over its 4 frames; frame 0 is the lowered one, and so is the
    mod's model in "Stand".
  - Distortion: painting with the sprite's pixels puts them on the wrong parts wherever the mod's shape differs from
    the Remastered building. Rendered with the mod's own texture (`out/sprite3d/mod/own/`): the Machine Shop is another
    design (a block marked "37", not the radiator, pipes and cylinder), the Control Tower has a striped box where the
    sprite has its dish, the Observatory's pillars stand elsewhere (two of its three blue heads land on gold pixels),
    the Missile Turret's pods are much larger, and the Shield Battery is all thin spikes (a few px off is background).
- `trellis_compose.py ... recolour` / `mod_batch.py --recolour` (the user's suggestion): the mod's own model, UVs and
  textures at full detail, recoloured to the sprite's palette, into `out/sprite3d/mod/recolour/`; no smearing, but the
  details are the mod's. Recolourings tried on six buildings: a lookup learnt per point (each mod colour -> the median
  sprite colour where it lies), its robust affine version, and a mix all turned the yellow hazard stripes white and the
  Nexus crystal black (most pairs are grey, and misplaced ones pull every colour to grey); per-channel quantiles kept
  some accents; iterative distribution transfer (Pitie 2005, the whole 3D colour cloud matched by ranks) kept them all
  (stripes, the Command Center's orange, the Nexus's teal and blue crystal, the Spawning Pool's green) and is the one
  used. Compare: `recolour_compare.png` (sprite | sprite pixels | recolour).
- The user's picks (2026-10-21): recoloured for most Terran, Zerg and Protoss buildings, painted for the rest (list in
  `starcraft-data/sprite3d-models/v1/README.txt`), with three fixes:
  - Recoloured Protoss gold read washed out. The colour cloud does match the sprite's (Lab L, chroma and hue
    percentiles agree on the sampled points), but the mod's broad panels are its lightest colour, so ranking gives them
    the sprite's highlights: the sprite has its lighting baked in, the mod's diffuse maps have none. Tried: optimal
    transport (exact assignment between samples, lightness down-weighted so hues pair with hues) took the Stargate's
    pale cyan rims off but lost the Nexus crystal and was no deeper; a chroma boost on yellow-orange hues was, and
    darkening it too turns the Stargate's light yellow olive. Used: `gold:1.6:6` (`colour_map`) for every Protoss
    recolour.
  - Glow maps: the game draws diffuse + emissive x multiplier (`fp_render.h`); `m3_obj` now writes each batch's glow
    ("# glow tN MULT") and `load_obj` adds it into a texture of its own, so the recolour starts from what the game shows.
    The colours it learns from now leave out parts without a mod texture (the Assimilator's dome, the Gateway crystal,
    the Fleet Beacon sphere), whose sprite colours had been spread over the rest (the Assimilator's sandy fins went cyan).
  - Assimilator: no "Stand" (only "Stand B"/"Stand D", the same pose); its dome is in the model twice, full (region 5)
    and depleted (region 2), both opaque, so both drawn they fought (jagged holes): `m3_fx.py ... drop:2`.
  - Holes: `Piece.without`'s speck filter (TRELLIS leaves loose bits around its meshes) also ran on the mod's models,
    and faces joined only by edges three or more faces share (trimesh's adjacency skips those) counted as loose: it
    cut 148 faces out of the Assimilator's dome (the jagged hole) and real small parts elsewhere, up to 4% of the
    surface (Missile Turret 4.4%, Shield Battery 3.8%, Pylon 1.9%, Control Tower 1.5%, Photon Cannon 1.2%, Stargate
    1.0%; 21 of the 39 batch buildings; the Hatchery, Lair and Hive 0.0%). It no longer runs on SC2 models; the 21
    were rebuilt (old ones in `before_fix/`, `recolour/before_fix/`, `recolour/before_gold/`).
  - Creep Colony: every animation stands its tentacle about twice as tall as the sprite's ribbed bulb, so the bulb's
    pixels were stretched up it and its top took the yellow spikes. `squash:0.5:0.5` shrinks the model above 0.5 units
    by half before fitting; the fit then turns it 180 degrees (IoU 0.71 -> 0.75) and the bulb gets its own pixels.
- In the game (2026-10-22, `./run.sh --test-base`): the Nexus's pyramid read small beside the Command Center's body at
  eye level (about 0.65 as wide, 0.75 as tall; its arches make its box the wider one). The user asked for 1.4x:
  `size:1.4` (`trellis_compose.py`, `mod_batch.py` SIZE) scales the finished model around the unit's position; the
  rebuild is the old one times 1.4 exactly (same faces, UVs, textures; old ones in `before_size/`). 202 x 202 px on a
  128 x 96 footprint. Next look (2026-10-23): the Nexus "still small", 1.4x again (`size:1.96`, 1.4x builds in
  `before_size/nexus_1.4/`), and the Pylon too big: "2/3 the size of a gateway". It was 0.77 of the Gateway's height
  (77 vs 98 px; already well under 2/3 of its width), so `size:0.85` makes it 2/3 as tall.

## Eye-level unit sprites: the Marine (2026-10-25)
Goal (the user's): the Marine as a 2D billboard ("cardboard"), but drawn from eye level in the Remastered art. Two
routes were asked for: (1) an image model fine-tuned on the original art that draws eye-level frames, (2) voxels of the
sprites, reshaped for the eye-level view, with GazPrash's 2d-to-3d-voxelizer.
- **Camera, measured on the Marine** (`unit_voxel.calibrate`, the silhouette hull of the 17 drawn directions against
  each silhouette): the turning axis is **6 HD px right of the box centre** (column 162.5 of 313), and the tilt that
  fits best is **31 degrees**, not the buildings' 35. With both, the hull reprojects onto all 17 silhouettes at IoU
  0.992 (worst 0.984); on the centre column at 35 degrees it was 0.89. Standing and walking frames agree.
- **Depth scale differs for units**: the hull's upper depth envelope gives w = c - 1.28..1.34 D (beta -0.75..-0.78), not
  -0.6, and one offset c for all 17 directions (49.8-51.4), as a turntable should give. With -0.6 the depth carving cut
  a third of the Marine away.
- `unit_voxel.py ANIM GROUP OUT.npz [TILT AXIS]`: one pose (frame group) as voxels, 1 per HD px: silhouette hull, then
  emptied in front of the surface each direction's depth layer shows (two directions must agree), specks dropped,
  the cone under each foot cut off (`cut_soles`, v3).
  Surface voxels keep the colour of the direction that faces them most (the left side from the mirrored directions
  where their depth agrees, else the nearest coloured voxel). Marine standing: 131k voxels, ~3 s. Through the sprite
  camera the model covers each frame at IoU 0.96-0.99.
- `eyeview.py`: draws a model in any direction from any height. `render` uses the voxel colours (blocky, speckled:
  neighbouring voxels come from differently lit directions); `render_vdt` is **view-dependent texturing**: each pixel
  finds its surface point and samples the original frames there (the same direction weighted most, sigma 6 degrees,
  only where that frame sees the point), so seen from eye level in direction d it is mostly frame d's own pixels on
  the true shape. 25-30% of eye-level pixels are surfaces no frame shows (under the shoulder pads, behind the rifle,
  the lower legs, hull filler in hollows no direction can carve): filled from the seen pixels around them in the
  picture, 0.8x darker. `eyeview.py check|sheet|zoom`.
- **GazPrash's voxelizer** (headless build: `cmd/headless` + `vox2d3d` in
  `diablo-hellfire-fp/research/2d-to-3d-voxelizer`, same backend as the app; `gazprash_unit.py`, quad sheet
  [direction 8, 16, 8 mirrored, 0 mirrored]): it extrudes the front picture and trims it by the side pictures, i.e. it
  expects level side views. On the Marine it shows its four input pictures on a slab, so they still look down from
  31 degrees, and the diagonals double up (two Marines, streaks at 10 degrees). It cannot change the viewpoint.
- **Zero-1-to-3 XL** (`zero123_eyelevel.py`, pretrained, each direction lowered by 26 degrees): lowers the camera, but
  invents (the visor becomes a blob, a second rifle at the front), 256 px and soft, and each frame is generated on its
  own, so 221 frames would flicker.
- Route (1) as asked does not have the data it needs: the Remastered frames are all drawn from 31 degrees, so a model
  fine-tuned on them learns the style but has no eye-level picture to learn the view from. The viewpoint has to come
  from a 3D prior (Zero123: invents) or from geometry (the voxels). Where training does fit: a refiner trained on
  leave-one-direction-out renders of the voxel model (`render_vdt` without frame d, through the sprite camera ->
  frame d itself), which teaches exactly the fixes the eye-level frames need (the filled patches, resampling softness)
  from the original art; not built yet.
- Side by side: `out/sprite3d/marine/compare_g0.png` (frame | GazPrash at 0 and 10 degrees | voxels + frame pixels at
  0 and 10 degrees), `zero123_g0.png`, `game_compare.png` (fp_view, today's billboards above, eye-level below).
- `eyelevel_anim.py ANIM OUT_DIR --elev 5 --calib 31,6`: every directional group (Marine: 13, i.e. standing, aiming x2,
  firing, walking x9) drawn at 5 degrees in the 17 directions, the non-directional frames (death, 221-228) copied
  with their axis and ground line moved onto the new box's; written as a drop-in anim (diffuse DXT5 + teamcolor DXT1,
  16-byte frame records, the turning axis on the centre column) plus `sheet.png` and `models/gNN.npz`. ~4-6 min for
  the Marine on 7 processes. The game reads it from `--eyelevel DIR` (branch fp-eyelevel, worktree
  `../starcraft-fp-eyelevel`; run.sh adds `starcraft-data/eyelevel` when it exists, V toggles).
- **v1** (user, 2026-10-25: "good results, this can be v1 for marines sprites"): frozen in
  `starcraft-data/eyelevel-versions/v1`. The user then saw two faults: it is blurrier than the art and loses detail,
  and there is a dark "shadow" at the base of the feet on some angles.
- **v2** (now in `starcraft-data/eyelevel`, also `eyelevel-versions/v2`):
  - Blur was the pipeline's: drawn through the sprite camera, v1 kept 52% of a frame's sharpness (mean |Laplacian|)
    at 26.5 dB, even with that very frame available (`eyeview.py vdtcheck`). It averaged 16 bilinear samples per
    pixel and blended neighbouring directions (sigma 6). One sample per pixel (the centre of its front-most covered
    subpixels; coverage still from 4x4), Catmull-Rom clamped to the 4 nearest pixels, and the nearest direction's frame
    wherever it sees the point (sigma 2, relative to the nearest direction's angle) keep 98% at 32.9 dB (73% with
    the frame left out; nearest-neighbour keeps 93% but jags). Also 4x faster (75 s for the Marine).
  - The shadow was the soles: carved feet taper to a point underneath (12 voxels at the floor, ~1100 ten layers up),
    and at eye level that underside was painted with the darkened fill. v2 carried each foot's widest low section
    down to the floor (`flat_soles`, since replaced by `cut_soles`, see v3).
  - `out/sprite3d/marine/v1_v2.png` (frame | v1 | v2 at 5 degrees, 3x), `game_v1_v2.png` (fp_view: original, v1, v2).
- **v3** (now in `starcraft-data/eyelevel`; user on v2: "the shoes still have huge soles that must be removed"): the
  flat soles had made the cone into a block, a band below the boots' studs. The cone is not boot at all: from 31
  degrees above, a point under a sole stays inside the foot's silhouette in every direction until about 0.6 x the
  foot's half-length down (tan 31), so carving never removes it (9 layers under each Marine foot, 9-10 walking).
  `unit_voxel.cut_soles` cuts it off at the real sole (the first layer with 95% of the foot's largest section) and the
  model is grounded again. `out/sprite3d/marine/feet_v2_v3.png`, `v2_v3.png`, `game_v2_v3.png`.
- **All units** (2026-10-25, the user: "apply this same process we did on the marine with all other units", Terran core
  first, then Zerg, then Protoss with the Dragoon last). `eyelevel_batch.py OUT terran|zerg|protoss|NAME...` runs
  `eyelevel_anim.py` per unit and writes a side-by-side sheet (`compare.png`, read back from the written anim).
  Found on the way:
  - **Units were drawn from different heights**: calibrated tilts Marine 31, Medic 41, Firebat 44, SCV 49 (the SCV's
    hull IoU peaks at 50 degrees as clearly as the Marine's at 31); the search now runs 20-60 degrees.
  - Steeper art leaves more unseen at eye level: SCV 47% of its eye-level pixels (5 degrees; 34% at 20 degrees, so it
    is the hollows, not the height), Marine 17%. `render_vdt(relax=16)`: a point no frame sees, but at most 16 px
    behind what the nearest direction shows, takes that pixel instead of the blur (SCV 53% -> 22%).
  - The Medic has no depth layer (ao_depth alpha all 0; a Brood War unit): carving read 0 as "far" and cut 80% of
    it into slivers. Depth 0 now means no depth (also on glowing parts of other units), and a unit without depth gets
    the silhouette hull only.
  - Frames that stick out of the box (the Firebat's): `anim.Anim` grows the box evenly, so its centre stays the unit's
    position.
  - Eye-level files keep the original box-centre convention (the box centre is the same point of the unit), so the
    game composes turrets on hulls and copied frames line up; the box grows evenly when eye-level poses need room.
    The original art's tilt is kept in the sprite record's second u16 (tenths of a degree) for the turret offsets.
  - Game side (fp-eyelevel): the Siege Tank's turret was not drawn at all (OpenBW keeps it as a subunit); it is now,
    on the hull's box centre plus OpenBW's turret offset, scaled by cos 5 / cos(tilt) at eye level. The Lurker drew as
    a flat box (image 921 has no file; its GRP name differs from the remnants' only by case): images fall back to the
    first image with the same GRP, compared ignoring case.
  - Siege mode stays original art: its hull is 6 frames of one view (no directions to carve from).
  - **The cone under a unit scales with it**: a flat bottom of half-size r seen from tilt t leaves a cone r tan t
    deep (9 voxels under a Marine boot at 31 degrees, ~115 under a Siege Tank at 45). A per-foot cone rule (first
    layer at 95% of a foot's section) took a running Zergling's legs (legs and belly are one part near the ground) and
    a turret's base ring (wider at its barrels). `unit_voxel.cut_underside` replaces it: in every column of the lower
    part (as deep as the cone can be, 0.5-0.9 of the height), what lies below the lowest surface some direction sees
    goes; across columns with none low down (inside a leg, under a belly) the heights of the seen rims are filled in,
    so an underside becomes a membrane between its seen rims. Flyers and hovering units keep theirs (`--no-soles`).
  - **Turrets**: OpenBW's turret offset is for the hull's absolute heading seen from the original's camera; the game
    now takes the table entry for the hull frame it draws (fp_render.h `turret_lo`), and at eye level fits the turret
    point in 3D from the 17 directions (`turret_point`: the Siege Tank's is 13 px behind the hull's centre, 14 up).
    `turret_seat.py` gives the hull roof under that point in the turret's frame, and `--floor` cuts the turret there.
    (fp_view's test scenes now step one frame after `--spawn`, or turret offsets keep their spawn value.)
  - **Relief fallback** (`relief_eye.py`, `--relief`): poses whose directions do not agree as one turning model are
    redrawn frame by frame from their own depth (each column warped, neighbours within JOIN = 10 px joined, spray
    cleaned). Used for the Vulture (hull IoU 0.83), the Lurker (0.70; many of its groups repeat one frame), the Drone's
    and Zergling's burrowing poses, and the whole Overlord: the carving cannot part its dangling tentacles, which fill
    in as one curtain; by relief they stay strands (thin diagonal ones break up).
    `--relief-tidy` (the Lurker, 2026-10-27; the user: "still has some empty pixels and also floating sections that
    are not connected with the main body"): a frame that is one piece in the original (no second part of 30 px or
    more: its walk and attack, not its death splatter) comes out in one piece. `tidy` opens it 3x3, which removes
    drips and teeth narrower than 3 px. It keeps only the largest part: a claw whose leg the warp lost floats below the
    body, up to 960 px. It closes notches narrower than 3 px and fills holes up to 1500 px: the hollow under the
    Lurker's shell opens 500-900 px holes in groups 9-21, past `clean`'s 400. Added pixels take the nearest colour and
    join the unsure mask for the repaint.
  - Calibrated tilts: Marine 31, Medic 41, Hydralisk 42, Lurker 42 (poor fit), Firebat 44, turret 44.5, tank 45,
    Overlord 46, Vulture 46 (poor), Drone 47, SCV 49, Zergling 38.5.
  - Memory: the Siege Tank's grid bounds are 306 x 306 x 633 (59M voxels, 5 GB a pose; three in parallel ran the
    machine out of memory). A coarse 4-px hull first gives the box the unit is in (13.7M, 2 GB); the batch picks its
    process count from free memory. Never edit these scripts while a batch runs: Python 3.14's process pools start
    workers that import them again from disk.
- **Clean-up model** (2026-10-26, the user's choice after "SCV, firebat images need some work"): `refine.py`, a 3.1M
  U-Net that mends a render in the unit's own art. There are no eye-level frames to learn from, so `refine_data.py`
  makes pairs from the original camera: the voxel model drawn without the frame it stands in for (and, harder, without
  its two neighbours too) against that frame, which carries the eye-level renders' faults (filled surfaces, borrowed
  pixels, a neighbour's lighting and registration, stretching) with a known answer. Input: RGB, coverage, filled and
  borrowed masks (`render_vdt(info=)`); output: an RGB correction (the outline stays). 128-px patches, L1 + gradient
  L1, filled/borrowed pixels double. v1: 2618 pairs from 77 poses (Marine, SCV, Firebat, Medic, tank and turret,
  Zergling, Hydralisk, Drone), 20k steps in 17 min on the local RTX 3060; held-out directions (3, 11): 18.7 -> 21.2
  dB against the frames. `eyelevel_anim.py --refine MODEL` cleans the rebuilt frames.
- **Clean-up v2** (Kaggle, 2026-10-26): 6188 pairs from 91 poses with 'loo5' and 'blob' variants (blobs 20-35% of the
  unit filled as the renderer fills, 28% average; loo1 had 0% filled, so v1 never learnt to paint big areas), plus a
  conditional PatchGAN critic (spectral norm, hinge, 0.03 after 2000 steps), 30k steps on the T4: held-out 17.9 -> 19.7
  dB. At eye level it trades v1's blur for grit and colour speckles; on units that already read well (the Zealot)
  both versions make them worse. Neither is used: the game keeps the plain renders. Leave-one-out pairs carry the
  faults of renders through the original camera, not eye level's (stretching, surfaces seen edge-on), and ~90 poses
  are too few for the critic to recover texture rather than invent it. Steep units (SCV, Firebat) need a better shape
  (e.g. the mod's model posed per frame) more than a better painter. `out/sprite3d/refine/eye_v2_*.png`.
- **The mod's shape for steep units** (`mod_unit.py`, 2026-10-26, Firebat): the mod's FirebatSCBW.m3 posed per sprite
  group (its Stand/Walk/Attack sampled through their lengths, keys interpolated; the pose whose outline covers the 17
  silhouettes best), fitted once (turn, size, place: outline IoU 0.66), and the pose's voxels = the sprite's hull inside
  the posed model grown by GROW voxels (`unit_voxel.build(prior=)`). The mod's Firebat is another design (outline IoU
  0.69-0.75 per pose, slimmer): grown by 2 it cut real parts away (thin, holed), grown by 6 it looks like the plain
  build. The walk times picked jump around the cycle (161, 201, 604 ms...): with this much shape mismatch the pose
  scores barely differ. Not used; the Firebat's blotches are a texture problem (the frames disagree where the steep
  art shows little), not a shape one. `out/sprite3d/eyelevel/firebat_mod_compare.png`, `firebat_mod6_compare.png`.
- **Generative repaint** (`repaint.py`, Kaggle T4, 2026-10-26; the user picked it over the mod's recoloured model): Stable
  Diffusion 1.5 with a LoRA (rank 16, 1200 steps, ~25 min) trained on the unit's own Remastered frames and their
  mirrors, then img2img with ControlNet-tile (lllyasviel/control_v11f1e_sd15_tile) from each eye-level render on a
  512 canvas, strength 0.4, guidance 6, one seed per direction; back to sprite size inside the render's own outline
  (coverage and team mask unchanged). Pilot (strengths 0.3/0.45/0.6, ControlNet 0.6, no LoRA): 0.3-0.45 clean the
  blotches and keep the unit's look; 0.6 invents parts (hoses); without the LoRA it drifts to a generic look. Firebat
  (170 frames) and SCV (51) live (`eyelevel-versions/repaint-v1`): the Firebat's chest is clean armour, the SCV's back
  detailed machinery; detail changes a little between poses, no flicker in the walk cycle. ~3.5 s a frame on the T4.
  Kaggle image: uninstall its old torchao before upgrading diffusers/transformers.
- **Smeared pixels: the DXT encoder** (2026-10-26; the user: some repainted frames look smeared, colours in the wrong
  places, visible pixels as if lower resolution). Most of it was Pillow's DXT5 encoder, used by `write_anim` since the
  first Marine: it picks poor block endpoints, so a 4x4 block mixing red armour and grey metal comes out as a red or
  grey square. On repainted frames it measures 22-23 dB (errors up to 221/255) against 29-30 dB for quicktex at level
  18 (`out/sprite3d/kaggle_refine_pilot/`, tiles cut from the pilot sheet). It hit twice: the render that feeds the
  repaint was already blocky (strength 0.4 keeps the squares as if they were detail), then the repaint was blocky again
  when written. `eyelevel_anim.dds` now encodes with quicktex (BC3/BC1 level 18, 2.8 s for the largest atlas; Kaggle
  jobs need `quicktex` in their pip list) and first gives transparent texels their block's mean visible colour, so
  edge blocks don't spend an endpoint on the black around the sprite. Every eye-level file is re-rendered from the
  same models and options (`out/sprite3d/eyelevel_q/`).
- **Depth-tested texturing and masked repaint** (same day): the rest of the smears are wrong-surface samples, a colour
  taken from a direction where something else hides the point. `eyeview.Sampler(depth_tol=)` keeps a direction's
  colour only where its depth layer agrees with the model's surface there, within 6 levels (`--depth-tol`, default
  6). A point no direction agrees on takes the pixel of a direction that shows a surface at most 16 px in front of it
  (relax), or failing that is filled from the pixels around it. `eyelevel_anim.py` writes those relaxed and filled
  pixels per frame to `NAME.masks.npz` (8-21% of a unit's pixels). `repaint.py --masks` repaints the whole frame at
  `--strength` (0.4), then inpaints the unsure pixels again at `--mask-strength` (0.75; ControlNet 0.6, mask grown 2
  px and blurred). In the pilot, 0.3 and 0.4 look alike, 0.9 adds structure the art doesn't have (bright plates on
  the SCV), and 0.75 replaces the smeared thighs and arms with clean armour. Firebat and SCV redone from the clean
  renders with v1's LoRAs (`eyelevel-versions/repaint-v2`, ~8 s a frame for the two passes): no red/grey squares, the
  side and attack views read as whole armour plates; left over, a small invented green panel on one Firebat fuel tank.
  Medic, Zealot, Marine, Hydralisk and Zergling the same way (LoRAs from the first batch, split over both kernels,
  ~1.5 h each) and live: the Medic's shredded back becomes solid armour, the Zealot's silver wraps and blue gems match
  the art again, the Marine's back and sides read as plates, the Zerg are sharper (a little more purple in shadow, a
  bony pattern on the Hydralisk's back the art does not show). The Siege Tank hull and turret are not used: on hard
  surfaces the repaint invents light panels everywhere, blurs the treads and turns the barrels into machinery (their
  clean renders stay; a lower strength without the mask pass would be the next try).
  Relief frames get a mask too (2026-10-27): `relief_eye.warp` marks every pixel drawn from a source pair spread over
  more than `STRETCH` (2) rows, i.e. a surface the old camera saw nearly edge-on that the eye-level view stretches out
  (the Overlord's hanging tentacles, the Lurker's shell sides, the Dragoon's legs), and `clean` adds the holes it
  fills. 3-30% of a frame; `eyelevel_anim.py` saves them with the rest, the colours unchanged (files byte-identical).
  Drone, Overlord, Vulture, Probe, Lurker and Dragoon then go through the same repaint, each training its LoRA first
  (~28 min on a T4). Only the Probe improves (back and sides read as shell and dome instead of murk; live). The rest
  are worse than their clean renders, because the whole-frame pass at 0.4 redraws what was already right: cockpit glass
  invented on the Vulture's nose and its engines turned to lumps, a face and eyes on the Overlord, swirls on the
  Drone's sides, the Lurker's shell turned red-pink, the Dragoon darker with murky legs. The tank again single-pass at
  0.25 and 0.3 still grows panels (the tile ControlNet adds detail by design).
  Hence **mask-only repaint** (`--strength 0`): no whole-frame pass. The unsure areas are inpainted from the render and
  pasted back at sprite size (`paste`: the unsure pixels plus a 1 px soft ring). Every other pixel stays the render's
  own, since even the round trip through the 512 canvas softens it. The masks are made solid first (`solid`: 3x3
  closing then opening), because a relief stretch mask comes in row stripes and, grown and blurred as-is, would cover
  60-80% of the frame. Pilot at mask strength 0.6, 0.75 and 0.9 (`out/sprite3d/kaggle_mpilot/`):
  - Improved:
    - Overlord: the tentacles get segment rings and the shell sides clean up, while the face stays the art's.
    - Lurker: the drips under its shell, the user's "empty textures", become ribbed underbody in the art's colours.
    - Tank turret: the black smear under the barrels becomes machinery.
  - Not used:
    - Tank hull: bright panels painted into the dark wheel area between the tracks.
    - Dragoon: purple patches.
    - Vulture: white blobs under the pilot.
    - Drone: little change, because its drips are coverage (alpha), which the repaint keeps, not colour.
  Full run at 0.75 (turret 0.6) on the three that improved (`out/sprite3d/kaggle_mrepaint/`; every masked frame
  changes, a quarter of its pixels on average, 44% on the Overlord). On every frame, only the Lurker holds up (live, in
  `starcraft-data/eyelevel-versions/mask-v1`). The Overlord's hanging pods grow red-eyed faces and in the side views
  its middle pod turns into a pink column. The turret gets bright bits along the bottom edge and a block under the
  barrels. Both stay on their clean renders.
  The user then saw empty pixels and floating sections on the Lurker. Those were outline (alpha), which the repaint
  keeps, so the Lurker was rendered again with `--relief-tidy` (Relief fallback above). Then the same mask-only
  repaint ran, split by `--groups` over both kernels (0-12 and 13-25; 26-34, the death, render unchanged and are taken
  from mask-v1), merged into one anim. It is live as `mask-v2`: walk and attack frames in one piece, ribbed underbody
  where the hollow under the shell was.
- **Kaggle** (the user: from here the GPU work on Kaggle, to spare the local GPU): private dataset
  `kayo01/sc-eyelevel-inputs` (tools/*.py, anim/, models/UNIT voxel models, refine/, todo.json) and private kernel
  `kayo01/sc-eyelevel` (`kaggle_eyelevel/eyelevel_kernel.py`, T4 + 4 CPUs) that runs the job list: units (built, or
  re-rendered from given models with --refine), refiner training, pairs. Update with `kaggle datasets version -p
  out/sprite3d/kaggle_eyelevel_in -m MSG --dir-mode zip`, `kaggle kernels push -p kaggle_eyelevel`; fetch with
  `kaggle kernels output kayo01/sc-eyelevel -p DIR`. Push the kernel only once the new dataset version is listed
  (`kaggle datasets files ... --page-size 200` shows the new files): `datasets status` says "ready" for the old version
  while the new one is processing, and a kernel pushed then mounts the old one (it happened once; the kernel now stops
  if a train job's pairs file is missing).
  A second kernel, `kayo01/sc-eyelevel-b` (`kaggle_eyelevel_b/`, the same script with `KERNEL = 'b'`), runs alongside
  (Kaggle allows two GPU sessions at once). Both mount the same dataset: a job with `"kernel": "a"` or `"b"` runs only
  there, a job without one on either.
- `vox_io.py export|import`: MagicaVoxel .vox round trip for fixing a pose by hand (occupied box + 8 voxels of room,
  255-colour palette; the frames still take their colours from the Remastered frames). Marine poses in
  `out/sprite3d/marine/eye5/vox/`; import over `models/gNN.npz` and run `eyelevel_anim.py` again.

## Recommendation
- **Dragoon: the mod's shape painted by TRELLIS from the sprite** (above) for a solid walker from every side; static
  for now (the mesh has no rig; TRELLIS's colours could be baked onto the mod's own rigged mesh's UVs instead).
- **Units: relief impostors.** Draw each sprite frame as its depth relief, turned to face the viewer, with
  the directional frame chosen as today. StarCraft itself always shows the Dragoon's front, so nothing is invented. At eye
  level it stands as a four-legged walker instead of a top-down picture stood upright, and from high ground it shows its
  top. Animation comes free (every frame has depth).
- **Buildings: fixed relief, plus one of:**
  1. the art swings within ±40° as you walk around (the building turns by `rel - 40°·sin(rel)`): exact art, no invention,
     but it does turn, like today's billboards;
  2. fixed in the world with a mirrored north half: true parallax all round, the back is the front's mirror, east and
     west stay thin;
  3. fixed with generated sides: Zero-1-to-3 is not good enough for buildings; TRELLIS (MIT) is (see above). Either the
     relief with TRELLIS's hidden side, or TRELLIS's mesh alone on the sprite's palette, which is one clean mesh the
     engine's model renderer could draw like the SC2 models. The Nexus, which the relief cannot show well, needs this.
- Where more GPU helps: only the sides that were never drawn (the relief's problems were in the lifting and the
  drawing, not compute). TRELLIS on the free 16 GB Kaggle GPU (above) gives whole buildings; TRELLIS.2 needs 24 GB or
  more (a rented GPU). Fine-tuning a novel-view model on the 17-direction units was not tried: tens of GPU hours, and it
  would still learn nothing about the backs of buildings.
- Engine path: build a mesh per frame from the depth grid (2 HD px per vertex, cut at depth jumps, alpha-tested sprite
  texture, no lighting since the art has it baked) and draw it with the existing rasteriser (`fp_m3.h`/`fp_render.h`).
