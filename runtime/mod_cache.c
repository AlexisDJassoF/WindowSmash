/* The rooms' mods kept here: see mod_cache.h.
 *
 * hosts/<host_id>/host.toml:
 *   host_id = "9f3a0000000000c1"
 *   name = "Alexis"
 *   last = 1732600000
 *   manifest = "0011223344556677"
 *
 *   [[mod]]
 *   id = "..."
 *   name = "Pikachu Ice"
 *   version = "1.0"
 *   fighter = "pikachu"
 *   gameplay = false
 *   active = true
 *   to = [-1, -1, -1, -1, 6, -1, -1, -1]
 *   file = "<sha256> <size> fighter/pikachu/model/body/c04/model.numdlb"
 *   ... */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _CRT_SECURE_NO_WARNINGS
#include "mod_cache.h"
#include "mod_arc.h"
#include "mod_sha256.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <dirent.h>
#include <unistd.h>
#define make_dir(p) mkdir(p, 0755)
#else
#include <direct.h>
#define make_dir(p) _mkdir(p)
#endif

const char *mod_cache_dir(const char *mods_root, char *out, size_t n) {
    char base[1024];   /* mods/'s folder: mods_join/ goes beside it */
    snprintf(base, sizeof base, "%s", mods_root);
    size_t l = strlen(base);
    while (l > 1 && base[l - 1] == '/') base[--l] = 0;
    char *slash = strrchr(base, '/');
    if (slash) *slash = 0; else snprintf(base, sizeof base, ".");
    snprintf(out, n, "%s/mods_join", base);
    struct stat st;
    if (stat(out, &st)) {   /* (an earlier build kept them in mods/.netplay: moved here) */
        char old[1100];
        snprintf(old, sizeof old, "%s/.netplay", mods_root);
        if (!stat(old, &st) && !rename(old, out)) fprintf(stderr, "[mods] the rooms' mods moved from %s to %s\n", old, out);
    }
    make_dir(out);
    char readme[1100];
    snprintf(readme, sizeof readme, "%s/README.txt", out);
    if (stat(readme, &st)) {
        FILE *f = fopen(readme, "wb");
        if (f) {
            fprintf(f, "The mods of the rooms you joined (NETPLAY.md, \"Mods in a room\"): a folder for each host, and in it a folder\n"
                       "for each of its mods, its files where the game loads them (moved to the colors the host plays them in).\n"
                       "They are used only in that host's room; your own mods, in mods/, are never touched.\n"
                       "Changing files here changes nothing: .hosts/ says what each host has and .blobs/ holds the files,\n"
                       "each once. The Mods page deletes a host; one not joined for %d days goes by itself.\n", HOST_DAYS);
            fclose(f);
        }
    }
    return out;
}

const char *mod_cache_blobs(const char *cache, char *out, size_t n) {
    snprintf(out, n, "%s/.blobs", cache);
    make_dir(out);
    return out;
}

static void host_dir(const char *cache, uint64_t id, char *out, size_t n) { snprintf(out, n, "%s/.hosts/%016llx", cache, (unsigned long long)id); }

void host_index_free(HostIndex *h) {
    free(h->files);
    memset(h, 0, sizeof *h);
}

/* ---- host.toml ------------------------------------------------------------------------------------ */
static void put_string(FILE *f, const char *key, const char *v) {
    fprintf(f, "%s = \"", key);
    for (; *v; v++) {
        if (*v == '"' || *v == '\\') fputc('\\', f);
        fputc((unsigned char)*v < 32 ? '?' : *v, f);
    }
    fprintf(f, "\"\n");
}

static int get_string(const char *v, char *out, size_t n) {   /* "..." with \" and \\: 1 */
    while (*v == ' ') v++;
    if (*v++ != '"') return 0;
    size_t o = 0;
    for (; *v && *v != '"'; v++) {
        if (*v == '\\' && v[1]) v++;
        if (o + 1 < n) out[o++] = *v;
    }
    out[o] = 0;
    return *v == '"';
}

int host_index_write(const char *cache, const HostIndex *h) {
    char dir[1100], file[1200], tmp[1210];
    snprintf(dir, sizeof dir, "%s/.hosts", cache);
    make_dir(dir);
    host_dir(cache, h->host_id, dir, sizeof dir);
    make_dir(dir);
    snprintf(file, sizeof file, "%s/host.toml", dir);
    snprintf(tmp, sizeof tmp, "%s.new", file);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    fprintf(f, "# WindowSmash: the mods kept from a host's room (mods_join/). The Mods page deletes it; it goes by itself\n"
               "# %d days after the host was last joined.\n", HOST_DAYS);
    fprintf(f, "host_id = \"%016llx\"\n", (unsigned long long)h->host_id);
    put_string(f, "name", h->name);
    fprintf(f, "last = %lld\nmanifest = \"%016llx\"\n", h->last, (unsigned long long)h->manifest);
    for (int i = 0; i < h->nmods; i++) {
        const HostMod *m = &h->mods[i];
        fprintf(f, "\n[[mod]]\nid = \"%016llx\"\n", (unsigned long long)m->id);
        put_string(f, "name", m->name);
        put_string(f, "version", m->version);
        put_string(f, "fighter", m->fighter);
        fprintf(f, "gameplay = %s\nactive = %s\nto = [", m->gameplay ? "true" : "false", m->active ? "true" : "false");
        for (int c = 0; c < MOD_COLORS; c++) fprintf(f, "%s%d", c ? ", " : "", m->to[c]);
        fprintf(f, "]\n");
        for (int k = 0; k < m->nfiles; k++) {
            const HostFile *x = &h->files[m->first + k];
            char hex[65];
            sha256_hex(x->sha, hex);
            fprintf(f, "file = \"%s %u %s\"\n", hex, x->size, x->path);
        }
    }
    int ok = !ferror(f);
    ok &= fclose(f) == 0;
    remove(file);   /* (Windows: rename does not replace) */
    if (!ok || rename(tmp, file)) { remove(tmp); return 0; }
    return 1;
}

static int add_file(HostIndex *h, int *cap, const HostFile *x) {
    if (h->nfiles == *cap) {
        HostFile *f = realloc(h->files, sizeof *f * (size_t)(*cap = *cap ? *cap * 2 : 256));
        if (!f) return 0;
        h->files = f;
    }
    h->files[h->nfiles++] = *x;
    return 1;
}

int host_index_read(const char *cache, uint64_t host_id, HostIndex *h) {
    memset(h, 0, sizeof *h);
    h->host_id = host_id;
    char file[1200];
    host_dir(cache, host_id, file, sizeof file);
    snprintf(file + strlen(file), sizeof file - strlen(file), "/host.toml");
    FILE *f = fopen(file, "rb");
    if (!f) return 0;
    char line[1400];
    int cap = 0;
    HostMod *m = NULL;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "[[mod]]")) {
            if (h->nmods == HOST_MODS) { m = NULL; continue; }
            m = &h->mods[h->nmods++];
            memset(m, 0, sizeof *m);
            memset(m->to, -1, sizeof m->to);
            m->first = h->nfiles;
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *key = line, *v = eq + 1;
        size_t kl = strlen(key);
        while (kl && key[kl - 1] == ' ') key[--kl] = 0;
        while (*v == ' ') v++;
        char text[1200];
        if (!m) {
            if (!strcmp(key, "name")) get_string(v, h->name, sizeof h->name);
            else if (!strcmp(key, "last")) h->last = atoll(v);
            else if (!strcmp(key, "manifest") && get_string(v, text, sizeof text)) h->manifest = strtoull(text, NULL, 16);
            continue;
        }
        if (!strcmp(key, "id") && get_string(v, text, sizeof text)) m->id = strtoull(text, NULL, 16);
        else if (!strcmp(key, "name")) get_string(v, m->name, sizeof m->name);
        else if (!strcmp(key, "version")) get_string(v, m->version, sizeof m->version);
        else if (!strcmp(key, "fighter")) get_string(v, m->fighter, sizeof m->fighter);
        else if (!strcmp(key, "gameplay")) m->gameplay = !strncmp(v, "true", 4);
        else if (!strcmp(key, "active")) m->active = !strncmp(v, "true", 4);
        else if (!strcmp(key, "to") && *v == '[') {
            int c = 0;
            for (char *p = v + 1; *p && c < MOD_COLORS;) {
                long t = strtol(p, &p, 10);
                m->to[c++] = (signed char)(t >= -1 && t < MOD_COLORS ? t : -1);
                while (*p == ',' || *p == ' ') p++;
                if (*p == ']') break;
            }
        } else if (!strcmp(key, "file") && get_string(v, text, sizeof text)) {   /* "<sha> <size> <path>" */
            HostFile x;
            memset(&x, 0, sizeof x);
            char *sp = strchr(text, ' '), *sp2 = sp ? strchr(sp + 1, ' ') : NULL;
            if (!sp || !sp2 || !sha256_parse(text, x.sha)) continue;
            x.size = (uint32_t)strtoul(sp + 1, NULL, 10);
            snprintf(x.path, sizeof x.path, "%s", sp2 + 1);
            if (!mani_path_ok(x.path) || !x.size || x.size > MANI_FILE_MAX) continue;   /* (the file was edited by hand: only what a manifest may have) */
            if (add_file(h, &cap, &x)) m->nfiles++;
        }
    }
    fclose(f);
    return 1;
}

/* ---- the hosts kept -------------------------------------------------------------------------------- */
#ifndef _WIN32
static int remove_tree(const char *path, int depth) {
    if (depth > 24) return 0;
    DIR *d = opendir(path);
    if (!d) return remove(path) == 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[1400];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        struct stat st;
        if (!lstat(sub, &st) && S_ISDIR(st.st_mode)) remove_tree(sub, depth + 1);
        else remove(sub);
    }
    closedir(d);
    return rmdir(path) == 0;
}

/* ---- the folders to look at: mods_join/<host>/<mod>/<its files where they load> ---- */
static void make_dirs(char *path) {   /* (every folder of path but its last part) */
    for (char *p = path + 1; *p; p++)
        if (*p == '/') { *p = 0; make_dir(path); *p = '/'; }
}

static void view_name(const char *s, char *out, size_t n) {   /* a name as a folder's: no '/', no leading '.' */
    size_t o = 0;
    for (; *s && o + 1 < n; s++) out[o++] = *s == '/' || *s == '\\' || (unsigned char)*s < 32 || *s == ':' || *s == '*' || *s == '?' ? '_' : *s;
    out[o] = 0;
    while (o && (out[o - 1] == ' ' || out[o - 1] == '.')) out[--o] = 0;
    if (!o) snprintf(out, n, "_");
    if (out[0] == '.') out[0] = '_';
}

static uint64_t view_host(const char *dir) {   /* whose folder it is (its .host_id), 0 none */
    char p[1400], line[64] = "";
    snprintf(p, sizeof p, "%s/.host_id", dir);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    return strtoull(line, NULL, 16);
}

static void remove_view(const char *cache, uint64_t id) {
    DIR *d = opendir(cache);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", cache, e->d_name);
        if (view_host(p) == id) remove_tree(p, 0);
    }
    if (d) closedir(d);
}

static void toml_string(FILE *f, const char *v) {
    fputc('"', f);
    for (; *v; v++) { if (*v == '"' || *v == '\\') fputc('\\', f); fputc((unsigned char)*v < 32 ? '?' : *v, f); }
    fputc('"', f);
}

int mod_cache_views(const char *cache, const HostIndex *h, const char *blobs, char *dir_out, size_t dn) {
    remove_view(cache, h->host_id);
    char name[MANI_NAME + 1], dir[1100];
    view_name(h->name[0] ? h->name : "host", name, sizeof name);
    snprintf(dir, sizeof dir, "%s/%s", cache, name);
    struct stat st;
    if (!stat(dir, &st)) snprintf(dir, sizeof dir, "%s/%s (%016llx)", cache, name, (unsigned long long)h->host_id);   /* (another host by that name) */
    make_dir(dir);
    if (dir_out) snprintf(dir_out, dn, "%s", dir);
    char p[1400];
    snprintf(p, sizeof p, "%s/.host_id", dir);
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    fprintf(f, "%016llx\n", (unsigned long long)h->host_id);
    fclose(f);
    /* the room's configuration: each mod's colors as the host has them (mod_scan.c reads it as mods/'s) */
    snprintf(p, sizeof p, "%s/windowsmash.toml", dir);
    FILE *slots = fopen(p, "wb");
    if (slots) fprintf(slots, "# %s's room: the colors the host plays its mods in (made again each time you join).\n", h->name);
    int made = 0;
    for (int i = 0; i < h->nmods; i++) {
        const HostMod *m = &h->mods[i];
        if (!m->active) continue;
        char mod[100], at[1200];
        view_name(m->name, mod, sizeof mod);
        snprintf(at, sizeof at, "%s/%s", dir, mod);
        for (int k = 2; !stat(at, &st) && k < 100; k++) snprintf(at, sizeof at, "%s/%s %d", dir, mod, k);
        make_dir(at);
        const char *key = strrchr(at, '/') + 1;
        for (int k = 0; k < m->nfiles; k++) {   /* its files where the mod has them: a normal mod folder */
            const HostFile *x = &h->files[m->first + k];
            char file[3300], blob[1100];
            snprintf(file, sizeof file, "%s/%s", at, x->path);
            mod_blob_path(blobs, x->sha, blob, sizeof blob);
            make_dirs(file);
            remove(file);
            if (!link(blob, file) || mod_copy_file(blob, file)) made++;   /* (a hard link: the file once on disk) */
        }
        snprintf(p, sizeof p, "%s/info.toml", at);
        FILE *info = fopen(p, "wb");
        if (info) {
            fprintf(info, "display_name = ");
            toml_string(info, m->name);
            fprintf(info, "\nversion = ");
            toml_string(info, m->version);
            fprintf(info, "\ndescription = ");
            char d[160];
            snprintf(d, sizeof d, "From %s's room", h->name);
            toml_string(info, d);
            fprintf(info, "\n");
            fclose(info);
        }
        if (slots && m->fighter[0]) {   /* (every color named: no folder name's -c0Y moves it otherwise) */
            fprintf(slots, "\n[mod.");
            toml_string(slots, key);
            fprintf(slots, "]\n");
            for (int c = 0; c < MOD_COLORS; c++) fprintf(slots, "%s.c%02d = \"c%02d\"\n", m->fighter, c, m->to[c] >= 0 ? m->to[c] : c);
        }
    }
    if (slots) fclose(slots);
    return made;
}

int mod_cache_delete_host(const char *cache, uint64_t host_id) {
    char dir[1100];
    remove_view(cache, host_id);
    host_dir(cache, host_id, dir, sizeof dir);
    int ok = remove_tree(dir, 0);
    if (ok) fprintf(stderr, "[mods] the mods kept from host %016llx deleted\n", (unsigned long long)host_id);
    return ok;
}

static int by_last(const void *a, const void *b) {
    long long x = ((const HostSummary *)a)->last, y = ((const HostSummary *)b)->last;
    return x < y ? 1 : x > y ? -1 : 0;
}

int mod_cache_hosts(const char *cache, HostSummary *out, int most) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/.hosts", cache);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < most) {
        char *end = NULL;
        unsigned long long id = strtoull(e->d_name, &end, 16);
        if (!id || !end || *end || strlen(e->d_name) != 16) continue;
        HostIndex *h = malloc(sizeof *h);
        if (!h) break;
        if (host_index_read(cache, id, h)) {
            HostSummary *s = &out[n++];
            memset(s, 0, sizeof *s);
            s->host_id = id;
            snprintf(s->name, sizeof s->name, "%s", h->name[0] ? h->name : "?");
            s->last = h->last;
            s->mods = h->nmods;
            for (int i = 0; i < h->nmods; i++) s->active += h->mods[i].active;
            for (int i = 0; i < h->nfiles; i++) s->bytes += h->files[i].size;
        }
        host_index_free(h);
        free(h);
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, by_last);
    return n;
}

typedef struct ShaSet { uint8_t (*v)[32]; size_t n, cap; } ShaSet;
static int by_sha(const void *a, const void *b) { return memcmp(a, b, 32); }

void mod_cache_tidy(const char *cache, long long now, int days) {
    char blobs[1100], dir[1100];
    snprintf(blobs, sizeof blobs, "%s/.blobs", cache);
    snprintf(dir, sizeof dir, "%s/.hosts", cache);
    ShaSet used = {0};
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {   /* the hosts: forgotten after `days`, else their files kept */
        char *end = NULL;
        unsigned long long id = strtoull(e->d_name, &end, 16);
        if (!id || !end || *end) continue;
        HostIndex *h = malloc(sizeof *h);
        if (!h) break;
        int read = host_index_read(cache, id, h);
        if (!read || now - h->last > (long long)days * 86400) {
            fprintf(stderr, "[mods] %s's room mods: %s; forgotten\n", h->name[0] ? h->name : e->d_name,
                    read ? "not joined for a month" : "no index");
            char sub[1400];
            snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
            remove_tree(sub, 0);
            remove_view(cache, id);
        } else
            for (int i = 0; i < h->nfiles; i++) {
                if (used.n == used.cap) {
                    void *v = realloc(used.v, 32 * (used.cap = used.cap ? used.cap * 2 : 1024));
                    if (!v) break;
                    used.v = v;
                }
                memcpy(used.v[used.n++], h->files[i].sha, 32);
            }
        host_index_free(h);
        free(h);
    }
    if (d) closedir(d);
    if (used.n) qsort(used.v, used.n, 32, by_sha);
    d = opendir(blobs);
    int gone = 0;
    while (d && (e = readdir(d))) {   /* the blobs: a .part is an unfinished one; the rest kept while a host uses it */
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", blobs, e->d_name);
        size_t l = strlen(e->d_name);
        if (l > 5 && !strcmp(e->d_name + l - 5, ".part")) { remove(path); continue; }
        uint8_t sha[32];
        if (l != 64 || !sha256_parse(e->d_name, sha)) continue;
        if (used.n && bsearch(sha, used.v, used.n, 32, by_sha)) continue;
        struct stat st;
        if (!stat(path, &st) && now - (long long)st.st_mtime < 86400) continue;   /* (a cancelled download's, kept a day for the next try) */
        if (!remove(path)) gone++;
    }
    if (d) closedir(d);
    if (gone) fprintf(stderr, "[mods] %d file%s of the rooms' mods no host uses any more deleted\n", gone, gone == 1 ? "" : "s");
    free(used.v);
}
#else
int mod_cache_delete_host(const char *cache, uint64_t host_id) { (void)cache; (void)host_id; return 0; }
int mod_cache_views(const char *cache, const HostIndex *h, const char *blobs, char *dir_out, size_t dn) { (void)cache; (void)h; (void)blobs; if (dir_out && dn) dir_out[0] = 0; return 0; }
int mod_cache_hosts(const char *cache, HostSummary *out, int most) { (void)cache; (void)out; (void)most; return 0; }
void mod_cache_tidy(const char *cache, long long now, int days) { (void)cache; (void)now; (void)days; }
#endif

/* ---- reconciling ------------------------------------------------------------------------------------ */
static const ManiFile *own_file(const OwnFiles *own, const uint8_t sha[32]) {
    if (!own) return NULL;
    for (int i = 0; i < own->n; i++) if (!memcmp(own->files[i].sha, sha, 32)) return &own->files[i];
    return NULL;
}

static int first_color(const signed char to[MOD_COLORS], unsigned colors, int moved) {
    for (int c = 0; c < MOD_COLORS; c++) if (colors & 1u << c) return moved && to[c] >= 0 ? to[c] : c;
    return -1;
}

/* the colors a mod's files are in, of its fighter */
static unsigned mod_colors(const ManiFile *files, int n, const char *fighter) {
    unsigned colors = 0;
    for (int k = 0; k < n; k++) {
        char f[64];
        int c;
        if (mod_path_color(files[k].path, f, sizeof f, &c) && !strcmp(f, fighter) && c < MOD_COLORS) colors |= 1u << c;
    }
    return colors;
}

static int same_files(const HostIndex *h, const HostMod *o, const Manifest *m, const ManiMod *x) {
    if (o->nfiles != x->nfiles) return 0;
    for (int k = 0; k < x->nfiles; k++) {
        const ManiFile *f = &m->files[x->first + k];
        int found = 0;
        for (int j = 0; j < o->nfiles && !found; j++)
            found = !strcmp(h->files[o->first + j].path, f->path) && !memcmp(h->files[o->first + j].sha, f->sha, 32);
        if (!found) return 0;
    }
    return 1;
}

void mod_reconcile(const Manifest *m, const HostIndex *old, const char *blobs, const OwnFiles *own, Reconcile *r) {
    memset(r, 0, sizeof *r);
    char *matched = calloc((size_t)(old->nmods ? old->nmods : 1), 1);
    for (int i = 0; i < m->nmods && r->n < (int)(sizeof r->mods / sizeof r->mods[0]); i++) {
        const ManiMod *x = &m->mods[i];
        RecMod *s = &r->mods[r->n++];
        memset(s, 0, sizeof *s);
        s->mani = i, s->old = -1;
        snprintf(s->name, sizeof s->name, "%s", x->name);
        snprintf(s->fighter, sizeof s->fighter, "%s", x->fighter);
        s->gameplay = (x->flags & MM_GAMEPLAY) != 0;
        if (x->why[0]) { s->state = RS_DROPPED; r->dropped++; r->gameplay_missing += s->gameplay; continue; }
        if (!(x->flags & MM_FILES)) {   /* (told by its name alone: a gameplay mod this player cannot have here) */
            s->state = RS_UNAVAILABLE;
            r->gameplay_missing += s->gameplay;
            continue;
        }
        /* the one kept: the same id (the same files), else the same name (changed) */
        for (int j = 0; j < old->nmods && s->old < 0; j++) if (!matched || !matched[j]) if (old->mods[j].id == x->id) s->old = j;
        for (int j = 0; j < old->nmods && s->old < 0; j++)
            if ((!matched || !matched[j]) && !strcmp(old->mods[j].name, x->name)) s->old = j;
        if (s->old >= 0 && matched) matched[s->old] = 1;
        const HostMod *o = s->old >= 0 ? &old->mods[s->old] : NULL;
        if (o && o->id == x->id && same_files(old, o, m, x)) {
            unsigned colors = mod_colors(m->files + x->first, x->nfiles, x->fighter);
            s->state = memcmp(o->to, x->to, MOD_COLORS) ? RS_MOVED : RS_SAME;
            s->from_color = first_color(o->to, colors, 1);
            s->to_color = first_color(x->to, colors, 1);
        } else s->state = o ? RS_CHANGED : RS_NEW;
        for (int k = 0; k < x->nfiles; k++) {   /* what is not here: from the player's own mods, or fetched */
            const ManiFile *f = &m->files[x->first + k];
            if (mod_blob_has(blobs, f->sha, f->size)) continue;
            int dup = 0;   /* (a file two of the host's mods share: once) */
            for (int j = 0; j < k && !dup; j++) dup = !memcmp(m->files[x->first + j].sha, f->sha, 32);
            if (dup) continue;
            const ManiFile *mine = own_file(own, f->sha);
            s->files++, s->bytes += f->size;
            if (mine) s->own++, s->own_bytes += f->size;
        }
        if (s->state == RS_SAME && s->files) s->state = RS_CHANGED;   /* (kept, but its files went: fetched again) */
        if (s->state == RS_MOVED && s->files) s->state = RS_CHANGED;
        r->files += s->files, r->bytes += s->bytes, r->own += s->own, r->own_bytes += s->own_bytes;
        if (s->files > s->own && (x->flags & MM_URL)) r->origin = 1;
    }
    for (int j = 0; j < old->nmods && r->n < (int)(sizeof r->mods / sizeof r->mods[0]); j++) {
        if ((matched && matched[j]) || !old->mods[j].active) continue;
        RecMod *s = &r->mods[r->n++];
        memset(s, 0, sizeof *s);
        s->state = RS_REMOVED;
        s->mani = -1, s->old = j;
        snprintf(s->name, sizeof s->name, "%s", old->mods[j].name);
        snprintf(s->fighter, sizeof s->fighter, "%s", old->mods[j].fighter);
        s->gameplay = old->mods[j].gameplay;
    }
    free(matched);
    for (int i = 0; i < r->n; i++)
        switch (r->mods[i].state) {
        case RS_SAME: r->same++; break;
        case RS_MOVED: r->moved++; break;
        case RS_CHANGED: r->changed++; break;
        case RS_NEW: r->news++; break;
        case RS_REMOVED: r->removed++; break;
        }
    /* only the same ones, moved or gone: applied without asking (a note says so) */
    r->dialog = r->news || r->changed || r->files || r->gameplay_missing || r->dropped;
}

int mod_fetch_list(const Manifest *m, const Reconcile *r, const char *blobs, const OwnFiles *own, FetchFile **files, int *n,
                   FetchOrigin **origins, int *norigins) {
    *files = malloc(sizeof **files * (size_t)(m->nfiles ? m->nfiles : 1));
    *origins = malloc(sizeof **origins * MANI_MODS);
    *n = *norigins = 0;
    if (!*files || !*origins) { free(*files); free(*origins); *files = NULL; *origins = NULL; return 0; }
    for (int i = 0; i < r->n; i++) {
        const RecMod *s = &r->mods[i];
        if (s->mani < 0 || !s->files) continue;
        const ManiMod *x = &m->mods[s->mani];
        int origin = -1;
        if ((x->flags & MM_URL) && !(x->flags & MM_GAMEPLAY)) {   /* (visual mods only: mani_parse drops a gameplay one's address) */
            FetchOrigin *o = &(*origins)[(*norigins)];
            snprintf(o->url, sizeof o->url, "%s", x->url);
            memcpy(o->sha, x->url_sha, 32);
            origin = (*norigins)++;
        }
        for (int k = 0; k < x->nfiles; k++) {
            const ManiFile *f = &m->files[x->first + k];
            if (mod_blob_has(blobs, f->sha, f->size)) continue;
            int dup = 0;
            for (int j = 0; j < *n && !dup; j++) dup = !memcmp((*files)[j].sha, f->sha, 32);
            if (dup) continue;
            FetchFile *q = &(*files)[(*n)++];
            memset(q, 0, sizeof *q);
            memcpy(q->sha, f->sha, 32);
            q->size = f->size;
            q->origin = origin;
            q->mod = s->mani;
            const ManiFile *mine = own_file(own, f->sha);
            if (mine && mine->src) snprintf(q->own, sizeof q->own, "%s", mine->src);
        }
    }
    return 1;
}

void host_index_update(HostIndex *h, const Manifest *m, const char *blobs, long long now, uint64_t manifest_hash) {
    snprintf(h->name, sizeof h->name, "%s", m->host);
    h->host_id = m->host_id;
    h->last = now;
    h->manifest = manifest_hash;
    HostIndex next;
    memset(&next, 0, sizeof next);
    next.host_id = h->host_id, next.last = h->last, next.manifest = h->manifest;
    memcpy(next.name, h->name, sizeof next.name);
    int cap = 0;
    char *taken = calloc((size_t)(h->nmods ? h->nmods : 1), 1);
    /* the manifest's mods whose files are all here: active, the host's names and colors */
    for (int i = 0; i < m->nmods && next.nmods < HOST_MODS; i++) {
        const ManiMod *x = &m->mods[i];
        if (x->why[0] || !(x->flags & MM_FILES)) continue;
        int all = 1;
        for (int k = 0; k < x->nfiles && all; k++) all = mod_blob_has(blobs, m->files[x->first + k].sha, m->files[x->first + k].size);
        if (!all) continue;
        for (int j = 0; j < h->nmods && taken; j++)   /* (the old one it replaces: not kept beside it) */
            if (!taken[j] && (h->mods[j].id == x->id || !strcmp(h->mods[j].name, x->name))) { taken[j] = 1; break; }
        HostMod *y = &next.mods[next.nmods++];
        memset(y, 0, sizeof *y);
        y->id = x->id;
        snprintf(y->name, sizeof y->name, "%s", x->name);
        snprintf(y->version, sizeof y->version, "%s", x->version);
        snprintf(y->fighter, sizeof y->fighter, "%s", x->fighter);
        y->gameplay = (x->flags & MM_GAMEPLAY) != 0;
        y->active = 1;
        memcpy(y->to, x->to, sizeof y->to);
        y->first = next.nfiles;
        for (int k = 0; k < x->nfiles; k++) {
            HostFile f;
            memset(&f, 0, sizeof f);
            snprintf(f.path, sizeof f.path, "%s", m->files[x->first + k].path);
            memcpy(f.sha, m->files[x->first + k].sha, 32);
            f.size = m->files[x->first + k].size;
            if (add_file(&next, &cap, &f)) y->nfiles++;
        }
    }
    /* the rest of what was kept: there still, unused (the host may bring them back) */
    for (int j = 0; j < h->nmods && next.nmods < HOST_MODS; j++) {
        if (taken && taken[j]) continue;
        const HostMod *o = &h->mods[j];
        HostMod *y = &next.mods[next.nmods++];
        *y = *o;
        y->active = 0;
        y->first = next.nfiles;
        y->nfiles = 0;
        for (int k = 0; k < o->nfiles; k++) if (add_file(&next, &cap, &h->files[o->first + k])) y->nfiles++;
    }
    free(taken);
    host_index_free(h);
    *h = next;
}

/* ---- what the game loads ------------------------------------------------------------------------------- */
static int by_dest(const void *a, const void *b) { return strcmp(((const ModArcFile *)a)->dest, ((const ModArcFile *)b)->dest); }

int mod_room_load_list(const ModList *own, const HostIndex *h, const char *cache, int mode, ModArcFile **out) {
    *out = NULL;
    char blobs[1100], dir[1100];
    mod_cache_blobs(cache, blobs, sizeof blobs);
    mod_cache_views(cache, h, blobs, dir, sizeof dir);   /* (its folders as the index says, made again) */
    ModList *room = calloc(1, sizeof *room);
    if (!room) return 0;
    mod_scan(room, dir);   /* the room's mods: read as mods/'s are - config.json, colors, everything */
    ModArcFile *v = NULL, *r = NULL;
    int n = 0, nr = mod_load_list(room, &r);
    mod_list_free(room);
    free(room);
    if (mode == ROOM_OVERLAY && own) {   /* the player's own skins first: they win where they clash */
        ModList *mine = malloc(sizeof *mine);
        if (mine) {
            memcpy(mine, own, sizeof *mine);   /* (a shallow copy: its paths are own's; only enabled changes) */
            for (int i = 0; i < mine->n; i++) if (mine->mods[i].gameplay) mine->mods[i].enabled = 0;
            n = mod_load_list(mine, &v);
            free(mine);
        }
    }
    if (!n) { *out = r; return nr; }
    ModArcFile *all = realloc(v, sizeof *v * (size_t)(n + nr));
    if (!all) { mod_load_free(r, nr); *out = v; return n; }
    v = all;
    int mine = n;
    for (int i = 0; i < nr; i++) {
        int taken = 0;
        for (int j = 0; j < mine && !taken; j++) taken = !strcmp(v[j].dest, r[i].dest);
        if (taken) { free(r[i].dest); free(r[i].src); free(r[i].mod); free(r[i].dir); continue; }
        v[n++] = r[i];
    }
    free(r);
    qsort(v, (size_t)n, sizeof *v, by_dest);
    *out = v;
    return n;
}

int mod_room_chosen(uint64_t *host_id, int *m) {
    const char *v = getenv("SSBU_ROOM_MODS");
    if (!v || !*v) return 0;
    char *end = NULL;
    unsigned long long id = strtoull(v, &end, 16);
    if (!id || !end || *end != ':') return 0;
    if (host_id) *host_id = id;
    if (m) *m = !strcmp(end + 1, "overlay") ? ROOM_OVERLAY : ROOM_MIRROR;
    return 1;
}
