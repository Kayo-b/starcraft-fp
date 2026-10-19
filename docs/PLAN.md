# StarCraft First-Person — Plan

Base: this repo (`main`), OpenBW headers in `../starcraft-data/openbw`, extracted Remastered data in
`../starcraft-data/casc-scratch/extracted`. Order of work: the map in pseudo-3D, then the base FPS, then commander mode.

## Locked decisions
1. Software renderer (CPU, SDL2, threaded columns), extended. The world is rebuilt from the map's own data and art, the
   way DevilutionX1p rebuilds Diablo. The `gzdoom-mashup` branch stays parked (OpenBW licence, separate engine).
2. OpenBW is the authority for simulation, orders and combat. Only exception: the hero's position in free movement.
3. The world is a minitile heightfield (8 px cells). Floor heights come from the per-minitile vf4 flags; every
   unwalkable cell is a solid block with a height (cliff: the upper level; doodad: its art height; void: no floor).
4. Textures come only from the map's own HD art. No external 3D assets. Sprites stay 2D billboards of the HD anim art.
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
- **M3** `todo` Faces from the map's art: a south-facing cliff's face is its band's HD art read from the plateau edge
  down (StarCraft's view is a vertical oblique projection, so no un-skew); diagonal cliffs along the band; back faces
  reuse the nearest south column, mirrored; band tops from the nearest high ground. The SD face path goes.
- **M4** `todo` All sprites as the original draws them: `sprites_on_tile_line` (units, bullets, explosions, deaths,
  doodad sprites), every image of a sprite, shadows as floor decals; flyers at altitude; smoothing and a size clamp up
  close (also the sprite fill cost: front-to-back without overdraw); buildings anchored on their footprint.
- **M5** `todo` Doodads standing up: each placed doodad's art rebuilt from its cv5 group and drawn as an upright quad
  on its footprint; obstacle heights from the art.
- **M6** `todo` Tileset sweep: golden views for all 8 tilesets, per-tileset water/lava/ice/space void, sky per
  tileset, map edges as void/fog. Accepted when every pixel is written, no face stripes, every cliff is visible from
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

## Verification (every milestone)
- `make test DATA_DIR=... MAP=".../(2)Bottleneck.scm" MOD_DIR=...` (plus `fp_terraintest` from M1).
- Window self-tests with `SDL_AUDIODRIVER=dummy`: `fp_play --game MAP --fps --weapons MOD_DIR --selftest-input`,
  `--game MAP --selftest-input`, `--autotest` (collision vs `is_walkable`, render ms within budget).
- `tools/golden.sh` contact sheets before/after, inspected.
- A short play check by a person: feel cannot be tested headless.

## Known risks
- Cliff art exists only for the faces StarCraft's camera shows (south-facing); back faces are reused and mirrored.
- Doodads are baked into the terrain art; some have no clean footprint and need per-tileset handling.
- Water, lava and the space platform's void need tileset-specific classification (cv5 groups are not documented here
  yet; M1 starts with a survey).
- Large buildings are flat billboards; up close they may still read poorly after the size clamp.
- The stock maps cover four tilesets (badlands, platform, AshWorld, Jungle). Installation, Desert, Ice and Twilight need
  campaign or Brood War maps for M6.
- OpenBW's licence is unresolved: fine for local play, blocks distribution.
