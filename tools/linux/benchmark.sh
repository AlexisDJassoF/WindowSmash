#!/usr/bin/env bash
# A repeatable performance run of the Linux build: boot offline on a copy of the save, scripted input
# (SSBU_INPUT: by default into a 1v1 battle, as tools/autobattle.sh), the Vulkan renderer's counters
# every 300 frames (SSBU_LOG_STATS), and a summary: CPU time, frames a second per 10 s, frames over
# SSBU_HITCH ms, and the GPU's busy time a frame.
#
# Usage: tools/linux/benchmark.sh <program> <out dir> [seconds] [renderer]
#   <program>   e.g. dist-linux/ssbu-linux/ssbu (run in its folder: its lib/, dump/ and save/)
#   renderer    vulkan (default) or dxvk
# From the environment (besides any SSBU_*): SSBU_MODE, SSBU_SCALE, SSBU_UPSCALE (docked, 100, linear
# by default); SSBU_BENCH_HZ=<hz>@<second> to measure throughput instead of the 60 Hz clock (start it
# once the input has reached the scene: menus move faster too); SSBU_SPIRV_CACHE to keep another
# build's shader cache apart from the game folder's.
# Captures are 6 MB bitmaps (SSBU_CAPTURE_MAX, 8 by default): keep <out dir> off a RAM-backed /tmp for many runs.
# Examples:
#   tools/linux/benchmark.sh dist-linux/ssbu-linux/ssbu /tmp/run 100
#   SSBU_MODE=handheld SSBU_SCALE=75 SSBU_UPSCALE=fsr SSBU_BENCH_HZ=240@60 tools/linux/benchmark.sh dist-linux/ssbu-linux/ssbu /tmp/fast 100
set -u
exe=$(readlink -f "$1"); out=$(readlink -m "$2"); secs=${3:-100}; renderer=${4:-vulkan}
game=$(dirname "$exe")
rm -rf "$out" && mkdir -p "$out/data" "$out/shots"
cp -r "$game/save" "$out/data/" 2>/dev/null
cp "$game/netplay_id.txt" "$out/data/" 2>/dev/null
cd "$game" || exit 1
export SSBU_DATA_ROOT="$out/data" SSBU_MENU=0 SSBU_RENDERER="$renderer" SSBU_MODE=${SSBU_MODE:-docked} SSBU_SCALE=${SSBU_SCALE:-100} SSBU_UPSCALE=${SSBU_UPSCALE:-linear}
export SSBU_WINDOW=${SSBU_WINDOW:-windowed} SSBU_PACING=clock SSBU_WATCHDOG=0 SSBU_MINIDUMP=0
export SSBU_HITCH=${SSBU_HITCH:-25} SSBU_LOG_STATS=1
export SSBU_CAPTURE="$out/shots" SSBU_CAPTURE_EVERY=${SSBU_CAPTURE_EVERY:-5} SSBU_CAPTURE_MAX=${SSBU_CAPTURE_MAX:-8}
export SSBU_INPUT=${SSBU_INPUT:-"8:A,10:A,13:B,15:A,18:A,22:A,24.5:A,26:A,27.5:A,29:A,30.5:A,32:SUP/0.35,33:A,35:SUP/0.25,36:A,38:SUP/0.2,39:A,41:A,43:PLUS,44.5:PLUS,46:PLUS,52:A,54:A,56:A"}
unset SSBU_NET_RELAY SSBU_NET_HOST
/usr/bin/time -v -o "$out/time.txt" timeout -s INT "$secs" "$exe" dump/modules --status 10 > "$out/log.txt" 2>&1
mv build/profile_*.txt "$out/" 2>/dev/null
echo "== $out"
grep -E "Elapsed|User time|System time|Maximum resident" "$out/time.txt"
grep '^\[status\] [0-9]* s into' "$out/log.txt" | sed -E 's/.*\[status\] ([0-9]+) s.* ([0-9]+) frames presented/\1 \2/' |
    awk 'NR > 1 { printf "%3d s: %.1f frames a second\n", $1, ($2 - pf) / ($1 - pt) } { pt = $1; pf = $2 }'
echo "frames over ${SSBU_HITCH} ms: $(grep -c '^\[hitch\] frame' "$out/log.txt")"
grep -m1 '^\[gfx\] Vulkan: constant' "$out/log.txt"
echo "GPU busy, ms a frame (every 300 frames): $(grep -o 'GPU busy [0-9.]*' "$out/log.txt" | awk '{ printf "%s ", $3 }')"
grep -m3 -A10 'host exception\|stopped during' "$out/log.txt"
