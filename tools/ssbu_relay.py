#!/usr/bin/env python3
"""Netplay relay for the SSBU native port, standalone (the host player's game can run the
same relay itself; see runtime/net_relay.c for the protocol and NETPLAY.md for use).

Run it on any machine every player can reach - a cheap or free cloud VM, a Raspberry Pi with
the port forwarded - and have every player join it with "Join SSBU Netplay.bat". Players only
send to the relay, so they need no port forwarding; the relay's machine needs its UDP port
open.

    python3 ssbu_relay.py [--port 39420] [--verbose]

Python 3.8+, standard library only."""
import argparse
import asyncio
import os
import struct
import time

PROTOCOL = 1
JOIN, WELCOME, REJECT, PING, PONG, DATA, MEMBERS, LEAVE = range(1, 9)
NAME_LEN = 32
ROOM_MEMBERS = 8
TIMEOUT = 10.0
GATEWAY = 0x0A0D0001
BROADCASTS = (0x0A0DFFFF, 0xFFFFFFFF)


def header(kind, version=PROTOCOL):
    return b"SN" + bytes((version, kind))


def text(raw):
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


class Member:
    def __init__(self, at, nonce, ip, name):
        self.at, self.nonce, self.ip, self.name, self.seen = at, nonce, ip, name, time.monotonic()


class Relay(asyncio.DatagramProtocol):
    def __init__(self, verbose):
        self.verbose = verbose
        self.rooms = {}       # room name -> [Member]
        self.by_addr = {}     # (host, port) -> (room name, Member)

    def connection_made(self, transport):
        self.transport = transport

    def log(self, *what):
        if self.verbose:
            print(time.strftime("%H:%M:%S"), *what, flush=True)

    def member_list(self, members):
        out = bytes((len(members),))
        for m in members:
            out += struct.pack(">I", m.ip) + m.name.encode("utf-8")[:NAME_LEN].ljust(NAME_LEN, b"\0")
        return out

    def tell_members(self, room):
        members = self.rooms.get(room, [])
        packet = header(MEMBERS) + self.member_list(members)
        for m in members:
            self.transport.sendto(packet, m.at)

    def reject(self, at, nonce, reason, why, version=PROTOCOL):
        self.transport.sendto(header(REJECT, version) + struct.pack(">QB", nonce, reason) +
                              why.encode()[:64].ljust(64, b"\0"), at)

    def drop(self, room, member):
        members = self.rooms.get(room, [])
        if member in members:
            members.remove(member)
            self.by_addr.pop(member.at, None)
            self.log(f"{member.name} left room {room} ({len(members)} left)")
            if members:
                self.tell_members(room)
            else:
                del self.rooms[room]

    def datagram_received(self, p, at):
        if len(p) < 4 or p[:2] != b"SN":
            return
        kind = p[3]
        if p[2] != PROTOCOL:
            if kind == JOIN and len(p) >= 12:
                self.reject(at, struct.unpack(">Q", p[4:12])[0], 2,
                            f"different netplay version (relay {PROTOCOL}, you {p[2]})", p[2])
            return
        known = self.by_addr.get(at)
        if kind == JOIN and len(p) >= 12 + 2 * NAME_LEN:
            nonce = struct.unpack(">Q", p[4:12])[0]
            room, name = text(p[12:12 + NAME_LEN]), text(p[12 + NAME_LEN:12 + 2 * NAME_LEN])
            if known and known[1].nonce != nonce:   # a new game from the same place
                self.drop(*known)
                known = None
            if not known:   # a member back from a new place, or a new one
                for m in self.rooms.get(room, []):
                    if m.nonce == nonce:
                        self.by_addr.pop(m.at, None)
                        m.at = at
                        self.by_addr[at] = (room, m)
                        known = (room, m)
                        break
            if not known:
                members = self.rooms.setdefault(room, [])
                if len(members) >= ROOM_MEMBERS:
                    self.reject(at, nonce, 1, "the room is full (8 players)")
                    return
                used = {m.ip for m in members}
                ip = GATEWAY + 1
                while ip in used:
                    ip += 1
                m = Member(at, nonce, ip, name)
                members.append(m)
                self.by_addr[at] = (room, m)
                known = (room, m)
                self.log(f"{name} joined room {room} as 10.13.{ip >> 8 & 255}.{ip & 255} from {at[0]}:{at[1]}")
                self.tell_members(room)
            room, m = known
            m.seen = time.monotonic()
            self.transport.sendto(header(WELCOME) + struct.pack(">QI", nonce, m.ip) +
                                  self.member_list(self.rooms[room]), at)
        elif kind == PING and known and len(p) >= 20:
            room, m = known
            m.seen = time.monotonic()
            self.transport.sendto(header(PONG) + p[12:20] + bytes((len(self.rooms[room]),)), at)
        elif kind == LEAVE and known:
            self.drop(*known)
        elif kind == DATA and known and len(p) >= 16:
            room, m = known
            m.seen = time.monotonic()
            src, dst = struct.unpack(">II", p[4:12])
            if src != m.ip:
                return
            for other in self.rooms[room]:
                if other is not m and (dst in BROADCASTS or other.ip == dst):
                    self.transport.sendto(p, other.at)

    async def sweep(self):
        while True:
            await asyncio.sleep(1)
            now = time.monotonic()
            for room, members in list(self.rooms.items()):
                for m in list(members):
                    if now - m.seen > TIMEOUT:
                        self.drop(room, m)


async def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", type=int, default=int(os.environ.get("SSBU_NET_PORT", 39420)))
    parser.add_argument("--verbose", action="store_true", help="log joins and leaves")
    parser.add_argument("--bind", default="0.0.0.0", help="the address to listen on (default: all)")
    args = parser.parse_args()
    loop = asyncio.get_running_loop()
    transport, relay = await loop.create_datagram_endpoint(lambda: Relay(args.verbose), local_addr=(args.bind, args.port))
    print(f"SSBU netplay relay on UDP port {args.port} (protocol {PROTOCOL}); Ctrl+C to stop", flush=True)
    try:
        await relay.sweep()
    finally:
        transport.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
