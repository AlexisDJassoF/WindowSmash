#!/usr/bin/env bash
# Super Smash Bros. Ultimate (native port) on Linux, online: the game joins the lobby named in
# online_server.txt, where Local Wireless lists everyone's rooms (see NETPLAY.md). If the lobby
# does not answer, the game plays offline; run-offline.sh never goes online.
# Usage: bash run.sh [--trace]. The log goes to build/play.log (and this terminal). See README-LINUX.md.
cd "$(dirname "$(readlink -f "$0")")" || exit 1
. ./scripts/common.sh
find_game_data || exit 1
check_libraries || exit 1
go_online
mkdir -p build
echo "Starting the game (log: build/play.log)..."
./ssbu dump/modules --status 30 "$@" 2>&1 | tee build/play.log
