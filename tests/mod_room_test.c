/* runtime/mod_room.c without a window: tests/test_mod_room.py builds this with it and the rest of the
 * room's mods. One process is the host (its mods folder) and who joins (its own mods folder, which
 * holds its mods/.netplay too), over a network made here (no loss).
 *   mod_room_test <host mods> <own mods> <script> [lobby] [never|always] [overlay] [gameplay-lobby]
 *     script: one letter each - u d l r a b (up down left right accept back), w (wait until the stage
 *     changes or 8 s pass), t (one tick), 0 1 2 (click a button), m (click the mode)
 * Prints after each step "stage <s> ev <e> sel <n>", and at the end the view ("title", "line <k> <text>",
 * "mode", "button <i> <text>", "progress"), "env <SSBU_ROOM_MODS>", "keep <SSBU_ROOM_MODS_KEEP>". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_menu.h"
#include "mod_cache.h"
#include "mod_room.h"
#include "mod_sync.h"
#include "mod_thread.h"
#include "net_menu.h"

const char *rt_data_dir = ".";

#define HOST_IP 0x0A0D0002u
#define ME_IP 0x0A0D0003u

typedef struct Packet { int kind; uint32_t src; size_t n; uint8_t p[1200]; struct Packet *next; } Packet;
static ModLock qlock = MOD_LOCK_INIT;
static ModWake qwake = MOD_WAKE_INIT;
static Packet *head, *tail;
static void push(int kind, uint32_t src, const void *p, size_t n) {
    Packet *k = malloc(sizeof *k);
    if (!k || n > sizeof k->p) { free(k); return; }
    k->kind = kind, k->src = src, k->n = n, k->next = NULL;
    memcpy(k->p, p, n);
    mod_lock(&qlock);
    if (tail) tail->next = k; else head = k;
    tail = k;
    mod_wake(&qwake);
    mod_unlock(&qlock);
}
static void deliver(void *unused) {
    (void)unused;
    for (;;) {
        mod_lock(&qlock);
        while (!head) mod_wait(&qwake, &qlock, 100);
        Packet *k = head;
        head = k->next;
        if (!head) tail = NULL;
        mod_unlock(&qlock);
        if (k->kind == 0) mod_sync_data(k->src, k->p, k->n);
        else if (k->kind == 1) { ModRoute r = {1, k->src, 1, 0}; mod_sync_get(&r, k->p, k->n); }
        else mod_sync_part(k->src, k->p, k->n);
        free(k);
    }
}
static int old_host;
static void net_data(uint32_t dst, const void *p, size_t n) {
    (void)dst;
    int from_host = n >= 2 && ((const uint8_t *)p)[1] == 'L';
    if (old_host && !from_host) return;
    push(0, from_host ? HOST_IP : ME_IP, p, n);
}
static int net_get(uint32_t h, const void *p, size_t n) { (void)h; push(1, ME_IP, p, n); return 1; }
static void net_part(const ModRoute *to, const void *p, size_t n) { (void)to; push(2, HOST_IP, p, n); }

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    int lobby = 0, pref = ROOM_MODS_ASK, overlay = 0;
    for (int i = 4; i < argc; i++) {
        lobby |= !strcmp(argv[i], "lobby") || !strcmp(argv[i], "gameplay-lobby");
        if (!strcmp(argv[i], "never")) pref = ROOM_MODS_NEVER;
        if (!strcmp(argv[i], "always")) pref = ROOM_MODS_ALWAYS;
        overlay |= !strcmp(argv[i], "overlay");
        old_host |= !strcmp(argv[i], "old");
    }
    ModNet n = {net_data, net_get, net_part, NULL, NULL};
    mod_sync_init(&n);
    mod_thread(deliver, NULL);
    if (!old_host) {
        ModHost h;
        memset(&h, 0, sizeof h);
        h.lobby = lobby;
        h.host_id = 0x1234;
        snprintf(h.name, sizeof h.name, "Alexis");
        h.share = 1;
        h.rate = 64L << 20;
        snprintf(h.mods, sizeof h.mods, "%s", argv[1]);
        mod_host_set(&h);
    }
    RoomPrefs p;
    memset(&p, 0, sizeof p);
    p.room_mods = pref;
    p.mode = overlay;
    snprintf(p.mods_root, sizeof p.mods_root, "%s", argv[2]);
    snprintf(p.hash_cache, sizeof p.hash_cache, "%s/../hashes.txt", argv[2]);
    mod_room_setup(&p);
    if (lobby) mod_room_joined(HOST_IP, 1);
    else mod_room_begin(0, 0);
    for (const char *s = argv[3]; *s; s++) {
        int ev = -1, r = RM_EV_NONE;
        switch (*s) {
        case 'u': ev = MENU_UP; break;
        case 'd': ev = MENU_DOWN; break;
        case 'l': ev = MENU_LEFT; break;
        case 'r': ev = MENU_RIGHT; break;
        case 'a': ev = MENU_ACCEPT; break;
        case 'b': ev = MENU_BACK; break;
        case '0': case '1': case '2': r = mod_room_click(*s - '0'); break;
        case 'm': r = mod_room_click(-1); break;
        case 't': mod_room_tick(); break;
        case 'w': {
            mod_room_tick();
            int was = mod_room_stage();
            uint64_t until = mod_now_ms() + 8000;
            while (mod_room_stage() == was && mod_now_ms() < until) { mod_sleep(5); mod_room_tick(); }
            break;
        }
        }
        if (ev >= 0) r = mod_room_event(ev);
        RoomView v;
        mod_room_view(&v);
        printf("stage %d ev %d sel %d\n", mod_room_stage(), r, v.sel);
    }
    RoomView v;
    mod_room_view(&v);
    printf("show %d\ntitle %s\n", v.show, v.title);
    for (int i = 0; i < v.nlines; i++) printf("line %c %s\n", v.lines[i][0], v.lines[i] + 1);
    if (v.mode_row) printf("mode %s %s\n", v.mode, v.mode_text);
    for (int i = 0; i < v.nbuttons; i++) printf("button %d %s\n", i, v.buttons[i]);
    printf("progress %d\n", v.progress);
    const char *e = getenv("SSBU_ROOM_MODS"), *k = getenv("SSBU_ROOM_MODS_KEEP");
    printf("env %s\nkeep %s\nnote %s\n", e ? e : "", k ? k : "", mod_room_note());
    return 0;
}
