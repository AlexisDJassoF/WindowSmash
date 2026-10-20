#!/bin/bash
# Starts the native Super Smash Bros. Ultimate offline (Play SSBU.command plays online). Every game
# version built here has a folder of its own with its own save (the port's folder: build-mac/, save/;
# others: v13.0.5/ and so on - see docs/VERSIONS.md); with more than one, this asks which to start.
# Its log goes to <version folder>/build/play.log and this window. (tools/mac/launch.sh does the work.)
exec /bin/bash "$(cd "$(dirname "$0")/.." && pwd -P)/tools/mac/launch.sh" offline "$@"
