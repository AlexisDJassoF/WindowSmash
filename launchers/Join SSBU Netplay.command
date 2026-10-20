#!/bin/bash
# Starts the game as a netplay player joining a host's relay (or a standalone relay,
# tools/ssbu_relay.py). No port forwarding needed here. See docs/MULTIPLAYER.md. The log goes to
# <version folder>/build/play.log and this window. With several game versions built it asks which
# (docs/VERSIONS.md): the host needs the same version. (tools/mac/launch.sh does the work.)
exec /bin/bash "$(cd "$(dirname "$0")/.." && pwd -P)/tools/mac/launch.sh" join "$@"
