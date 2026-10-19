#!/usr/bin/env bash
# Run the first-person viewer (fp_play). Builds it first if the binary is missing or older than the sources.
#
#   ./run.sh                          watch the bundled demo replay, from frame 6000
#   ./run.sh --replay my.rep          watch another replay
#   ./run.sh --map "path/to.scx"      load a map with neutral units only (use --game for a live game)
#   ./run.sh --fps                    FPS wave mode on GAME_MAP, order-driven movement (gun art/sounds from WEAPONS_DIR if it exists)
#   ./run.sh --fps --fps-move-shoot   FPS wave mode with free FPS-style movement (walk and shoot at once; experimental)
#   ./run.sh --game MAP               live RTS-style sandbox on a map (add --fps for the wave mode there)
#   ./run.sh --game "(2)The Small Divide"   a map name alone is looked up in MAPS_DIR (.scm/.scx optional)
#   ./run.sh --no-capture             leave the mouse pointer alone
#
# Every other argument is passed to fp_play (e.g. --race R, --enemy R, --fire-rate N, --range N, --speed N, --no-vsync; see README).
# Overrides: DATA_DIR (extracted CASC files), OPENBW_DIR (OpenBW headers), REPLAY (default replay), GAME_MAP (map for --fps),
# MAPS_DIR (where map names are looked up), WEAPONS_DIR (unpacked "Boltgun Doom" mod directory, added to every --fps run
# when it exists; local third-party assets, see README).
# RUN_SH_DRY=1 prints the fp_play command instead of running it.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

DATA_DIR="${DATA_DIR:-../starcraft-data/casc-scratch/extracted}"
OPENBW_DIR="${OPENBW_DIR:-../starcraft-data/openbw}"
REPLAY="${REPLAY:-../starcraft-data/titan-reactor/bundled/demo.rep}"
MAPS_DIR="${MAPS_DIR:-$HOME/Games/battlenet/drive_c/Program Files (x86)/StarCraft/Maps}"
GAME_MAP="${GAME_MAP:-$MAPS_DIR/(2)Bottleneck.scm}"
WEAPONS_DIR="${WEAPONS_DIR:-$HOME/Downloads/W40K_Boltgun_Doom_a0.9.1/W40K_Boltgun_Doom_a0.9.1}"
export OPENBW_DIR

[ -d "$DATA_DIR" ]    || { echo "run.sh: DATA_DIR not found: $DATA_DIR (extract with tools/casc_get.cjs; see README)" >&2; exit 1; }
[ -d "$OPENBW_DIR" ]  || { echo "run.sh: OPENBW_DIR not found: $OPENBW_DIR" >&2; exit 1; }

if [ ! -x build/fp_play ] || [ -n "$(find src Makefile -newer build/fp_play -print -quit)" ]; then
	echo "Building..." >&2
	make build/fp_play >&2
fi

# A --game/--map value that is not a file is looked up in MAPS_DIR, with or without its .scm/.scx extension.
resolve_map() {
	local c
	for c in "$1" "$MAPS_DIR/$1" "$MAPS_DIR/$1.scm" "$MAPS_DIR/$1.scx"; do [ -f "$c" ] && { printf '%s' "$c"; return; }; done
	echo "run.sh: map not found: $1 (not a file, nor in MAPS_DIR=$MAPS_DIR)" >&2; exit 1
}
args=(); prev=""
for a in "$@"; do
	if [ "$prev" = "--game" ] || [ "$prev" = "--map" ]; then a="$(resolve_map "$a")"; fi
	args+=("$a"); prev="$a"
done
set -- ${args[@]+"${args[@]}"}

# --fps: FPS wave mode, on GAME_MAP unless a map was given; the gun art/sounds come from WEAPONS_DIR when it exists.
has_input=0; fps=0; has_weapons=0
for a in "$@"; do case "$a" in --replay|--map|--game) has_input=1 ;; --fps) fps=1 ;; --weapons) has_weapons=1 ;; esac; done
if [ "$fps" = 1 ] && [ "$has_input" = 0 ]; then
	[ -f "$GAME_MAP" ] || { echo "run.sh: map not found: $GAME_MAP (set GAME_MAP)" >&2; exit 1; }
	set -- --game "$GAME_MAP" "$@"; has_input=1
fi
if [ "$fps" = 1 ] && [ "$has_weapons" = 0 ]; then
	if [ -d "$WEAPONS_DIR" ]; then set -- "$@" --weapons "$WEAPONS_DIR"; else echo "run.sh: WEAPONS_DIR not found; playing without gun art/sounds" >&2; fi
fi
# Default to the demo replay unless the caller chose a replay, a map or a game.
if [ "$has_input" = 0 ]; then
	[ -f "$REPLAY" ] || { echo "run.sh: replay not found: $REPLAY (set REPLAY or pass --replay)" >&2; exit 1; }
	set -- --replay "$REPLAY" --start-frame 6000 "$@"
fi

[ -n "${RUN_SH_DRY:-}" ] && { printf "%q " ./build/fp_play --data "$DATA_DIR" "$@"; echo; exit 0; }
exec ./build/fp_play --data "$DATA_DIR" "$@"
