# Multiplayer

[← README](README.md) · [Setup guide](docs/BUILDING.md) · [Playing](docs/PLAYING.md) · [Technical guide](docs/TECHNICAL.md)

WindowSmash plays the game's **Local Wireless** mode over the internet. To the game, the other players look like Switches nearby. Nintendo's online service isn't used.

> [!NOTE]
> **Tested so far:** connecting and finding rooms, including the online lobby with several games on one PC, through the public lobby, and between a Windows and a Linux PC. Full matches between two PCs, every DLC fighter online, and every kind of home network haven't been fully verified yet.

## Contents

- [Choose how to play](#choose-how-to-play)
- [Before you play](#before-you-play)
- [Online lobby](#online-lobby)
- [Direct connections and your internet address](#direct-connections-and-your-internet-address)
- [Private room](#private-room)
- [With a Switch on your network (LAN mode)](#with-a-switch-on-your-network-lan-mode)
- [Mods in a room](#mods-in-a-room)
- [If a room doesn't appear](#if-a-room-doesnt-appear)
- [Run your own lobby](#run-your-own-lobby)
- [Logs and privacy](#logs-and-privacy)
- [Reference: environment variables](#reference-environment-variables)

## Choose how to play

| Way | Best for | Router setup | Start with |
| :--- | :--- | :--- | :--- |
| [**Online lobby**](#online-lobby) | Playing with anyone | None | `Play SSBU.bat` |
| [**Lobby code**](#friends-only-a-lobby-code) | Friends only, through the lobby | None | `Play SSBU.bat` |
| [**Private room**](#private-room) | A group with one host | The host may need to open a port | `Host SSBU Netplay.bat` and `Join SSBU Netplay.bat` |
| [**Your own lobby**](#run-your-own-lobby) | Communities with a server | The server's port | `Play SSBU.bat` |
| [**LAN mode**](#with-a-switch-on-your-network-lan-mode) | A Switch (or PCs) on the same home network | None | `Play SSBU LAN.bat` |

The online lobby is the easiest: nobody hosts anything outside the game, and nobody touches their router.

On Linux, the start-up menu's **Network** page (Tab in the settings menu, or F8 twice in the game) does what the host and join launchers do, without them: it hosts or joins a private room, remembers the name, room, port and the last rooms joined in `netplay.ini`, shows the host's addresses and whether the router opens the port, and lists the rooms hosted on the same network so nobody types an address. See [README-LINUX.md](README-LINUX.md#network-page).

On Linux ([README-LINUX.md](README-LINUX.md)), `run.sh`, `run-offline.sh`, `run-lan.sh`, `host-netplay.sh` and `join-netplay.sh` do what `Play SSBU.bat`, `Play SSBU Offline.bat`, `Play SSBU LAN.bat`, `Host SSBU Netplay.bat` and `Join SSBU Netplay.bat` do. Windows and Linux players with the same game version play together.

## Before you play

Whichever way you choose:

1. **Everyone uses the same WindowSmash and the same game version.** A 13.0.5 player can't play with an 11.0.0 player.
2. **Every PC is set up with its own game data**, following the [setup guide](docs/BUILDING.md). Match the DLC between players too.
3. **Every PC keeps its own save and its own `netplay_id.txt`.** Never copy `netplay_id.txt` to another PC: two PCs with the same one look like one player.
4. **Check the game works offline first** with `Play SSBU Offline.bat`, then close it.

Each WindowSmash window is one player. A private room holds up to eight members, and the game's own room rules decide how many can play a match.

## Online lobby

This is what `Play SSBU.bat` does by default. Every Local Wireless room that anyone creates shows up for every player with the same game version.

1. Double-click **`Play SSBU.bat`**. The first time, it asks for the name other players see. Type one and press <kbd>Enter</kbd>.
2. If Windows Security asks about `ssbu.exe`, click **Allow**. No port forwarding is needed: your game only sends to the lobby.
3. In the game's main menu, choose **Local Wireless**.
   - **To host:** choose **Create a Room**. When a challenger appears, accept them.
   - **To join:** open **Rooms List** and pick a room. Open rooms come first, fuller ones before emptier ones.
4. Press <kbd>F1</kbd> to see the connection, for example `Online: 37 players, 4 rooms - you are Alpha, ping 48 ms`. A second line lists the other players in your room and how each one is reached ([see below](#direct-connections-and-your-internet-address)).

`online_server.txt`, in the WindowSmash folder, names the lobby. Its first line that doesn't start with `#` is the lobby's address, `smash.recomps.wtf:39420` (the public lobby) unless you change it.

### Friends only: a lobby code

A lobby code hides your rooms from everyone who doesn't use the same code.

1. Close the game and open `online_server.txt` in Notepad.
2. Add a space and your code after the address, for example:

   ```text
   smash.recomps.wtf:39420 ourcode
   ```

3. Save it. Everyone in your group does the same, with the same code.

The overlay shows the code after `Online`. A lobby code is not a password: anyone who knows it can join.

### Playing offline

- `Play SSBU Offline.bat` never goes online.
- To make `Play SSBU.bat` start offline too, write `off` in `online_server.txt` in place of the address.
- If the lobby doesn't answer for five seconds, the game plays offline by itself and keeps trying in the background.

## Direct connections and your internet address

Online, the games in a room send their traffic **straight to each other** whenever the players' routers allow it. The lobby introduces them, and the games open a path through their routers themselves (UDP hole punching), so nobody forwards a port for this either. Where that doesn't work, the traffic keeps going through the lobby. Some mobile networks, and providers that share one address between many customers (symmetric NAT), don't allow direct paths. A direct path that goes quiet falls back to the lobby within about a second and is tried again later.

Through the lobby, the delay between two players is roughly both players' pings to the lobby added together. Directly, it's just the delay between the two PCs.

The overlay's second line shows each player in your room, for example `Room: Bravo direct 12 ms, Kim relayed`:

| Shown | Meaning |
| :--- | :--- |
| `direct 12 ms` | Straight to that player, 12 ms there and back. |
| `relayed` | Through the lobby. |
| `relayed (trying direct)` | Still opening a direct path. |
| `relayed (direct off)` | That player turned direct connections off. |
| `relayed (direct off here)` | You turned them off. |
| `relayed (older game)` | That player's WindowSmash is too old for direct connections. |

Direct connections need a lobby running this version of `tools/ssbu_relay.py`. With an older lobby, or a player with an older WindowSmash, play goes through the lobby as before.

> [!NOTE]
> **Your internet address.** The players in a room with you learn your internet (IP) address, as they would playing online on a console. The lobby introduces only players whose games are already playing together in a room, and introduces each player in at most 20 rooms an hour.
>
> To keep your address to yourself, create a file named `online_direct.txt` next to `Play SSBU.bat` (on Linux, next to `run.sh`) containing the word `off`. All your traffic then goes through the lobby, which gives your address to nobody.

Private rooms always go through the host.

## Private room

One player hosts a room on their own PC; the others join it with the host's address. Agree on a room name first, for example `smash`.

### The host

1. Double-click **`Host SSBU Netplay.bat`**.
2. It asks three questions:
   - **Your player name:** type your name and press <kbd>Enter</kbd>.
   - **Room name:** type the name you agreed on and press <kbd>Enter</kbd>.
   - **UDP port:** press <kbd>Enter</kbd> to keep **39420**.
3. If Windows Security asks about `ssbu.exe`, click **Allow**. If you set up your firewall by hand, it must let in **UDP** on that port.
4. The launcher asks your router to open the port (UPnP). The address to give your friends, for example `203.0.113.7:39420`, appears in the launcher's window or in `netplay_address.txt`, and in the game's overlay (<kbd>F2</kbd>). Send it to the others.
5. In the game's main menu, choose **Local Wireless** (the icon of a console with waves, in the right-hand column), then **Create a Room**. When a friend's challenge appears, accept it.

The address above is only an example. Use the one your own launcher shows, or a VPN or home-network address ([which address](#which-address-to-give)).

### Joining players

1. Double-click **`Join SSBU Netplay.bat`**.
2. It asks three questions:
   - **Host's address:** type the address the host sent you, including the `:` and port, and press <kbd>Enter</kbd>.
   - **Your player name:** type your name and press <kbd>Enter</kbd>.
   - **Room name:** type **exactly** the host's room name and press <kbd>Enter</kbd>.
3. In the game, press <kbd>F2</kbd> and check that the relay is reached and the player count looks right.
4. Choose **Local Wireless**, pick the host's room from **Rooms List**, and wait for the host to accept.
5. If the game asks for a Smash Tag, make your own.

Joining players don't need to change anything on their routers.

### Which address to give

| Where the players are | Address the others type | What the host sets up |
| :--- | :--- | :--- |
| Same home network | The host PC's local address and port | Let incoming UDP through the host's firewall. |
| Different homes, UPnP works | The public address the launcher shows | Nothing more: the launcher opened the port. |
| Different homes, port forwarded by hand | The host's public address and port | Forward **UDP 39420** on the router to the host PC; allow it in the firewall. |
| Everyone on one VPN or virtual LAN | The host's VPN address and port | Everyone joins the same virtual network and allows traffic between its members. |
| A relay on a server | The server's address and port | Everyone, the host included, uses `Join SSBU Netplay.bat`. |

If your internet provider uses carrier-grade NAT, or your router sits behind another router, forwarding a port at home may not be enough. A virtual LAN, a relay on a server, or the [online lobby](#online-lobby) is usually easier.

## With a Switch on your network (LAN mode)

> [!NOTE]
> **Experimental, not yet tested against a console.** The PC side starts and uses the network; whether a Switch sees its rooms hasn't been verified.

The game has a LAN mode of its own, made for tournaments: instead of the console's local wireless radio it plays over an ordinary network (Wi-Fi or cable). `Play SSBU LAN.bat` (`run-lan.sh` on Linux) gives the game this PC's own network instead of the virtual one the lobby builds, so a Switch on the same network can play with it. The Switch needs no modification, no homebrew and no Nintendo Switch Online.

1. Connect the Switch to the same network as the PC (*System Settings > Internet*). The PC and the Switch have to be on the same subnet, for example both `192.168.1.x`.
2. Make sure the Switch has the **same game version** as the PC (*Smash's* version, for example 13.0.x). Different versions don't see each other's rooms.
3. Start the game on the PC with `Play SSBU LAN.bat` (or `bash run-lan.sh`, which uses the Vulkan renderer unless `SSBU_RENDERER` says otherwise).
4. On **both** the Switch and the PC: in the main menu choose **Local Wireless**, then press **L + R + left stick (click)** together. The screen's title changes to **LAN mode**.
5. One side creates a room, the other joins it from the room list.

If the room doesn't show up:

- Let the game through the PC's firewall: UDP, incoming, on your home network (on Windows, accept the prompt as *Private network*; on Linux, for example `sudo ufw allow from 192.168.1.0/24 proto udp`).
- Some routers keep Wi-Fi devices apart (*AP isolation*, *client isolation*, a guest network). Turn it off, or connect the PC by cable.
- On a PC with more than one network (VPN, virtual machines, Docker), set `SSBU_NET_LAN_IP` to the PC's address on the Switch's network. The overlay (<kbd>F2</kbd>) shows the address the game uses.
- `SSBU_NET_LOG=2` logs every datagram the PC receives (`[net] lan: ...`).

## Mods in a room

Only the color each player picks travels over the network; every game draws it with its own files. So if the host plays Mario with a skin in color 4 and you don't have that skin, you see the normal Mario. And if the host has a mod that changes how the game plays (fighter parameters, `.prc` files, animations) and you don't, the match can desync. When you join a room, the game can fetch the host's mods and use them for that room, without touching your own `mods` folder.

> [!NOTE]
> Linux only for now: the Windows build doesn't read the `mods` folder yet. A Windows player joins and plays as before, without the dialog.

### What is shared

| | Private room | Online lobby |
| :--- | :--- | :--- |
| Visual mods (skins, portraits, voices, effects) | Their files | Their files, only over a direct connection with the host |
| Mods that change how the game plays | Their files | Their names only, to warn you |

Only files at `data.arc` paths, and each mod's `config.json` (which says where its new files go), are ever shared. Code (`.nro` plugins, `.lua` scripts) and files outside `data.arc` never are. The mods you downloaded from a room are never shared again when you host.

### When you join

- **Private room:** after you press Start with *Join a room* (or Join on a room in the list, which starts at once), the game asks the host for its list. If there's something to download or to warn about, a dialog lists each mod (new, changed, moved, you have it, the host no longer uses it), the size, where it comes from and the mode. **Play without them** is selected when it opens; **Download and apply** fetches them and starts the game with them; **Menu** goes back to the Network page. During the download, Back cancels (what has arrived is kept) and Select+Start or Esc held for a second goes back to the menu.
- **Online lobby:** the game is already running, so the dialog appears over it when you enter a room. The download continues while you play; afterwards **Restart and rejoin** starts the game again on the Network page with the mods, and **Later** keeps them for the next time you join that host. **Menu** leaves the room and restarts on the menu.

If you already have everything from last time, there is no dialog: the mods are used (a mod the host moved to another color just moves) and a short note says so. A host running an older version doesn't answer; after 3 seconds you play as before.

### Mirror and overlay

| Mode | Your own mods | The host's mods |
| :--- | :--- | :--- |
| **Mirror** (default) | All off | All on: you see and play exactly what the host has |
| **Overlay** | Your skins on, and they win where they clash; your gameplay mods off | All on where your skins don't cover them |

In both modes the mods that change how the game plays are exactly the host's, which is what keeps the match in sync. The mode applies to that start only: offline, or in another host's room, your own mods load as always.

### Settings (`netplay.ini`)

| Key | Values | Meaning |
| :--- | :--- | :--- |
| `room_mods` | `ask` (default), `always`, `never` | Show the dialog, download without asking, or never even ask the host for its list. |
| `room_mods_mode` | `mirror` (default), `overlay` | The mode the dialog opens with (and `always` uses). |
| `share_mods` | `on` (default), `off` | When you host: give your mods' files. With `off`, only the names of your gameplay mods are told, so others know. |
| `room_mods_rate` | `4M` (default), `512K`... | When you host: how fast you give them, at most. Nothing is given while you are in a fight, so the download never lags a match. |
| `host_id` | 16 hex digits, made once | Who you are as a host to those who keep your mods. It's random and says nothing about your PC. If you copy `netplay.ini` to another PC, delete this line there to get a new one. |

On the **Mods** page, the left trigger (or **S**) marks a mod **Don't share**. A visual mod marked that way isn't announced at all, so others see the normal fighter. A gameplay mod marked that way is announced without its files, so others are warned.

### Where they are kept

Downloaded mods go to `mods_join/`, beside `mods/`. It has a folder for each host, and in it each of its mods as a normal mod folder (its files, its `config.json`, an `info.toml`), plus `windowsmash.toml` with the colors the host plays them in: the room's configuration. In that host's room the game reads that folder just as it reads `mods/`. Anything you already have isn't downloaded again: a file of your own mods with the same content is linked instead, and each file is stored once in `.blobs/` (the mod folders are hard links to it), even when several hosts use it. `.hosts/<host_id>/host.toml` says what each host has; editing the folders changes nothing, they're made again from it each time. Rejoining a host downloads only what changed. A mod the host drops stays there unused, so if the host brings it back it isn't downloaded again. A host you haven't joined for 30 days is forgotten with its files (files another host still uses are kept). The **Mods** page lists each host; X deletes one.

### Where they come from, and limits

- From the host: through the private room's relay, or straight to you in the lobby. Never through the public lobby, which drops mod files.
- From where the mod is published: if a visual mod's `info.toml` has `url = "https://.../mod.zip"` and `url_sha256 = "<the .zip's SHA-256>"`, the game downloads the `.zip` from there first (Linux, with the system's libcurl) and takes from it the files whose hashes match. The dialog warns you that the site sees your IP address. If that fails, the files come from the host.
- Every file is checked against its SHA-256 and kept only when it matches. If one fails, its whole mod isn't used and the dialog says so.
- A room announces at most 128 mods, 20,000 files and 4 GB, and 128 MB per file. A mod beyond that isn't announced, and the host's log says why (`[mods]`).

Mods are shared between the players of one room when one of them asks. Respect their authors: mark mods you may not pass on as **Don't share**, and prefer mods that declare where they come from.

## If a room doesn't appear

| What you see | What to check |
| :--- | :--- |
| The overlay stays on `connecting` | The host's address and port, that the host's game is still running, the host's firewall rule for incoming UDP, the router's port mapping, and whether the relay can be reached. |
| `no answer from … - playing offline` | The lobby in `online_server.txt` hasn't answered for five seconds. Check the address with `python tools/ssbu_relay.py --probe <address>`. |
| Online, a room is missing from the list | Both players need the same game version and the same lobby code (or none), and both overlays must say `Online`. The list refreshes every few seconds. |
| Both are connected, but in different groups | Private-room names must match exactly. |
| The relay count is right, but no Local Wireless room | The host must create a room in the game: being connected doesn't create one. Wait for the list to refresh. |
| Two people show up as one player | Their installs share an identity. Close the game on one PC and **rename** its `netplay_id.txt`, so a new one is made. Keep both saves. |
| *Communication error* after joining | Packet loss, whether the relay can be reached, matching versions and DLC, and whether the host or relay closed or changed networks. |
| Works at home, not over the internet | The host's firewall, UDP port forwarding, double NAT or carrier-grade NAT. Or use a virtual LAN, a relay on a server, or the online lobby. |
| Online, a player is always `relayed` | Their router or yours doesn't allow direct paths (symmetric NAT, common on mobile networks), a firewall drops the game's packets, or the lobby is older than direct connections. Play still works through the lobby. If Windows Firewall was told to block `ssbu.exe`, allow it in **Windows Security → Firewall & network protection → Allow an app through firewall**. |
| It works, but feels delayed | Traffic through a relay or the lobby adds both players' pings to it. Pick a host or lobby close to the players, and check the overlay for players who aren't `direct`. |

In a private room, the host has no relay hop to itself; each joining player's traffic goes through the host's relay, including traffic between two joining players. Local Wireless was designed for players in the same room, so internet delay can change how the game behaves.

## Run your own lobby

`tools/ssbu_relay.py` is the online lobby. It needs **Python 3.8 or newer**, a UDP port others can reach, and no game files:

```powershell
python tools/ssbu_relay.py --port 39420 --verbose
```

1. Let incoming UDP 39420 through that machine's firewall, and any network firewall in front of it.
2. Every player puts the lobby's address in `online_server.txt` and starts the game with `Play SSBU.bat`.
3. One player creates a Local Wireless room; the others pick it.

`Join SSBU Netplay.bat` with the lobby's address works too; the room name then acts as the lobby code.

| Task | How |
| :--- | :--- |
| Check that a lobby answers | `python tools/ssbu_relay.py --probe <address>` |
| Install it on a Linux server | `tools/deploy/deploy_lobby.sh <ssh host> install` sets it up as a sandboxed systemd service, `ssbu-lobby`. |
| Turn addresses away | `--banlist FILE` refuses the addresses or networks listed in that file, and re-reads it when it changes. |
| Watch it | It logs a line of statistics every minute. |

The lobby checks where messages come from, limits how fast each player can send, and carries each room's traffic only between that room's players. A game accepts direct packets only from addresses the lobby gave it, carrying that pair's secret, and only from players in its own room. Even so, it's a transport for players you trust: lobby codes and room names aren't passwords, and the traffic isn't encrypted or authenticated.

## Logs and privacy

The launchers write `build\play.log`. For more network detail, set `SSBU_NET_LOG=1` before starting the game (`2` logs every call). Logs and the overlay can show player names, folder names and internet addresses, so read any excerpt before you share it.

## Reference: environment variables

| Variable | Meaning |
| :--- | :--- |
| `SSBU_NET_HOST=1` | Start a relay inside the game and join it (hosting a private room). |
| `SSBU_NET_RELAY=address[:port]` | Join the online lobby or the relay at this address. |
| `SSBU_NET_ROOM` | The lobby code online, or the room name in a private room. Default `smash`, the public lobby. |
| `SSBU_NET_NAME` | The player name others see. |
| `SSBU_NET_PORT` | The relay's port. Default `39420`. |
| `SSBU_NET_UPNP=0` | Don't ask the router to open the port. |
| `SSBU_NET_BIND` | Limit which address the built-in relay listens on; used by local tests. |
| `SSBU_NET_DIRECT=0` | Online, send all traffic through the lobby. `Play SSBU.bat` sets it when `online_direct.txt` says `off`. |
| `SSBU_NET_TEST_NAT=1`, `SSBU_NET_TEST_BLOCK=<seconds>` | Local tests: filter incoming packets as a router would, or cut direct paths that many seconds after the start. |
| `SSBU_NET_LAN=1` | LAN mode: no lobby or relay, the game's sockets are the PC's own on its own network. `Play SSBU LAN.bat` sets it. |
| `SSBU_NET_LAN_IP` | In LAN mode, the PC's address to use. Default: the one the PC reaches the internet by. |
| `SSBU_NET_LOG=1` / `2` | Network logging: a summary, or more detail. |
| `SSBU_NET_ANNOUNCE=0` | A hosted room does not answer the Network page's search for rooms on the same network (Linux and Windows hosts answer it by default, only from private addresses). |
| `SSBU_NETPLAY_INI` | Linux: the Network page's settings file instead of `netplay.ini`. |
| `SSBU_MODS_FIGHT_PPS` | When hosting room mods: the game's packets per second above which a fight is assumed and nothing is given. Default `30`. |
| `SSBU_ROOM_MODS` | Set by the game itself (`<host_id>:mirror\|overlay`): the room mods this start loads. |

Without a host or relay setting, the game stays offline. `Play SSBU Offline.bat` clears both; `Play SSBU.bat` takes the relay from `online_server.txt`; the host and join launchers set their own.
