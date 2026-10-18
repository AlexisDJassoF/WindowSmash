#!/usr/bin/env bash
# Super Smash Bros. Ultimate (native port) on Linux, offline (run.sh plays online).
# Usage: bash run-offline.sh [--trace]. The log goes to build/play.log (and this terminal).
cd "$(dirname "$(readlink -f "$0")")" || exit 1
. ./scripts/common.sh
find_game_data || exit 1
check_libraries || exit 1
unset SSBU_NET_RELAY SSBU_NET_HOST
mkdir -p build
echo "Starting the game offline (log: build/play.log)..."
./ssbu dump/modules --status 30 "$@" 2>&1 | tee build/play.log
