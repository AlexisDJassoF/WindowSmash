/* The start-up menu's Network page (net_menu.c): how this start goes online, saved in netplay.ini.
 *
 * Connection: "As launched" leaves the launcher's choice (run.sh's lobby, run-offline.sh, run-lan.sh...)
 * as the environment has it; "Create a room" hosts a private room here; "Join a room" joins one at an
 * address, typed, picked from the recent ones or from the rooms found on this PC's network. Applied
 * through the SSBU_NET_* variables (net.h) before the game's network starts, so no restart is
 * needed at start-up; in the game, F8 twice starts the program again on this page (gpu_menu.c).
 *
 * The model here has no window and no sockets (tests/test_net_menu.py drives it); what the page
 * shows of this PC's network (addresses, the router, rooms found) the caller fills in from
 * net_discover.c. */
#pragma once

#define NET_TEXT 33           /* a name or a room: 32 characters (net_relay.c's NAME_LEN) */
#define NET_ADDR 64
#define NET_RECENT 5
#define NET_CHOICES 16

enum { NET_MODE_LAUNCHER, NET_MODE_HOST, NET_MODE_JOIN, NET_MODES };
enum { NROW_MODE, NROW_NAME, NROW_ROOM, NROW_PORT, NROW_HOST, NROW_ADDRS, NROW_ROUTER, NROW_LAN, NROW_TIP, NROW_PLAY, NROWS };
enum { NET_EV_NONE, NET_EV_PLAY };   /* what net_menu_event says happened */

typedef struct NetChoice { char addr[NET_ADDR], room[NET_TEXT], who[NET_TEXT]; int found; } NetChoice;   /* found: on the LAN now, else recent */

typedef struct NetSettings {
    int mode;
    char name[NET_TEXT], room[NET_TEXT], port[6], host[NET_ADDR];
    NetChoice recent[NET_RECENT];   /* (addr, room) of the last rooms joined, newest first */
    int nrecent;
    int file_mode;                  /* netplay.ini's mode (mode may differ: a launcher's own choice, below) */
} NetSettings;

typedef struct NetMenu {
    NetSettings s, initial;
    int row;
    NetChoice choices[NET_CHOICES];   /* the Host row's < >: rooms found on the LAN, then recent ones */
    int nchoices, choice;             /* (choice: -1 while the address is typed) */
    /* shown, filled in by the caller */
    char launched[96];                /* what "As launched" means now: "Online lobby smash.recomps.wtf:39420"... */
    char addrs[96], router[96], lan[96], tip[128];
} NetMenu;

/* ---- the model ---- */
void net_menu_init(NetMenu *m, const NetSettings *s);
int net_menu_visible(const NetMenu *m, int row);
int net_menu_selectable(const NetMenu *m, int row);
int net_menu_text_row(const NetMenu *m);   /* the selected row takes typed characters */
const char *net_menu_label(int row);
/* The value shown; a leading '\1' asks for it drawn as it is (no < >): text being typed, information. */
void net_menu_value(const NetMenu *m, int row, char *out, unsigned n);
/* MENU_UP/DOWN/LEFT/RIGHT/ACCEPT (gpu_menu.h), or MENU_CHAR with c, MENU_ERASE: NET_EV_PLAY when Play was chosen */
int net_menu_event(NetMenu *m, int event, int c);
/* The rooms found on the LAN now (the Host row's choices first, then the recent ones) */
void net_menu_set_found(NetMenu *m, const NetChoice *found, int n);

/* What net_discover.c found out about this PC's network, for the information rows and the Tip */
enum { NET_UPNP_UNKNOWN, NET_UPNP_OPEN, NET_UPNP_NONE, NET_UPNP_REFUSED, NET_UPNP_BEHIND_NAT };
typedef struct NetProbe {
    int upnp;                        /* NET_UPNP_*: the router, asked to open the port */
    int searching;                   /* still looking for rooms on the LAN */
    char lan_ip[NET_ADDR], vpn_ip[NET_ADDR], wan[NET_ADDR];   /* this PC's LAN and VPN addresses, the router's outside one */
} NetProbe;
void net_menu_describe(NetMenu *m, const NetProbe *p);   /* This PC, Router, On your LAN and Tip rows */

/* ---- netplay.ini and the environment ---- */
/* netplay.ini (SSBU_NETPLAY_INI names another), over defaults from the environment and online_name.txt.
 * The mode: the one in use before F8 started the program again (SSBU_NET_MENU_MODE, which
 * net_settings_apply sets); else As launched when the launcher is a choice of its own (host-netplay.sh,
 * join-netplay.sh, run-lan.sh, run-offline.sh: SSBU_NET_MODE=launcher); else netplay.ini's. */
void net_settings_load(NetSettings *s);
int net_settings_save(const NetSettings *s);   /* 1 written */
void net_settings_remember(NetSettings *s);   /* Join: the address and room to the front of the recent ones */
/* The launcher's own SSBU_NET_* kept aside (once per process tree), so that "As launched" still means
 * them after another mode set the variables, and after F8 started the program again. */
void net_launch_snapshot(void);
void net_launch_describe(char *out, unsigned n);   /* "Online lobby ...", "Offline", "Hosting room smash"... */
void net_settings_apply(const NetSettings *s);     /* the SSBU_NET_* variables, before the network starts */
