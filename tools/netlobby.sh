#!/bin/sh
# Three games on this PC in the online lobby (tools/ssbu_relay.py, run here on loopback): Alpha
# and Bravo each create a Local Wireless room, Charlie opens the Rooms List - which must show
# both - and joins the first. Menus only (no battle), no router or firewall change.
# Usage: tools/netlobby.sh <out prefix> [seconds]
#   SSBU_EXE (default build/ssbu.exe), SSBU_LOBBY_PORT (39510), SSBU_CAPTURE_MAX (80, Charlie's frames)
#   SSBU_LOBBY=host:port: use that lobby (e.g. the online one) instead of starting one here
#   SSBU_INPUT_HOST, SSBU_INPUT_JOIN: the scripted input of the hosts and of Charlie (defaults:
#     11.0.0's menus - Local Wireless, create a room; Rooms List, first room)
#   SSBU_KEEP=1: keep Charlie's frames (<out>-charlie/) instead of deleting them at the end
# Logs: <out>-lobby.log, <out>-alpha.log, -bravo.log, -charlie.log. Prints PASS or FAIL.
out=$1
secs=${2:-110}
exe=${SSBU_EXE:-build/ssbu.exe}
port=${SSBU_LOBBY_PORT:-39510}
nav="9:A,13:DOWN,13.5:DOWN,14:DOWN,14.5:RIGHT,15:RIGHT,15.5:RIGHT,16:RIGHT,17:UP,17.6:UP,18.6:A,21.5:PLUS"
host_input=${SSBU_INPUT_HOST:-"$nav,25:A,28.5:PLUS,36:A"}
join_input=${SSBU_INPUT_JOIN:-"$nav,30:DOWN,31:A"}
for who in alpha bravo charlie; do
    rm -rf "$out-$who" "build/testdata-$who"
    mkdir -p "build/testdata-$who"
    cp -r save "build/testdata-$who/" 2>/dev/null
done
mkdir -p "$out-charlie"
lobby=
if [ -n "$SSBU_LOBBY" ]; then
    where=$SSBU_LOBBY
    : > "$out-lobby.log"
else
    where=127.0.0.1:$port
    python -I tools/ssbu_relay.py --bind 127.0.0.1 --port "$port" --verbose --stats 10 > "$out-lobby.log" 2>&1 &
    lobby=$!
    sleep 1
fi
run() {   # who input seconds [capture dir]
    env SSBU_NET_RELAY="$where" SSBU_NET_LOG="${SSBU_NET_LOG:-1}" SSBU_NET_NAME="$1" \
        SSBU_DATA_ROOT="build/testdata-$(echo $1 | tr A-Z a-z)" SSBU_INPUT="$2" ${4:+SSBU_CAPTURE="$4"} ${4:+SSBU_CAPTURE_EVERY=1} \
        ${4:+SSBU_CAPTURE_MAX="${SSBU_CAPTURE_MAX:-80}"} \
        timeout "$3" "./$exe" dump/modules --status 30 > "$out-$(echo $1 | tr A-Z a-z).log" 2>&1
}
# Alpha accepts its challenger at 85 s (Charlie asks at about 71 s); Bravo never presses A in its room.
run Alpha "$host_input,85:A" "$secs" &
a=$!
sleep 12
run Bravo "$host_input" $((secs - 12)) &
b=$!
sleep 28
run Charlie "$join_input" $((secs - 40)) "$out-charlie" &
c=$!
wait $a $b $c
if [ -n "$lobby" ]; then
    kill "$lobby" 2>/dev/null
    wait "$lobby" 2>/dev/null
fi
ok=1
check() {   # what file pattern
    if grep -q "$3" "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; ok=0; fi
}
echo "netlobby $out:"
check "Alpha joined the lobby" "$out-alpha.log" "joined the lobby"
check "Alpha created a room" "$out-alpha.log" "network created by"
check "Bravo created a room" "$out-bravo.log" "network created by"
check "Charlie saw both rooms" "$out-charlie.log" "scan found 2 networks"
check "Charlie joined a room" "$out-charlie.log" "network: joined"
check "Alpha took Charlie in" "$out-alpha.log" "joined as node"
grep -h 'exception\|stopped during' "$out-alpha.log" "$out-bravo.log" "$out-charlie.log" | head -4
grep 'player' "$out-lobby.log" | tail -2
[ -n "$SSBU_KEEP" ] || rm -rf "$out-charlie"
rm -rf build/testdata-alpha build/testdata-bravo build/testdata-charlie
if [ $ok = 1 ]; then echo PASS; else echo FAIL; fi
