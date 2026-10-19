# starcraft-fp — first-person view prototype over OpenBW

Status: **prototype v0.4**. First-person view over OpenBW state with Remastered HD terrain and sprites; playable FPS wave mode, RTS sandbox and replay viewer.

## What it is / is not
- Rules and simulation come from OpenBW (a reimplementation of classic Brood War). They are *assumed* to match
  Remastered's rules; this is **not verified** (needs a Remastered-recorded replay with a known outcome).
- Not the Remastered client; no Battle.net compatibility. Game data is read from files you extract yourself from
  your own licensed install (`tools/extract_casc.cjs`; see the bw-casclib buffer-size caveat in that file's header).
- OpenBW and Titan Reactor licences are **unverified**; no code from either is copied here.

## Modes (`fp_play`)
    ./run.sh                          # watch the bundled demo replay from inside the world (starts at frame 6000)
    ./run.sh --fps                    # FPS wave mode on GAME_MAP, order-driven movement (the default)
    ./run.sh --fps --fps-move-shoot   # FPS wave mode with free FPS-style movement: walk and shoot at the same time
    ./run.sh --game MAP               # live RTS-style sandbox: possess a unit, give orders
    ./run.sh --replay FILE            # another replay;  --map FILE loads a map with neutral units only

`run.sh` builds `fp_play` when needed and passes `--data`. A `--game`/`--map` value that is not a file is looked up in your StarCraft
maps folder (`MAPS_DIR`), so `./run.sh --game "(2)The Small Divide"` works. `--fps` without `--game` plays `GAME_MAP`, and every `--fps`
run gets `--weapons "$WEAPONS_DIR"` when that directory exists (without it there is no gun art or sound). Extra arguments are passed through.

### FPS (`--game MAP --fps`)
You are Jim Raynor's Marine (`--race protoss`: Fenix's Dragoon, `--race zerg`: Hunter-Killer). Waves of the enemy race (`--enemy`,
default protoss) hunt you. The camera is locked to the hero, so F and TAB do nothing here.
- **Movement, order-driven (default)**: WASD issues short StarCraft move orders in the direction you face (pathing and collision are
  OpenBW's); RMB is a smart order at the crosshair (move to the floor point, or attack the unit). Shots are attack orders, so the hero
  stands still while firing at a target in range: a StarCraft unit cannot move and shoot at once.
- **Movement, free (`--fps-move-shoot`, experimental)**: WASD drives the hero directly with velocity physics (acceleration, friction,
  wall slide) in any direction, with no orders, so you can walk and shoot at the same time. RMB sets an auto-walk goal at the crosshair
  floor point (any WASD input cancels it). Damage goes straight through OpenBW's weapon-hit resolution. Top speed is fixed at 192 px/s.
- **Shooting**: mouse aims, hold LMB to fire at the enemy under the crosshair (small aim assist; no shooting through walls). The hero
  only fires while the trigger is held (StarCraft units normally auto-attack). Crosshair: green = enemy in weapon range, red = enemy
  out of range; firing out of range still shoots and always misses (with a bullet-impact sound from the mod).
- **Enemies**: melee units first (Zealots vs Protoss, Firebats vs Terran, Zerglings vs Zerg; 2 per wave, growing to 6), ranged ones
  (Dragoons, Marines, Hydralisks) join from wave 3 (up to 3). A wave spawns 420-760 px away, reachable and spread out, once the previous
  one is cleared and at least 8 s have passed. The AI is our own and simple (OpenBW has none): every enemy keeps attacking the hero.
- **HUD**: wave, kills and enemies alive; HP bar; name/owner/HP of the unit under the crosshair; red flash when hit. R restarts (also
  after "YOU DIED").
- **Tuning** (percent of the StarCraft data, 10..1000): `--fire-rate N` and `--range N` (default 200 each), `--speed N` (movement
  speed, default 90; order-driven mode only).

### Replay and game sandbox
- **Replay** (`--replay FILE [--start-frame N]`): watch from inside the world; no orders.
- **Game sandbox** (`--game MAP`): you are one player, a second base sits idle (OpenBW has no AI). You start possessing a worker.
  LMB select (Shift adds), RMB smart order (move/attack/gather/rally), E possess/release the selected unit (WASD then drives it with
  move orders), T train worker, Y train combat unit, B build supply at the crosshair (Terran/Protoss workers only), X stop.

### Common controls and options
- Mouse look; WASD moves the free camera (Shift = 3x speed); F free camera; TAB follow the next unit; P pause; `=`/`-` sim speed
  (x0.125..x16); R restart; ESC release/capture the mouse; F12 screenshot (`fp_shot_NNN.ppm` in the working directory); Q quit.
- `--race R` / `--enemy R` (terran, protoss, zerg; default terran vs protoss), `--res WxH` render resolution (default 640x400),
  `--window WxH` (default 1280x800), `--fullscreen`, `--sim-speed X` starting sim speed (0.125..16), `--no-capture` leaves your
  pointer alone, `--no-vsync` turns off the 60 Hz vsync cap (soft-capped at 240 fps; the software renderer runs at ~2-3.5 ms/frame). `--view-dist PX`, `--eye PX`, `--level-h PX` (see
  Rendering model).

## External assets and licences (nothing below is in this repo)
- **StarCraft data**: extracted by you from your own install (`tools/casc_get.cjs`; `bw-casclib` has a buffer-size bug, see the script header).
- **Gun art and sounds** (`--weapons`): a local copy of the GZDoom mod "Boltgun Doom" (credits: Ultra.Boi; based on *Warhammer 40,000: Boltgun* by
  Auroch Digital). No licence file ships with it and it is third-party/Games Workshop IP: keep it local, do not commit or redistribute it. The code
  reads `Patches/BLTR{G,F}*.png` and `Sounds/Weapons/Boltgun`, `Sounds/Player/{Pain,Death}` from that directory only.
- OpenBW and Titan Reactor licences are unverified; no code from either is copied here.
- System libraries: SDL2, SDL2_ttf, libpng, libvorbisfile (all already installed here; none were installed by this project).

## Tests
`make test DATA_DIR=... MAP=".../(2)Bottleneck.scm" MOD_DIR=.../W40K_Boltgun_Doom_a0.9.1` runs the headless suites: `fp_terraintest` (the terrain
model against OpenBW's walkability and ground heights; `TERRAIN_MAPS=...` adds maps), `fp_gametest` (RTS orders, determinism),
`fp_fpstest` (arena, waves, enemy AI, fire/trigger/range, death), `fp_assettest` (PNG/OGG decode vs independent values, mixer).
Window tests (they open a window briefly): `fp_play ... --selftest-input` for replay, `--game MAP` and `--game MAP --fps --weapons MOD_DIR`
(use `SDL_AUDIODRIVER=dummy` to stay silent); `--autotest` checks collision against OpenBW `is_walkable`.
Golden views: `tools/golden.sh LABEL` renders fixed first-person views (`fp_view --spot K`: start locations, map centre,
quarter points; four headings) of one stock map per tileset plus the demo replay into `out/golden/LABEL/*.png` contact sheets,
to compare rendering before and after a change. `fp_view --print-tileset` prints a map's tileset; `fp_view --heightmap F.ppm` the terrain model
(green floor by height, yellow ramps, red cliffs, grey obstacles, blue water, black void). `build/terrain_survey DATA_DIR [--typemap DIR] MAP...`
prints what a map's terrain data says (levels, unwalkable regions, cv5 types, cliff band depths).

## Roadmap / possible goals
The working plan, with milestones and their status, is [`docs/PLAN.md`](docs/PLAN.md). Ideas beyond it:
- **Hero + squad mode** (idea, not started): play as one hero unit in first person while commanding other units (move, attack-move, hold position,
  follow) and fighting alongside them. Looks achievable: possession, crosshair picking and OpenBW's order API (`action_order`, `action_hold_position`,
  `action_stop`) already exist here. Needs: a squad concept (OpenBW selects at most 12 units per order, so group in 12s), an order wheel or hotkeys that
  target the crosshair ("attack here", "move here", "hold", "follow me"), squad unit AI so they do not stand idle, and a minimap/ally markers in the HUD.
- Weapons beyond the Boltgun (shotgun, heavy bolter, ...: art is in the mod), distinct damage/cooldown per weapon (needs a mod-side weapon layer
  because StarCraft's weapon stats are per unit type), more enemy types and waves, pickups.
- Shadows and the other sprite layers (overlays); rendering the upper cliff tiers.
- Fidelity test against a Remastered-recorded replay with a known outcome.
- Known design risks: the default order-driven movement is not twitchy like Doom and cannot move and shoot at once (`--fps-move-shoot`
  fixes both by bypassing StarCraft movement for the hero, which then ignores unit collision); the art is isometric so billboards look
  viewed from above; only one cliff step is drawn, so tall height differences look flat.

## Build / run (still frames)
    make                                  # needs OpenBW headers: OPENBW_DIR=../starcraft-data/openbw
    ./build/fp_view --data DATA_DIR --replay demo.rep --frames 6000 --heading 0 --out out/fp.ppm --topdown out/td.ppm
    python3 tools/ppm2png.py out/fp.ppm out/fp.png

## Rendering model (prototype choices, not StarCraft facts)
- 3D models (`--models DIR`, `src/fp_m3.h`, `src/fp_models.h`): finished buildings and mineral fields are drawn as StarCraft II `.m3`
  models from a local copy of the SC Evo Complete mod (its `SCEvo_Assets.SC2Mod/Base.SC2Assets` directory; `run.sh` adds it from
  `MODELS_DIR` when it exists, `NO_MODELS=1` turns it off). The models are kitbashes of Blizzard's SC2 art: local use only, never
  committed. A software rasteriser draws them into the same per-pixel depth buffer (rest pose, diffuse + emissive textures, team colour
  from the diffuse alpha); units and buildings without a model stay sprites. `fp_view --models DIR --model FILE --model-at X,Y,YAW`
  places any model in a view.
- Terrain: a heightfield of minitiles (8 px, `src/fp_terrain.h`). Walkable cells sit at their StarCraft level (low/mid/high, 112 px apart);
  floors are one continuous surface (heights at cell corners), so ramps are smooth slopes. StarCraft paints a cliff's face onto the
  map as the band between its foot and its top: on natural tilesets that band becomes a slope from foot to top with the map art draped
  on it (Titan Reactor's approach); on Space Platform and Installation it stays a block whose vertical face carries the band's art.
  Obstacles stand 32 px (72 with vf4's "very high" bit) above the floor beside them, as draped mounds on natural tilesets and blocks
  on artificial ones; water lies just below its shore, the space
  platform's void far below. Walls follow the isometric grid's 2:1 edges (the solid/open field is smoothed and contoured, a boundary
  cell holds the straight line through it) instead of 8 px staircases.
- Renderer: each screen column marches its ray through the heightfield front to back, drawing a cell's top where it is below the eye
  and its face where it rises above the cell before; rays go on past cliffs to `--view-dist` (default 3000 px), so high ground shows
  from below at full height and low ground shows from above. Fog closes in toward the view distance. Every pixel keeps its depth, so
  sprites hide behind terrain per pixel and the crosshair picks units and floor points through it.
- Eye 12 px above the floor under the camera; FOV 66 degrees. Units are flat billboards of their HD sprite frame, standing on the floor
  under them on the sprite's median frame bottom (the original renderer centres the box on the position, which drawn upright sinks
  the lower half of the art into the floor). Works with any map OpenBW can load (all
  eight tilesets via their HD DDS data). `--eye PX`, `--level-h PX` and `--view-dist PX` tune the look (fp_play and fp_view).

## HD formats (verified on real 1.23.10.13515 files, 2026-10-17)
- `TileSet/<name>.dds.vr4`: u16 frame count @4 (= number of megatiles, same index space as `.vf4`/`.vx4ex`, 7,038 for Jungle);
  frames from offset 8 as `[u16,u16,u16 w,u16 h,u32 size,DDS]`; every frame is a 128x128 DXT1 DDS (size 8,320). Tile
  index = OpenBW `tiles_mega_tile_index`. Checked: parsing consumes the file exactly; a top-down render is seamless.
- `anim/main_NNN.anim` (NNN = OpenBW main image id): `ANIM`, version 0x204, 7 layers (diffuse DXT5, bright, teamcolor DXT1,
  emissive, normal, specular, ao_depth), one sprite record @0x14c, 16-byte frame records `x,y,xoff,yoff,w,h`.
  HD pixels are 4x SD pixels (assumed from the tile ratio; billboard sizes look right but this is not verified).
- Frame choice for directional images follows OpenBW `set_image_heading`: `off=(dir+4)/8`, mirrored+flipped above 16;
  here `dir` is the unit heading relative to the camera bearing (derived, not yet validated against a known view).

## Verified so far (2026-10-17, Hyprland/Wayland, 12 cores)
- Autotest: walls stop movement at exactly the last 6 px step before the first pixel OpenBW reports unwalkable, in 4 directions;
  a mutant with collision disabled fails it (the wander part alone would not have caught that). Render 2.5 ms at 640x400.
- Live window: 60-79 fps with vsync, letterboxed; scripted input passes. Real human keyboard/mouse feel (sensitivity, pointer
  capture and ESC toggle, speed keys) has NOT been exercised by a person.

## Known limits
- The free camera and the `--fps-move-shoot` hero collide with terrain only (not units/buildings/resources); a possessed unit and the
  order-driven FPS hero collide like any StarCraft unit.
- Block faces (artificial tilesets) read their art inward from the face; doodads are mounds or blocks until M5 stands them up. Creep effects and water animation are not drawn. Beyond the map edge the world continues as a darkened mirror image of the map,
  then a plain ground plane (scenery only: nothing can go there).
- Walkability and height levels are read once per map load; creep-induced changes during play are not tracked.
- Minor black fringe on some sprite edges (opaque-alpha dark texels; not investigated).
- Only the main image of each sprite is drawn (no shadows/overlays).
- Models are drawn in their rest pose (no animation yet); additive glows, decals and normal maps are skipped.
