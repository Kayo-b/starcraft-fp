#!/usr/bin/env bash
# Golden views: render a fixed set of first-person frames per map and lay them out as one contact sheet per map, so a
# milestone's rendering can be compared before and after by eye.
#
#   tools/golden.sh LABEL     writes out/golden/LABEL/<map>.png: one row per camera spot, columns = headings 0/90/180/270
#
# Spots are fp_view's deterministic --spot list (start locations, then the map centre and quarter points). The demo
# replay gets one row, seen from its first player unit at frame 6000.
# Overrides: DATA_DIR (extracted CASC files), MAPS_DIR (stock maps), REPLAY, SPOTS (default "0 1 2").
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."

label="${1:?usage: tools/golden.sh LABEL}"
DATA_DIR="${DATA_DIR:-../starcraft-data/casc-scratch/extracted}"
MAPS_DIR="${MAPS_DIR:-$HOME/Games/battlenet/drive_c/Program Files (x86)/StarCraft/Maps}"
REPLAY="${REPLAY:-../starcraft-data/titan-reactor/bundled/demo.rep}"
SPOTS="${SPOTS:-0 1 2}"
command -v magick >/dev/null || { echo "golden.sh: needs ImageMagick (magick)" >&2; exit 1; }
make -s build/fp_view >&2

out="out/golden/$label"; frames="$out/frames"; mkdir -p "$frames"
headings=(0 90 180 270)

# One stock map per tileset the stock maps cover (there is no stock Installation, Desert, Ice or Twilight map).
maps=("(2)Bottleneck.scm" "(2)Challenger.scm" "(2)Volcanis.scm" "(2)The Small Divide.scm")

row() { # row OUT.png fp_view-args... : the four headings side by side
	local dst="$1"; shift; local tiles=()
	for h in "${headings[@]}"; do
		local f="$frames/$(basename "$dst" .png)_h$h.ppm"
		./build/fp_view --data "$DATA_DIR" "$@" --heading "$h" --out "$f" >/dev/null || return 1
		tiles+=("$f")
	done
	magick "${tiles[@]}" +append "$dst"
}

for m in "${maps[@]}"; do
	name="$(echo "${m%.*}" | sed 's/^([0-9]*)//' | tr -c 'A-Za-z0-9\n' '_')"; rows=()
	for s in $SPOTS; do
		if row "$frames/${name}_s$s.png" --map "$MAPS_DIR/$m" --spot "$s"; then rows+=("$frames/${name}_s$s.png"); fi
	done
	[ "${#rows[@]}" -gt 0 ] || { echo "golden.sh: no views for $m" >&2; continue; }
	magick "${rows[@]}" -append -resize 50% "$out/$name.png"
	echo "$out/$name.png"
done

if [ -f "$REPLAY" ]; then
	row "$frames/demo_replay.png" --replay "$REPLAY" --frames 6000 && magick "$frames/demo_replay.png" -resize 50% "$out/demo_replay.png" && echo "$out/demo_replay.png"
fi
