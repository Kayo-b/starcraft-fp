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
- **N3** `done` Feel over a real network. Latency from the round trip: the host pings the joiner in the lobby (our own messages,
  taken out of the TCP stream before OpenBW's sync layer by `NetServer`), takes the 90th percentile of 12+ samples and picks
  1 + ceil((rtt/2 + 25 ms) / 42 ms) frames, at least 2 (`pick_latency`; `--latency N` on the host fixes it). Both games must switch
  while no scheduled message is on its way, since a game schedules what it receives with its own latency of that moment: after
  both slots and races have been applied, before the start, which is sent after the latency (so the joiner has switched by then);
  the lobby itself runs at 2. Fixed for the game (changed later, scheduled commands could land out of order). Ping every 0.5 s in
  the game for the HUD. Drawing between frames: the renderer keeps each unit's position on the previous simulation frame and draws
  `acc / sim_dt` of the way to the current one (new units, gaps over 4 frames and jumps over 64 px are drawn where they are); the
  follow camera reads the same positions. Lockstep pacing: a game blocked on the other no longer keeps up to two frames of backlog
  to catch up with; it drops the time. Kept, the game that started earlier on its clock stayed early and waited a few ms before
  nearly every step, then stepped in pairs (stutter; over a 100 ms relay the host was blocked on 5.5 s of 7). Dropped, it settles
  later on its clock, where neither game waits (0-0.15 s of 7). "Waiting for NAME..." (`WaitIndicator`): on after 0.3 s standing
  still or 40% of about the last second waiting, off after 1 s of running without either. Found and fixed (N2): a walking
  direction of 255 (just west of north) was sent as one byte, 255 being "no input", so walking that way braked the hero for a frame
  in the simulation while the camera kept going (about 2 px off); the nettest's prediction check failed in 3 of 4 runs of the
  committed N2 code (each run is a different game; the baseline run had passed by luck). Now two bytes; `kProtocol` 3.
  Tests: `fp_nettest` sessions 5-6 through `SlowLink` (a relay holding everything 50 ms each way; games paced like the window at
  60 Hz): round trip measured at ~101-104 ms, latency 3 for both, the games keep up with the clock and seldom wait; at latency 2
  over the same relay they fall ~13% behind and wait ~2 s of 8. Plus the latency table, the indicator's timing on synthetic stop
  patterns, auto latency over localhost (2), `fp_fpstest` units drawn between frames and the direction-255 walk (fails with the
  old encoding). Not checked: play by a person over a real network (feel cannot be tested headless); the 25 ms margin assumes
  windows rendering at 60 Hz or more (the ping is answered once per rendered frame: ~30-70 ms with two windows on one computer).
- **N4** `done` Fog of war, drawn for one player (`Renderer::set_viewer`; renderer only, plus one simulation rule). Terrain:
  per map tile from OpenBW's vision bits, 1 where the viewer sees, 0.55 where it has seen, 0.3 where it never has (darker than
  StarCraft's fog, lighter than its black: the world stays readable at a distance), blended between tile centres, faded over
  0.25 s (OpenBW clears vision every 100 frames and units add theirs back: drawn as it is, the fog closes in jumps); sprites and
  models take the brightness of their spot. Remembered buildings: the last view of each enemy building seen, not targetable,
  drawn while out of sight; forgotten once a tile of its footprint (the tiles OpenBW's own visibility test reads) is in sight and
  the building is not seen there. Cloaked units: an enemy's undetected one is a shimmer (the pixels behind it, displaced up and
  down in waves, read from a copy of the column, so the render threads stay apart) and cannot be picked; burrowed ones are not
  drawn; detected and own ones are see-through. The FPS layer does not target undetected units and the simulation drops a
  free-movement shot at one (`kProtocol` 4). Found and fixed: images without an anim file of their own drew as flat boxes (the
  Command Center, the Dark Templar, the Lurker: "missing HD sprites" at exit); they share the anim of the first image with
  their GRP (as `tools/hd_units.py` found), which covers 129 of 131. `fp_fogtest` (new): brightness lit/fogged/unexplored and
  the fade, a view of an unexplored base drawn at ~0.3 of its brightness, a Nexus scouted, remembered (dimmed, picks nothing),
  destroyed unseen and still remembered, forgotten when seen again; a Dark Templar undetected (shimmer, not picked, not an FPS
  target) and detected by a Missile Turret (see-through, a target); a burrowed Zergling not drawn; without a viewer no fog;
  every unit drawn has art; 3.1 ms per 640x400 frame with fog, 2.6 without. Multiplayer `--selftest-input`: your base lit, the
  other one dark. Golden views unchanged (no viewer there). Not covered: nothing in the game's controls makes cloaked or burrowed
  units yet; the shimmer was checked on frames from the test, not in play; the free-movement shot rule is untested (only a
  modified client could send such a shot).
- **N5** `done` (as chosen 2026-10-25: a virtual private network for now, our own recordings; a relay and rejoining later).
  Internet play: README instructions for Tailscale/ZeroTier (no code; the lobby's latency pick covers the extra round trip) and
  port forwarding; not tried over the internet yet. Recordings (`fp_replay.h`, `.fpr`): the setup as the lobby leaves it (map
  path and CRC, slots, players, random seed, FPS settings), every command of both players with the frame it took effect, the state
  hash after every frame (~13 bytes a frame). OpenBW's own replays cannot hold this game (standard commands only; the heroes are
  not made by commands) and its sync layer applies standard commands where we cannot see them, so a two-player game now sends
  them as one of our messages (`kChanCmd`, `kProtocol` 5) and every command of either player is applied, and recorded, in one
  place (`Game::apply`). A player leaving or dropping is OpenBW's own command, applied outside that stream: recorded where the
  other game is seen gone (checked before each command and after each network poll), as that command. Playback
  (`Game::open_recording`, `fp_play --watch FILE`): the same setup, the recorded commands on their frames, each frame's hash
  compared ("went its own way at frame N"); 1 / 2 through a player's eyes (their hero, their fog of war), 0 all. Two-player games
  record into `replays/` by default (`--no-record`, `--record FILE`; single player with `--record FILE`). Found and fixed: OpenBW's
  lobby `sync()` can leave behind an expired 50 ms timer whose callback writes a local of the finished `sync()` (stack use after
  return, AddressSanitizer, once over the 50 ms test relay); `NetServer::set_timeout` runs only the latest callback, as its one
  timer intends (3 clean runs since). `fp_fogtest`'s cost check now times one thread, fastest of 15 alternated runs (another
  program loading the machine made the multithreaded timing swing 3-9 ms). Tests: `fp_nettest` records sessions 1, 3, 4 and a
  single-player FPS game and plays each back to its last frame, every frame's hash and the end (kills, deaths, winner) as in the
  live game; multiplayer `--selftest-input` writes recordings, `--watch FILE --selftest-input` plays them (views switch, to the
  end, as recorded). Not covered: our own relay, NAT traversal, rejoining after a drop; a recording of a game whose program
  crashed ends at its last flushed frame (every 240 frames) without the 'e' record (plays as far as it goes).

### Phase 5 — top-down view (`fp-topdown`)
Asked for 2026-10-27 (after an assessment: OpenBW's own `ui/ui.h` draws the 1998 view from the 1.16.1 MPQs, or our own renderer
from the HD art the game already loads; the user chose the latter).
- **T1** `done` The classic top-down camera in every mode (`src/fp_topdown.h`; V, `--topdown`). Drawn from the HD art: terrain
  tiles, creep (the tileset's creep tiles, per tile) and its edges (`TileSet/<tileset>.dds.grp`, 8 more files to extract; format
  and placement in the README), every image of every sprite (shadows darken, effects keep their own colours, turrets, overlays,
  bullets, remnants) placed by the anim box rules, in StarCraft's order (elevation, then y up to the ground units' level, turrets
  over their base; images bottom up; selection circles under a unit's first image that is not a shadow). What is drawn comes from
  `Renderer::update_units`, so the two views agree: units between simulation frames, the viewer's fog of war (ground dimmed,
  other players' sprites and effects only where seen, remembered buildings dimmed and not pickable, an undetected cloaked unit as
  a shimmer; `UnitView::src` added for it). Art is decoded once into a cache at the zoom's mip level (premultiplied, box-filtered;
  player-colour mask kept), so zooms 1, 2 and 4 copy texels 1:1 (1.5 and 3 pick); rows split across threads, the same pixels on any
  number. 1280x800 (twice `--res`) in ~2.5 ms on 12 cores, ~11 ms on one; a first view decodes for ~10-100 ms. Controls: scroll
  (WASD, arrows, window edge), wheel zoom about the mouse, MMB drag, minimap (terrain, units, the view; click/drag moves it, RMB
  orders there); LMB select / box (own units, units before buildings, 12: `Game::select(vector)`), RMB smart order, B at the mouse,
  E into the unit in first person; in a replay a click follows a unit. With a hero (multiplayer and FPS) F commands from above (V:
  the first-person free camera instead), V from the hero steps out too. `fp_view --topdown` renders it (the debug minitile map it
  replaced is gone); `tools/golden.sh` adds a top-down image per map. Tests: `fp_topdowntest` (new: terrain texel for texel, creep and
  edges, every pixel, order and picking, box, circles and bars, fog, memory, shimmer, threads, cost); every window self-test plan goes
  through it (replay: V, scroll, wheel, click-follow, minimap; sandbox: click and box select, RMB order; FPS: V and F; multiplayer: F
  from above with the other base hidden; recordings). First-person golden views unchanged (52 frames, pixel for pixel). Not done:
  the warp-in texture, hallucination tint, emissive layers, control groups and hotkeys; not played by a person yet.

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
