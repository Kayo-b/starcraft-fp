# starcraft-fp — first-person view prototype over OpenBW

Status: **prototype v0.4**. First-person view over OpenBW state with Remastered HD terrain and sprites; playable FPS wave mode, RTS sandbox and replay viewer.

## What it is / is not
- Rules and simulation come from OpenBW (a reimplementation of classic Brood War). They are *assumed* to match
  Remastered's rules; this is **not verified** (needs a Remastered-recorded replay with a known outcome).
- Not the Remastered client; no Battle.net compatibility. Game data is read from files you extract yourself from
  your own licensed install (`tools/extract_casc.cjs`; see the bw-casclib buffer-size caveat in that file's header).
- OpenBW and Titan Reactor licences are **unverified**; no code from either is copied here.

## Modes (`fp_play`)
    ./run.sh                 # watch a replay from inside the world
    ./run.sh --fps           # FPS: you are a hero unit; waves of enemies hunt you (needs --weapons MOD_DIR for the gun/sounds)
    ./run.sh --game MAP      # live RTS-style sandbox: possess a unit, give orders (kept; see below)
- **FPS** (`--game MAP --fps [--weapons MOD_DIR]`): you are Jim Raynor's Marine (Protoss: Fenix's Dragoon, Zerg: Hunter-Killer).
  WASD move (movement orders to the unit, so SC pathing/physics apply), mouse aims, hold LMB fires at the enemy under the crosshair
  (small aim assist; no shooting through cliffs), RMB moves to the crosshair floor point, R restarts. The hero only fires while the trigger
  is held (StarCraft units normally auto-attack). Enemies: waves of the opposing race, simple chase AI of our own (OpenBW has no AI).
  HUD: HP, wave, kills, hover info; crosshair green = enemy in weapon range, orange = out of range. Damage/shots/range are OpenBW's.
- **Replay** (`--replay FILE`): watch from inside the world. **Game sandbox** (`--game MAP`): LMB select, RMB smart order, E possess,
  T/Y train, B build supply, X stop (Terran/Protoss workers only for building).
- Common: mouse look, F free camera, TAB next unit, P pause, `=`/`-` sim speed, ESC release mouse, F12 screenshot, Q quit. `--no-capture` leaves your pointer alone.

## External assets and licences (nothing below is in this repo)
- **StarCraft data**: extracted by you from your own install (`tools/casc_get.cjs`; `bw-casclib` has a buffer-size bug, see the script header).
- **Gun art and sounds** (`--weapons`): a local copy of the GZDoom mod "Boltgun Doom" (credits: Ultra.Boi; based on *Warhammer 40,000: Boltgun* by
  Auroch Digital). No licence file ships with it and it is third-party/Games Workshop IP: keep it local, do not commit or redistribute it. The code
  reads `Patches/BLTR{G,F}*.png` and `Sounds/Weapons/Boltgun`, `Sounds/Player/{Pain,Death}` from that directory only.
- OpenBW and Titan Reactor licences are unverified; no code from either is copied here.
- System libraries: SDL2, SDL2_ttf, libpng, libvorbisfile (all already installed here; none were installed by this project).

## Tests
`make test DATA_DIR=... MAP=".../(2)Bottleneck.scm" MOD_DIR=.../W40K_Boltgun_Doom_a0.9.1` runs the headless suites: `fp_gametest` (RTS orders, determinism),
`fp_fpstest` (arena, waves, enemy AI, fire/trigger/range, death), `fp_assettest` (PNG/OGG decode vs independent values, mixer).
Window tests (they open a window briefly): `fp_play ... --selftest-input` for replay, `--game MAP` and `--game MAP --fps --weapons MOD_DIR`
(use `SDL_AUDIODRIVER=dummy` to stay silent); `--autotest` checks collision against OpenBW `is_walkable`.

## Roadmap / possible goals
- **Hero + squad mode** (idea, not started): play as one hero unit in first person while commanding other units (move, attack-move, hold position,
  follow) and fighting alongside them. Looks achievable: possession, crosshair picking and OpenBW's order API (`action_order`, `action_hold_position`,
  `action_stop`) already exist here. Needs: a squad concept (OpenBW selects at most 12 units per order, so group in 12s), an order wheel or hotkeys that
  target the crosshair ("attack here", "move here", "hold", "follow me"), squad unit AI so they do not stand idle, and a minimap/ally markers in the HUD.
- Weapons beyond the Boltgun (shotgun, heavy bolter, ...: art is in the mod), distinct damage/cooldown per weapon (needs a mod-side weapon layer
  because StarCraft's weapon stats are per unit type), more enemy types and waves, pickups.
- Known design risks: movement is order-driven so it is not twitchy like Doom; a lone unit cannot move and shoot at once in StarCraft; the art is
  isometric so billboards look viewed from above; terrain has no height.

## Build / run (still frames)
    make                                  # needs OpenBW headers: OPENBW_DIR=../starcraft-data/openbw
    ./build/fp_view --data DATA_DIR --replay demo.rep --frames 6000 --heading 0 --out out/fp.ppm --topdown out/td.ppm
    python3 tools/ppm2png.py out/fp.ppm out/fp.png

## Rendering model (prototype choices, not StarCraft facts)
Unwalkable minitiles (8 px) are walls (48 px high); floor shade follows tile height flags; units are flat
billboards sized from unit-type dimensions; eye height 12 px; FOV 66 degrees.

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
- The free camera collides with terrain only (not units/buildings/resources); the possessed/FPS unit collides like any StarCraft unit.
- Minor black fringe on some sprite edges (opaque-alpha dark texels; not investigated).
- Only the main image of each sprite is drawn (no shadows/overlays).
- Wall faces are one stretched texture row; map-edge walls clamp to the border tile. Flat floor; height levels not used for eye height.
- Fixed camera, still images only; no input.

## Next
1. Shadows and other sprite layers, unit animation frames, cliff/height rendering.
2. Interactive loop (SDL2) and player input through OpenBW actions.
3. Fidelity test against a Remastered-recorded replay.
