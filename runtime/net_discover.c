/* What the start-up menu's Network page shows of this PC's network (see net_discover.h). Linux:
 * Windows has no start-up menu. */
#ifdef _WIN32
typedef int net_discover_unused;   /* (nothing here on Windows) */
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "host.h"
#include "net.h"
#include "net_discover.h"

int rt_upnp_check(u16 port, const char *lan_ip, char *public_ip, size_t n);   /* (linux/upnp.c) */

#define FOUND_MAX 16
#define FOUND_FOR_MS 4000   /* a room unheard of this long is gone */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static NetChoice found[FOUND_MAX];
static u64 found_at[FOUND_MAX];
static int nfound;
static volatile int running, searching;
static pthread_t searcher;
static int upnp = NET_UPNP_UNKNOWN, upnp_busy;
static char upnp_port[6], wan[NET_ADDR];
static u32 self_ips[NET_SELF];   /* this PC's IPv4 addresses (host order): its own rooms are not offered */
static int nself;
/* The address asked directly (the room Join chose: the last one joined, maybe outside the LAN) */
static char watch_text[NET_ADDR];
static u32 watch_ip;              /* (host order; 0 until looked up) */
static u16 watch_port;
static int watch_failed, watch_asks;
static u64 watch_heard;

static u64 now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000 + (u64)t.tv_nsec / 1000000;
}

/* ---- this PC's addresses -------------------------------------------------------------------- */
static int is_vpn(const char *ifname, u32 ip) {   /* Tailscale, ZeroTier, WireGuard, OpenVPN, Hamachi... */
    static const char *const names[] = {"tailscale", "zt", "wg", "tun", "tap", "ham", "nordlynx", "proton"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strncmp(ifname, names[i], strlen(names[i]))) return 1;
    return (ip >> 22) == (100u << 2 | 1);   /* 100.64.0.0/10: Tailscale's addresses (and carrier-grade NAT's) */
}

static int is_virtual(const char *ifname) {   /* containers' and virtual machines' bridges: not a network friends are on */
    static const char *const names[] = {"docker", "br-", "veth", "virbr", "vmnet", "vboxnet", "lxc", "lxd", "podman", "cni"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strncmp(ifname, names[i], strlen(names[i]))) return 1;
    return 0;
}

static u32 outbound_address(void) {   /* the address this PC goes out on, host order (no packet is sent) */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    socklen_t len = sizeof a;
    u32 ip = 0;
    if (s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &len)) ip = ntohl(a.sin_addr.s_addr);
    if (s >= 0) close(s);
    return ip;
}

static void ip_text(u32 ip, char *out, size_t n) { snprintf(out, n, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255); }

static int local_ips(u32 *out, int max) {   /* every IPv4 address of this PC's interfaces, host order */
    int n = 0;
    struct ifaddrs *all = NULL;
    if (getifaddrs(&all)) return 0;
    for (struct ifaddrs *i = all; i && n < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        u32 ip = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        if ((ip >> 24) == 127) continue;   /* (the loopback: always this PC, checked apart) */
        int dup = 0;
        for (int j = 0; j < n && !dup; j++) dup = out[j] == ip;
        if (!dup) out[n++] = ip;
    }
    freeifaddrs(all);
    return n;
}

static int is_self(u32 ip) {   /* (lock held) */
    if ((ip >> 24) == 127 || !ip) return 1;
    for (int i = 0; i < nself; i++) if (self_ips[i] == ip) return 1;
    return 0;
}

/* The address friends on this network join (the one the PC goes out on, unless that is a VPN's),
 * and a VPN's when there is one. */
static void addresses(char *lan, char *vpn, size_t n) {
    lan[0] = vpn[0] = 0;
    u32 out = outbound_address(), first_lan = 0;
    int out_is_lan = 0;
    struct ifaddrs *all = NULL;
    if (getifaddrs(&all)) all = NULL;
    for (struct ifaddrs *i = all; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        u32 ip = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        if (is_vpn(i->ifa_name, ip)) { if (!vpn[0]) ip_text(ip, vpn, n); continue; }
        if (is_virtual(i->ifa_name)) continue;
        if (ip == out) out_is_lan = 1;
        if (!first_lan) first_lan = ip;
    }
    if (all) freeifaddrs(all);
    if (out_is_lan) ip_text(out, lan, n);
    else if (first_lan) ip_text(first_lan, lan, n);
}

static int unreachable_address(const char *text) {   /* (as net_relay.c's: private, or carrier-grade NAT's) */
    unsigned a, b, c, d;
    if (sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    return a == 10 || (a == 172 && (b & 0xF0) == 16) || (a == 192 && b == 168) || (a == 100 && (b & 0xC0) == 64) || a == 0;
}

/* ---- the router ----------------------------------------------------------------------------- */
static void *check_router(void *arg) {
    u16 port = (u16)(uintptr_t)arg;
    char lan[NET_ADDR], vpn[NET_ADDR], outside[NET_ADDR] = "";
    addresses(lan, vpn, sizeof lan);
    int r = lan[0] ? rt_upnp_check(port, lan, outside, sizeof outside) : 2;
    int state = r == 1 ? (unreachable_address(outside) ? NET_UPNP_BEHIND_NAT : NET_UPNP_OPEN) : r == 3 ? NET_UPNP_REFUSED : NET_UPNP_NONE;
    pthread_mutex_lock(&lock);
    upnp = state;
    snprintf(wan, sizeof wan, "%s", outside);
    upnp_busy = 0;
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "[net] menu: UPnP check on port %u: %s%s%s\n", port,
            state == NET_UPNP_OPEN ? "works" : state == NET_UPNP_BEHIND_NAT ? "works, behind another NAT" : state == NET_UPNP_REFUSED ? "refused" : "no UPnP router",
            outside[0] ? ", outside address " : "", outside);
    return NULL;
}

void net_discover_check_router(const char *port) {
    int p = atoi(port);
    if (p <= 0 || p > 65535) return;
    pthread_mutex_lock(&lock);
    int start = !upnp_busy && strcmp(upnp_port, port);
    if (start) {
        snprintf(upnp_port, sizeof upnp_port, "%s", port);
        upnp = NET_UPNP_UNKNOWN;
        wan[0] = 0;
        upnp_busy = 1;
    }
    pthread_mutex_unlock(&lock);
    if (!start) return;
    pthread_t t;
    if (pthread_create(&t, NULL, check_router, (void *)(uintptr_t)p) == 0) pthread_detach(t);
    else { pthread_mutex_lock(&lock); upnp_busy = 0; upnp = NET_UPNP_NONE; pthread_mutex_unlock(&lock); }
}

/* ---- rooms on the LAN ----------------------------------------------------------------------- */
/* The watched address looked up (outside the lock: a name may take a while), then asked: the
 * short query (older relays, on a private network) and the padded one (answered from anywhere). */
static void ask_watched(int s) {
    char text[NET_ADDR];
    pthread_mutex_lock(&lock);
    snprintf(text, sizeof text, "%s", watch_text);
    u32 ip = watch_ip;
    u16 port = watch_port;
    int failed = watch_failed;
    pthread_mutex_unlock(&lock);
    if (!text[0] || failed) return;
    if (!ip) {
        char host[NET_ADDR];
        snprintf(host, sizeof host, "%s", text);
        char *colon = strrchr(host, ':');
        port = NET_DEFAULT_PORT;
        if (colon && strchr(host, ':') == colon) { *colon = 0; long p = strtol(colon + 1, NULL, 10); if (p > 0 && p < 65536) port = (u16)p; }
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        int ok = !getaddrinfo(host, NULL, &hints, &res) && res;
        if (ok) ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
        if (res) freeaddrinfo(res);
        pthread_mutex_lock(&lock);
        if (!strcmp(text, watch_text)) {   /* (unless another address is watched now) */
            if (ok) { watch_ip = ip; watch_port = port; } else watch_failed = 1;
        }
        pthread_mutex_unlock(&lock);
        if (!ok) return;
    }
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(ip);
    u8 padded[NET_FIND_ANSWER_SIZE];
    memset(padded, 0, sizeof padded);
    memcpy(padded, NET_FIND_QUERY, 8);
    sendto(s, NET_FIND_QUERY, 8, 0, (struct sockaddr *)&to, sizeof to);
    sendto(s, padded, sizeof padded, 0, (struct sockaddr *)&to, sizeof to);
    pthread_mutex_lock(&lock);
    if (!strcmp(text, watch_text)) watch_asks++;
    pthread_mutex_unlock(&lock);
}

static void ask(int s) {   /* the query, to every network's broadcast address */
    u32 mine[NET_SELF];
    int n = local_ips(mine, NET_SELF);
    pthread_mutex_lock(&lock);
    memcpy(self_ips, mine, sizeof mine[0] * (size_t)n);
    nself = n;
    pthread_mutex_unlock(&lock);
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(NET_DEFAULT_PORT);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(s, NET_FIND_QUERY, 8, 0, (struct sockaddr *)&to, sizeof to);
    struct ifaddrs *all = NULL;
    if (getifaddrs(&all)) return;
    for (struct ifaddrs *i = all; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_BROADCAST) || !i->ifa_broadaddr)
            continue;
        if (is_virtual(i->ifa_name)) continue;
        to.sin_addr = ((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr;
        sendto(s, NET_FIND_QUERY, 8, 0, (struct sockaddr *)&to, sizeof to);
    }
    freeifaddrs(all);
}

static void heard(const u8 *p, int n, const struct sockaddr_in *from) {
    if (n < NET_FIND_SIZE || memcmp(p, NET_FIND_ANSWER, 8)) return;
    NetChoice c;
    memset(&c, 0, sizeof c);
    char ip[32];
    inet_ntop(AF_INET, &from->sin_addr, ip, sizeof ip);
    u16 port = (u16)(p[8] << 8 | p[9]);
    snprintf(c.addr, sizeof c.addr, "%s:%u", ip, (unsigned)port);
    memcpy(c.room, p + 10, NET_FIND_TEXT);
    memcpy(c.who, p + 10 + NET_FIND_TEXT, NET_FIND_TEXT);
    c.room[NET_FIND_TEXT] = c.who[NET_FIND_TEXT] = 0;
    for (char *t = c.room; *t; t++) if (*t < 32 || *t > 126) *t = '?';
    for (char *t = c.who; *t; t++) if (*t < 32 || *t > 126) *t = '?';
    if (n >= NET_FIND_ANSWER_SIZE) { c.players = p[NET_FIND_SIZE]; c.most = p[NET_FIND_SIZE + 1]; }
    c.found = 1;
    u32 from_ip = ntohl(from->sin_addr.s_addr);
    pthread_mutex_lock(&lock);
    if (is_self(from_ip)) { pthread_mutex_unlock(&lock); return; }   /* (this PC's own room: not one to join) */
    if (watch_ip && from_ip == watch_ip && ntohs(from->sin_port) == watch_port) watch_heard = now_ms();
    int at = -1;
    for (int i = 0; i < nfound && at < 0; i++) {
        if (!strcmp(found[i].addr, c.addr)) at = i;
        /* (the same host heard at two of its addresses - its LAN's, a VPN's: the first one kept) */
        else if (!strcmp(found[i].room, c.room) && !strcmp(found[i].who, c.who) && !strcmp(strchr(found[i].addr, ':'), strchr(c.addr, ':'))) {
            found_at[i] = now_ms();
            found[i].players = c.players;
            found[i].most = c.most;
            pthread_mutex_unlock(&lock);
            return;
        }
    }
    if (at < 0 && nfound < FOUND_MAX) at = nfound++;
    if (at < 0) {   /* (full: the one unheard longest makes room) */
        at = 0;
        for (int i = 1; i < nfound; i++) if (found_at[i] < found_at[at]) at = i;
    }
    found[at] = c;
    found_at[at] = now_ms();
    pthread_mutex_unlock(&lock);
}

static void *search(void *unused) {
    (void)unused;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { searching = 0; return NULL; }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
    struct timeval tv = {0, 200000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    u64 asked = 0;
    u8 buf[512];
    while (running) {
        if (now_ms() - asked >= 1000) { asked = now_ms(); ask(s); ask_watched(s); }
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        int n = (int)recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
        if (n > 0) heard(buf, n, &from);
    }
    close(s);
    return NULL;
}

void net_discover_start(void) {
    if (running) return;
    running = searching = 1;
    if (pthread_create(&searcher, NULL, search, NULL)) running = searching = 0;
}

void net_discover_stop(void) {
    if (!running) return;
    running = 0;
    pthread_join(searcher, NULL);   /* (within 200 ms: its receive times out) */
    searching = 0;
}

int net_discover_watch(const char *addr) {
    u64 t = now_ms();
    pthread_mutex_lock(&lock);
    if (!addr) addr = "";
    if (strcmp(addr, watch_text)) {   /* (another address: asked from the start) */
        snprintf(watch_text, sizeof watch_text, "%s", addr);
        watch_ip = 0;
        watch_port = 0;
        watch_failed = watch_asks = 0;
        watch_heard = 0;
    }
    int state = !watch_text[0] ? NET_WATCH_NONE
              : watch_heard && t - watch_heard < FOUND_FOR_MS ? NET_WATCH_UP
              : watch_failed || watch_asks >= 3 ? NET_WATCH_DOWN : NET_WATCH_CHECKING;
    pthread_mutex_unlock(&lock);
    return state;
}

int net_discover_found(NetChoice *out, int max) {
    int n = 0;
    u64 t = now_ms();
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nfound && n < max; i++)
        if (t - found_at[i] < FOUND_FOR_MS) out[n++] = found[i];
    pthread_mutex_unlock(&lock);
    return n;
}

void net_discover_probe(NetProbe *p) {
    static char lan[NET_ADDR], vpn[NET_ADDR];
    static u32 mine[NET_SELF];
    static int nmine;
    static u64 looked;
    if (!looked || now_ms() - looked > 3000) {   /* (the addresses change rarely: looked at every few seconds) */
        looked = now_ms();
        addresses(lan, vpn, sizeof lan);
        nmine = local_ips(mine, NET_SELF);
    }
    memset(p, 0, sizeof *p);
    snprintf(p->lan_ip, sizeof p->lan_ip, "%s", lan);
    snprintf(p->vpn_ip, sizeof p->vpn_ip, "%s", vpn);
    for (int i = 0; i < nmine; i++) ip_text(mine[i], p->self[p->nself++], NET_ADDR);
    pthread_mutex_lock(&lock);
    p->upnp = upnp;
    snprintf(p->wan, sizeof p->wan, "%s", wan);
    pthread_mutex_unlock(&lock);
    p->searching = searching;
}
#endif
