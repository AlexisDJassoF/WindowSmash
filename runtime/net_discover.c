/* What the start-up menu's Network page shows of this PC's network (see net_discover.h). Linux:
 * Windows has no start-up menu. */
#ifdef _WIN32
typedef int net_discover_unused;   /* (nothing here on Windows) */
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
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
static void ask(int s) {   /* the query, to every network's broadcast address (and this PC itself) */
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(NET_DEFAULT_PORT);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(s, NET_FIND_QUERY, 8, 0, (struct sockaddr *)&to, sizeof to);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* (a host in another window of this PC) */
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
    snprintf(c.addr, sizeof c.addr, "%s:%u", ip, (unsigned)(p[8] << 8 | p[9]));
    memcpy(c.room, p + 10, NET_FIND_TEXT);
    memcpy(c.who, p + 10 + NET_FIND_TEXT, NET_FIND_TEXT);
    c.room[NET_FIND_TEXT] = c.who[NET_FIND_TEXT] = 0;
    for (char *t = c.room; *t; t++) if (*t < 32 || *t > 126) *t = '?';
    for (char *t = c.who; *t; t++) if (*t < 32 || *t > 126) *t = '?';
    c.found = 1;
    int loopback = (ntohl(from->sin_addr.s_addr) >> 24) == 127;
    pthread_mutex_lock(&lock);
    int at = -1;
    for (int i = 0; i < nfound && at < 0; i++) {
        if (!strcmp(found[i].addr, c.addr)) at = i;
        /* (the same host heard at two of its addresses - the loopback, its LAN's, a VPN's: the first
         * network address kept) */
        else if (!strcmp(found[i].room, c.room) && !strcmp(found[i].who, c.who) && !strcmp(strchr(found[i].addr, ':'), strchr(c.addr, ':'))) {
            if (!loopback && !strncmp(found[i].addr, "127.", 4)) at = i;
            else { found_at[i] = now_ms(); pthread_mutex_unlock(&lock); return; }
        }
    }
    if (at < 0 && nfound < FOUND_MAX) at = nfound++;
    if (at >= 0) { found[at] = c; found_at[at] = now_ms(); }
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
        if (now_ms() - asked >= 1000) { asked = now_ms(); ask(s); }
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
    static u64 looked;
    if (!looked || now_ms() - looked > 3000) {   /* (the addresses change rarely: looked at every few seconds) */
        looked = now_ms();
        addresses(lan, vpn, sizeof lan);
    }
    memset(p, 0, sizeof *p);
    snprintf(p->lan_ip, sizeof p->lan_ip, "%s", lan);
    snprintf(p->vpn_ip, sizeof p->vpn_ip, "%s", vpn);
    pthread_mutex_lock(&lock);
    p->upnp = upnp;
    snprintf(p->wan, sizeof p->wan, "%s", wan);
    pthread_mutex_unlock(&lock);
    p->searching = searching;
}
#endif
