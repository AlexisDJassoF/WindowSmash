# Multiplayer setup

[README](README.md) · [Runtime setup](docs/RUNTIME-SETUP.md) · [Build from source](docs/BUILDING.md) · [Technical guide](docs/TECHNICAL.md)

WindowSmash carries the game's **Local Wireless** mode through a UDP relay. One player can run the relay inside their game, or everyone can connect to a standalone relay. Nintendo's online service is not used.

> **Validation status:** connection/discovery paths have been exercised during development. Full two-PC matches, all DLC interactions, and every network environment have not been comprehensively verified.

## Before either player starts

1. Install the **same WindowSmash release/build revision** on both PCs.
2. Configure your own patched game data and fonts using the [runtime setup guide](docs/RUNTIME-SETUP.md), or complete a [source build](docs/BUILDING.md). The current target is game version **11.0.0**.
3. Match game version and relevant DLC configuration. Each PC keeps its own save and its own `netplay_id.txt`; do not copy that identity file between players.
4. Test that each PC can start an offline session. Close it before switching to a host/join session.
5. Choose one person as host and agree on a room name, for example `smash`.

There is one local player per WindowSmash instance. The relay can hold up to eight members; the game room's rules determine the actual supported match size.

## Quick setup: host

1. Double-click **`Host SSBU Netplay.bat`**.
2. Enter your player name, agreed room name, and a UDP port. Keep **39420** unless you need another port.
3. If Windows Firewall asks, allow `ssbu.exe` on the appropriate network. A manually configured firewall must allow incoming **UDP** on the relay port.
4. The launcher attempts a UPnP port mapping. Give your friend the host address shown in `netplay_address.txt` or the game's details overlay (**F2**). An example address is `203.0.113.7:39420`.
5. In the game's main menu, select **Local Wireless**, the console-with-waves icon in the right sidebar.
6. Select **Create a Room**. When the challenger notice appears, accept your friend.

The example address above is documentation only. Use the address produced by your own host, or the VPN/LAN address described below.

## Quick setup: joining player

1. Double-click **`Join SSBU Netplay.bat`**.
2. Enter the host's address including port, your own player name, and the **exact same room name**.
3. Press **F2** in the game and check that the relay connection/player count looks correct.
4. Open **Local Wireless**, choose the host's room from **Rooms List**, and wait for acceptance.
5. If prompted for a Smash Tag, create your own tag.

Joining players make outbound connections to the relay. They do not need to forward ports on their own routers.

## Choose a reachable address

| Situation | Address for joining players | Setup |
| :--- | :--- | :--- |
| Same home network | Host's local LAN IP plus port | Permit inbound UDP in the host firewall. |
| Different networks, working UPnP | Host's reachable public IP plus port | Use the launcher's reported mapping and address. |
| Manual port forwarding | Host's reachable public IP plus port | Forward **UDP 39420** to the host PC's LAN IP; allow the same port in its firewall. |
| Shared virtual LAN / VPN | Host's VPN IP plus port | Both players join the same virtual network and permit traffic between peers. |
| Standalone relay | Relay server IP/hostname plus port | Every game instance uses the Join launcher. |

If your ISP uses carrier-grade NAT, or the router is behind another router, a home port forward may be insufficient. A shared virtual LAN or a reachable standalone relay is often the more practical route.

## Standalone relay

The standalone relay needs **Python 3.8+**, a reachable UDP port, and no game files:

```powershell
python tools/ssbu_relay.py --port 39420 --verbose
```

Allow inbound UDP 39420 on that machine and any network firewall in front of it. Then **all players**, including the person who will create the in-game room, use **Join SSBU Netplay.bat** with the relay's address. One player creates the Local Wireless room; the others select it.

The relay is a transport for trusted players. Room names are not passwords; the protocol is not encrypted or designed as a hardened public matchmaking service.

## If the room does not appear

| Symptom | Check |
| :--- | :--- |
| Overlay stays on `connecting` | Host address/port, host process still running, inbound UDP rule, router mapping, and relay reachability. |
| Both are connected but in different member groups | Room names must match exactly. |
| Relay count matches but Local Wireless room is missing | Host must create an in-game room; being connected to the relay alone does not create one. Wait for list refresh. |
| Two people look like one player | Each install needs its own identity. Close the game and **rename** the duplicated `netplay_id.txt` so a new one is generated. Keep each player's save. |
| Communication error after joining | Check packet loss, relay reachability, matching versions/configuration, and whether the host/relay closed or changed networks. |
| LAN works, internet does not | Investigate host firewall, UDP forwarding, double NAT/CGNAT, or use a virtual LAN/standalone relay. |
| Works but feels delayed | All traffic goes through the relay. Prefer a low-latency host near the other players. |

The host has no relay network hop to itself. Each joining player's path includes its connection to the relay; communication between joining players passes through that relay. The game's Local Wireless mode was designed for nearby peers, so internet latency can affect behavior.

## Logs and privacy

Launchers write `build/play.log`. For more network diagnostics, set `SSBU_NET_LOG=1` before launching; `2` logs individual calls. Logs and the overlay may contain player names, local paths, and IP addresses. Review excerpts before sharing them publicly.

## Environment reference

| Variable | Meaning |
| :--- | :--- |
| `SSBU_NET_HOST=1` | Start an integrated relay and join it locally. |
| `SSBU_NET_RELAY=address[:port]` | Join the relay at this address. |
| `SSBU_NET_ROOM` | Relay room name; default `smash`. |
| `SSBU_NET_NAME` | Displayed player name. |
| `SSBU_NET_PORT` | Relay port; default `39420`. |
| `SSBU_NET_UPNP=0` | Disable automatic router mapping. |
| `SSBU_NET_BIND` | Restrict the integrated relay's bind address; used by local diagnostics. |
| `SSBU_NET_LOG=1` / `2` | Enable summary / more detailed network logging. |

Without a host or relay setting, the runtime stays offline. The offline launcher clears both; host and join launchers choose their corresponding modes.
