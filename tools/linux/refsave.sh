#!/usr/bin/env bash
# Make the reference battle's save (refbattle.sh): a copy of the package's save whose "Clasico"
# rules pick Battlefield every time. Those rules choose the stage at random from their "random
# stages" list, so the scripted input opens them (Smash > Normal battle > rules > X > Edit), clears
# that list ("None") but for Battlefield, and saves. The package's own save is never written.
#
# Usage: refsave.sh <package folder>   -> linux-toolchain/test/refsave/save
# The presses are timed for a warm start (about 20 s to the rules); check the captures in
# linux-toolchain/test/runs/refsave/shots if a slower machine misses one.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(cd "${SSBU_LINUX_TOOLCHAIN:-$HERE/../../../linux-toolchain}" && pwd)/test/refsave"
IN="10:A,14:A,18:A,22:A,26:X,29:A"                       # title, Smash, Normal battle, rules: Edit "Clasico"
for i in $(seq 0 8); do IN="$IN,$((32 + i)):DOWN"; done   # down to "Random stages"
IN="$IN,42:A,45:LEFT,46.5:A,48:RIGHT,49.5:A,52:B,55:PLUS" # None, Battlefield on, back, Save
REF_SAVE= SSBU_INPUT="$IN" SSBU_CAPTURE_EVERY=1 SSBU_CAPTURE_MAX=70 "$HERE/refbattle.sh" "$1" refsave 64 > /dev/null
RUN="$OUT/../runs/refsave"
if cmp -s "$1"/save/*/save_data/system_data.bin "$RUN"/root/save/*/save_data/system_data.bin; then
    echo "The save did not change: see $RUN/shots"; exit 1
fi
rm -rf "$OUT"; mkdir -p "$OUT"; cp -r "$RUN/root/save" "$OUT/save"
echo "$OUT/save"
