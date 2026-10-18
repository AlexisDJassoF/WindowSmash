/* Netplay: the game's own Local Wireless mode, on a virtual LAN carried through a relay.
 *
 * Each player's game gets an address in 10.13.0.0/16 from the relay; local wireless
 * (runtime/hle_net.c emulates nn::ldn) and the game's sockets live in that network, and their
 * datagrams - broadcasts included - travel to the other players through the relay
 * (runtime/net_relay.c). Clients
 * only send to the relay, so no player needs port forwarding; the relay runs inside the
 * host player's game (its port opened with UPnP) or as tools/ssbu_relay.py anywhere reachable.
 *
 * Configured by the launchers (Host/Join SSBU Netplay.bat) through the environment:
 *   SSBU_NET_HOST=1         run the relay here and join it (port SSBU_NET_PORT, 39420)
 *   SSBU_NET_RELAY=addr     join the relay at addr[:port]
 *   SSBU_NET_ROOM, SSBU_NET_NAME   room to join, player name
 *   SSBU_NET_LOG=1          log every socket/network call (debugging)
 * With neither set the game stays offline, as before. */
#pragma once
#include "recomp.h"

#define NET_SUBNET      0x0A0D0000u   /* 10.13.0.0 */
#define NET_MASK        0xFFFF0000u
#define NET_GATEWAY     0x0A0D0001u   /* 10.13.0.1: the relay's own address */
#define NET_BROADCAST   0x0A0DFFFFu
#define NET_DEFAULT_PORT 39420
#define NET_PROTOCOL    1

int net_configured(void);            /* netplay asked for (host or join) */
int net_log(void);                   /* SSBU_NET_LOG */
u32 net_local_ip(void);              /* this player's virtual address (host order), 0 until the relay gives one */
const char *net_player_name(void);

/* A datagram from one of this player's sockets to another player (or the room: NET_BROADCAST). */
void net_send(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port, const void *data, u32 len);
/* (hle_net.c) A datagram for this player's sockets, from the relay or looped back. */
void vsock_deliver(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port, const void *data, u32 len);

/* One line about the connection for the overlay (empty when netplay is off). */
void net_status(char *out, size_t n);
