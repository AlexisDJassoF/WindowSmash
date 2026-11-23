/* The start-up menu's Network page: its model, netplay.ini, and the SSBU_NET_* variables it sets
 * (see net_menu.h). No window and no sockets here: gpu_menu.c draws it and feeds it input, and
 * net_discover.c finds what it shows of this PC's network. */
#define _CRT_SECURE_NO_WARNINGS
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_menu.h"
#include "net_menu.h"

#define DEFAULT_PORT "39420"   /* (net.h's NET_DEFAULT_PORT) */
#define DEFAULT_ROOM "smash"

static void copy(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); }
static int mode_of(const char *v) { return !strcmp(v, "host") ? NET_MODE_HOST : !strcmp(v, "join") ? NET_MODE_JOIN : NET_MODE_LAUNCHER; }
static const char *mode_name(int m) { return m == NET_MODE_HOST ? "host" : m == NET_MODE_JOIN ? "join" : "launcher"; }


static void env_set(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

/* ---- the model ------------------------------------------------------------------------------ */
void net_menu_init(NetMenu *m, const NetSettings *s) {
    memset(m, 0, sizeof *m);
    m->s = m->initial = *s;
    m->row = NROW_PLAY;
    m->host_auto = 1;
    m->tip_kind = '\5';
    net_menu_set_found(m, NULL, 0);
}

int net_menu_visible(const NetMenu *m, int row) {
    int mode = m->s.mode;
    switch (row) {
    case NROW_ROOM: return mode != NET_MODE_LAUNCHER;
    case NROW_PORT: case NROW_ADDRS: case NROW_ROUTER: return mode == NET_MODE_HOST;
    case NROW_HOST: case NROW_LAN: return mode == NET_MODE_JOIN;
    case NROW_TIP: return m->tip[0] != 0;
    default: return row >= 0 && row < NROWS;
    }
}

int net_menu_selectable(const NetMenu *m, int row) {
    return net_menu_visible(m, row) && (row == NROW_MODE || row == NROW_NAME || row == NROW_ROOM || row == NROW_PORT || row == NROW_HOST || row == NROW_PLAY);
}

int net_menu_text_row(const NetMenu *m) {
    int r = m->row;
    return net_menu_visible(m, r) && (r == NROW_NAME || r == NROW_ROOM || r == NROW_PORT || r == NROW_HOST);
}

const char *net_menu_label(int row) {
    static const char *const L[NROWS] = {"Connection", "Your name", "Room name", "UDP port", "Host address", "This PC", "Router", "On your LAN", "Tip", "Play"};
    return row >= 0 && row < NROWS ? L[row] : "";
}

void net_menu_value(const NetMenu *m, int row, char *out, unsigned n) {
    const char *cursor = row == m->row ? "_" : "";
    switch (row) {
    case NROW_MODE:
        snprintf(out, n, "%s", m->s.mode == NET_MODE_HOST ? "Create a room (host)" : m->s.mode == NET_MODE_JOIN ? "Join a room" : "As launched");
        return;
    case NROW_NAME: snprintf(out, n, "\2%s%s", m->s.name, cursor); return;
    case NROW_ROOM: snprintf(out, n, "\2%s%s", m->s.room, cursor); return;
    case NROW_PORT: snprintf(out, n, "%s%s", m->s.port, cursor); return;   /* (< >: a step) */
    case NROW_HOST:
        if (!m->nchoices) snprintf(out, n, "\2%s%s", m->s.host[0] || row == m->row ? m->s.host : "(type it)", cursor);
        else snprintf(out, n, "%s%s", m->s.host[0] || row == m->row ? m->s.host : "(< > or type)", cursor);
        return;
    case NROW_ADDRS: snprintf(out, n, "\1%s", m->addrs); return;
    case NROW_ROUTER: snprintf(out, n, "\1%s", m->router); return;
    case NROW_LAN: snprintf(out, n, "\1%s", m->lan); return;
    case NROW_TIP: snprintf(out, n, "%c%s", m->tip_kind ? m->tip_kind : '\5', m->tip); return;
    default: if (n) out[0] = 0; return;
    }
}

static void move(NetMenu *m, int dir) {
    for (int i = 0; i < NROWS; i++) {
        m->row = (m->row + dir + NROWS) % NROWS;
        if (net_menu_selectable(m, m->row)) return;
    }
}

static void pick(NetMenu *m, int i) {   /* the Host row's choice i */
    m->choice = i;
    copy(m->s.host, sizeof m->s.host, m->choices[i].addr);
    if (m->choices[i].room[0]) copy(m->s.room, sizeof m->s.room, m->choices[i].room);
}

static void step(NetMenu *m, int dir) {
    switch (m->row) {
    case NROW_MODE:
        m->s.mode = ((m->s.mode + dir) % NET_MODES + NET_MODES) % NET_MODES;
        break;
    case NROW_PORT: {
        long p = strtol(m->s.port, NULL, 10) + dir;
        if (p < 1 || p > 65535) p = strtol(DEFAULT_PORT, NULL, 10);
        snprintf(m->s.port, sizeof m->s.port, "%ld", p);
        break;
    }
    case NROW_HOST:
        if (!m->nchoices) break;
        m->host_auto = 0;   /* (picked: Join chooses no more) */
        if (m->choice < 0) pick(m, dir > 0 ? 0 : m->nchoices - 1);
        else pick(m, ((m->choice + dir) % m->nchoices + m->nchoices) % m->nchoices);
        break;
    }
}

static int allowed(int row, int c) {
    if (c < 32 || c > 126) return 0;
    if (row == NROW_PORT) return isdigit(c);
    if (row == NROW_HOST) return isalnum(c) || c == '.' || c == ':' || c == '-' || c == '_' || c == '[' || c == ']';
    return 1;
}

static void type(NetMenu *m, int c) {
    char *t = m->row == NROW_NAME ? m->s.name : m->row == NROW_ROOM ? m->s.room : m->row == NROW_PORT ? m->s.port : m->s.host;
    size_t cap = m->row == NROW_PORT ? sizeof m->s.port : m->row == NROW_HOST ? sizeof m->s.host : NET_TEXT;
    size_t l = strlen(t);
    if (c < 0) { if (l) t[l - 1] = 0; }
    else if (allowed(m->row, c) && l + 1 < cap && (m->row != NROW_PORT || strtol(t, NULL, 10) * 10 + (c - '0') <= 65535)) {
        t[l] = (char)c;
        t[l + 1] = 0;
    } else return;
    if (m->row == NROW_HOST) { m->choice = -1; m->host_auto = 0; }   /* (typed: Join chooses no more) */
}

int net_menu_event(NetMenu *m, int event, int c) {
    switch (event) {
    case MENU_UP: move(m, -1); break;
    case MENU_DOWN: move(m, 1); break;
    case MENU_LEFT: step(m, -1); break;
    case MENU_RIGHT: step(m, 1); break;
    case MENU_CHAR: if (net_menu_text_row(m)) type(m, c); break;
    case MENU_ERASE: if (net_menu_text_row(m)) type(m, -1); break;
    case MENU_ACCEPT:
        if (m->row != NROW_PLAY) { move(m, 1); break; }
        if (m->s.mode == NET_MODE_JOIN && !m->s.host[0]) { m->row = NROW_HOST; break; }   /* (nothing to join yet) */
        if (!m->s.room[0]) copy(m->s.room, sizeof m->s.room, DEFAULT_ROOM);
        if (!m->s.port[0]) copy(m->s.port, sizeof m->s.port, DEFAULT_PORT);
        return NET_EV_PLAY;
    }
    if (!net_menu_selectable(m, m->row)) move(m, 1);   /* (the mode changed under it) */
    return NET_EV_NONE;
}

/* An address's host and port, for comparing: "10.0.0.2" is "10.0.0.2:39420" */
static void address_key(const char *a, char *host, size_t n, long *port) {
    const char *colon = strrchr(a, ':');
    int one = colon && strchr(a, ':') == colon;   /* (one ':': host:port - more is IPv6's own) */
    size_t l = one ? (size_t)(colon - a) : strlen(a);
    if (l >= n) l = n - 1;
    for (size_t i = 0; i < l; i++) host[i] = (char)tolower((unsigned char)a[i]);
    host[l] = 0;
    *port = one ? strtol(colon + 1, NULL, 10) : 0;
    if (*port <= 0) *port = strtol(DEFAULT_PORT, NULL, 10);
}

static int same_address(const char *a, const char *b) {
    char ha[NET_ADDR], hb[NET_ADDR];
    long pa, pb;
    address_key(a, ha, sizeof ha, &pa);
    address_key(b, hb, sizeof hb, &pb);
    return pa == pb && !strcmp(ha, hb);
}

int net_menu_is_self(const NetMenu *m, const char *addr) {
    char h[NET_ADDR];
    long port;
    if (!addr || !*addr) return 0;
    address_key(addr, h, sizeof h, &port);
    if (!strcmp(h, "localhost") || !strncmp(h, "127.", 4) || !strcmp(h, "0.0.0.0") || !strcmp(h, "::1") || !strcmp(h, "[::1]")) return 1;
    for (int i = 0; i < m->nself; i++)
        if (m->self[i][0] && !strcmp(h, m->self[i])) return 1;
    return 0;
}

void net_menu_set_self(NetMenu *m, const NetProbe *p) {
    m->nself = 0;
    for (int i = 0; i < p->nself && m->nself < NET_SELF; i++) copy(m->self[m->nself++], NET_ADDR, p->self[i]);
    if (p->lan_ip[0] && m->nself < NET_SELF) copy(m->self[m->nself++], NET_ADDR, p->lan_ip);
    if (p->vpn_ip[0] && m->nself < NET_SELF) copy(m->self[m->nself++], NET_ADDR, p->vpn_ip);
}

void net_menu_set_found(NetMenu *m, const NetChoice *found, int n) {
    m->nchoices = 0;
    for (int i = 0; i < n && m->nchoices < NET_CHOICES; i++) {
        if (net_menu_is_self(m, found[i].addr)) continue;   /* (this PC's own room: not one to join) */
        m->choices[m->nchoices] = found[i];
        m->choices[m->nchoices++].found = 1;
    }
    for (int i = 0; i < m->s.nrecent && m->nchoices < NET_CHOICES; i++) {
        int dup = net_menu_is_self(m, m->s.recent[i].addr);
        for (int j = 0; j < m->nchoices && !dup; j++) dup = same_address(m->choices[j].addr, m->s.recent[i].addr);
        if (dup) continue;
        m->choices[m->nchoices] = m->s.recent[i];
        m->choices[m->nchoices].players = m->choices[m->nchoices].most = 0;
        m->choices[m->nchoices++].found = 0;
    }
    m->choice = -1;
    for (int j = 0; j < m->nchoices; j++)
        if (m->s.host[0] && same_address(m->choices[j].addr, m->s.host)) { m->choice = j; break; }
}

const char *net_menu_last(const NetMenu *m) {
    if (m->initial.host[0] && !net_menu_is_self(m, m->initial.host)) return m->initial.host;
    for (int i = 0; i < m->s.nrecent; i++)
        if (m->s.recent[i].addr[0] && !net_menu_is_self(m, m->s.recent[i].addr)) return m->s.recent[i].addr;
    return NULL;
}

static int joinable(const NetChoice *c) { return c->found && (c->most <= 0 || c->players < c->most); }

static int found_at(const NetMenu *m, const char *addr) {   /* the choice answering at addr now, -1 if none */
    for (int i = 0; i < m->nchoices; i++)
        if (m->choices[i].found && same_address(m->choices[i].addr, addr)) return i;
    return -1;
}

static void auto_set(NetMenu *m, const char *addr) {   /* the address, Join's choice (and the room found there) */
    int i = found_at(m, addr);
    char keep[NET_ADDR];
    copy(keep, sizeof keep, addr);
    copy(m->s.host, sizeof m->s.host, keep);
    m->choice = -1;
    for (int j = 0; j < m->nchoices && m->choice < 0; j++)
        if (same_address(m->choices[j].addr, keep)) m->choice = j;
    if (i >= 0 && m->choices[i].room[0]) copy(m->s.room, sizeof m->s.room, m->choices[i].room);
}

void net_menu_auto_host(NetMenu *m, int watch) {
    m->watch = watch;
    if (m->s.mode != NET_MODE_JOIN || !m->host_auto) { m->auto_state = NET_AUTO_NONE; return; }
    const char *last = net_menu_last(m);
    int first = -1;
    for (int i = 0; i < m->nchoices && first < 0; i++)
        if (joinable(&m->choices[i])) first = i;
    int on_last = last && same_address(m->s.host, last);
    int up = m->s.host[0] && (found_at(m, m->s.host) >= 0 || watch == NET_WATCH_UP);
    if (last && on_last && up) { m->last_down = 0; auto_set(m, last); m->auto_state = NET_AUTO_LAST; return; }   /* (back, or there) */
    if (last && !m->last_down) {
        if (!on_last) { auto_set(m, last); m->auto_state = found_at(m, last) >= 0 ? NET_AUTO_LAST : NET_AUTO_CHECKING; return; }
        if (watch != NET_WATCH_DOWN) { m->auto_state = NET_AUTO_CHECKING; return; }
        m->last_down = 1;   /* (no room at the last address: the first one found instead) */
    }
    if (first >= 0) {
        int keep = m->choice >= 0 && !on_last && joinable(&m->choices[m->choice]);   /* (the one chosen before, still open) */
        auto_set(m, m->choices[keep ? m->choice : first].addr);
        m->auto_state = NET_AUTO_FIRST;
    } else if (last) {
        if (!on_last) auto_set(m, last);
        m->auto_state = NET_AUTO_LAST_DOWN;
    } else {
        m->s.host[0] = 0;
        m->choice = -1;
        m->auto_state = NET_AUTO_NONE;
    }
}

void net_menu_describe(NetMenu *m, const NetProbe *p) {
    const char *port = m->s.port[0] ? m->s.port : DEFAULT_PORT;
    m->addrs[0] = m->router[0] = m->lan[0] = m->tip[0] = 0;
    m->tip_kind = '\5';
    switch (m->s.mode) {
    case NET_MODE_LAUNCHER:
        snprintf(m->tip, sizeof m->tip, "%s. Create or join a room: < > on Connection", m->launched[0] ? m->launched : "The launcher decides");
        break;
    case NET_MODE_HOST:
        if (!p->lan_ip[0]) snprintf(m->addrs, sizeof m->addrs, "no network address found");
        else if (p->vpn_ip[0]) snprintf(m->addrs, sizeof m->addrs, "%s:%s   VPN %s:%s", p->lan_ip, port, p->vpn_ip, port);
        else snprintf(m->addrs, sizeof m->addrs, "%s:%s", p->lan_ip, port);
        switch (p->upnp) {
        case NET_UPNP_OPEN:
            snprintf(m->router, sizeof m->router, "UPnP works - internet: %s:%s", p->wan[0] ? p->wan : "?", port);
            snprintf(m->tip, sizeof m->tip, "Give %s:%s to friends at home, %s:%s to the rest", p->lan_ip, port, p->wan[0] ? p->wan : "your public IP", port);
            break;
        case NET_UPNP_BEHIND_NAT:
            snprintf(m->router, sizeof m->router, "behind another NAT / CGNAT (%s)", p->wan);
            if (p->vpn_ip[0]) snprintf(m->tip, sizeof m->tip, "Outside your home: friends on your VPN join %s:%s", p->vpn_ip, port);
            else snprintf(m->tip, sizeof m->tip, "Outside your home: a VPN (Tailscale, ZeroTier) or the online lobby");
            break;
        case NET_UPNP_NONE: case NET_UPNP_REFUSED:
            snprintf(m->router, sizeof m->router, "%s: forward UDP %s by hand", p->upnp == NET_UPNP_NONE ? "no UPnP" : "UPnP refused", port);
            if (p->vpn_ip[0]) snprintf(m->tip, sizeof m->tip, "At home: %s:%s - friends on your VPN: %s:%s", p->lan_ip, port, p->vpn_ip, port);
            else snprintf(m->tip, sizeof m->tip, "At home: %s:%s - elsewhere: forward the port or use a VPN", p->lan_ip, port);
            break;
        default:
            snprintf(m->router, sizeof m->router, "checking the router (UPnP)...");
            if (p->lan_ip[0]) snprintf(m->tip, sizeof m->tip, "Friends at home join %s:%s with the room name", p->lan_ip, port);
            break;
        }
        break;
    case NET_MODE_JOIN: {
        int found = 0;
        for (int i = 0; i < m->nchoices; i++) found += m->choices[i].found;
        if (found) snprintf(m->lan, sizeof m->lan, "%d room%s found - < > on Host address", found, found == 1 ? "" : "s");
        else snprintf(m->lan, sizeof m->lan, "%s", p->searching ? "searching..." : "no rooms found");
        const NetChoice *c = m->choice >= 0 ? &m->choices[m->choice] : NULL;
        const char *last = net_menu_last(m);
        char room[96] = "";
        if (c && c->found) {
            if (c->most > 0) snprintf(room, sizeof room, "%s's room \"%s\" (%d/%d players)", c->who[0] ? c->who : "A", c->room, c->players, c->most);
            else snprintf(room, sizeof room, "%s's room \"%s\"", c->who[0] ? c->who : "A", c->room);
        }
        if (m->s.host[0] && net_menu_is_self(m, m->s.host)) {
            m->tip_kind = '\3';
            snprintf(m->tip, sizeof m->tip, "%s is this PC: join another PC's room", m->s.host);
        } else if (m->auto_state == NET_AUTO_CHECKING) {
            snprintf(m->tip, sizeof m->tip, "Looking for the last room, at %s...", m->s.host);
        } else if (m->auto_state == NET_AUTO_LAST) {
            m->tip_kind = '\4';
            if (room[0]) snprintf(m->tip, sizeof m->tip, "Last room: %s is open", room);
            else snprintf(m->tip, sizeof m->tip, "Last room: open at %s", m->s.host);
        } else if (m->auto_state == NET_AUTO_FIRST && m->last_down && last) {
            m->tip_kind = '\3';
            snprintf(m->tip, sizeof m->tip, "No room exists at %s - %s instead", last, room[0] ? room : m->s.host);
        } else if (m->auto_state == NET_AUTO_FIRST) {
            m->tip_kind = '\4';
            snprintf(m->tip, sizeof m->tip, "%s on your network", room[0] ? room : m->s.host);
        } else if (m->auto_state == NET_AUTO_LAST_DOWN && last) {
            m->tip_kind = '\3';
            snprintf(m->tip, sizeof m->tip, "No room exists at %s - is its host playing? Searching...", last);
        } else if (c && c->found && c->most > 0 && c->players >= c->most) {
            m->tip_kind = '\3';
            snprintf(m->tip, sizeof m->tip, "That room is full (%d/%d players)", c->players, c->most);
        } else if (c && c->found)
            snprintf(m->tip, sizeof m->tip, "%s's room \"%s\" on your network", c->who[0] ? c->who : "A", c->room);
        else if (!m->s.host[0]) snprintf(m->tip, sizeof m->tip, found ? "Pick a room found on your LAN, or type an address" : "Type the host's address (their Network page shows it)");
        else if (m->watch == NET_WATCH_DOWN) {
            m->tip_kind = '\3';
            snprintf(m->tip, sizeof m->tip, "No room exists at %s", m->s.host);
        } else if (m->watch == NET_WATCH_UP) {
            m->tip_kind = '\4';
            snprintf(m->tip, sizeof m->tip, "A room answers at %s - in the game: Local Wireless", m->s.host);
        } else if (m->watch == NET_WATCH_CHECKING) snprintf(m->tip, sizeof m->tip, "Looking for a room at %s...", m->s.host);
        else snprintf(m->tip, sizeof m->tip, "In the game: Local Wireless, then the host's room");
        break;
    }
    }
}

/* ---- the launcher's variables --------------------------------------------------------------- */
static const char *const VARS[] = {"SSBU_NET_HOST", "SSBU_NET_RELAY", "SSBU_NET_ROOM", "SSBU_NET_PORT", "SSBU_NET_LAN", "SSBU_NET_NAME"};
#define NVARS (int)(sizeof VARS / sizeof VARS[0])

void net_launch_snapshot(void) {
    const char *done = getenv("SSBU_NET_LAUNCH_SAVED");
    if (done && *done) return;   /* (kept already: this is the program started again) */
    for (int i = 0; i < NVARS; i++) {
        char key[64], val[512];
        const char *v = getenv(VARS[i]);
        snprintf(key, sizeof key, "SSBU_NET_LAUNCH_%s", VARS[i] + 9);
        snprintf(val, sizeof val, "%s%s", v ? "=" : "", v ? v : "");   /* ("=value": set, "": not set) */
        env_set(key, val);
    }
    env_set("SSBU_NET_LAUNCH_SAVED", "1");
}

static const char *launched(const char *var) {   /* the launcher's value of SSBU_NET_<var>, NULL when it set none */
    char key[64];
    snprintf(key, sizeof key, "SSBU_NET_LAUNCH_%s", var);
    const char *v = getenv(key);
    if (!getenv("SSBU_NET_LAUNCH_SAVED")) {   /* (no snapshot: the environment as it is) */
        snprintf(key, sizeof key, "SSBU_NET_%s", var);
        return getenv(key);
    }
    return v && v[0] == '=' ? v + 1 : NULL;
}

static int on(const char *v) { return v && *v && strcmp(v, "0"); }

void net_launch_describe(char *out, unsigned n) {
    const char *relay = launched("RELAY"), *room = launched("ROOM");
    if (on(launched("LAN"))) snprintf(out, n, "LAN mode (with a Switch)");
    else if (on(launched("HOST"))) snprintf(out, n, "Hosting room %s", room && *room ? room : DEFAULT_ROOM);
    else if (relay && *relay && room && *room && strcmp(room, DEFAULT_ROOM)) snprintf(out, n, "Online: %s, code %s", relay, room);
    else if (relay && *relay) snprintf(out, n, "Online: %s", relay);
    else snprintf(out, n, "Offline");
}

void net_settings_apply(const NetSettings *s) {
    if (s->mode == NET_MODE_LAUNCHER) {   /* the launcher's variables back, as it set them */
        for (int i = 0; i < NVARS; i++) env_set(VARS[i], launched(VARS[i] + 9));
    } else {
        env_set("SSBU_NET_LAN", NULL);
        env_set("SSBU_NET_ROOM", s->room[0] ? s->room : DEFAULT_ROOM);
        if (s->mode == NET_MODE_HOST) {
            env_set("SSBU_NET_HOST", "1");
            env_set("SSBU_NET_RELAY", NULL);
            env_set("SSBU_NET_PORT", s->port[0] ? s->port : DEFAULT_PORT);
        } else {
            env_set("SSBU_NET_HOST", NULL);
            env_set("SSBU_NET_RELAY", s->host);
        }
    }
    if (s->name[0]) env_set("SSBU_NET_NAME", s->name);
    env_set("SSBU_NET_MENU_MODE", mode_name(s->mode));   /* (F8: the page opens on the mode in use) */
}

/* ---- netplay.ini ---------------------------------------------------------------------------- */
static const char *ini_path(void) {
    const char *p = getenv("SSBU_NETPLAY_INI");
    return p && *p ? p : "netplay.ini";
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r' || s[l - 1] == '\n')) s[--l] = 0;
    return s;
}

static void printable(char *s) {   /* (the overlay's font: printable ASCII) */
    for (; *s; s++) if ((unsigned char)*s < 32 || (unsigned char)*s > 126) *s = '?';
}

void net_settings_load(NetSettings *s) {
    memset(s, 0, sizeof *s);
    s->mode = NET_MODE_LAUNCHER;
    const char *v = launched("NAME");
    if (v && *v) copy(s->name, sizeof s->name, v);
    else {
        FILE *f = fopen("online_name.txt", "r");   /* (run.sh's: the online name) */
        char line[128];
        if (f && fgets(line, sizeof line, f)) copy(s->name, sizeof s->name, trim(line));
        if (f) fclose(f);
    }
    if (!s->name[0] && (v = getenv("USER")) && *v) copy(s->name, sizeof s->name, v);
    if (!s->name[0] && (v = getenv("USERNAME")) && *v) copy(s->name, sizeof s->name, v);
    copy(s->room, sizeof s->room, DEFAULT_ROOM);
    v = launched("PORT");
    copy(s->port, sizeof s->port, v && atoi(v) > 0 && atoi(v) < 65536 ? v : DEFAULT_PORT);
    FILE *f = fopen(ini_path(), "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char *l = trim(line), *eq = strchr(l, '=');
            if (!*l || *l == ';' || *l == '#' || !eq) continue;
            *eq = 0;
            char *key = trim(l), *val = trim(eq + 1);
            printable(val);
            if (!strcmp(key, "mode")) s->mode = mode_of(val);
            else if (!strcmp(key, "name") && *val) copy(s->name, sizeof s->name, val);
            else if (!strcmp(key, "room") && *val) copy(s->room, sizeof s->room, val);
            else if (!strcmp(key, "port") && atoi(val) > 0 && atoi(val) < 65536) snprintf(s->port, sizeof s->port, "%d", atoi(val));
            else if (!strcmp(key, "host")) copy(s->host, sizeof s->host, val);
            else if (!strcmp(key, "recent") && *val && s->nrecent < NET_RECENT) {
                NetChoice *c = &s->recent[s->nrecent++];
                memset(c, 0, sizeof *c);
                char *sp = strchr(val, ' ');
                if (sp) { *sp = 0; copy(c->room, sizeof c->room, trim(sp + 1)); }
                copy(c->addr, sizeof c->addr, val);
            }
        }
        fclose(f);
    }
    printable(s->name);
    s->file_mode = s->mode;
    if ((v = getenv("SSBU_NET_MENU_MODE")) && *v) s->mode = mode_of(v);
    else if ((v = getenv("SSBU_NET_MODE")) && !strcmp(v, "launcher")) s->mode = NET_MODE_LAUNCHER;
}

int net_settings_save(const NetSettings *s) {
    FILE *f = fopen(ini_path(), "w");
    if (!f) return 0;
    fprintf(f, "; netplay.ini: the start-up menu's Network page (Tab in the menu; F8 twice in the game). Read at every start.\n"
               "; Delete this file to start over. A line starting with ';' is a comment.\n\n"
               "; mode = launcher | host | join\n"
               ";   launcher: as the launcher you started chose (run.sh: the online lobby, run-offline.sh: offline...).\n"
               ";   host:     create a private room here (UDP port below, opened on the router with UPnP when it can).\n"
               ";   join:     join the room at host = address:port.\n"
               "mode = %s\n"
               "; name: the name other players see. room: the private room's name - the same for everyone in it.\n"
               "name = %s\nroom = %s\n"
               "; port: the UDP port a hosted room listens on.\nport = %s\n"
               "; host: the address to join (an IP or a name, then :port).\nhost = %s\n"
               "; recent = address room: the last rooms joined, newest first (the Network page offers them).\n",
            mode_name(s->mode), s->name, s->room, s->port, s->host);
    for (int i = 0; i < s->nrecent; i++) fprintf(f, "recent = %s %s\n", s->recent[i].addr, s->recent[i].room);
    return fclose(f) == 0;
}

void net_settings_remember(NetSettings *s) {
    if (s->mode != NET_MODE_JOIN || !s->host[0]) return;
    NetChoice c;
    memset(&c, 0, sizeof c);
    copy(c.addr, sizeof c.addr, s->host);
    copy(c.room, sizeof c.room, s->room);
    int n = 0;
    NetChoice keep[NET_RECENT];
    keep[n++] = c;
    for (int i = 0; i < s->nrecent && n < NET_RECENT; i++)
        if (strcmp(s->recent[i].addr, c.addr)) keep[n++] = s->recent[i];
    memcpy(s->recent, keep, sizeof keep[0] * (size_t)n);
    s->nrecent = n;
}
