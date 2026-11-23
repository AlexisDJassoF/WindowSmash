/* What the start-up menu's Network page shows of this PC's network (net_discover.c, Linux):
 * its addresses, whether the router opens a port (UPnP), and the private rooms hosted on the
 * same network - found by asking every network's broadcast address on the relay's port, where a
 * host's relay answers (net_relay.c: NET_FIND_*), so no other port is opened anywhere. */
#pragma once
#include "net_menu.h"

void net_discover_start(void);                    /* the LAN search, every second until stopped */
void net_discover_stop(void);
int net_discover_found(NetChoice *out, int max);  /* the rooms that answered in the last few seconds (never this PC's) */
/* addr ("" for none) asked directly every second, from anywhere (the last room joined may be outside the
 * LAN): NET_WATCH_* - checking, a room answers there, or none did (3 asks) */
int net_discover_watch(const char *addr);
void net_discover_check_router(const char *port); /* UPnP asked once per port, in the background */
void net_discover_probe(NetProbe *p);             /* what is known now */
