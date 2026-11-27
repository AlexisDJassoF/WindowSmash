/* runtime/mod_manifest.c without a game: tests/test_mod_manifest.py builds this with it.
 *   mod_manifest_test build <mods> <out> [lobby] [off]   the host's manifest of the mods folder, written to out
 *                                                         (lobby: an online lobby's room; off: share_mods = off;
 *                                                         HASH_CACHE: the hashes' file) and printed
 *   mod_manifest_test parse <file> [lobby]               a manifest read and printed, or why it was refused
 *   mod_manifest_test path <path>...                     1 or 0 for each: mani_path_ok
 * Printed: "host id name lobby mods files bytes", then a line a mod ("mod id flags name|version|fighter
 * to files bytes why") and a line a file ("file path size sha256"). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod_manifest.h"
#include "mod_sha256.h"

const char *rt_data_dir = ".";

static void print(const Manifest *m) {
    printf("host %016llx %s %d %d %d %llu\n", (unsigned long long)m->host_id, m->host, m->lobby, m->nmods, m->nfiles, (unsigned long long)m->bytes);
    for (int i = 0; i < m->nmods; i++) {
        const ManiMod *x = &m->mods[i];
        char to[64] = "";
        for (int c = 0; c < MOD_COLORS; c++) snprintf(to + strlen(to), sizeof to - strlen(to), "%s%d", c ? "," : "", x->to[c]);
        printf("mod\t%016llx\t%d\t%s|%s|%s\t%s\t%d\t%llu\t%s%s%s\n", (unsigned long long)x->id, x->flags, x->name, x->version, x->fighter, to,
               x->flags & 2 ? x->nfiles : 0, (unsigned long long)x->bytes, x->why[0] ? "dropped:" : "kept", x->why, x->flags & 4 ? " url" : "");
        if (x->flags & 2)
            for (int k = 0; k < x->nfiles; k++) {
                char hex[65];
                sha256_hex(m->files[x->first + k].sha, hex);
                printf("file\t%s\t%u\t%s\n", m->files[x->first + k].path, m->files[x->first + k].size, hex);
            }
    }
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "path")) {
        for (int i = 2; i < argc; i++) printf("%d\n", mani_path_ok(argv[i]));
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "build")) {
        static ModList l;
        mod_scan(&l, argv[2]);
        int lobby = 0, share = 1;
        for (int i = 4; i < argc; i++) lobby |= !strcmp(argv[i], "lobby"), share &= strcmp(argv[i], "off") != 0;
        ManiOptions o = {0x1122334455667788ull, "Alexis", lobby, share, getenv("HASH_CACHE")};
        Manifest m;
        if (!mani_build(&m, &l, &o)) return 1;
        uint8_t *b;
        size_t n;
        if (!mani_encode(&m, &b, &n)) return 1;
        FILE *f = fopen(argv[3], "wb");
        if (!f || fwrite(b, 1, n, f) != n) return 1;
        fclose(f);
        print(&m);
        printf("hash %016llx\n", (unsigned long long)mani_hash(b, n));
        free(b);
        mani_free(&m);
        mod_list_free(&l);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "parse")) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) return 1;
        static uint8_t buf[1 << 20];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        Manifest m;
        char err[160];
        if (!mani_parse(&m, buf, n, argc > 3 && !strcmp(argv[3], "lobby"), err, sizeof err)) { printf("refused %s\n", err); return 0; }
        print(&m);
        mani_free(&m);
        return 0;
    }
    fprintf(stderr, "usage: see the top of this file\n");
    return 2;
}
