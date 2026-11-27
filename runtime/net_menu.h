/* The start-up menu's Network page (net_menu.c): how this start goes online, saved in netplay.ini.
 *
 * Connection: "As launched" leaves the launcher's choice (run.sh's lobby, run-offline.sh, run-lan.sh...)
 * as the environment has it; "Create a room" hosts a private room here; "Join a room" joins one at an
 * address, typed, picked from the recent ones or from the rooms found on this PC's network - never
 * this PC's own. Until the address is typed or picked, Join chooses it: the last room joined when it
 * answers, else (saying no room is there) the first room found that has room for another player. Applied
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
#define NET_PAGE 4            /* rooms listed at a time on Join's page */
/* Join's list: NROW_LAN its heading, NROW_LIST.. a room each (choices[page * NET_PAGE + k]), NROW_PAGER Refresh and the pages */
enum { NROW_MODE, NROW_NAME, NROW_ROOM, NROW_PORT, NROW_HOST, NROW_LAN, NROW_LIST, NROW_LIST_END = NROW_LIST + NET_PAGE - 1, NROW_PAGER,
       NROW_ADDRS, NROW_ROUTER, NROW_TIP, NROW_MODS, NROW_PLAY, NROWS };   /* (NROW_MODS: the mods that can desync, when there are) */
enum { NET_EV_NONE, NET_EV_PLAY, NET_EV_REFRESH };   /* what net_menu_event says happened (REFRESH: look for rooms again) */

/* found: answering now (on the LAN, or the address asked), else recent; players of most (most 0: not said);
 * ms: how long it took to answer (found ones) */
typedef struct NetChoice { char addr[NET_ADDR], room[NET_TEXT], who[NET_TEXT]; int found, players, most, ms; } NetChoice;
#define NET_SELF 8

/* The address asked whether a room is there (net_discover_watch): */
enum { NET_WATCH_NONE, NET_WATCH_CHECKING, NET_WATCH_UP, NET_WATCH_DOWN };
/* What Join chose by itself (NetMenu.auto_state): */
enum { NET_AUTO_NONE, NET_AUTO_CHECKING, NET_AUTO_LAST, NET_AUTO_FIRST, NET_AUTO_LAST_DOWN };

/* The room's mods (openspec change netplay-mod-sync, mod_sync.h): what this player does with a host's,
 * and what it shares when it hosts */
enum { ROOM_MODS_ASK, ROOM_MODS_ALWAYS, ROOM_MODS_NEVER };   /* room_mods: the dialog, download without asking, never ask the host */
enum { ROOM_MODE_MIRROR, ROOM_MODE_OVERLAY };                /* room_mods_mode: the host's mods alone, or this player's skins over them */
#define ROOM_RATE_DEFAULT (4L << 20)                         /* room_mods_rate: bytes a second the host serves at most */

typedef struct NetSettings {
    int mode;
    char name[NET_TEXT], room[NET_TEXT], port[6], host[NET_ADDR];
    NetChoice recent[NET_RECENT];   /* (addr, room) of the last rooms joined, newest first */
    int nrecent;
    int file_mode;                  /* netplay.ini's mode (mode may differ: a launcher's own choice, below) */
    char host_id[17];               /* this game's identity as a host: 16 hex digits, random ("" not made yet: net_host_id) */
    int share_mods;                 /* share_mods = on | off (1, the default: the host serves its mods' files) */
    int room_mods, room_mods_mode;  /* ROOM_MODS_*, ROOM_MODE_* */
    long room_mods_rate;            /* bytes a second */
} NetSettings;

typedef struct NetMenu {
    NetSettings s, initial;
    int row;
    NetChoice choices[NET_CHOICES];   /* the Host row's < >: rooms found on the LAN, then recent ones */
    int nchoices, choice;             /* (choice: -1 while the address is typed) */
    int page;                         /* Join's list: the page of NET_PAGE rooms shown */
    int no_play;                      /* no Play row (the start-up menu plays from its Display page): a room's Join plays at once */
    int host_auto;                    /* the address is Join's own choice still (not typed, not picked) */
    int auto_state, last_down;        /* NET_AUTO_*; the last room joined did not answer */
    int watch;                        /* NET_WATCH_*: whether a room answers at the address */
    char self[NET_SELF][NET_ADDR];    /* this PC's addresses (never offered to join) */
    int nself;
    /* shown, filled in by the caller */
    char launched[96];                /* what "As launched" means now: "Online lobby smash.recomps.wtf:39420"... */
    char addrs[96], router[96], lan[96], tip[128];
    char tip_kind;                    /* how the Tip is drawn: '\5' a note, '\3' a warning, '\4' good news */
    char mods_warn[200];              /* the enabled mods that change more than looks (mod_gameplay_warning), "" none */
} NetMenu;

/* ---- the model ---- */
void net_menu_init(NetMenu *m, const NetSettings *s);
int net_menu_visible(const NetMenu *m, int row);
int net_menu_selectable(const NetMenu *m, int row);
int net_menu_text_row(const NetMenu *m);   /* the selected row takes typed characters */
const char *net_menu_label(int row);
/* The value shown. A leading control character asks for it drawn as it is (no < >): '\1' information,
 * '\2' a text field, '\3' a warning, '\4' good news, '\5' a note (the last three across the row);
 * Join's list: '\6' its heading (the label, then the value under it), '\7' a room and '\b' the pages
 * (see net_menu_value). */
void net_menu_value(const NetMenu *m, int row, char *out, unsigned n);
/* MENU_UP/DOWN/LEFT/RIGHT/ACCEPT (gpu_menu.h), or MENU_CHAR with c, MENU_ERASE: NET_EV_PLAY when Play was chosen */
int net_menu_event(NetMenu *m, int event, int c);
int net_menu_pages(const NetMenu *m);   /* Join's list: how many pages of rooms (1 at least) */
int net_menu_room_joinable(const NetMenu *m, int row);   /* a room row whose Join starts the game (found, not full) */
/* The rooms found on the LAN now (the Host row's choices first, then the recent ones) */
void net_menu_set_found(NetMenu *m, const NetChoice *found, int n);
int net_menu_is_self(const NetMenu *m, const char *addr);   /* this PC (its addresses, the loopback) */
const char *net_menu_last(const NetMenu *m);                /* the last room joined (not this PC), NULL if none */
/* Join, the address not typed or picked: the last room joined while it answers (watch: the address's
 * NET_WATCH_*), else the first room found with room for another player. Call after net_menu_set_found. */
void net_menu_auto_host(NetMenu *m, int watch);

/* What net_discover.c found out about this PC's network, for the information rows and the Tip */
enum { NET_UPNP_UNKNOWN, NET_UPNP_OPEN, NET_UPNP_NONE, NET_UPNP_REFUSED, NET_UPNP_BEHIND_NAT };
typedef struct NetProbe {
    int upnp;                        /* NET_UPNP_*: the router, asked to open the port */
    int searching;                   /* still looking for rooms on the LAN */
    char lan_ip[NET_ADDR], vpn_ip[NET_ADDR], wan[NET_ADDR];   /* this PC's LAN and VPN addresses, the router's outside one */
    char self[NET_SELF][NET_ADDR];   /* all of this PC's addresses */
    int nself;
} NetProbe;
void net_menu_set_self(NetMenu *m, const NetProbe *p);   /* before net_menu_set_found: this PC's addresses left out */
void net_menu_describe(NetMenu *m, const NetProbe *p);   /* This PC, Router, On your LAN and Tip rows */

/* ---- netplay.ini and the environment ---- */
/* netplay.ini (SSBU_NETPLAY_INI names another), over defaults from the environment and online_name.txt.
 * The mode: the one in use before F8 started the program again (SSBU_NET_MENU_MODE, which
 * net_settings_apply sets); else As launched when the launcher is a choice of its own (host-netplay.sh,
 * join-netplay.sh, run-lan.sh, run-offline.sh: SSBU_NET_MODE=launcher); else netplay.ini's. */
void net_settings_load(NetSettings *s);
int net_settings_save(const NetSettings *s);   /* 1 written */
/* This game's host_id (netplay.ini's), made the first time it is asked for - 64 random bits, from
 * neither the PC nor the name - and saved there. Never 0. */
unsigned long long net_host_id(void);
void net_settings_remember(NetSettings *s);   /* Join: the address and room to the front of the recent ones */
/* The launcher's own SSBU_NET_* kept aside (once per process tree), so that "As launched" still means
 * them after another mode set the variables, and after F8 started the program again. */
void net_launch_snapshot(void);
void net_launch_describe(char *out, unsigned n);   /* "Online lobby ...", "Offline", "Hosting room smash"... */
void net_settings_apply(const NetSettings *s);     /* the SSBU_NET_* variables, before the network starts */
