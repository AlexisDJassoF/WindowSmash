/* runtime/net_menu.c without a window: tests/test_net_menu.py builds this with it.
 *   net_menu_test <script>   one letter each: u d l r a (up down left right accept), k<c> (type c),
 *                            e (Backspace), f (a room found on the LAN: 192.168.1.9:39420 "dojo" by Kim),
 *                            g (rooms found: this PC's own 192.168.1.5:39420, a full one 192.168.1.7, then "dojo"),
 *                            N (no Play row, as the start-up menu's page), m (rooms found: six, room0..room5 at 10.0.0.1..6 - two pages of the list),
 *                            j<0-3> (Join chooses, the address answering NET_WATCH_*),
 *                            p<0-4> (what the network looks like: NET_UPNP_*, LAN 192.168.1.5, VPN from TEST_VPN),
 *                            S (the launcher's variables kept aside), W (remembered and saved), A (applied),
 *                            I (net_host_id: made and saved if netplay.ini has none).
 * netplay.ini: SSBU_NETPLAY_INI. Prints the page and the SSBU_NET_* variables, '|' between them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_menu.h"
#include "net_menu.h"

static const char *env(const char *k) { const char *v = getenv(k); return v ? v : "(unset)"; }

int main(int argc, char **argv) {
    NetSettings s;
    const char *script = argc > 1 ? argv[1] : "";
    if (*script == 'S') { net_launch_snapshot(); script++; }
    net_settings_load(&s);
    NetMenu m;
    net_menu_init(&m, &s);
    net_launch_describe(m.launched, sizeof m.launched);
    if (getenv("TEST_MODS_WARN")) snprintf(m.mods_warn, sizeof m.mods_warn, "%s", getenv("TEST_MODS_WARN"));   /* (the mods that can desync: gpu_menu.c's) */
    int plays = 0, refreshes = 0;
    NetChoice many[6];
    for (int i = 0; i < 6; i++) {
        memset(&many[i], 0, sizeof many[i]);
        snprintf(many[i].addr, sizeof many[i].addr, "10.0.0.%d:39420", i + 1);
        snprintf(many[i].room, sizeof many[i].room, "room%d", i);
        many[i].found = 1;
    }
    NetChoice room = {"192.168.1.9:39420", "dojo", "Kim", 1, 2, 8};
    NetChoice rooms[3] = {{"192.168.1.5:39420", "mine", "Me", 1}, {"192.168.1.7:39420", "full", "Ana", 1, 8, 8}, room};
    NetProbe self = {0};
    strcpy(self.lan_ip, "192.168.1.5");
    net_menu_set_self(&m, &self);
    for (const char *p = script; *p; p++) {
        switch (*p) {
        case 'u': net_menu_event(&m, MENU_UP, 0); break;
        case 'd': net_menu_event(&m, MENU_DOWN, 0); break;
        case 'l': net_menu_event(&m, MENU_LEFT, 0); break;
        case 'r': net_menu_event(&m, MENU_RIGHT, 0); break;
        case 'a': { int r = net_menu_event(&m, MENU_ACCEPT, 0); plays += r == NET_EV_PLAY; refreshes += r == NET_EV_REFRESH; break; }
        case 'e': net_menu_event(&m, MENU_ERASE, 0); break;
        case 'k': if (p[1]) net_menu_event(&m, MENU_CHAR, *++p); break;
        case 'f': net_menu_set_found(&m, &room, 1); break;
        case 'g': net_menu_set_found(&m, rooms, 3); break;
        case 'm': net_menu_set_found(&m, many, 6); break;
        case 'N': m.no_play = 1; break;   /* (as the start-up menu has it: no Play row) */
        case 'j': net_menu_auto_host(&m, p[1] ? *++p - '0' : 0); break;
        case 'p': {
            NetProbe probe = {0};
            probe.upnp = p[1] ? *++p - '0' : 0;
            strcpy(probe.lan_ip, "192.168.1.5");
            if (getenv("TEST_VPN")) strcpy(probe.vpn_ip, getenv("TEST_VPN"));
            strcpy(probe.wan, getenv("TEST_WAN") ? getenv("TEST_WAN") : "203.0.113.7");
            net_menu_set_self(&m, &probe);
            net_menu_describe(&m, &probe);
            break;
        }
        case 'W': {   /* (as gpu_menu.c saves: an unchanged mode leaves netplay.ini's) */
            net_settings_remember(&m.s);
            NetSettings keep = m.s;
            if (keep.mode == m.initial.mode) keep.mode = keep.file_mode;
            net_settings_save(&keep);
            break;
        }
        case 'A': net_settings_apply(&m.s); break;
        case 'I': printf("id=%016llx|", net_host_id()); break;
        }
    }
    char v[NROWS][200];
    for (int r = 0; r < NROWS; r++) {
        net_menu_value(&m, r, v[r], sizeof v[r]);
        if (v[r][0] && v[r][0] < 32) memmove(v[r], v[r] + 1, strlen(v[r]));
    }
    char room0[200] = "";   /* (the first room shown on the list's page, its signal left out) */
    if (net_menu_visible(&m, NROW_LIST)) snprintf(room0, sizeof room0, "%s", v[NROW_LIST] + 3);
    for (char *c = room0; *c; c++) if (*c == '|') *c = '/';   /* (the parts of its line: '/' here, '|' being the output's) */
    printf("page=%d|pages=%d|refreshes=%d|room0=%s|", m.page + 1, net_menu_pages(&m), refreshes, room0);
    printf("mods=%s|", net_menu_visible(&m, NROW_MODS) ? v[NROW_MODS] + (v[NROW_MODS][0] == 3) : "(hidden)");
    printf("kind=%d|auto=%d|choices=%d|plays=%d|row=%s|mode=%s|name=%s|room=%s|port=%s|host=%s|addrs=%s|router=%s|lan=%s|tip=%s|launched=%s|text=%d|recent=%d|"
           "HOST=%s|RELAY=%s|ROOM=%s|PORT=%s|NAME=%s|LAN=%s|MENU_MODE=%s|"
           "share_mods=%d|room_mods=%d|room_mods_mode=%d|room_mods_rate=%ld|host_id=%s\n",
           net_menu_visible(&m, NROW_TIP) ? m.tip_kind : 0, m.auto_state, m.nchoices, plays, net_menu_label(m.row), v[NROW_MODE], m.s.name, m.s.room, m.s.port, m.s.host, v[NROW_ADDRS], v[NROW_ROUTER], v[NROW_LAN],
           v[NROW_TIP], m.launched, net_menu_text_row(&m), m.s.nrecent, env("SSBU_NET_HOST"), env("SSBU_NET_RELAY"), env("SSBU_NET_ROOM"),
           env("SSBU_NET_PORT"), env("SSBU_NET_NAME"), env("SSBU_NET_LAN"), env("SSBU_NET_MENU_MODE"),
           m.s.share_mods, m.s.room_mods, m.s.room_mods_mode, m.s.room_mods_rate, m.s.host_id);
    return 0;
}
