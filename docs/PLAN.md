# StarCraft First-Person — Plan

Base: this repo (`main`), OpenBW headers in `../starcraft-data/openbw`, extracted Remastered data in
`../starcraft-data/casc-scratch/extracted`. Order of work: the map in pseudo-3D, then the base FPS, then commander mode.

## Locked decisions
1. Software renderer (CPU, SDL2, threaded columns), extended. The world is rebuilt from the map's own data and art, the
   way DevilutionX1p rebuilds Diablo. The `gzdoom-mashup` branch stays parked (OpenBW licence, separate engine).
2. OpenBW is the authority for simulation, orders and combat. Only exception: the hero's position in free movement.
3. The world is a minitile heightfield (8 px cells). Floor heights come from the per-minitile vf4 flags; every
   unwalkable cell is a solid block with a height (cliff: the upper level; doodad: its art height; void: no floor).
4. Terrain textures come only from the map's own HD art. Units are 2D billboards of the HD anim art, except where a
   3D model exists: SC2 `.m3` models of the Brood War roster from a local copy of the SC Evo Complete mod (MIT for the
   mod's own work; the models are kitbashes of Blizzard art: local use only, never committed). Buildings and mineral
   fields first (revised 2026-10-19: billboards of them fit a first-person view worst). The 2D sprites stay available
   for everything: the M key switches sprites only / 3D buildings (default) / 3D buildings and units, so each kind can
   stay 2D where it reads better; buildings stay 3D unless their sprites can be made to work in first person.
5. Free movement is the `--fps` default; `--fps-order-move` keeps the order-driven mode; `--fps-move-shoot` stays
   accepted as an alias.
6. Commander mode (FPS + simplified RTS) is a new mode, `--commander`, planned in detail once phases 1-2 are accepted.
7. One branch per milestone off `main`. Each milestone ends runnable, tests green, golden views reviewed, README and this
   file updated, committed and merged to `main`. Nothing is pushed unless asked.

## Milestones
Status: `todo`, `doing`, `done`.

### Phase 0 — baseline green (`fp-baseline`)
- **M0** `done` Dead-target guard in `Fps` (the fpstest crash, and the same stale `pick.unit` in `fp_play`); vx4ex flip
  bit in `SdTiles::texel`; `--sim-speed` (was shadowed by `--speed`); usage string and header comments; golden-view
  harness (`tools/golden.sh`, `fp_view --print-tileset`).

### Phase 1 — the map in pseudo-3D (`fp-terrain-*`)
- **M1** `done` Terrain model (`src/fp_terrain.h`): cell kind (floor, cliff, obstacle, water, space, edge), level,
  height, cv5 type, flags (ramp, doodad, very high); ramps blend two levels over 4 cells each side; `fp_view
  --heightmap`; `fp_terraintest` (walkability = OpenBW `is_walkable`, levels = `get_ground_height_at`, cliffs never
  below the floor they border, no solid block sinks, no unramped jumps, deterministic). Survey (`tools/terrain_survey`):
  south-facing cliff bands are 4-16 minitiles deep, north-facing 2-5; cv5 type 1 = doodad on every tileset, 5 = water
  on Badlands and Jungle, 2 = the space platform's void. The renderer's grid now comes from the model (per-minitile
  levels, three of them). Faces' art sources move to M3, which needs them.
- **M2** `done` Heightfield raycaster: front-to-back cells with a per-column clip row, faces and tops at any height,
  rays continue past cliffs to `--view-dist`; per-pixel depth buffer for sprites and picking; fog toward the horizon
  colour; eye and unit grounding from a bilinear floor height. 2.2-3.3 ms at 640x400 (autotest). A level is 64 px
  (`--level-h`; 32 read flat at eye height). Faces read the solid cell's art inward as a stand-in for M3. Found and
  fixed: water took level 2 (the per-level BFS never enters liquids); fp_terraintest now checks liquids lie low.
- **M2.1** `done` Play-check fixes. Units stand on their art's median frame bottom (box-centre anchoring sank their
  lower half into the floor once the floor had depth; the lowest frame bottom floated most frames 11-13 px). Floors
  are one surface (corner heights, interpolated inside each cell), so ramps are smooth slopes (kRamp 8, ~25 degrees).
  Walls follow the isometric 2:1 edges: the solid/open field is smoothed over 6 cells and contoured per cell, a split
  cell holds the straight line through it; face art is read along the normal snapped to the iso directions. Flat
  cells take a fast path: terrain 2.1 ms in the demo-replay base view, sprites another ~2 ms there (big buildings up
  close: M4). Found and fixed: R kept last frame's crosshair pick into the freed game (crash after restart).
- **M2.2** `done` Map edge: beyond the border the world continues as the map's mirror image, darkened (96 cells),
  then a plain ground plane in the map's average colour; resources near the edge stood against the sky below the
  horizon and looked like they floated. 3.2 ms on Bottleneck, 4.4 ms in the demo base view (sprites: M4).
- **M3** `done` Cliffs from the map's art. StarCraft paints a cliff's face onto the map as the band between its foot
  and its top. Natural tilesets (Badlands, Jungle, AshWorld, Desert, Ice, Twilight): the band is a slope rising from
  foot to top (by its distances to the two levels) with the art draped on it, Titan Reactor's approach (its terrain
  generator, `src/image/generate-map/`, was read for this). Space Platform and Installation: the band stays a block,
  its vertical face carrying the band's art from foot (bottom) to far side (top). A level is 112 px (Titan Reactor:
  mid ~80, high ~128); obstacles stand 32 px (72 with "very high") above the highest floor beside them; ramps blend
  over 12 cells each side (~30 degrees). Terrain 2.7 ms in the demo base view. The SD face path is unused (removal
  with M5). Back faces of block cliffs read their own (thin) band.
  Play-check follow-up: obstacles on natural tilesets are draped mounds too (rising to full height 3 cells in from
  their edge, smoothed over neighbouring obstacle cells, never below the floor beside them); the jungle canopy's
  unwalkable rim was a flat 32 px wall.
- **M3.5** `done` 3D models for buildings and minerals (`src/fp_m3.h`, `src/fp_models.h`, `--models DIR`). An `.m3`
  (MD34) reader (layouts from m3addon's structures.xml): rest-pose mesh, opaque standard materials (composites by
  their first opaque section), diffuse and emissive DDS textures (DXT1/3/5, ARGB). A software rasteriser draws them
  into the per-pixel depth buffer (perspective-correct, scanline spans, back faces culled, near-plane clipping, light
  from the south-west, team colour from the diffuse alpha, same fog as the terrain), with a per-pixel unit id for
  picking. 44 building and mineral types; Lair, Hive, Starport, Physics Lab, Armory, Hydralisk Den, Spire, Greater
  Spire, Ultralisk Cavern, Nydus Canal and the Vespene Geyser have no model in the mod and stay sprites; unfinished
  buildings stay sprites. Scaled so the model's footprint covers the unit's (by area). Zealot spike: renders
  correctly at ~22 px per model unit (`fp_view --model ... --model-at X,Y,YAW --model-scale 22`). Demo base view
  5.1-5.7 ms with models (4.9 with sprites). Found and fixed: a model under the crosshair beat a sprite in front of it.
- **M3.5b** `done` Model modes, switchable while playing (M): sprites only, buildings and minerals (default), and
  experimentally units too (Zealot, Dragoon, Marine, Firebat, Zergling, Hydralisk, Ultralisk and the FPS heroes;
  rest pose, turned to the unit's heading; `--model-units` starts there). The FPS self-test checks the key and can
  save screenshots (`--screenshot F.ppm`: `F_crosshair.ppm`, `F_fight.ppm`).
- **M3.7** `done` Fighting across levels (play check). The drop is at the walkable edge, so a unit on top can look over
  it: a draped cliff falls three quarters of its height within 3 cells of the edge and runs out as an apron to its
  foot; a block cliff's whole band is apron at the foot's level, its wall standing at the walkable edge (the contour
  follows ground a level above an apron; the wall's art is the band in front of it). Corners only join surfaces within
  0.4 level of each other (no twisted sheets between an apron and the plateau). Ramps blend over 20 cells each side
  (~20 degrees). Look up/down: the horizon shifts up to 2.5 render heights (~64 degrees). fpstest checks, rendered on
  Bottleneck: from the edge the hero sees and targets an enemy in the trench below, and from the open ground below one
  standing at the high platform's edge. Line of sight is real: from 12 px eye height a steep look down needs the hero
  at the edge, and an enemy far back from an edge is hidden from below.
- **M3.6** `todo` Animated unit models: bones and sequences (Stand/Walk/Attack/Death) driven by OpenBW state, heading,
  per-unit scale, death models; sprites for units without a model. Decide after a play check of M3.5.
- **M4** `todo` All sprites as the original draws them: `sprites_on_tile_line` (units, bullets, explosions, deaths,
  doodad sprites), every image of a sprite, shadows as floor decals; flyers at altitude; smoothing and a size clamp up
  close (also the sprite fill cost: front-to-back without overdraw); buildings anchored on their footprint.
- **M5** `todo` Doodads standing up: each placed doodad's art rebuilt from its cv5 group and drawn as an upright quad
  on its footprint; obstacle heights from the art.
- **M6** `todo` Tileset sweep: golden views for all 8 tilesets, per-tileset water/lava/ice/space void (water and lava
  by SD palette index 1-13 / 248-254, as Titan Reactor detects them, instead of the cv5 type table), sky per
  tileset. Accepted when every pixel is written, no face stripes, every cliff is visible from
  both sides and the budget holds.

### Phase 2 — base FPS (`fp-feel-*`)
- **M7** `todo` Free movement default; hero collision with units and building footprints (slide); `--speed` scales
  the free top speed; tests for being blocked by a unit and a building.
- **M8** `todo` Combat feedback: enemy projectiles/impacts (M4), hero tracers and impact puffs, hit marker,
  directional damage indicator, low-HP effect.
- **M9** `todo` HUD: minimap (terrain, unit dots, view cone), compass, help overlay, `--fov`, `--sens`.
- **M10** `todo` Game loop: difficulty curve, score/time/best wave, clean restart, enemy attack/death sounds.

### Phase 3 — commander mode (`--commander`; revise this section before starting)
- **C1** Hero plus your base (workers, town hall) and an enemy base with a simple scripted AI (OpenBW has none).
- **C2** Simplified economy: workers gather on their own; number keys queue the race's core units at the right building.
- **C3** Squad orders at the crosshair (follow me, attack here, move here, hold) in groups of 12; squad AI.
- **C4** Building placement in first person: footprint ghost at the crosshair with OpenBW's placement check.
- **C5** Enemy build order and attack waves; win/lose by the main building.
- **C6** Tactical map while TAB is held.

### Phase 4 — multiplayer (`fp-multiplayer`)
Asked for 2026-10-25: keep the game at the FPS level plus the RTS commands and production that exist; no full StarCraft
client (command card, hotkeys) and no Remastered client (closed, no API; see the 2026-10-22 assessment). OpenBW's `sync.h`
(lockstep over TCP, two players) is the network layer.
- **N1** `done` 1v1 over TCP (`--host [PORT]` / `--join HOST[:PORT]`, `src/fp_net.h`). Lobby inside the map load (OpenBW's
  setup callback, before starting units exist): slots, races, host starts, melee start for both. Every command goes through
  one path (`Game::command`/`custom`, replay-format actions): single player applies it at once (the gametest checksum is
  unchanged), multiplayer sends it to both games for the same frame. The FPS layer is per player (`Fps` versus mode): a hero
  each, trigger and out-of-range misses as commands, respawn after 10 s, weapon tuning once per weapon. Non-blocking lockstep
  in the render loop; F switches between the hero and commanding the base (the hero fights on its own meanwhile). Start
  checks (version, map CRC, latency, tuning) and a state hash every frame ("out of sync at frame N"); TCP_NODELAY (without it
  localhost lockstep ran at ~50 frames/s, with it ~4600). Fog of war for the other player's units (`Renderer::set_viewer`).
  Found and fixed: `Renderer::depth_at` read the depth buffer before the first render (crash when an enemy is in view on
  frame 0, which only multiplayer has); OpenBW's TCP server frees its client list before its io_service, so a read still
  pending at exit released a client into freed memory (abort on quit in 4 of 4 window runs; `Net::~Net` closes the
  sockets and runs the aborted handlers first). `fp_nettest` (AddressSanitizer build: the release build passed with the
  corruption); multiplayer `--selftest-input` (both games).
- **N2** `done` Free movement in multiplayer (`--fps-move-shoot`, both players; part of the start check). The hero's
  physics runs once per simulation frame for every hero, in integers (1/256 px; OpenBW's direction table; acceleration,
  friction, speed cap and per-axis wall slide in steps of at most 4 px), in `Fps::tick`; `move_unit` keeps OpenBW's unit
  (finder, collision flags) on it, and OpenBW's idle targeting is stopped so it never walks the hero. Input as commands: the
  walk direction (one byte, sent before a step when it changed; `Game::set_before_step`), the RMB walk target, halt, and
  shots (target unit id; range checked in integers when the shot lands). Commanding the base hands the hero to OpenBW and
  back. Camera: `Fps::view_position` runs the same physics ahead over the player's own input still in flight
  (`Game::command_frame`: `latency` frames, one more while waiting for the other game) and draws between frames, so the
  view moves on the key press; the simulation then ends up exactly there (the nettest compares every frame: 0 off in
  2300+). The mover state is in the per-frame hash. Single player uses the same code: speed and feel as before (192 px/s,
  ~0.6 s to stop), now smooth between frames rather than per render frame. Found and fixed: free movement counted every
  shot twice (when fired and again from the weapon cooldown), so the gun animation and sound played twice per shot (on `main`:
  2 shots, 4 counted; `fp_fpstest` now checks it). Not done: the other player's hero is drawn at simulation steps
  (interpolation is N3).
- **N3** `todo` Feel over a real network: camera interpolation between frames, latency picked from the measured round trip,
  a "waiting" indicator that does not flicker.
- **N4** `todo` Fog of war: remembered enemy buildings, cloaked units, terrain dimmed outside vision.
- **N5** `todo` Internet play: a relay or NAT punching, reconnecting, a replay of the match (`sync.h` can save one).

## Verification (every milestone)
- `make test DATA_DIR=... MAP=".../(2)Bottleneck.scm" MOD_DIR=...` (plus `fp_terraintest` from M1).
- Window self-tests with `SDL_AUDIODRIVER=dummy`: `fp_play --game MAP --fps --weapons MOD_DIR --selftest-input`,
  `--game MAP --selftest-input`, `--autotest` (collision vs `is_walkable`, render ms within budget).
- `tools/golden.sh` contact sheets before/after, inspected.
- A short play check by a person: feel cannot be tested headless.

## Known risks
- Cliff art exists only for the faces StarCraft's camera shows (south-facing); draped slopes show the band's art on
  every side, block faces facing north show their thin band.
- Doodads are baked into the terrain art; some have no clean footprint and need per-tileset handling.
- Water, lava and the space platform's void need tileset-specific classification (cv5 groups are not documented here
  yet; M1 starts with a survey).
- Buildings without a model (see M3.5) are flat billboards; up close they may still read poorly after the size clamp.
- Model materials beyond diffuse and emissive (normal, specular, decals, additive glows) are not drawn; some SC2
  base-game textures the mod's models reference are not in the mod.
- The stock maps cover four tilesets (badlands, platform, AshWorld, Jungle). Installation, Desert, Ice and Twilight need
  campaign or Brood War maps for M6.
- OpenBW's licence is unresolved: fine for local play, blocks distribution.
