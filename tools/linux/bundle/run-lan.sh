#!/usr/bin/env bash
# Super Smash Bros. Ultimate (native port) on Linux in LAN mode: no lobby or relay - the game uses
# this PC's own network, where a Switch on the same network plays with it in the game's LAN mode
# (Local Wireless, then L+R+left stick). See NETPLAY.md.
# Usage: bash run-lan.sh [--trace]. The log goes to build/play.log (and this terminal).
cd "$(dirname "$(readlink -f "$0")")" || exit 1
. ./scripts/common.sh
find_game_data || exit 1
check_libraries || exit 1
unset SSBU_NET_RELAY SSBU_NET_HOST
export SSBU_NET_LAN=1 SSBU_RENDERER="${SSBU_RENDERER:-vulkan}"   # (the Vulkan renderer unless asked otherwise)
mkdir -p build
echo "Starting the game in LAN mode (log: build/play.log)..."
echo "In the game: Local Wireless, then L+R+left stick (click) to switch to LAN mode - on the Switch too."
./ssbu dump/modules --status 30 "$@" 2>&1 | tee build/play.log
