#!/bin/bash
# Unlocks every base-game fighter in a game version's save (tools/unlock_all.py), keeping a copy of
# the save first in save_backups/ (as Unlock All Fighters.bat). Close the game first. With several
# game versions built it asks which; it knows the save layouts of 11.0.0 and 13.0.5 and changes
# nothing for another version. See docs/DLC-AND-UNLOCKS.md. Options such as --dry-run are passed on
# to tools/unlock_all.py. (tools/mac/launch.sh does the work.)
exec /bin/bash "$(cd "$(dirname "$0")/.." && pwd -P)/tools/mac/launch.sh" unlock "$@"
