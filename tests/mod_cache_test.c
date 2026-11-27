/* runtime/mod_cache.c without a game: tests/test_mod_cache.py builds this with it.
 *   mod_cache_test reconcile <cache> <manifest> [own mods]   what the manifest is against what is kept: a
 *                                                            line a mod "rec <state> <name> <files> <bytes> <own>
 *                                                            <from> <to>", then "total <files> <bytes> <own> <dialog>
 *                                                            <gameplay missing>"
 *   mod_cache_test accept <cache> <manifest> <now> <store> [own mods]   as if accepted: every file fetched from
 *                                                            store (files named by their SHA-256; the player's own
 *                                                            copied), then the index written
 *   mod_cache_test load <cache> <host id> mirror|overlay [own mods]   what the game would load: "load <dest> <mod>"
 *   mod_cache_test tidy <cache> <now>                        the start's tidying
 *   mod_cache_test hosts <cache>                             "host <id> <name> <last> <mods> <active> <bytes>"
 *   mod_cache_test delete <cache> <host id> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod_arc.h"
#include "mod_cache.h"
#include "mod_sha256.h"

const char *rt_data_dir = ".";

static int read_manifest(const char *file, Manifest *m) {
    FILE *f = fopen(file, "rb");
    if (!f) return 0;
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    char err[160];
    if (!mani_parse(m, buf, n, 0, err, sizeof err)) { printf("refused %s\n", err); return 0; }
    return 1;
}

static ModList own_list;
static OwnFiles own_files(const char *dir) {
    OwnFiles o = {NULL, 0};
    if (!dir) return o;
    mod_scan(&own_list, dir);
    o.n = mani_hash_all(&own_list, NULL, &o.files);
    return o;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const char *cmd = argv[1], *cache = argv[2];
    char blobs[1100];
    mod_cache_blobs(cache, blobs, sizeof blobs);
    if (!strcmp(cmd, "reconcile") || !strcmp(cmd, "accept")) {
        Manifest m;
        if (!read_manifest(argv[3], &m)) return 0;
        int accept = !strcmp(cmd, "accept");
        const char *own_dir = argc > (accept ? 6 : 4) ? argv[accept ? 6 : 4] : NULL;
        OwnFiles own = own_files(own_dir);
        HostIndex *h = malloc(sizeof *h);
        host_index_read(cache, m.host_id, h);
        static Reconcile r;
        mod_reconcile(&m, h, blobs, &own, &r);
        for (int i = 0; i < r.n; i++)
            printf("rec %d %s|%d %llu %d %d %d\n", r.mods[i].state, r.mods[i].name, r.mods[i].files, (unsigned long long)r.mods[i].bytes,
                   r.mods[i].own, r.mods[i].from_color, r.mods[i].to_color);
        printf("total %d %llu %d %d %d\n", r.files, (unsigned long long)r.bytes, r.own, r.dialog, r.gameplay_missing);
        if (accept) {
            FetchFile *q;
            FetchOrigin *o;
            int n, no;
            mod_fetch_list(&m, &r, blobs, &own, &q, &n, &o, &no);
            for (int i = 0; i < n; i++) {
                char hex[65], src[1100];
                sha256_hex(q[i].sha, hex);
                snprintf(src, sizeof src, "%s/%s", argv[5], hex);
                int ok = q[i].own[0] ? mod_blob_copy(blobs, q[i].sha, q[i].size, q[i].own) : mod_blob_copy(blobs, q[i].sha, q[i].size, src);
                printf("fetch %s %s %d\n", hex, q[i].own[0] ? "own" : "host", ok);
            }
            host_index_update(h, &m, blobs, atoll(argv[4]), 0x77);
            printf("written %d\n", host_index_write(cache, h));
            printf("views %d\n", mod_cache_views(cache, h, blobs, NULL, 0));
            free(q);
            free(o);
        }
        host_index_free(h);
        free(h);
        mani_free(&m);
        return 0;
    }
    if (!strcmp(cmd, "load") && argc >= 5) {
        HostIndex *h = malloc(sizeof *h);
        if (!host_index_read(cache, strtoull(argv[3], NULL, 16), h)) { printf("none\n"); return 0; }
        ModList *own = NULL;
        if (argc > 5) { own = &own_list; mod_scan(own, argv[5]); }
        ModArcFile *files;
        int n = mod_room_load_list(own, h, cache, !strcmp(argv[4], "overlay") ? ROOM_OVERLAY : ROOM_MIRROR, &files);
        for (int i = 0; i < n; i++) printf("load %s %s%s%s\n", files[i].dest, files[i].mod, files[i].dir ? " @" : "", files[i].dir ? files[i].dir : "");
        mod_load_free(files, n);
        return 0;
    }
    if (!strcmp(cmd, "tidy") && argc >= 4) { mod_cache_tidy(cache, atoll(argv[3]), HOST_DAYS); return 0; }
    if (!strcmp(cmd, "delete") && argc >= 4) { printf("deleted %d\n", mod_cache_delete_host(cache, strtoull(argv[3], NULL, 16))); return 0; }
    if (!strcmp(cmd, "hosts")) {
        HostSummary s[64];
        int n = mod_cache_hosts(cache, s, 64);
        for (int i = 0; i < n; i++)
            printf("host %016llx %s %lld %d %d %llu\n", (unsigned long long)s[i].host_id, s[i].name, s[i].last, s[i].mods, s[i].active,
                   (unsigned long long)s[i].bytes);
        return 0;
    }
    return 2;
}
