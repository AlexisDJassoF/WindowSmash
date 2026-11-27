/* A room's mods, for who joins: see mod_room.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _CRT_SECURE_NO_WARNINGS
#include "mod_room.h"
#include "gpu_menu.h"
#include "mod_cache.h"
#include "mod_fetch.h"
#include "mod_menu.h"
#include "mod_sync.h"
#include "mod_thread.h"
#include "net_menu.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BROADCAST 0x0A0DFFFFu   /* (net.h's NET_BROADCAST: a private room's host answers it) */

static ModLock lock = MOD_LOCK_INIT;
static RoomPrefs prefs = {ROOM_MODS_ASK, ROOM_MIRROR, "", ""};
static int stage = RM_IDLE, lobby, sel = 1, mode, after_kind, preparing, prepared;
static uint32_t host;
static Manifest man;
static int have_man;
static uint64_t man_hash;
static HostIndex *index_;
static Reconcile *rec;
static OwnFiles own;
static FetchFile *queue;
static FetchOrigin *origins;
static int nqueue, norigins, reachable;
static char cache[600], blobs[700], note[240], status_line[200];
static volatile int pending;
static volatile uint32_t pending_host;
static int failed_mods;
static char failed[8][160];
enum { AFTER_LOBBY, AFTER_FAILED, AFTER_READY };   /* what RM_AFTER shows */

extern const char *rt_data_dir;

void mod_room_setup(const RoomPrefs *p) {
    mod_lock(&lock);
    prefs = *p;
    if (!prefs.hash_cache[0]) {   /* (cache/mods/hashes.txt, as mod_manifest.c's host side keeps it) */
        const char *root = getenv("SSBU_DATA_ROOT");
        if (root) snprintf(prefs.hash_cache, sizeof prefs.hash_cache, "%s/cache/mods/hashes.txt", root);
        else snprintf(prefs.hash_cache, sizeof prefs.hash_cache, "%s/../../cache/mods/hashes.txt", rt_data_dir ? rt_data_dir : ".");
    }
    mode = p->mode;
    mod_cache_dir(p->mods_root, cache, sizeof cache);
    mod_cache_blobs(cache, blobs, sizeof blobs);
    mod_unlock(&lock);
}

static int ready_paths(void) {   /* (lock held) the paths, from the defaults if setup was not called */
    if (cache[0]) return 1;
    if (!prefs.mods_root[0]) {
        mod_root(prefs.mods_root, sizeof prefs.mods_root);
        NetSettings s;
        net_settings_load(&s);
        prefs.room_mods = s.room_mods;
        prefs.mode = mode = s.room_mods_mode == ROOM_MODE_OVERLAY ? ROOM_OVERLAY : ROOM_MIRROR;
    }
    mod_cache_dir(prefs.mods_root, cache, sizeof cache);
    mod_cache_blobs(cache, blobs, sizeof blobs);
    return 1;
}

void mod_room_tidy(void) {
    static int done;
    mod_lock(&lock);
    ready_paths();
    int first = !done++;
    char c[600];
    snprintf(c, sizeof c, "%s", cache);
    mod_unlock(&lock);
    if (first) mod_cache_tidy(c, (long long)time(NULL), HOST_DAYS);
}

void mod_room_joined(uint32_t h, int is_lobby) {
    (void)is_lobby;
    mod_lock(&lock);
    pending_host = h;
    pending = 1;
    mod_unlock(&lock);
}

static void clear(void) {   /* (lock held) what the last host left */
    if (have_man) mani_free(&man);
    have_man = 0;
    if (index_) { host_index_free(index_); free(index_); index_ = NULL; }
    free(rec);
    rec = NULL;
    mani_free_files(own.files, own.n);
    own.files = NULL, own.n = 0;
    free(queue); free(origins);
    queue = NULL, origins = NULL, nqueue = norigins = 0;
    failed_mods = 0;
    note[0] = status_line[0] = 0;
    prepared = 0;
}

void mod_room_begin(uint32_t h, int is_lobby) {
    mod_lock(&lock);
    ready_paths();
    clear();
    host = h, lobby = is_lobby, sel = 1, mode = prefs.mode;
    if (prefs.room_mods == ROOM_MODS_NEVER) {   /* (not even asked) */
        stage = is_lobby ? RM_IDLE : RM_READY;
        mod_unlock(&lock);
        return;
    }
    stage = RM_ASKING;
    mod_unlock(&lock);
    mod_ask(h ? h : BROADCAST, is_lobby);
}

int mod_room_stage(void) {
    mod_lock(&lock);
    int s = stage;
    mod_unlock(&lock);
    return s;
}

int mod_room_busy(void) {
    mod_lock(&lock);
    int b = stage == RM_DIALOG || stage == RM_AFTER || (!lobby && stage == RM_FETCHING);
    mod_unlock(&lock);
    return b;
}

/* ---- comparing with what is kept (a thread: the player's own files may need hashing) ---- */
static void prepare_thread(void *unused) {
    (void)unused;
    mod_lock(&lock);
    Manifest m = man;   /* (read only meanwhile: the UI waits for `prepared`) */
    char mods_root[512], hashes[512], b[700], c[600];
    snprintf(mods_root, sizeof mods_root, "%s", prefs.mods_root);
    snprintf(hashes, sizeof hashes, "%s", prefs.hash_cache);
    snprintf(b, sizeof b, "%s", blobs);
    snprintf(c, sizeof c, "%s", cache);
    mod_unlock(&lock);
    HostIndex *h = malloc(sizeof *h);
    Reconcile *r = malloc(sizeof *r);
    ModList *l = calloc(1, sizeof *l);
    OwnFiles o = {NULL, 0};
    if (h) host_index_read(c, m.host_id, h);
    if (l) {
        mod_scan(l, mods_root);
        o.n = mani_hash_all(l, hashes[0] ? hashes : NULL, &o.files);
        mod_list_free(l);
        free(l);
    }
    if (h && r) mod_reconcile(&m, h, b, &o, r);
    mod_lock(&lock);
    index_ = h, rec = r, own = o;
    prepared = h && r ? 1 : -1;
    preparing = 0;
    mod_unlock(&lock);
}

/* ---- choosing ---- */
static void choose(void) {   /* (lock held) this start loads the host's mods (hle_fs.c) */
    char v[64];
    snprintf(v, sizeof v, "%016llx:%s", (unsigned long long)man.host_id, mode == ROOM_OVERLAY ? "overlay" : "mirror");
#ifdef _WIN32
    _putenv_s("SSBU_ROOM_MODS", v);
#else
    setenv("SSBU_ROOM_MODS", v, 1);
#endif
    fprintf(stderr, "[mods] this start uses %s's room mods (%s)\n", man.host, mode == ROOM_OVERLAY ? "overlay: your own skins win" : "mirror");
}

static void write_index(void) {   /* (lock held) */
    if (!index_ || !have_man) return;
    host_index_update(index_, &man, blobs, (long long)time(NULL), man_hash);
    if (!host_index_write(cache, index_)) fprintf(stderr, "[mods] cannot write %s's room index in %s\n", man.host, cache);
    else mod_cache_views(cache, index_, blobs, NULL, 0);   /* (mods_join/<host>/<mod>/: normal mod folders) */
}

static int loaded_already(void) {   /* (lock held) this start already loads this host's mods */
    uint64_t id;
    int m;
    return mod_room_chosen(&id, &m) && id == man.host_id && m == mode;
}

static void start_fetch(void) {   /* (lock held) */
    free(queue); free(origins);
    mod_fetch_list(&man, rec, blobs, &own, &queue, &nqueue, &origins, &norigins);
    if (!nqueue) { stage = RM_FETCHING; return; }   /* (the next tick finishes it) */
    if (mod_fetch_start(host ? host : BROADCAST, blobs, queue, nqueue, origins, norigins)) stage = RM_FETCHING;
    else { snprintf(note, sizeof note, "!The download did not start"); stage = RM_DIALOG; sel = 1; }
}

/* the download over: what failed, the index written, and what comes next (lock held) */
static void fetch_done(int cancelled) {
    failed_mods = 0;
    for (int i = 0; i < man.nmods; i++) {
        int bad = 0;
        const char *why = "";
        for (int k = 0; k < nqueue; k++)
            if (queue[k].mod == i && mod_fetch_result(k) == FR_FAILED) { bad = 1; why = mod_fetch_why(k); break; }
        if (bad && failed_mods < 8) snprintf(failed[failed_mods++], sizeof failed[0], "%s: %s", man.mods[i].name, why);
        if (bad) fprintf(stderr, "[mods] %s (from %s's room): not used - %s\n", man.mods[i].name, man.host, why);
    }
    write_index();
    if (cancelled) {
        stage = RM_DIALOG;
        sel = 1;
        snprintf(note, sizeof note, "Download cancelled: what had come is kept for the next time");
        mod_reconcile(&man, index_, blobs, &own, rec);   /* (what is still missing) */
        return;
    }
    if (lobby) { after_kind = AFTER_LOBBY; stage = RM_AFTER; sel = 0; return; }
    if (failed_mods) { after_kind = AFTER_FAILED; stage = RM_AFTER; sel = 0; return; }
    choose();
    stage = RM_READY;
}

void mod_room_tick(void) {
    mod_lock(&lock);
    if (pending && (stage == RM_IDLE || stage == RM_READY)) {
        uint32_t h = pending_host;
        pending = 0;
        mod_unlock(&lock);
        mod_room_begin(h, 1);
        mod_lock(&lock);
    }
    if (stage == RM_ASKING && !have_man && !preparing) {
        char err[160];
        int s = mod_ask_state(&man, &man_hash, &host, err, sizeof err);
        if (s == MQ_GOT) {
            have_man = 1;
            mod_ask_stop();
            if (!man.nmods) { stage = lobby ? RM_IDLE : RM_READY; mani_free(&man); have_man = 0; }   /* (a host without mods: as before) */
            else { preparing = 1; if (!mod_thread(prepare_thread, NULL)) { preparing = 0; prepared = -1; } }
        } else if (s == MQ_NONE || s == MQ_BAD || s == MQ_IDLE) {
            if (s == MQ_BAD) snprintf(note, sizeof note, "!The room's mod list was refused: %s", err);
            stage = lobby ? RM_IDLE : RM_READY;
            mod_ask_stop();
        }
    }
    if (stage == RM_ASKING && have_man && prepared) {
        if (prepared < 0) { stage = lobby ? RM_IDLE : RM_READY; snprintf(note, sizeof note, "!Out of memory for the room's mods"); }
        else {
            reachable = mod_sync_reachable(host);
            if (prefs.room_mods == ROOM_MODS_ALWAYS && (rec->files || rec->news || rec->changed)) start_fetch();
            else if (!rec->dialog) {   /* (as last time, moved or dropped: applied by itself) */
                write_index();
                char moved[64] = "";
                if (rec->moved) snprintf(moved, sizeof moved, ", %d moved", rec->moved);
                snprintf(note, sizeof note, "%s's room: %d mod%s as last time%s", man.host, rec->same + rec->moved, rec->same + rec->moved == 1 ? "" : "s", moved);
                if (!lobby) { choose(); stage = RM_READY; }
                else if (loaded_already() || !(rec->same + rec->moved)) stage = RM_IDLE;
                else { after_kind = AFTER_READY; stage = RM_AFTER; sel = 0; }
            } else if (prefs.room_mods == ROOM_MODS_ALWAYS && !rec->files) {   /* (only warnings: nothing to agree to) */
                write_index();
                if (!lobby) { choose(); stage = RM_READY; } else { after_kind = AFTER_READY; stage = RM_AFTER; sel = 0; }
            } else { stage = RM_DIALOG; sel = 1; }
        }
    }
    if (stage == RM_FETCHING) {
        FetchStatus st;
        mod_fetch_status(&st);
        if (!nqueue || st.state == FS_DONE || st.state == FS_CANCELLED) fetch_done(nqueue && st.state == FS_CANCELLED);
    }
    mod_unlock(&lock);
}

/* ---- the dialog ---- */
static int buttons(void) {   /* (lock held) */
    if (stage == RM_DIALOG) return 3;
    if (stage == RM_AFTER) return after_kind == AFTER_FAILED ? 3 : 2;
    return 0;
}

static int decline(void) {   /* Play without them (lock held) */
    stage = lobby ? RM_IDLE : RM_READY;
    fprintf(stderr, "[mods] %s's room mods: not used (Play without them)\n", have_man ? man.host : "?");
    return RM_EV_PLAY;
}

static int press(int b) {   /* (lock held) */
    if (stage == RM_DIALOG) {
        if (b == 0) {
            if (!lobby && !reachable && !rec->origin && rec->files > rec->own) { snprintf(note, sizeof note, "!The host cannot give its files"); return RM_EV_NONE; }
            if (lobby && !reachable && !rec->origin && rec->files > rec->own) { snprintf(note, sizeof note, "!No direct connection with the host: these cannot be downloaded"); return RM_EV_NONE; }
            start_fetch();
            return lobby ? RM_EV_PLAY : RM_EV_NONE;   /* (the lobby: behind the game) */
        }
        if (b == 1) return decline();
        if (b == 2) { stage = RM_IDLE; return RM_EV_MENU; }
    }
    if (stage == RM_AFTER) {
        if (after_kind == AFTER_FAILED) {
            if (b == 0) { choose(); stage = RM_READY; return RM_EV_PLAY; }   /* (with the rest) */
            if (b == 1) return decline();
            if (b == 2) { stage = RM_IDLE; return RM_EV_MENU; }
        } else {
            if (b == 0) {
                choose();
#ifdef _WIN32
                _putenv_s("SSBU_ROOM_MODS_KEEP", "1");
#else
                setenv("SSBU_ROOM_MODS_KEEP", "1", 1);   /* (the menu keeps them for the start it starts) */
#endif
                stage = RM_IDLE;
                return RM_EV_RESTART;
            }
            if (b == 1) { stage = RM_IDLE; snprintf(note, sizeof note, "Kept for the next time you join %s", man.host); return RM_EV_PLAY; }
        }
    }
    return RM_EV_NONE;
}

int mod_room_event(int ev) {
    mod_lock(&lock);
    int r = RM_EV_NONE, nb = buttons();
    if (stage == RM_FETCHING && !lobby) {
        if (ev == MENU_BACK) mod_fetch_cancel();   /* (back to the dialog once it has stopped) */
    } else if (nb) {
        int mode_row = stage == RM_DIALOG;
        switch (ev) {
        case MENU_UP: if (mode_row) sel = -1; break;
        case MENU_DOWN: if (sel < 0) sel = 1; break;
        case MENU_LEFT:
            if (sel < 0) mode = !mode;
            else if (sel > 0) sel--;
            break;
        case MENU_RIGHT:
            if (sel < 0) mode = !mode;
            else if (sel + 1 < nb) sel++;
            break;
        case MENU_ACCEPT: r = sel < 0 ? (mode = !mode, RM_EV_NONE) : press(sel); break;
        case MENU_BACK:   /* (Play without them; after the download: Later) */
            r = stage == RM_DIALOG ? decline() : press(1);   /* (Play without them, or Later) */
            break;
        }
    }
    mod_unlock(&lock);
    return r;
}

int mod_room_click(int b) {
    mod_lock(&lock);
    int r = RM_EV_NONE;
    if (b < 0 && stage == RM_DIALOG) mode = !mode, sel = -1;
    else if (b >= 0 && b < buttons()) { sel = b; r = press(b); }
    mod_unlock(&lock);
    return r;
}

/* ---- what is drawn ---- */
static void size_text(uint64_t b, char *out, size_t n) {
    if (b >= 10ull << 20) snprintf(out, n, "%llu MB", (unsigned long long)((b + (1 << 19)) >> 20));
    else if (b >= 1 << 20) snprintf(out, n, "%.1f MB", b / 1048576.0);
    else snprintf(out, n, "%llu KB", (unsigned long long)((b + 1023) >> 10));
}

static void who(const RecMod *s, char *out, size_t n) {   /* "Pikachu Ice (Pikachu, color 5)" */
    int color = -1;
    if (s->mani >= 0) {
        const ManiMod *x = &man.mods[s->mani];
        for (int k = 0; k < x->nfiles && color < 0; k++) {
            char f[64];
            int c;
            if (mod_path_color(man.files[x->first + k].path, f, sizeof f, &c) && !strcmp(f, x->fighter) && c < MOD_COLORS)
                color = x->to[c] >= 0 ? x->to[c] : c;
        }
    }
    char name[96];
    mod_ascii(s->name, name, sizeof name);
    if (s->fighter[0] && color >= 0) snprintf(out, n, "%s (%s, color %d)", name, mod_fighter_name(s->fighter), color + 1);
    else if (s->fighter[0]) snprintf(out, n, "%s (%s)", name, mod_fighter_name(s->fighter));
    else snprintf(out, n, "%s", name);
}

static void line(RoomView *v, char kind, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void line(RoomView *v, char kind, const char *fmt, ...) {
    if (v->nlines >= ROOM_LINES) return;
    char *l = v->lines[v->nlines++];
    l[0] = kind;
    va_list a;
    va_start(a, fmt);
    vsnprintf(l + 1, sizeof v->lines[0] - 1, fmt, a);
    va_end(a);
}

void mod_room_view(RoomView *v) {
    memset(v, 0, sizeof *v);
    v->progress = -1;
    mod_lock(&lock);
    char host_name[MANI_NAME + 1] = "?";
    if (have_man) mod_ascii(man.host, host_name, sizeof host_name);
    if (stage == RM_ASKING && !lobby) {
        v->show = 1;
        snprintf(v->title, sizeof v->title, "Joining the room...");
        line(v, 'i', "Asking the host which mods it plays with");
        v->progress = -1;
    } else if (stage == RM_DIALOG && rec) {
        v->show = 1;
        char total[32];
        size_text(rec->bytes - rec->own_bytes, total, sizeof total);
        int shown = 0;
        for (int i = 0; i < rec->n; i++) shown += rec->mods[i].state != RS_REMOVED;
        if (rec->files > rec->own) snprintf(v->title, sizeof v->title, "%s's room uses %d mod%s (%s to download)", host_name, shown, shown == 1 ? "" : "s", total);
        else snprintf(v->title, sizeof v->title, "%s's room uses %d mod%s", host_name, shown, shown == 1 ? "" : "s");
        for (int i = 0; i < rec->n; i++) {
            const RecMod *s = &rec->mods[i];
            char w[160], sz[32];
            who(s, w, sizeof w);
            size_text(s->bytes - s->own_bytes, sz, sizeof sz);
            const char *play = s->gameplay ? " - changes how the game plays" : "";
            switch (s->state) {
            case RS_NEW: line(v, s->gameplay ? '!' : '+', "%s - new (%s)%s", w, sz, play); break;
            case RS_CHANGED: line(v, s->gameplay ? '!' : '~', "%s - changed (%s)%s", w, sz, play); break;
            case RS_MOVED: line(v, '>', "%s - moved from color %d, nothing to download", w, s->from_color + 1); break;
            case RS_SAME: line(v, '=', "%s - you have it%s", w, play); break;
            case RS_REMOVED: line(v, '-', "%s - the host no longer uses it", w); break;
            case RS_UNAVAILABLE: line(v, '!', "%s - changes how the game plays; it cannot be had here", w); break;
            case RS_DROPPED: line(v, 'x', "%s - refused: %s", w, man.mods[s->mani].why); break;
            }
        }
        if (rec->gameplay_missing)
            line(v, '!', "The room uses %d mod%s that change%s how the game plays and that you cannot get: the match may desync", rec->gameplay_missing,
                 rec->gameplay_missing == 1 ? "" : "s", rec->gameplay_missing == 1 ? "s" : "");
        if (rec->own) line(v, 'i', "%d file%s copied from your own mods", rec->own, rec->own == 1 ? "" : "s");
        if (rec->files > rec->own) {
            if (rec->origin) line(v, 'i', "Some come from where they are published: that site sees your IP address. The rest from the host");
            else if (!reachable && lobby) line(v, '!', "No direct connection with the host: these cannot be downloaded");
            else line(v, 'i', "Downloaded from the host%s", lobby ? ", while you play; then the game starts again with them" : "");
        }
        if (note[0]) line(v, note[0] == '!' ? '!' : 'i', "%s", note + (note[0] == '!'));
        v->mode_row = 1;
        snprintf(v->mode, sizeof v->mode, "%s", mode == ROOM_OVERLAY ? "Overlay" : "Mirror");
        snprintf(v->mode_text, sizeof v->mode_text, "%s", mode == ROOM_OVERLAY ? "You play with the host's mods, but your skins win over theirs."
                                                                               : "You play and see exactly what the host has.");
        v->nbuttons = 3;
        snprintf(v->buttons[0], sizeof v->buttons[0], rec->files ? "Download and apply" : "Apply");
        snprintf(v->buttons[1], sizeof v->buttons[1], "Play without them");
        snprintf(v->buttons[2], sizeof v->buttons[2], "Menu");
        v->sel = sel;
    } else if (stage == RM_FETCHING) {
        FetchStatus st;
        mod_fetch_status(&st);
        v->show = lobby ? 2 : 1;   /* (the lobby: a line over the game, not a panel) */
        snprintf(v->title, sizeof v->title, "Downloading %s's room mods", host_name);
        char done[32], total[32];
        size_text(st.done, done, sizeof done);
        size_text(st.total, total, sizeof total);
        v->progress = st.total ? (int)(st.done * 1000 / st.total) : 0;
        if (st.paused) snprintf(v->status, sizeof v->status, "%s of %s - the host is in a fight: it goes on after it", done, total);
        else snprintf(v->status, sizeof v->status, "%s of %s, file %d of %d%s", done, total, st.files_done + 1 > st.files ? st.files : st.files_done + 1,
                      st.files, st.failed ? " (some failed)" : "");
        if (!lobby) line(v, 'i', "Back cancels (what has come is kept); Select+Start held goes back to the menu");
    } else if (stage == RM_AFTER) {
        v->show = 1;
        if (after_kind == AFTER_FAILED) {
            snprintf(v->title, sizeof v->title, "Some of %s's mods could not be used", host_name);
            for (int i = 0; i < failed_mods; i++) line(v, 'x', "%s", failed[i]);
            v->nbuttons = 3;
            snprintf(v->buttons[0], sizeof v->buttons[0], "Play with the rest");
            snprintf(v->buttons[1], sizeof v->buttons[1], "Play without them");
            snprintf(v->buttons[2], sizeof v->buttons[2], "Menu");
        } else {
            snprintf(v->title, sizeof v->title, after_kind == AFTER_READY ? "%s's room mods are here" : "%s's room mods are downloaded", host_name);
            for (int i = 0; i < failed_mods; i++) line(v, 'x', "%s", failed[i]);
            line(v, 'i', "They are used when the game starts again: you come back to the Network page, then join the room again");
            if (note[0] && after_kind == AFTER_READY) line(v, 'i', "%s", note);
            v->nbuttons = 2;
            snprintf(v->buttons[0], sizeof v->buttons[0], "Restart and rejoin");
            snprintf(v->buttons[1], sizeof v->buttons[1], "Later");
        }
        v->sel = sel < 0 ? 0 : sel;
    }
    mod_unlock(&lock);
}

const char *mod_room_note(void) {
    static char out[240];
    mod_lock(&lock);
    snprintf(out, sizeof out, "%s", note);
    note[0] = 0;
    mod_unlock(&lock);
    return out;
}
