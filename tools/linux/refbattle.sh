#!/usr/bin/env bash
# The reference battle for comparing renderers (DXVK and Vulkan): boot the Linux package, go
# through the menus into a 1v1 Smash battle with scripted input (tools/autobattle.sh's presses),
# time one frame of it on the GPU (SSBU_GPU_TIMES) and summarize the frame rate.
#
# Its settings come from the environment, not display.ini: handheld, FSR, scale 50 %, a
# 1920 x 1080 window, clock pacing, no start-up menu, offline. The game runs on a copy of the save,
# so the user's own is never written; the shader cache is the package's own (warm, as a player's).
#
# Usage: refbattle.sh <package folder> <name> [seconds]
#   <name>: a folder under linux-toolchain/test/runs for the log, captures and summary
#   REF_SAVE=<save folder>: the save to start from (copied; default the package's own). The
#     reference battle's is linux-toolchain/test/refsave/save: the user's, with its "Clasico" rules
#     choosing the stage (made once by refsave.sh)
#   REF_AT=<second>: the frame SSBU_GPU_TIMES times (default 80, well into the battle)
#   REF_MANUAL=1: no scripted input - the player goes to the battle and plays it (give more seconds)
#   SSBU_INPUT, SSBU_CAPTURE_EVERY, SSBU_CAPTURE_MAX: other presses or captures (to work out a route)
#   REF_EXE=<program>: another build in the package folder (default ./ssbu)
#   SSBU_RENDERER and other SSBU_* settings pass through from the environment.
set -u
PKG="$(cd "$1" && pwd)"; NAME="$2"; SECS="${3:-120}"
HERE="$(cd "$(dirname "$0")" && pwd)"
RUN="$(cd "${SSBU_LINUX_TOOLCHAIN:-$HERE/../../../linux-toolchain}" && pwd)/test/runs/$NAME"
rm -rf "$RUN"; mkdir -p "$RUN/root" "$RUN/shots"
cp -r "${REF_SAVE:-$PKG/save}" "$RUN/root/save"
cd "$PKG"
set +u   # (the launchers' helpers read unset variables)
. ./scripts/common.sh
find_game_data || exit 1
set -u
unset SSBU_NET_RELAY SSBU_NET_HOST
export SSBU_DATA_ROOT="$RUN/root" SSBU_CAPTURE="$RUN/shots" SSBU_CAPTURE_EVERY="${SSBU_CAPTURE_EVERY:-5}" SSBU_CAPTURE_MAX="${SSBU_CAPTURE_MAX:-40}"
export SSBU_MENU=0 SSBU_MODE=handheld SSBU_UPSCALE=fsr SSBU_SCALE=50 SSBU_WINDOW=windowed SSBU_WINDOW_SIZE=1920x1080
export SSBU_PACING=clock SSBU_GPU_TIMES="${REF_AT:-80}" SSBU_NET_UPNP=0
[ -n "${REF_MANUAL:-}" ] || export SSBU_INPUT="${SSBU_INPUT:-8:A,10:A,13:B,15:A,18:A,22:A,24.5:A,26:A,27.5:A,29:A,30.5:A,32:SUP/0.35,33:A,35:SUP/0.25,36:A,38:SUP/0.2,39:A,41:A,43:PLUS,44.5:PLUS,46:PLUS,52:A,54:A,56:A}"
timeout "$SECS" "${REF_EXE:-./ssbu}" dump/modules --status 10 > "$RUN/play.log" 2>&1
# Frames a second and draws a frame over each 10 s, from the status lines
awk '/^\[status\] [0-9]+ s into/ {
    t = $2; for (i = 1; i <= NF; i++) { if ($(i+1) == "draws,") d = $i; if ($(i+1) == "frames") f = $i }
    if (pt) printf "%4d s  %5.1f fps  %6.0f draws/frame\n", t, (f - pf) / (t - pt), (f > pf ? (d - pd) / (f - pf) : 0)
    pt = t; pd = d; pf = f }' "$RUN/play.log" > "$RUN/summary.txt"
grep -E '^\[gpu times\]' "$RUN/play.log" >> "$RUN/summary.txt"
grep -E '^\[gfx\] (Direct3D|Vulkan)' "$RUN/play.log" | head -1 >> "$RUN/summary.txt"
cat "$RUN/summary.txt"
echo "$RUN: $(ls "$RUN/shots" | wc -l) captures"
grep -A10 'host exception\|stopped during' "$RUN/play.log" | head -14
