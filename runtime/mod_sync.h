/* The room's mods on the network (openspec change netplay-mod-sync): who joins a room asks its host
 * for the manifest (mod_manifest.h), and the host answers it and gives the files.
 *
 * The manifest travels inside DATA, between port 65512s (as local wireless's own messages use 65511),
 * so every relay and lobby - older ones too - carries it in the room as it carries the game's traffic:
 *   MODQ    'M' 'Q' 1, nonce u64, from u16                       who joins -> the host: the manifest,
 *                                                                from that chunk on
 *   MODLIST 'M' 'L' 1, nonce u64, host_id u64, hash u64, chunk u16, total u16, bytes (1100 at most)
 *                                                                the host -> who asked (total 0: being
 *                                                                made, ask again)
 * The host answers a MODQ with up to MODQ_WINDOW chunks from `from` on; who asks asks again from the
 * first chunk it lacks, every 400 ms, and gives up after 3 s without an answer (a host older than
 * this ignores port 65512: no manifest, and the game plays as before).
 *
 * The files travel in packets of their own (net_relay.c), types 16 and 17, only straight between the
 * two players or through a private room's relay - the online lobby drops them:
 *   16 MODGET  who joins: secret u64, then sha u64 (a file's SHA-256, its first 8 bytes), first block
 *              u32, count u16                                    (blocks of MOD_BLOCK bytes, MOD_WINDOW at most)
 *   17 MODPART the host:  secret u64, then sha u64, block u32, bytes (block MOD_PAUSED: the host is in a
 *              fight - ask again later; MOD_NOT_HERE: not a file it gives)
 * secret: the pair's from the lobby (direct), or the cookie a private room's relay gave the player.
 * The host gives at most room_mods_rate bytes a second, and nothing while it plays a fight.
 *
 * The transport is net_relay.c's (or tests/mod_sync_test.c's): ModNet. Nothing here touches a socket. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "mod_manifest.h"

#define MODS_PORT 65512
#define MOD_BLOCK 1024
#define MOD_WINDOW 64
#define MODQ_WINDOW 64
#define MODLIST_CHUNK 1100
#define MOD_PAUSED 0xFFFFFFFFu
#define MOD_NOT_HERE 0xFFFFFFFEu
#define MODQ_GIVE_UP_MS 3000

/* Where a MODGET came from, for its answer (net_relay.c's: its socket, address and secret) */
typedef struct ModRoute { int via; uint32_t ip; uint16_t port; uint64_t secret; } ModRoute;

typedef struct ModNet {
    void (*data)(uint32_t dst, const void *p, size_t n);          /* a datagram between port 65512s (dst: an address, or the room's broadcast) */
    int (*get)(uint32_t host, const void *p, size_t n);           /* a MODGET's body (after the secret) to the host: 0 when there is no way */
    void (*part)(const ModRoute *to, const void *p, size_t n);    /* a MODPART's body back where a MODGET came from */
    int (*fight)(void);                                           /* this game is in a fight now */
    int (*reachable)(uint32_t host);                              /* the host's files can come (a direct path, or a private room's relay); NULL yes */
} ModNet;
void mod_sync_init(const ModNet *net);

/* ---- from the network (net_relay.c) ---- */
void mod_sync_data(uint32_t src, const uint8_t *p, size_t n);            /* a datagram to port 65512 */
void mod_sync_get(const ModRoute *from, const uint8_t *p, size_t n);     /* a MODGET's body (its secret checked) */
void mod_sync_part(uint32_t src, const uint8_t *p, size_t n);            /* a MODPART's body (its secret checked) */

/* ---- the host ---- */
typedef struct ModHost {
    int lobby;                 /* a room of the online lobby: no gameplay mod's files */
    uint64_t host_id;
    char name[MANI_NAME + 1];
    int share;                 /* share_mods */
    long rate;                 /* bytes a second, at most */
    char mods[512];            /* the mods folder */
    char hash_cache[512];      /* the file the hashes are kept in */
    int (*may_ask)(uint32_t src);   /* who may ask (the lobby: the players of this host's network); NULL anyone */
} ModHost;
/* This game hosts now (the manifest made again, on a thread of its own, when it differs); NULL: not */
void mod_host_set(const ModHost *h);
int mod_host_ready(void);      /* the manifest is made */
/* (tests, the log) bytes given so far, MODGETs not answered for a fight */
void mod_host_stats(uint64_t *bytes, uint64_t *paused);

/* ---- who joins ---- */
enum { MQ_IDLE, MQ_ASKING, MQ_GOT, MQ_NONE, MQ_BAD };
/* Ask host for its manifest (the room's broadcast address in a private room: only its host answers) */
void mod_ask(uint32_t host, int lobby);
/* MQ_*. With MQ_GOT the manifest read (*m: a copy, mani_free it), its hash and the host's address;
 * MQ_BAD: why in err */
int mod_ask_state(Manifest *m, uint64_t *hash, uint32_t *from, char *err, size_t en);
void mod_ask_stop(void);

/* (mod_fetch.c) A MODGET's body to the host, through ModNet.get: 0 no way */
int mod_sync_send_get(uint32_t host, const void *p, size_t n);
int mod_sync_reachable(uint32_t host);
/* (mod_fetch.c) The MODPARTs come here first */
void mod_fetch_part(uint32_t src, const uint8_t *p, size_t n);
