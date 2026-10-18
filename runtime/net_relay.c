/* Netplay's relay (see net.h): the client every player's game runs, and the relay itself,
 * which the host player's game runs (tools/ssbu_relay.py is the same relay, standalone).
 *
 * Protocol, UDP, every field big-endian; each packet starts 'S' 'N' version type:
 *   1 JOIN     client: nonce u64, room[32], name[32]        (repeated until answered)
 *   2 WELCOME  relay:  nonce u64, address u32, then the member list as in MEMBERS
 *   3 REJECT   relay:  nonce u64, reason u8, text[64]
 *   4 PING     client: nonce u64, time u64                   (every second; keeps NAT open)
 *   5 PONG     relay:  time u64, members u8
 *   6 DATA     both:   source u32, destination u32, source port u16, destination port u16, payload
 *   7 MEMBERS  relay:  count u8, count x (address u32, name[32])
 *   8 LEAVE    client: nonce u64
 * The relay gives each member of a room an address in 10.13.0.0/16 and forwards DATA by
 * destination (the room's broadcast address: every other member), checking the source. A
 * member that is silent for 10 s is gone; a JOIN with its nonce from a new place takes it
 * back. Up to 8 members a room. */
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_RAND_S
#define COBJMACROS
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <natupnp.h>
#include <oleauto.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "net.h"

#define MAGIC0 'S'
#define MAGIC1 'N'
enum { T_JOIN = 1, T_WELCOME, T_REJECT, T_PING, T_PONG, T_DATA, T_MEMBERS, T_LEAVE };
#define NAME_LEN 32
#define ROOM_MEMBERS 8
#define TIMEOUT_MS 10000

static u16 get16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 get32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u64 get64(const u8 *p) { return (u64)get32(p) << 32 | get32(p + 4); }
static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static void put64(u8 *p, u64 v) { put32(p, (u32)(v >> 32)); put32(p + 4, (u32)v); }
static u64 now_ms(void) { return GetTickCount64(); }
static void header(u8 *p, int type) { p[0] = MAGIC0; p[1] = MAGIC1; p[2] = NET_PROTOCOL; p[3] = (u8)type; }

/* ---- configuration ------------------------------------------------------------------ */
static int configured = -1, hosting, logging;
static char relay_text[256], room[NAME_LEN + 1] = "smash", name[NAME_LEN + 1] = "Player";
static u16 port = NET_DEFAULT_PORT;

static void configure(void) {
    if (configured >= 0) return;
    const char *h = getenv("SSBU_NET_HOST"), *r = getenv("SSBU_NET_RELAY"), *v;
    hosting = h && *h && strcmp(h, "0");
    if ((v = getenv("SSBU_NET_PORT")) && atoi(v) > 0 && atoi(v) < 65536) port = (u16)atoi(v);
    if ((v = getenv("SSBU_NET_ROOM")) && *v) snprintf(room, sizeof room, "%s", v);
    if ((v = getenv("SSBU_NET_NAME")) && *v) snprintf(name, sizeof name, "%s", v);
    if ((v = getenv("SSBU_NET_LOG"))) logging = atoi(v) > 0 ? atoi(v) : 1;
    if (hosting) snprintf(relay_text, sizeof relay_text, "127.0.0.1:%u", port);
    else if (r && *r) snprintf(relay_text, sizeof relay_text, "%s", r);
    configured = hosting || relay_text[0];
}

static void start(void);
int net_configured(void) { start(); return configured; }
int net_log(void) { configure(); return logging; }
const char *net_player_name(void) { configure(); return name; }

/* ---- the relay (host mode) ------------------------------------------------------------ */
typedef struct Member { struct sockaddr_in at; u64 nonce, seen; u32 ip; char name[NAME_LEN + 1]; } Member;
typedef struct Room { char name[NAME_LEN + 1]; Member members[ROOM_MEMBERS]; int count; } Room;
#define ROOMS 64
static Room rooms[ROOMS];
static SOCKET relay_sock = INVALID_SOCKET;

static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static int member_list(const Room *r, u8 *p) {
    p[0] = (u8)r->count;
    for (int i = 0; i < r->count; i++) {
        put32(p + 1 + i * (4 + NAME_LEN), r->members[i].ip);
        memcpy(p + 5 + i * (4 + NAME_LEN), r->members[i].name, NAME_LEN);
    }
    return 1 + r->count * (4 + NAME_LEN);
}

static void tell_members(const Room *r) {
    u8 p[4 + 1 + ROOM_MEMBERS * (4 + NAME_LEN)];
    header(p, T_MEMBERS);
    int n = 4 + member_list(r, p + 4);
    for (int i = 0; i < r->count; i++) sendto(relay_sock, (const char *)p, n, 0, (const struct sockaddr *)&r->members[i].at, sizeof r->members[i].at);
}

static void drop_member(Room *r, int i) {
    if (logging) fprintf(stderr, "[net] relay: %s left room %s\n", r->members[i].name, r->name);
    r->members[i] = r->members[--r->count];
    tell_members(r);
}

static void relay_packet(const u8 *p, int n, const struct sockaddr_in *from) {
    if (n < 4 || p[0] != MAGIC0 || p[1] != MAGIC1) return;
    u8 out[4 + 8 + 4 + 1 + ROOM_MEMBERS * (4 + NAME_LEN)];
    if (p[2] != NET_PROTOCOL) {   /* another version of the game's netplay: say so */
        if (n >= 12 && p[3] == T_JOIN) {
            memset(out, 0, 4 + 8 + 1 + 64);
            header(out, T_REJECT);
            memcpy(out + 4, p + 4, 8);
            out[12] = 2;
            snprintf((char *)out + 13, 64, "different netplay version (relay %d, you %d)", NET_PROTOCOL, p[2]);
            sendto(relay_sock, (const char *)out, 4 + 8 + 1 + 64, 0, (const struct sockaddr *)from, sizeof *from);
        }
        return;
    }
    /* who sent it */
    Room *r = NULL;
    int m = -1;
    for (int i = 0; i < ROOMS && m < 0; i++)
        for (int j = 0; j < rooms[i].count; j++)
            if (same_addr(&rooms[i].members[j].at, from)) { r = &rooms[i]; m = j; break; }
    switch (p[3]) {
    case T_JOIN: {
        if (n < 4 + 8 + 2 * NAME_LEN) return;
        u64 nonce = get64(p + 4);
        char want[NAME_LEN + 1] = {0}, who[NAME_LEN + 1] = {0};
        memcpy(want, p + 12, NAME_LEN);
        memcpy(who, p + 12 + NAME_LEN, NAME_LEN);
        if (m >= 0 && r->members[m].nonce != nonce) { drop_member(r, m); r = NULL; m = -1; }   /* (a new game from the same place) */
        if (m < 0) {   /* a member coming back from a new place, or a new one */
            for (int i = 0; i < ROOMS && m < 0; i++)
                for (int j = 0; j < rooms[i].count; j++)
                    if (rooms[i].members[j].nonce == nonce && !strcmp(rooms[i].name, want)) { r = &rooms[i]; m = j; r->members[j].at = *from; break; }
        }
        if (m < 0) {
            r = NULL;
            for (int i = 0; i < ROOMS && !r; i++) if (rooms[i].count && !strcmp(rooms[i].name, want)) r = &rooms[i];
            for (int i = 0; i < ROOMS && !r; i++) if (!rooms[i].count) { r = &rooms[i]; snprintf(r->name, sizeof r->name, "%s", want); }
            const char *why = !r ? "the relay is full" : r->count == ROOM_MEMBERS ? "the room is full (8 players)" : NULL;
            if (why) {
                memset(out, 0, 4 + 8 + 1 + 64);
                header(out, T_REJECT);
                put64(out + 4, nonce);
                out[12] = 1;
                snprintf((char *)out + 13, 64, "%s", why);
                sendto(relay_sock, (const char *)out, 4 + 8 + 1 + 64, 0, (const struct sockaddr *)from, sizeof *from);
                return;
            }
            u32 ip = 0;   /* the lowest free address from 10.13.0.2 */
            for (u32 cand = NET_GATEWAY + 1; !ip; cand++) {
                int used = 0;
                for (int j = 0; j < r->count; j++) used |= r->members[j].ip == cand;
                if (!used) ip = cand;
            }
            m = r->count++;
            memset(&r->members[m], 0, sizeof r->members[m]);
            r->members[m].at = *from;
            r->members[m].nonce = nonce;
            r->members[m].ip = ip;
            snprintf(r->members[m].name, sizeof r->members[m].name, "%s", who);
            if (logging) fprintf(stderr, "[net] relay: %s joined room %s as 10.13.%u.%u\n", who, r->name, ip >> 8 & 255, ip & 255);
            tell_members(r);
        }
        r->members[m].seen = now_ms();
        header(out, T_WELCOME);
        put64(out + 4, nonce);
        put32(out + 12, r->members[m].ip);
        int len = 16 + member_list(r, out + 16);
        sendto(relay_sock, (const char *)out, len, 0, (const struct sockaddr *)from, sizeof *from);
        return;
    }
    case T_PING:
        if (m < 0 || n < 20) return;
        r->members[m].seen = now_ms();
        header(out, T_PONG);
        memcpy(out + 4, p + 12, 8);
        out[12] = (u8)r->count;
        sendto(relay_sock, (const char *)out, 13, 0, (const struct sockaddr *)from, sizeof *from);
        return;
    case T_LEAVE:
        if (m >= 0) drop_member(r, m);
        return;
    case T_DATA: {
        if (m < 0 || n < 16) return;
        Member *src = &r->members[m];
        src->seen = now_ms();
        if (get32(p + 4) != src->ip) return;   /* (only from its own address) */
        u32 dst = get32(p + 8);
        for (int j = 0; j < r->count; j++)
            if (j != m && (dst == NET_BROADCAST || dst == 0xFFFFFFFFu || r->members[j].ip == dst))
                sendto(relay_sock, (const char *)p, n, 0, (const struct sockaddr *)&r->members[j].at, sizeof r->members[j].at);
        return;
    }
    }
}

static DWORD WINAPI relay_thread(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"netplay relay");
    static u8 buf[65536];
    u64 last_sweep = now_ms();
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        int n = recvfrom(relay_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
        if (n > 0) relay_packet(buf, n, &from);
        if (now_ms() - last_sweep >= 1000) {   /* members gone silent */
            last_sweep = now_ms();
            for (int i = 0; i < ROOMS; i++)
                for (int j = 0; j < rooms[i].count;)
                    if (now_ms() - rooms[i].members[j].seen > TIMEOUT_MS) drop_member(&rooms[i], j); else j++;
        }
    }
}

/* ---- UPnP: open the relay's port on the router ---------------------------------------- */
#ifdef _WIN32
static const CLSID CLSID_UPnPNAT_ = {0xAE1E00AA, 0x3FD5, 0x403C, {0x8A, 0x27, 0x2B, 0xBD, 0xC3, 0x0C, 0xD0, 0xE1}};
static const IID IID_IUPnPNAT_ = {0xB171C812, 0xCC76, 0x485A, {0x94, 0xD8, 0xB6, 0xB3, 0xA2, 0x79, 0x4E, 0x99}};
static IStaticPortMappingCollection *upnp_mappings;
#endif
static volatile LONG upnp_state;   /* 0 trying, 1 port open, 2 no UPnP router, 3 refused, 4 open but the router is behind another NAT */
static char lan_ip[64], public_ip[64];

static void lan_address(void) {   /* the address this PC goes out on (no packet is sent) */
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    socklen_t len = sizeof a;
    if (s != INVALID_SOCKET && !connect(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &len))
        inet_ntop(AF_INET, &a.sin_addr, lan_ip, sizeof lan_ip);
    if (s != INVALID_SOCKET) closesocket(s);
}

#ifdef _WIN32
static void upnp_close(void) {
    if (!upnp_mappings) return;
    BSTR udp = SysAllocString(L"UDP");
    IStaticPortMappingCollection_Remove(upnp_mappings, port, udp);
    SysFreeString(udp);
}
#else
/* (linux/upnp.c: the router's port mapping through SSDP and SOAP, as Windows' UPnP NAT API does) */
int rt_upnp_add(u16 port, const char *lan_ip, const char *what, char *public_ip, size_t n);
void rt_upnp_remove(void);
static void upnp_close(void) { rt_upnp_remove(); }
#endif

/* Whether an address is one the internet cannot reach: private, or carrier-grade NAT's 100.64/10 -
 * a router reporting one of these as its own outside address sits behind another NAT. */
static int unreachable_address(const char *text) {
    unsigned a, b, cc, d;
    if (sscanf(text, "%u.%u.%u.%u", &a, &b, &cc, &d) != 4) return 0;
    return a == 10 || (a == 172 && (b & 0xF0) == 16) || (a == 192 && b == 168) || (a == 100 && (b & 0xC0) == 64) || a == 0;
}

static void write_share_file(void) {
    char path[600];
    snprintf(path, sizeof path, "%s/../../netplay_address.txt", rt_data_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    if (upnp_state == 1) fprintf(f, "Give your friends this address to join: %s:%u\n", public_ip[0] ? public_ip : "(your public IP)", port);
    else if (upnp_state == 4) fprintf(f, "Your router opened the port, but its own internet address (%s) is not reachable from outside:\n"
                                         "it is behind another router or your provider's NAT. Friends on the same network can join %s:%u.\n"
                                         "For friends elsewhere, use a VPN or tools/ssbu_relay.py on a reachable machine (see NETPLAY.md).\n",
                                      public_ip, lan_ip, port);
    else fprintf(f, "Your router did not open the port (UPnP %s).\nFriends on the same network can join %s:%u.\n"
                    "For friends elsewhere, run tools/ssbu_relay.py on a reachable machine (see NETPLAY.md).\n",
                 upnp_state == 2 ? "not available" : "refused", lan_ip, port);
    fprintf(f, "Room: %s\n", room);
    fclose(f);
}

static DWORD WINAPI upnp_thread(LPVOID unused) {
    (void)unused;
    lan_address();
#ifndef _WIN32
    LONG state = rt_upnp_add(port, lan_ip, "SSBU netplay relay", public_ip, sizeof public_ip);
    if (state == 1) {
        atexit(upnp_close);
        if (unreachable_address(public_ip)) state = 4;
    }
#else
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IUPnPNAT *nat = NULL;
    LONG state = 2;
    if (SUCCEEDED(CoCreateInstance(&CLSID_UPnPNAT_, NULL, CLSCTX_ALL, &IID_IUPnPNAT_, (void **)&nat)) && nat) {
        IStaticPortMappingCollection *maps = NULL;
        if (SUCCEEDED(IUPnPNAT_get_StaticPortMappingCollection(nat, &maps)) && maps) {   /* (NULL: no UPnP router) */
            WCHAR client[64];
            MultiByteToWideChar(CP_UTF8, 0, lan_ip, -1, client, 64);
            BSTR udp = SysAllocString(L"UDP"), who = SysAllocString(client), what = SysAllocString(L"SSBU netplay relay");
            IStaticPortMapping *map = NULL;
            IStaticPortMappingCollection_Remove(maps, port, udp);   /* (a stale one from an earlier run) */
            if (SUCCEEDED(IStaticPortMappingCollection_Add(maps, port, udp, port, who, VARIANT_TRUE, what, &map)) && map) {
                BSTR ext = NULL;
                if (SUCCEEDED(IStaticPortMapping_get_ExternalIPAddress(map, &ext)) && ext) {
                    WideCharToMultiByte(CP_UTF8, 0, ext, -1, public_ip, sizeof public_ip, NULL, NULL);
                    SysFreeString(ext);
                }
                IStaticPortMapping_Release(map);
                upnp_mappings = maps;
                atexit(upnp_close);
                state = unreachable_address(public_ip) ? 4 : 1;
            } else {
                state = 3;
                IStaticPortMappingCollection_Release(maps);
            }
            SysFreeString(udp); SysFreeString(who); SysFreeString(what);
        }
        IUPnPNAT_Release(nat);
    }
#endif
    InterlockedExchange(&upnp_state, state);
    if (state == 1) fprintf(stderr, "[net] UPnP: port %u opened; friends join %s:%u (LAN address %s)\n", port, public_ip, port, lan_ip);
    else if (state == 4) fprintf(stderr, "[net] UPnP: port %u opened, but the router's outside address %s is behind another NAT; LAN address %s\n", port, public_ip, lan_ip);
    else fprintf(stderr, "[net] UPnP: %s; LAN address %s, port %u\n", state == 2 ? "no UPnP router found" : "the router refused the port", lan_ip, port);
    write_share_file();
    return 0;
}

/* ---- the client ---------------------------------------------------------------------- */
static SOCKET client_sock = INVALID_SOCKET;
static struct sockaddr_in relay_at;
static volatile LONG joined;           /* WELCOME seen and not timed out */
static volatile u32 my_ip;
static u64 nonce, last_pong;
static volatile LONG members, ping_ms = -1;
static char reject_text[80], member_names[ROOM_MEMBERS][NAME_LEN + 1];

u32 net_local_ip(void) { start(); return joined ? my_ip : 0; }

void net_send(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port, const void *data, u32 len) {
    if (!joined || client_sock == INVALID_SOCKET || len > 65000) return;
    u8 stack[1536], *p = len + 16 <= sizeof stack ? stack : malloc(len + 16);
    if (!p) return;
    header(p, T_DATA);
    put32(p + 4, src_ip);
    put32(p + 8, dst_ip);
    put16(p + 12, src_port);
    put16(p + 14, dst_port);
    memcpy(p + 16, data, len);
    sendto(client_sock, (const char *)p, (int)len + 16, 0, (const struct sockaddr *)&relay_at, sizeof relay_at);
    if (p != stack) free(p);
}

/* (ICMP "port unreachable" would otherwise make a UDP socket's next receive fail; Linux reports
 * that only for connected sockets) */
#ifdef _WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
static void no_connreset(SOCKET s) {
    BOOL off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bytes, NULL, NULL);
}
#else
static void no_connreset(SOCKET s) { (void)s; }
#endif

static int resolve_relay(void) {
    char hostpart[256];
    snprintf(hostpart, sizeof hostpart, "%s", relay_text);
    u16 rport = port;
    char *colon = strrchr(hostpart, ':');
    if (colon) { *colon = 0; if (atoi(colon + 1) > 0) rport = (u16)atoi(colon + 1); }
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(hostpart, NULL, &hints, &res) || !res) return 0;
    relay_at = *(struct sockaddr_in *)res->ai_addr;
    relay_at.sin_port = htons(rport);
    freeaddrinfo(res);
    if (relay_at.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {   /* (the relay is here: listen on loopback only) */
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(client_sock, (struct sockaddr *)&a, sizeof a);
    }
    return 1;
}

static void send_join(void) {
    u8 p[4 + 8 + 2 * NAME_LEN] = {0};
    header(p, T_JOIN);
    put64(p + 4, nonce);
    memcpy(p + 12, room, strlen(room));
    memcpy(p + 12 + NAME_LEN, name, strlen(name));
    sendto(client_sock, (const char *)p, sizeof p, 0, (const struct sockaddr *)&relay_at, sizeof relay_at);
}

static void read_members(const u8 *p, int n) {
    int count = n >= 1 ? p[0] : 0;
    if (count > ROOM_MEMBERS) count = ROOM_MEMBERS;
    for (int i = 0; i < count && 1 + (i + 1) * (4 + NAME_LEN) <= n; i++) {
        memcpy(member_names[i], p + 5 + i * (4 + NAME_LEN), NAME_LEN);
        member_names[i][NAME_LEN] = 0;
    }
    InterlockedExchange(&members, count);
}

static DWORD WINAPI client_thread(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"netplay client");
    static u8 buf[65536];
    while (!resolve_relay()) {
        snprintf(reject_text, sizeof reject_text, "cannot find the relay %s", relay_text);
        Sleep(3000);
    }
    reject_text[0] = 0;
    u64 last_join = 0, last_ping = 0;
    for (;;) {
        u64 t = now_ms();
        if (!joined && t - last_join >= 500) { send_join(); last_join = t; }
        if (joined && t - last_ping >= 1000) {
            u8 p[20];
            header(p, T_PING);
            put64(p + 4, nonce);
            put64(p + 12, t);
            sendto(client_sock, (const char *)p, sizeof p, 0, (const struct sockaddr *)&relay_at, sizeof relay_at);
            last_ping = t;
        }
        if (joined && t - last_pong > TIMEOUT_MS) {
            InterlockedExchange(&joined, 0);   /* the relay is gone: keep asking to join again */
            InterlockedExchange(&ping_ms, -1);
            fprintf(stderr, "[net] lost the relay %s; reconnecting\n", relay_text);
        }
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        int n = recvfrom(client_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
        if (n < 4 || buf[0] != MAGIC0 || buf[1] != MAGIC1 || !same_addr(&from, &relay_at)) continue;
        switch (buf[3]) {
        case T_DATA:
            if (n >= 16 && joined) vsock_deliver(get32(buf + 4), get16(buf + 12), get32(buf + 8), get16(buf + 14), buf + 16, (u32)(n - 16));
            break;
        case T_WELCOME:
            if (n < 17 || get64(buf + 4) != nonce) break;
            my_ip = get32(buf + 12);
            read_members(buf + 16, n - 16);
            last_pong = now_ms();
            reject_text[0] = 0;
            if (!joined) fprintf(stderr, "[net] joined room %s on %s as 10.13.%u.%u (%ld players)\n", room, relay_text, my_ip >> 8 & 255, my_ip & 255, (long)members);
            InterlockedExchange(&joined, 1);
            break;
        case T_REJECT:
            if (n >= 13 + 1 && get64(buf + 4) == nonce) {
                snprintf(reject_text, sizeof reject_text, "%.64s", (const char *)buf + 13);
                fprintf(stderr, "[net] the relay refused: %s\n", reject_text);
                Sleep(2000);
            }
            break;
        case T_PONG:
            if (n >= 13) {
                last_pong = now_ms();
                InterlockedExchange(&ping_ms, (LONG)(last_pong - get64(buf + 4)));
            }
            break;
        case T_MEMBERS:
            read_members(buf + 4, n - 4);
            break;
        }
    }
}

static void leave(void) {
    if (!joined) return;
    u8 p[12];
    header(p, T_LEAVE);
    put64(p + 4, nonce);
    sendto(client_sock, (const char *)p, sizeof p, 0, (const struct sockaddr *)&relay_at, sizeof relay_at);
}

/* Started on first use (the overlay asks every frame), once Winsock is up. */
static void start(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    InitOnceBeginInitialize(&once, 0, &pending, NULL);
    if (!pending) return;
    configure();
    if (configured) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        DWORD timeout = 100;
#else
        struct timeval timeout = {0, 100000};
#endif
        unsigned int r1, r2;
        rand_s(&r1); rand_s(&r2);
        nonce = (u64)r1 << 32 | r2;
        if (hosting) {
            relay_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            struct sockaddr_in a = {0};
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            const char *only = getenv("SSBU_NET_BIND");   /* (an address to listen on alone, e.g. 127.0.0.1 for tests) */
            if (only && *only) inet_pton(AF_INET, only, &a.sin_addr);
            if (relay_sock == INVALID_SOCKET || bind(relay_sock, (struct sockaddr *)&a, sizeof a)) {
                snprintf(reject_text, sizeof reject_text, "cannot use port %u for the relay (in use?)", port);
                fprintf(stderr, "[net] %s\n", reject_text);
            } else {
                setsockopt(relay_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
                no_connreset(relay_sock);
                CloseHandle(CreateThread(NULL, 0, relay_thread, NULL, 0, NULL));
                const char *upnp = getenv("SSBU_NET_UPNP");   /* 0: leave the router alone */
                if (upnp && !strcmp(upnp, "0")) { lan_address(); upnp_state = 3; }
                else CloseHandle(CreateThread(NULL, 0, upnp_thread, NULL, 0, NULL));
                fprintf(stderr, "[net] hosting the relay on UDP port %u, room %s\n", port, room);
            }
        }
        client_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (client_sock != INVALID_SOCKET) {
            setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
            no_connreset(client_sock);
            int big = 1 << 20;
            setsockopt(client_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof big);
            CloseHandle(CreateThread(NULL, 0, client_thread, NULL, 0, NULL));
            atexit(leave);
        }
    }
    InitOnceComplete(&once, 0, NULL);
}

void net_status(char *out, size_t n) {
    start();
    if (!configured) { if (n) out[0] = 0; return; }
    if (reject_text[0] && !joined) { snprintf(out, n, "Netplay: %s", reject_text); return; }
    if (!joined) { snprintf(out, n, "Netplay: connecting to %s (room %s)...", relay_text, room); return; }
    char share[128] = "";
    if (hosting) {
        LONG u = upnp_state;
        if (u == 0) snprintf(share, sizeof share, "  | opening port %u on the router...", port);
        else if (u == 1) snprintf(share, sizeof share, "  | friends join %s:%u", public_ip[0] ? public_ip : "(your public IP)", port);
        else if (u == 4) snprintf(share, sizeof share, "  | router behind another NAT: only same-network friends can join (%s:%u)", lan_ip, port);
        else snprintf(share, sizeof share, "  | UPnP %s: only same-network friends can join (%s:%u)", u == 2 ? "unavailable" : "refused", lan_ip, port);
    }
    snprintf(out, n, "Netplay: room %s, %ld player%s, %s 10.13.%u.%u, ping %ld ms%s", room, (long)members, members == 1 ? "" : "s",
             name, my_ip >> 8 & 255, my_ip & 255, (long)ping_ms, share);
}
