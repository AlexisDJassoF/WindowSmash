#!/bin/bash
# Starts the native Super Smash Bros. Ultimate, online (as Play SSBU.bat does on Windows): the game
# joins the lobby named in online_server.txt, where Local Wireless lists everyone's rooms (see
# docs/MULTIPLAYER.md). If the lobby does not answer, the game plays offline (launchers/Play SSBU
# Offline.command never goes online). The word off in online_direct.txt keeps all of a room's traffic
# going through the lobby (no direct connections, so the players in your rooms don't learn your
# internet address). Every game version built here has a folder of its own with its own save (the
# port's folder: build-mac/, save/; others: v13.0.5/ and so on - see docs/VERSIONS.md); with more
# than one, this asks which to start. Its log goes to <version folder>/build/play.log and this window.
# (tools/mac/launch.sh does the work; the Play SSBU app the installer puts in ~/Applications does the same.)
exec /bin/bash "$(cd "$(dirname "$0")" && pwd -P)/tools/mac/launch.sh" play "$@"
