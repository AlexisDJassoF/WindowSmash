#!/usr/bin/env bash
# Unlocks every fighter in this folder's save (save/), keeping a copy of it first in
# save_backups/. Run it with the game closed. See DLC_AND_UNLOCKS.md.
cd "$(dirname "$(readlink -f "$0")")" || exit 1
python3 tools/unlock_all.py "$@"
