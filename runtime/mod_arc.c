/* The mods' data.arc: see mod_arc.h. The table is built the way tools/mod_arc_proto.py does it
 * (openspec mod-support, decision 13; its comments say why each step is there):
 *
 * - A directory (DirInfo) loads its children: child k is FileInfo file_info_start_index + k, read
 *   from FileData file_start_index + k of its folder (folder_offsets[DirInfo.path.index]), the
 *   folder read in one pass. A mod's file goes into the child's own FileData, inside its folder,
 *   which is rebuilt past data.arc's end with its files in the same order and the mod's in place.
 * - A child that shares another's bytes (its info-to-data's load type 3: a shared group; 0: another
 *   color's own FileData) is unshared first: it reads its own FileData (an empty placeholder, or
 *   stale bytes nothing reads) through a FileInfoIndex of its own. If it was the shared file's
 *   source, the shared FileInfo keeps a copy of its path no lookup finds.
 * - Colors that read a replaced FileData directly (load type 0) get the original bytes in their own
 *   placeholder (ARCropolis' "reshare").
 * - Every file is a zstd frame with the header of the game's own (no content size, a 2 MiB window),
 *   of raw blocks: the game takes no uncompressed FileData. */
#define _CRT_SECURE_NO_WARNINGS
#include "mod_arc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../third_party/zstd/zstd.h"

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#define RAW_BLOCK (128u * 1024)
#define LOW40 ((1ull << 40) - 1)
#define CACHE_MAGIC "SSBUMODS"
#define CACHE_VERSION 21  /* (raise it when the table this builds changes; 2: every region of a regional file; 3: sound pairs; 4: models needing new files; 6: new FileData; 7: on, not for sounds; 9: voices on, cheers out; 10: params of another version; 11: last file stays last; 12: new files; 13: their flags; 14: their folder's FileData moved; 15: laid out in index order; 16: in the order the game reads them; 17: a slot something reads is not taken; 18: own files where they are; 19: their voices not regional; 20: bank IDs the game's; 21: their BINF the game's) */

int mod_arc_from_cache;

static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 rd64(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }
static void wr32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static void wr64(u8 *p, u64 v) { memcpy(p, &v, 8); }
static u64 align(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

static u64 hash40(const char *s) {   /* crc32 of the lower-case path | its length << 32 */
    static u32 table[256];
    if (!table[1])
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    u32 crc = 0xFFFFFFFFu;
    u64 n = 0;
    for (; s[n]; n++) {
        u8 ch = (u8)s[n];
        if (ch >= 'A' && ch <= 'Z') ch = (u8)(ch + 32);
        crc = table[(crc ^ ch) & 0xFF] ^ (crc >> 8);
    }
    return (u64)(crc ^ 0xFFFFFFFFu) | n << 32;
}

/* ---- zstd frames of raw blocks ---------------------------------------------------------------- */
/* A file's: magic, frame header 0x00 (no content size, not single segment), window 0x58 (2 MiB);
 * the table's: single segment with an 8-byte content size. Then 128 KiB blocks, 3-byte headers. */
static u64 raw_blocks(u64 n) { return n ? (n + RAW_BLOCK - 1) / RAW_BLOCK : 1; }
static u64 file_frame_size(u64 n) { return 6 + 3 * raw_blocks(n) + n; }
static u64 table_frame_size(u64 n) { return 13 + 3 * raw_blocks(n) + n; }

static void put_blocks(u8 *out, const u8 *data, u64 n) {
    u64 nb = raw_blocks(n);
    for (u64 b = 0; b < nb; b++) {
        u64 len = n - b * RAW_BLOCK < RAW_BLOCK ? n - b * RAW_BLOCK : RAW_BLOCK;
        u32 head = (u32)(len << 3) | (b + 1 == nb);
        out[0] = (u8)head, out[1] = (u8)(head >> 8), out[2] = (u8)(head >> 16);
        memcpy(out + 3, data + b * RAW_BLOCK, len);
        out += 3 + len;
    }
}

/* The size of mod file f as served (its own, or that of the bytes served in its place) */
static u64 fsize(const ModArc *a, int f) { return a->over && a->over[f] ? a->over_size[f] : a->files[f].size; }

/* Bytes [at, at + size) of the file frame of file `f` (size n), its block headers made here */
static int read_file_frame(const ModArcIO *io, int f, u64 n, u64 at, u8 *p, u64 size, const u8 *over) {
    static const u8 head[6] = {0x28, 0xB5, 0x2F, 0xFD, 0x00, 0x58};
    u64 nb = raw_blocks(n);
    while (size) {
        u64 got;
        if (at < 6) {
            got = 6 - at < size ? 6 - at : size;
            memcpy(p, head + at, got);
        } else {
            u64 r = at - 6, b = r / (3 + RAW_BLOCK), in = r % (3 + RAW_BLOCK);
            u64 len = n - b * RAW_BLOCK < RAW_BLOCK ? n - b * RAW_BLOCK : RAW_BLOCK;
            if (b >= nb) return 0;
            if (in < 3) {
                u32 h = (u32)(len << 3) | (b + 1 == nb);
                got = 3 - in < size ? 3 - in : size;
                for (u64 i = 0; i < got; i++) p[i] = (u8)(h >> 8 * (in + i));
            } else {
                u64 o = in - 3;
                if (o >= len) return 0;
                got = len - o < size ? len - o : size;
                u64 fo = b * RAW_BLOCK + o;
                if (over) memcpy(p, over + fo, got);
                else if (!io->mod(io->ctx, f, fo, p, got)) return 0;
            }
        }
        p += got, at += got, size -= got;
    }
    return 1;
}

/* ---- the file system table ------------------------------------------------------------------- */
typedef struct { u64 o; u32 size, count; } Tab;
typedef struct Table {
    u8 *t;
    u64 len;
    u32 buckets, regions;
    u64 bucket_table;
    Tab hash_to_path, paths, info_indices, dir_hash, dirs, folder_offsets, infos, i2d, datas;
} Table;
#define E(T, tab, i) ((T)->t + (T)->tab.o + (u64)(i) * (T)->tab.size)

static int parse(Table *T) {   /* tools/arc.py's layout (smash-arc's names) */
    const u8 *t = T->t;
    if (T->len < 0x58) return 0;
    u32 h[11];
    for (int i = 0; i < 11; i++) h[i] = rd32(t + 4 * i);
    u32 regions = t[56], extra_folders = rd32(t + 64), extra = rd32(t + 68), extra_2 = rd32(t + 80);
    u64 o = 0x58 + (u64)regions * 12;
    if (o + 16 > T->len) return 0;
    u32 quick_dirs = rd32(t + o), stream_hashes = rd32(t + o + 4), stream_indices = rd32(t + o + 8), stream_datas = rd32(t + o + 12);
    o += 16 + (u64)quick_dirs * 12;
    o += (u64)stream_hashes * 8 + (u64)stream_hashes * 12 + (u64)stream_indices * 4 + (u64)stream_datas * 16;
    if (o + 8 > T->len) return 0;
    u32 groups = rd32(t + o);
    T->buckets = rd32(t + o + 4);
    T->regions = regions;
    T->bucket_table = o + 8;
    o = T->bucket_table + (u64)T->buckets * 8;
#define TAB(name, n, sz) (T->name.o = o, T->name.size = (sz), T->name.count = (n), o += (u64)(n) * (sz))
    TAB(hash_to_path, groups, 8);
    TAB(paths, h[1], 32);
    TAB(info_indices, h[2], 8);
    TAB(dir_hash, h[3], 8);   /* dir_hash_to_info: hash40 | directory << 40 */
    TAB(dirs, h[3], 0x34);
    TAB(folder_offsets, h[4] + h[9] + extra_folders, 0x1C);
    o += (u64)h[5] * 8;   /* child_hashes */
    TAB(infos, h[6] + h[10] + extra, 16);
    TAB(i2d, h[7] + h[10] + extra_2, 12);
    TAB(datas, h[8] + h[10] + extra, 16);
#undef TAB
    return T->buckets && o <= T->len;
}

static long long path_index(const Table *T, const char *path) {
    u64 h = hash40(path);
    const u8 *b = T->t + T->bucket_table + h % T->buckets * 8;
    u32 start = rd32(b), count = rd32(b + 4);
    for (u64 k = start; k < (u64)start + count && k < T->hash_to_path.count; k++) {
        u64 v = rd64(E(T, hash_to_path, k));
        if ((v & LOW40) == h) return (long long)(v >> 40);
    }
    return -1;
}

static long long dir_index(const Table *T, const char *path) {   /* the directory, -1 none */
    u64 h = hash40(path);
    for (u32 i = 0; i < T->dir_hash.count; i++) {
        u64 v = rd64(E(T, dir_hash, i));
        if ((v & LOW40) == h) return (v >> 40) < T->dirs.count ? (long long)(v >> 40) : -1;
    }
    return -1;
}

typedef struct { u64 off, stored, size; u32 flags; } Where;
static int data_where(const Table *T, u64 file_section, u32 i2d, Where *w) {   /* arc.py's _data */
    if (i2d >= T->i2d.count) return 0;
    u32 folder = rd32(E(T, i2d, i2d)), data = rd32(E(T, i2d, i2d) + 4);
    if (folder >= T->folder_offsets.count || data >= T->datas.count) return 0;
    const u8 *d = E(T, datas, data);
    w->off = file_section + rd64(E(T, folder_offsets, folder)) + (u64)rd32(d) * 4;
    w->stored = rd32(d + 4), w->size = rd32(d + 8), w->flags = rd32(d + 12);
    return 1;
}

/* A file's bytes as the game gets them (decompressed): malloc'd, NULL if not readable */
static u8 *read_original(const ModArcIO *io, const Where *w) {
    u8 *raw = malloc(w->stored ? w->stored : 1), *out;
    if (!raw || !io->arc(io->ctx, w->off, raw, w->stored)) { free(raw); return NULL; }
    if (!(w->flags & 3)) return raw;
    out = malloc(w->size ? w->size : 1);
    size_t r = out ? ZSTD_decompress(out, w->size, raw, w->stored) : 0;
    free(raw);
    if (!out || ZSTD_isError(r) || r != w->size) { free(out); return NULL; }
    return out;
}

/* ---- building -------------------------------------------------------------------------------- */
typedef struct Job {
    u32 folder, own, kind, comp;   /* comp: the FileData's compression bits (3: a zstd frame) */
    u64 src, body, real;           /* where the bytes are (SEG_*), their length, decompressed */
    int modded;                    /* a mod's file (not an original given back) */
    u32 seq;
    u32 order;                     /* where it goes among the folder's files: its index, or for a new FileData the slot it stands for */
} Job;

typedef struct { u32 d, k; } Child;

typedef struct Build {
    Table T;
    const ModArcIO *io;
    const ModArcFile *files;
    u64 file_section;
    u32 *cstart; Child *child;     /* FileInfo -> its (directory, child number) pairs */
    u8 *referenced;                /* FileData some info-to-data reads (before any change) */
    Job *jobs; u32 njobs, capjobs;
    u8 *new_paths; u32 nnew_paths;
    u8 *new_ii; u32 nnew_ii;
    u8 *mem; u64 nmem;             /* originals given back, as frames */
    u32 unshared, reshared, identical, skipped;
    /* (mod-support 1.4h; SSBU_MODS_FRESH=0 turns it off) A child whose slot (start + k) is another
     * folder's file gets a new FileData (datas.count + j) as its own file (load type 1), its bytes laid
     * out in its directory's rebuilt folder where the slot would be - as the 9 own files of data.arc
     * whose FileData is not start + k. (A new folder and FileInfo read as another color's file was
     * tried first: the game never asked for it - a FileInfoIndex names a child of a directory.) */
    int fresh_on;
    int sounds_on;                      /* sounds too, made own files that are not regional (1.4i; SSBU_MODS_VOICES=0 turns it off) */
    u8 *fresh_datas; u32 nfresh;        /* the new FileData, 16 bytes each */
    /* (mod-support 1.4c; SSBU_MODS_NEW=0 turns it off) New files (config.json's new-dir-files): each a new
     * path, FileInfoIndex, FileInfo, info-to-data and FileData, its own file (load type 1) in its
     * directory's folder. A directory's children are a range of FileInfo, so a directory given new files
     * has its range copied past the table's FileInfo, the new ones after it (as ARCropolis does); its
     * children's FileInfoIndex follow the copies. */
    int new_on;
    struct Add { int f; u32 d, data; long long pi; } *adds; u32 nadds;
    struct Reloc { u32 d, start, count, to, added; } *relocs; u32 nrelocs;
    u8 *add_infos; u32 nadd_infos;      /* the new files' FileInfo, in the order of adds sorted by directory */
    u8 *new_i2d; u32 nnew_i2d;          /* 12 bytes each */
    u32 nmoved;                         /* the FileInfo past the table's: copies and new ones */
    u32 ncopied;                        /* the FileData copied with their folder (among the new FileData) */
    u8 *moved_folder;                   /* per folder: its FileData moved (laid out in index order) */
    ModArc *A;                          /* (the archive being made: its files' sizes as served) */
} Build;
/* FileData i, the new ones past the table's */
#define DATA(B, i) ((u32)(i) < (B)->T.datas.count ? E(&(B)->T, datas, (i)) : (B)->fresh_datas + 16ull * ((u32)(i) - (B)->T.datas.count))

static int grow(void **p, u64 need, u64 *cap, u64 size) {
    if (need <= *cap) return 1;
    u64 c = *cap ? *cap * 2 : 64;
    while (c < need) c *= 2;
    void *q = realloc(*p, c * size);
    if (!q) return 0;
    *p = q, *cap = c;
    return 1;
}

static int job_put_at(Build *B, u32 folder, u32 own, u32 kind, u64 src, u64 body, u64 real, u32 comp, int modded, u32 order) {
    for (u32 i = 0; i < B->njobs; i++)   /* (a FileData filled twice: the last one) */
        if (B->jobs[i].folder == folder && B->jobs[i].own == own) {
            B->jobs[i] = (Job){folder, own, kind, comp, src, body, real, modded, B->jobs[i].seq, order};
            return 1;
        }
    u64 cap = B->capjobs;
    if (!grow((void **)&B->jobs, B->njobs + 1, &cap, sizeof *B->jobs)) return 0;
    B->capjobs = (u32)cap;
    B->jobs[B->njobs] = (Job){folder, own, kind, comp, src, body, real, modded, B->njobs, order};
    B->njobs++;
    return 1;
}
static int job_put(Build *B, u32 folder, u32 own, u32 kind, u64 src, u64 body, u64 real, u32 comp, int modded) {
    return job_put_at(B, folder, own, kind, src, body, real, comp, modded, own);
}

/* (folder, own FileData, info-to-data entry) of child k of directory d whose FileInfo is `info` */
static int place(Build *B, u32 info, u32 d, u32 k, u32 *folder, u32 *own, u8 **i2d) {
    Table *T = &B->T;
    if (d >= T->dirs.count || info >= T->infos.count) return 0;
    *folder = (u32)(rd64(E(T, dirs, d)) >> 40);
    if (*folder >= T->folder_offsets.count) return 0;
    *own = rd32(E(T, folder_offsets, *folder) + 16) + k;
    u32 i = rd32(E(T, infos, info) + 8);
    if (*own >= T->datas.count || i >= T->i2d.count) return 0;
    *i2d = E(T, i2d, i);
    return 1;
}

/* The FileData if nothing reads it (empty, or with bytes: then emptied), claimed for the caller. One an
 * info-to-data reads, even empty, is another file's place: filled, its offset would count in two folders
 * (Wii Fit c02 and c06 both filled one, and Pyra's empty file took Wii Fit c04's) */
static int take_slot(Build *B, u32 own) {
    u8 *e = E(&B->T, datas, own);
    if (B->referenced[own]) return 0;
    if (rd32(e + 4)) memset(e, 0, 16);
    B->referenced[own] = 1;
    return 1;
}

/* A regional file (FileInfo flags 0x8000: voices, some UI) reads info-to-data i2d + r for region r.
 * A mod's file is for every region (as in ARCropolis): each region's entry becomes region 0's, which
 * now points at the mod's file. (Left as they were, the other regions kept reading the shared
 * placeholder - an empty file - and the game waited for it forever: "Rabbit Hole" Palutena.) */
static void spread_regions(Build *B, u32 info) {
    Table *T = &B->T;
    if (info >= T->infos.count || !(rd32(E(T, infos, info) + 12) & 0x8000) || T->regions < 2) return;
    u32 i = rd32(E(T, infos, info) + 8);
    if ((u64)i + T->regions > T->i2d.count) return;
    for (u32 r = 1; r < T->regions; r++) memcpy(E(T, i2d, i + r), E(T, i2d, i), 12);
}

/* The shared file's source (the FileInfo the path's FileInfoIndex names) is path pi itself: that
 * FileInfo gets a path of its own, a copy of pi's with length 0xFF that no lookup finds, keeping the
 * shared FileInfoIndex for the colors that still share it */
static int keep_source_path(Build *B, u32 ii, u32 pi) {
    Table *T = &B->T;
    u32 shared_info = rd32(E(T, info_indices, ii) + 4);
    if (shared_info >= T->infos.count || rd32(E(T, infos, shared_info)) != pi) return 1;
    u8 *p = realloc(B->new_paths, (u64)(B->nnew_paths + 1) * 32);
    if (!p) return 0;
    B->new_paths = p;
    u8 *dummy = p + (u64)B->nnew_paths * 32;
    memcpy(dummy, E(T, paths, pi), 32);
    u64 full = rd64(dummy);
    wr64(dummy, (full & 0xFFFFFFFFull) | 0xFFull << 32 | (u64)ii << 40);
    wr32(E(T, infos, shared_info), T->paths.count + B->nnew_paths);
    B->nnew_paths++;
    return 1;
}

static int unshare_to(Build *B, u32 info, u32 d, u32 k, u32 pi, u32 data);
static int unshare(Build *B, u32 info, u32 d, u32 k, u32 pi) { return unshare_to(B, info, d, k, pi, ~0u); }
/* A new FileData (fresh_on) for child k of directory d, its own file there: its index */
static u32 fresh_data(Build *B) {
    u8 *p = realloc(B->fresh_datas, 16ull * (B->nfresh + 1));
    if (!p) return ~0u;
    B->fresh_datas = p;
    memset(p + 16ull * B->nfresh, 0, 16);
    return B->T.datas.count + B->nfresh++;
}

/* Child k of directory d reads its own FileData (`data`, ~0: its slot start + k) through a FileInfoIndex of its own */
static int unshare_to(Build *B, u32 info, u32 d, u32 k, u32 pi, u32 data) {
    Table *T = &B->T;
    u32 folder, own;
    u8 *i2d;
    if (!place(B, info, d, k, &folder, &own, &i2d)) return 0;
    if (data != ~0u) own = data;
    u32 fl = rd32(i2d + 8);
    wr32(i2d, folder), wr32(i2d + 4, own), wr32(i2d + 8, (1u << 24) | (fl >> 24 == 3 ? fl & 0xFFFFFF : 0));
    u32 ii = rd32(E(T, infos, info) + 4);
    if (ii >= T->info_indices.count || pi >= T->paths.count) return 0;
    if (!keep_source_path(B, ii, pi)) return 0;
    u32 my_ii = T->info_indices.count + B->nnew_ii;
    u8 *q = realloc(B->new_ii, (u64)(B->nnew_ii + 1) * 8);
    if (!q) return 0;
    B->new_ii = q;
    wr32(q + (u64)B->nnew_ii * 8, d), wr32(q + (u64)B->nnew_ii * 8 + 4, info);
    B->nnew_ii++;
    wr32(E(T, infos, info) + 4, my_ii);
    if (fl >> 24 == 0) {   /* it was marked shared (0x100000, 0x200000); now it is its own file */
        u8 *f = E(T, infos, info) + 12;
        wr32(f, rd32(f) & ~0x300000u);
    }
    if (B->sounds_on) {   /* an own file of a color is never regional (a Koopaling's voice: flags 0x10, one entry); the
                           * shared voices are, by language folder. (mod-support 1.4i: with it, Wii Fit's and Palutena's
                           * voices from mods play) */
        u8 *f = E(T, infos, info) + 12;
        wr32(f, rd32(f) & ~0x8000u);
    }
    u8 *path = E(T, paths, pi);
    wr64(path, (rd64(path) & LOW40) | (u64)my_ii << 40);
    B->unshared++;
    return 1;
}

static const ModArcFile *sort_files;
static int cmp_dest(const void *a, const void *b) { return strcmp(sort_files[*(const int *)a].dest, sort_files[*(const int *)b].dest); }
static int cmp_u64(const void *a, const void *b) { u64 x = *(const u64 *)a, y = *(const u64 *)b; return x < y ? -1 : x > y; }
static int cmp_job(const void *a, const void *b) {
    const Job *x = a, *y = b;
    if (x->folder != y->folder) return x->folder < y->folder ? -1 : 1;
    return x->own < y->own ? -1 : x->own > y->own;
}
static int cmp_seg(const void *a, const void *b) { u64 x = ((const ModArcSeg *)a)->at, y = ((const ModArcSeg *)b)->at; return x < y ? -1 : x > y; }

static int in_sorted(const u64 *v, u64 n, u64 key) {
    u64 lo = 0, hi = n;
    while (lo < hi) {
        u64 mid = (lo + hi) / 2;
        if (v[mid] < key) lo = mid + 1; else hi = mid;
    }
    return lo < n && v[lo] == key;
}

/* Whether the mod's param file (.prc, "paracobn": u32 hash table size, u32 ref table size, then the
 * u64 hash40 keys) has every key of the game's: one made for another version of the game lacks some
 * (Splatsune Inku's fighter/common/param/effect.prc, from an older one: the match never loaded). The
 * number missing, 0 none (or not a param file) */
static int prc_missing_keys(Build *B, int f, u32 pi) {
    Table *T = &B->T;
    u32 ii = (u32)(rd64(E(T, paths, pi)) >> 40);
    if (ii >= T->info_indices.count) return 0;
    u32 info = rd32(E(T, info_indices, ii) + 4);
    Where w;
    if (info >= T->infos.count || !data_where(T, B->file_section, rd32(E(T, infos, info) + 8), &w) || w.size < 16) return 0;
    u8 *orig = read_original(B->io, &w), *mine = malloc(B->files[f].size ? B->files[f].size : 1);
    int missing = 0;
    if (orig && mine && B->files[f].size >= 16 && B->io->mod(B->io->ctx, f, 0, mine, B->files[f].size) && !memcmp(orig, "paracobn", 8) &&
        !memcmp(mine, "paracobn", 8)) {
        u64 on = rd32(orig + 8) / 8, mn = rd32(mine + 8) / 8;
        if (16 + on * 8 <= w.size && 16 + mn * 8 <= B->files[f].size) {
            u64 *mk = malloc(sizeof *mk * (mn ? mn : 1));
            if (mk) {
                for (u64 i = 0; i < mn; i++) mk[i] = rd64(mine + 16 + i * 8);
                qsort(mk, mn, sizeof *mk, cmp_u64);
                for (u64 i = 0; i < on; i++)
                    if (!in_sorted(mk, mn, rd64(orig + 16 + i * 8))) missing++;
                free(mk);
            }
        }
    }
    free(orig);
    free(mine);
    return missing;
}

/* A .nus3bank's sections: "NUS3", u32 size, "BANKTOC ", u32 TOC size, u32 count, (magic, size) each,
 * then each section (magic, size, data). Their count, 0 not one; at most 16 */
typedef struct { const u8 *magic, *data; u32 size; } Sec;
static int bank_sections(const u8 *b, u64 n, Sec *out) {
    if (n < 24 || memcmp(b, "NUS3", 4) || memcmp(b + 8, "BANKTOC ", 8)) return 0;
    u32 tsize = rd32(b + 16), count = rd32(b + 20);
    if (count > 16 || 24 + 8ull * count > n) return 0;
    u64 p = 20 + (u64)tsize;
    for (u32 i = 0; i < count; i++) {
        const u8 *e = b + 24 + 8ull * i;
        u32 size = rd32(e + 4);
        if (p + 8 + size > n || memcmp(b + p, e, 4)) return 0;
        out[i] = (Sec){e, b + p + 8, size};
        p += 8 + (u64)size;
    }
    return (int)count;
}

/* The mod's .nus3bank f replacing the game's (path pi), served with the game's BINF (the bank's name and
 * ID: the game finds a fighter's voice by them) and JUNK (the padding after it), the rest the mod's, when
 * those differ. Miku Fitness Boxing's banks for Wii Fit's male colors are copies of the female's, named
 * "vc_wiifit", ID 4048, where the game looks for "vc_wiifit_c01", 4049: the voice was read, never played
 * (ARCropolis changes the ID). */
static void bank_patch(ModArc *a, Build *B, int f, u32 pi) {
    Table *T = &B->T;
    const ModArcFile *F = &B->files[f];
    u32 ii = (u32)(rd64(E(T, paths, pi)) >> 40);
    if (ii >= T->info_indices.count || F->size > (64u << 20) || !a->over) return;
    u32 info = rd32(E(T, info_indices, ii) + 4);
    if (info >= T->infos.count) return;
    u32 x = rd32(E(T, infos, info) + 8), nr = rd32(E(T, infos, info) + 12) & 0x8000 ? T->regions : 1;
    Where w = {0};
    int have = 0;
    for (u32 r = 0; r < nr && !have; r++)   /* (a regional one: the first region with bytes; region 0 is often a placeholder) */
        have = data_where(T, B->file_section, x + r, &w) && w.stored;
    if (!have) return;
    u8 *orig = read_original(B->io, &w), *mine = malloc(F->size ? F->size : 1), *out = NULL;
    Sec gs[16], ms[16];
    int gn = orig ? bank_sections(orig, w.size, gs) : 0, mn = 0;
    if (mine && B->io->mod(B->io->ctx, f, 0, mine, F->size)) mn = bank_sections(mine, F->size, ms);
    const Sec *gbinf = NULL, *mbinf = NULL, *gjunk = NULL;
    for (int i = 0; i < gn; i++) {
        if (!memcmp(gs[i].magic, "BINF", 4)) gbinf = &gs[i];
        if (!memcmp(gs[i].magic, "JUNK", 4)) gjunk = &gs[i];
    }
    for (int i = 0; i < mn; i++) if (!memcmp(ms[i].magic, "BINF", 4)) mbinf = &ms[i];
    if (gbinf && mbinf && (gbinf->size != mbinf->size || memcmp(gbinf->data, mbinf->data, gbinf->size))) {
        u64 total = 24 + 8ull * mn;
        for (int i = 0; i < mn; i++) {
            const Sec *use = !memcmp(ms[i].magic, "BINF", 4) ? gbinf : !memcmp(ms[i].magic, "JUNK", 4) && gjunk ? gjunk : &ms[i];
            total += 8 + use->size;
        }
        if ((out = malloc(total))) {
            u64 o = 24 + 8ull * mn;
            memcpy(out, "NUS3", 4), wr32(out + 4, (u32)(total - 8));
            memcpy(out + 8, "BANKTOC ", 8), wr32(out + 16, 4 + 8 * (u32)mn), wr32(out + 20, (u32)mn);
            for (int i = 0; i < mn; i++) {
                const Sec *use = !memcmp(ms[i].magic, "BINF", 4) ? gbinf : !memcmp(ms[i].magic, "JUNK", 4) && gjunk ? gjunk : &ms[i];
                memcpy(out + 24 + 8 * i, ms[i].magic, 4), wr32(out + 28 + 8 * i, use->size);
                memcpy(out + o, ms[i].magic, 4), wr32(out + o + 4, use->size), memcpy(out + o + 8, use->data, use->size);
                o += 8 + use->size;
            }
            a->over[f] = out, a->over_size[f] = total;
            u32 mid = 0, gid = 0;
            if (mbinf->size >= 9) { u32 l = 9 + mbinf->data[8]; l += (4 - l % 4) % 4; if (l + 4 <= mbinf->size) mid = rd32(mbinf->data + l); }
            if (gbinf->size >= 9) { u32 l = 9 + gbinf->data[8]; l += (4 - l % 4) % 4; if (l + 4 <= gbinf->size) gid = rd32(gbinf->data + l); }
            fprintf(stderr, "[mods] %s: %s: served with the game's bank name and ID (\"%.*s\" %u, the mod's \"%.*s\" %u)\n", F->mod, F->dest,
                    gbinf->size > 9 ? (int)strnlen((const char *)gbinf->data + 9, gbinf->data[8]) : 0, gbinf->data + 9, gid,
                    mbinf->size > 9 ? (int)strnlen((const char *)mbinf->data + 9, mbinf->data[8]) : 0, mbinf->data + 9, mid);
        }
    }
    free(orig);
    free(mine);
}

/* Whether file f is byte for byte the data.arc file it replaces (mods often carry unchanged files) */
static int identical(Build *B, int f, u32 pi) {
    Table *T = &B->T;
    u32 ii = (u32)(rd64(E(T, paths, pi)) >> 40);
    if (ii >= T->info_indices.count) return 0;
    u32 info = rd32(E(T, info_indices, ii) + 4);
    if (info >= T->infos.count) return 0;
    Where w;
    if (!data_where(T, B->file_section, rd32(E(T, infos, info) + 8), &w) || w.size != B->files[f].size) return 0;
    u8 *orig = read_original(B->io, &w), *mine = malloc(w.size ? w.size : 1);
    int same = orig && mine && B->io->mod(B->io->ctx, f, 0, mine, w.size) && !memcmp(orig, mine, w.size);
    free(orig);
    free(mine);
    return same;
}

/* Sounds (.nus3audio, .nus3bank, .tonelabel) never get a new FileData: with "Rabbit Hole" Palutena's
 * voice in one the match did not load (a model's file in one did: Steve c03's wing), so they keep the
 * slot rule and the pair rule */
static int is_sound(const char *dest) {
    const char *dot = strrchr(dest, '.');
    return dot && (!strcmp(dot, ".nus3audio") || !strcmp(dot, ".nus3bank") || !strcmp(dot, ".tonelabel"));
}

/* Whether every child that loads path pi would take the mod's file (the main loop's checks, changing
 * nothing): its own FileData, or a shared one (load type 0 or 3) with a free placeholder */
static int placeable(Build *B, const u32 *pstart, const u32 *pinfo, long long pi, int sound) {
    if (pi < 0 || pstart[pi + 1] == pstart[pi]) return 0;
    for (u32 x = pstart[pi]; x < pstart[pi + 1]; x++)
        for (u32 c = B->cstart[pinfo[x]]; c < B->cstart[pinfo[x] + 1]; c++) {
            u32 folder, own, fl;
            u8 *i2d;
            if (!place(B, pinfo[x], B->child[c].d, B->child[c].k, &folder, &own, &i2d)) return 0;
            fl = rd32(i2d + 8);
            if ((rd32(i2d) == folder && rd32(i2d + 4) == own) || fl >> 24 == 1) continue;
            if (fl >> 24 != 0 && fl >> 24 != 3) return 0;
            if (B->referenced[own] && (!B->fresh_on || (sound && !B->sounds_on))) return 0;
        }
    return 1;
}

static int cmp_add(const void *a, const void *b) {
    const struct Add *x = a, *y = b;
    if (x->d != y->d) return x->d < y->d ? -1 : 1;
    return x->f < y->f ? -1 : x->f > y->f;
}

static int push(void *pp, u32 *n, const void *e, u64 size) {   /* append e to a malloc'd array */
    u8 **p = pp, *q = realloc(*p, (u64)(*n + 1) * size);
    if (!q) return 0;
    memcpy(q + (u64)*n * size, e, size);
    *p = q, (*n)++;
    return 1;
}

/* The new files (B->adds): a FileData each, filled in their directory's folder, and their entries in the
 * table: path (a new one, or the data.arc path no directory loaded), FileInfoIndex (the directory, the
 * new FileInfo), info-to-data (the folder, the FileData, load type 1) and FileInfo (new_file_flags). The directory's children move to the end of the
 * FileInfo (relocate(), with the table), the new ones right after them. */
/* A new file's FileInfo flags: those of a file of the same kind among the directory's children (not shared,
 * not regional), else 0x1000 for a texture or an effect (every .nutexb of data.arc has it, and never 0x10;
 * ARCropolis' "unknown1": with 0x10 Splatsune Inku's new textures were read and the match never loaded),
 * 0x10 for the rest */
static u32 new_file_flags(Build *B, u32 d, const char *dest) {
    Table *T = &B->T;
    const char *slash = strrchr(dest, '/'), *dot = strrchr(dest, '.');
    const char *ext = dot && (!slash || dot > slash) ? dot + 1 : "";
    u64 eh = hash40(ext);
    u32 start = rd32(E(T, dirs, d) + 0x20), count = rd32(E(T, dirs, d) + 0x24);
    for (u32 k = 0; k < count && (u64)start + k < T->infos.count; k++) {
        const u8 *info = E(T, infos, start + k);
        u32 pi = rd32(info);
        if (pi < T->paths.count && (rd64(E(T, paths, pi) + 8) & LOW40) == eh) return rd32(info + 12) & ~0x308000u;
    }
    return !strcmp(ext, "nutexb") || !strcmp(ext, "eff") ? 0x1000 : 0x10;
}

static int plan_adds(Build *B) {
    Table *T = &B->T;
    const ModArcFile *files = B->files;
    qsort(B->adds, B->nadds, sizeof *B->adds, cmp_add);
    for (u32 a0 = 0, a1; a0 < B->nadds; a0 = a1) {
        u32 d = B->adds[a0].d;
        for (a1 = a0; a1 < B->nadds && B->adds[a1].d == d; a1++) {}
        u8 *dir = E(T, dirs, d);
        u32 folder = (u32)(rd64(dir) >> 40), start = rd32(dir + 0x20), count = rd32(dir + 0x24);
        if (folder >= T->folder_offsets.count || (u64)start + count > T->infos.count) {
            for (u32 x = a0; x < a1; x++) {
                fprintf(stderr, "[mods] %s: %s: its directory %s cannot take files; not loaded\n", files[B->adds[x].f].mod,
                        files[B->adds[x].f].dest, files[B->adds[x].f].dir);
                B->skipped++;
            }
            continue;
        }
        struct Reloc r = {d, start, count, T->infos.count + B->nmoved, 0};
        u8 *fo = E(T, folder_offsets, folder);
        u32 after = rd32(fo + 16) + rd32(fo + 20);   /* (laid out among the folder's files, after the last by index) */
        u32 fstart = rd32(fo + 16), fcount = rd32(fo + 20);
        int moved = 0;
        /* Every file data.arc keeps in a folder has its FileData in the folder's range [start, start + count)
         * (a color's own file mostly at start + k, child k). The new files' FileData past the table were
         * outside it, and with them Wii Fit's match crashed copying a file past its buffer, Inkling's and
         * Min Min's never loaded. So the folder's FileData move too, copied past the table's, the new
         * ones right after: in the range, each new child at start + k. The info-to-data and the filled
         * FileData of the folder follow the copies. */
        if (fcount == count && (u64)fstart + fcount <= T->datas.count) {
            u32 to = T->datas.count + B->nfresh;
            for (u32 k = 0; k < fcount; k++) {
                u32 nd = fresh_data(B);
                if (nd == ~0u) return 0;
                memcpy(DATA(B, nd), E(T, datas, fstart + k), 16);
            }
            B->ncopied += fcount;
            for (u32 i = 0; i < T->i2d.count; i++) {
                u8 *e = E(T, i2d, i);
                u32 dd = rd32(e + 4);
                if (rd32(e) == folder && dd >= fstart && dd < fstart + fcount) wr32(e + 4, dd - fstart + to);
            }
            for (u32 j = 0; j < B->njobs; j++) {
                Job *J = &B->jobs[j];
                if (J->folder != folder) continue;
                if (J->own >= fstart && J->own < fstart + fcount) J->own = J->own - fstart + to;
                if (J->order >= fstart && J->order < fstart + fcount) J->order = J->order - fstart + to;
            }
            /* (a child given a new FileData of its own before, 1.4h, now takes its slot start + k: the copy
             * there was another folder's) */
            for (u32 j = 0; j < B->njobs; j++) {
                Job *J = &B->jobs[j];
                if (J->folder != folder || J->own < T->datas.count || J->own >= to || J->order < to || J->order >= to + fcount) continue;
                for (u32 i = 0; i < T->i2d.count; i++) {
                    u8 *e = E(T, i2d, i);
                    if (rd32(e) == folder && rd32(e + 4) == J->own) wr32(e + 4, J->order);
                }
                memset(DATA(B, J->order), 0, 16);
                memset(DATA(B, J->own), 0, 16);
                J->own = J->order;
            }
            wr32(fo + 16, to);
            after = to + fcount;
            moved = 1;
            if (!B->moved_folder && !(B->moved_folder = calloc((u64)T->folder_offsets.count + 1, 1))) return 0;
            B->moved_folder[folder] = 1;
        }
        for (u32 x = a0; x < a1; x++) {
            struct Add *A = &B->adds[x];
            const ModArcFile *F = &files[A->f];
            u32 info = r.to + count + r.added, ii = T->info_indices.count + B->nnew_ii, i2d = T->i2d.count + B->nnew_i2d;
            if ((A->data = fresh_data(B)) == ~0u) return 0;   /* (start + its k, when the folder moved) */
            u8 e_ii[8], e_i2d[12], e_info[16];
            wr32(e_ii, d), wr32(e_ii + 4, info);
            wr32(e_i2d, folder), wr32(e_i2d + 4, A->data), wr32(e_i2d + 8, 1u << 24);
            u32 pi;
            if (A->pi >= 0) {   /* a data.arc path that no directory loaded: it now names the new file */
                pi = (u32)A->pi;
                u8 *path = E(T, paths, pi);
                wr64(path, (rd64(path) & LOW40) | (u64)ii << 40);
            } else {            /* path | ii, extension | 0xFFFFFF, parent "dir/", name: as data.arc's */
                const char *slash = strrchr(F->dest, '/'), *dot = strrchr(F->dest, '.');
                char parent[1100];
                snprintf(parent, sizeof parent, "%.*s/", slash ? (int)(slash - F->dest) : 0, F->dest);
                u8 e_path[32];
                wr64(e_path, hash40(F->dest) | (u64)ii << 40);
                wr64(e_path + 8, hash40(dot && (!slash || dot > slash) ? dot + 1 : "") | 0xFFFFFFull << 40);
                wr64(e_path + 16, hash40(parent));
                wr64(e_path + 24, hash40(slash ? slash + 1 : F->dest));
                pi = T->paths.count + B->nnew_paths;
                if (!push(&B->new_paths, &B->nnew_paths, e_path, 32)) return 0;
            }
            wr32(e_info, pi), wr32(e_info + 4, ii), wr32(e_info + 8, i2d), wr32(e_info + 12, new_file_flags(B, d, F->dest));
            if (!push(&B->new_ii, &B->nnew_ii, e_ii, 8) || !push(&B->new_i2d, &B->nnew_i2d, e_i2d, 12) ||
                !push(&B->add_infos, &B->nadd_infos, e_info, 16))
                return 0;
            if (!job_put_at(B, folder, A->data, SEG_MOD, (u64)A->f, file_frame_size(fsize(B->A, A->f)), fsize(B->A, A->f), 3, 1, after)) return 0;
            r.added++;
        }
        B->nmoved += count + r.added;
        if (moved) wr32(fo + 20, fcount + r.added);   /* (the moved folder: its new ones too) */
        if (!push(&B->relocs, &B->nrelocs, &r, sizeof r)) return 0;
        fprintf(stderr, "[mods] %s: %u new files in %s (%u children before)\n", files[B->adds[a0].f].mod, r.added, files[B->adds[a0].f].dir, count);
    }
    return 1;
}

/* With the table about to be written: each directory given new files reads its children from their
 * copies past the table's FileInfo (out: B->nmoved entries), and the FileInfoIndex that named a child of
 * it name the copy */
static void relocate_infos(Build *B, u8 *out) {   /* the copies and the new FileInfo, in place order */
    Table *T = &B->T;
    u32 w = 0, na = 0;
    for (u32 x = 0; x < B->nrelocs; x++) {
        const struct Reloc *r = &B->relocs[x];
        memcpy(out + 16ull * w, E(T, infos, r->start), 16ull * r->count), w += r->count;
        memcpy(out + 16ull * w, B->add_infos + 16ull * na, 16ull * r->added), w += r->added, na += r->added;
    }
}
static void relocate(Build *B) {
    Table *T = &B->T;
    for (u32 x = 0; x < B->nrelocs; x++) {
        const struct Reloc *r = &B->relocs[x];
        u8 *dir = E(T, dirs, r->d);
        wr32(dir + 0x20, r->to), wr32(dir + 0x24, r->count + r->added);
    }
    for (u64 i = 0; i < (u64)T->info_indices.count + B->nnew_ii; i++) {
        u8 *e = i < T->info_indices.count ? E(T, info_indices, i) : B->new_ii + 8 * (i - T->info_indices.count);
        u32 d = rd32(e), info = rd32(e + 4);
        for (u32 x = 0; x < B->nrelocs; x++)
            if (B->relocs[x].d == d && info >= B->relocs[x].start && info < B->relocs[x].start + B->relocs[x].count) {
                wr32(e + 4, info - B->relocs[x].start + B->relocs[x].to);
                break;
            }
    }
}

static int build(ModArc *a, Build *B, u64 arc_size, const u8 head[0x38]) {
    Table *T = &B->T;
    const ModArcFile *files = B->files;
    int nfiles = a->nfiles;
    u32 ninfo = T->infos.count;

    /* children: FileInfo -> (directory, child number) */
    B->cstart = calloc((u64)ninfo + 2, sizeof *B->cstart);
    if (!B->cstart) return 0;
    for (u32 d = 0; d < T->dirs.count; d++) {
        u32 s = rd32(E(T, dirs, d) + 0x20), n = rd32(E(T, dirs, d) + 0x24);
        for (u32 k = 0; k < n; k++)
            if ((u64)s + k < ninfo) B->cstart[s + k + 1]++;
    }
    for (u32 i = 0; i < ninfo; i++) B->cstart[i + 1] += B->cstart[i];
    B->child = malloc(((u64)B->cstart[ninfo] + 1) * sizeof *B->child);
    u32 *fill = malloc(((u64)ninfo + 1) * sizeof *fill);
    if (!B->child || !fill) { free(fill); return 0; }
    memcpy(fill, B->cstart, (u64)ninfo * sizeof *fill);
    for (u32 d = 0; d < T->dirs.count; d++) {
        u32 s = rd32(E(T, dirs, d) + 0x20), n = rd32(E(T, dirs, d) + 0x24);
        for (u32 k = 0; k < n; k++)
            if ((u64)s + k < ninfo) B->child[fill[s + k]++] = (Child){d, k};
    }
    /* path -> its FileInfos that some directory loads */
    u32 npaths = T->paths.count;
    u32 *pstart = calloc((u64)npaths + 2, sizeof *pstart), *pinfo = malloc(((u64)B->cstart[ninfo] + 1) * sizeof *pinfo);
    if (!pstart || !pinfo) { free(fill); free(pstart); free(pinfo); return 0; }
    for (u32 i = 0; i < ninfo; i++) {
        u32 pi = rd32(E(T, infos, i));
        if (B->cstart[i + 1] > B->cstart[i] && pi < npaths) pstart[pi + 1]++;
    }
    for (u32 p = 0; p < npaths; p++) pstart[p + 1] += pstart[p];
    u32 *pfill = malloc(((u64)npaths + 1) * sizeof *pfill);
    if (!pfill) { free(fill); free(pstart); free(pinfo); return 0; }
    memcpy(pfill, pstart, (u64)npaths * sizeof *pfill);
    for (u32 i = 0; i < ninfo; i++) {
        u32 pi = rd32(E(T, infos, i));
        if (B->cstart[i + 1] > B->cstart[i] && pi < npaths) pinfo[pfill[pi]++] = i;
    }
    free(pfill);
    free(fill);
    B->referenced = calloc((u64)T->datas.count + 1, 1);
    if (!B->referenced) { free(pstart); free(pinfo); return 0; }
    for (u32 i = 0; i < T->i2d.count; i++) {
        u32 dd = rd32(E(T, i2d, i) + 4);
        if (dd < T->datas.count) B->referenced[dd] = 1;
    }

    /* the mods' files, in path order; whether each is the same as the original first, while the
     * table is still data.arc's */
    int *order = malloc(sizeof *order * (nfiles ? nfiles : 1));
    long long *pidx = malloc(sizeof *pidx * (nfiles ? nfiles : 1));
    char *same = calloc((size_t)(nfiles ? nfiles : 1), 1);   /* (byte for byte the original: nothing to load) */
    char *added = calloc((size_t)(nfiles ? nfiles : 1), 1);  /* (a new file, added to its directory) */
    if (!order || !pidx || !same || !added) { free(order); free(pidx); free(same); free(added); free(pstart); free(pinfo); return 0; }
    for (int i = 0; i < nfiles; i++) order[i] = i;
    sort_files = files;
    qsort(order, (size_t)nfiles, sizeof *order, cmp_dest);
    for (int n = 0; n < nfiles; n++) {
        int f = order[n];
        pidx[f] = path_index(T, files[f].dest);
        if (files[f].dir && (pidx[f] < 0 || pstart[pidx[f] + 1] == pstart[pidx[f]])) {   /* config.json's new file */
            long long d = B->new_on ? dir_index(T, files[f].dir) : -1;
            if (d < 0) {
                fprintf(stderr, "[mods] %s: %s: %s\n", files[f].mod, files[f].dest, B->new_on ? "its directory in config.json is not in data.arc; not loaded"
                                                                                       : "a new file (SSBU_MODS_NEW=0: not loaded)");
                pidx[f] = -1, B->skipped++;
                continue;
            }
            size_t dl = strlen(files[f].dest);
            if (dl > 4 && !strcmp(files[f].dest + dl - 4, ".prc")) {   /* (a new param file: against the same one of another color) */
                char alt[2048];
                snprintf(alt, sizeof alt, "%s", files[f].dest);
                char *c = strstr(alt, "/c0");
                long long api = -1;
                for (int k = 0; c && c[3] >= '0' && c[3] <= '9' && c[4] == '/' && k < 8 && api < 0; k++) {
                    c[3] = (char)('0' + k);
                    api = path_index(T, alt);
                }
                int missing = api >= 0 ? prc_missing_keys(B, f, (u32)api) : 0;
                if (missing) {
                    fprintf(stderr, "[mods] %s: %s was made for another version of the game (%d of the names of %s missing); not loaded\n",
                            files[f].mod, files[f].dest, missing, alt);
                    pidx[f] = -1, B->skipped++;
                    continue;
                }
            }
            struct Add *q = realloc(B->adds, sizeof *q * (B->nadds + 1));
            if (!q) { free(order); free(pidx); free(same); free(added); free(pstart); free(pinfo); return 0; }
            B->adds = q;
            B->adds[B->nadds++] = (struct Add){f, (u32)d, 0, pidx[f]};
            pidx[f] = -1, added[f] = 1;
            continue;
        }
        if (pidx[f] < 0) { fprintf(stderr, "[mods] %s: %s is not in data.arc\n", files[f].mod, files[f].dest); B->skipped++; continue; }
        if (pstart[pidx[f] + 1] == pstart[pidx[f]]) {
            fprintf(stderr, "[mods] %s: %s: no directory loads it (not handled yet)\n", files[f].mod, files[f].dest);
            pidx[f] = -1, B->skipped++;
            continue;
        }
        if (identical(B, f, (u32)pidx[f])) { pidx[f] = -1, same[f] = 1, B->identical++; continue; }
        size_t dl = strlen(files[f].dest);
        int missing = dl > 4 && !strcmp(files[f].dest + dl - 4, ".prc") ? prc_missing_keys(B, f, (u32)pidx[f]) : 0;
        if (missing) {
            fprintf(stderr, "[mods] %s: %s was made for another version of the game (%d of its names missing); the game's stays\n",
                    files[f].mod, files[f].dest, missing);
            pidx[f] = -1, B->skipped++;
        }
        if (dl > 9 && !strcmp(files[f].dest + dl - 9, ".nus3bank") && pidx[f] >= 0) bank_patch(a, B, f, (u32)pidx[f]);
    }
    /* A fighter's crowd cheer (sound/bank/fighter_voice/vc_<f>_cheer_*): "Rabbit Hole" Palutena's left
     * the match loading forever while its voice played (mod-support 1.4i); the game's stays */
    for (int f = 0; f < nfiles; f++)
        if (pidx[f] >= 0 && !strncmp(files[f].dest, "sound/bank/fighter_voice/vc_", 28) && strstr(files[f].dest, "_cheer")) {
            fprintf(stderr, "[mods] %s: %s: a crowd cheer, not loaded yet (the game's stays)\n", files[f].mod, files[f].dest);
            pidx[f] = -1, B->skipped++;
        }
    /* A sound is two files: .nus3audio (the samples) and .nus3bank (where each sound is in them). A
     * mod's pair goes in together or not at all: the mod's samples with the game's bank (its bank had
     * no FileData of its own to take) left the match loading forever ("Rabbit Hole" Palutena). */
    for (int f = 0; f < nfiles; f++) {
        const char *d = files[f].dest;
        size_t l = strlen(d);
        if (l < 10 || strcmp(d + l - 10, ".nus3audio")) continue;
        int g = -1;
        for (int k = 0; k < nfiles && g < 0; k++)
            if (strlen(files[k].dest) == l - 1 && !strncmp(files[k].dest, d, l - 10) && !strcmp(files[k].dest + l - 10, ".nus3bank") &&
                !strcmp(files[k].mod, files[f].mod))
                g = k;
        if (g < 0) continue;
        int ok_f = same[f] || added[f] || placeable(B, pstart, pinfo, pidx[f], 1), ok_g = same[g] || added[g] || placeable(B, pstart, pinfo, pidx[g], 1);
        if (ok_f && ok_g) continue;
        if (pidx[f] >= 0 || pidx[g] >= 0)
            fprintf(stderr, "[mods] %s: %s and its .nus3bank go together and %s cannot be loaded; neither is\n", files[f].mod, d,
                    ok_f ? "the bank" : "the samples");
        if (pidx[f] >= 0) pidx[f] = -1, B->skipped++;
        if (pidx[g] >= 0) pidx[g] = -1, B->skipped++;
    }
    /* A model folder whose material names a texture the mod brings as a new file (not in data.arc) that
     * config.json does not add (new-dir-files, mod-support 1.4c): the game waits for that texture
     * forever ("Splatsune Inku" Inkling c06 before 1.4c). That folder keeps the game's model; the rest
     * of the mod loads. Only when a .numatb of the mod in the same folder names it. */
    for (int f = 0; f < nfiles; f++) {
        const char *d = files[f].dest, *slash = strrchr(d, '/');
        size_t l = strlen(d);
        if (path_index(T, d) >= 0 || added[f] || strncmp(d, "fighter/", 8) || !strstr(d, "/model/") || !slash || l < 7 || strcmp(d + l - 7, ".nutexb"))
            continue;
        size_t dl = (size_t)(slash - d) + 1, nl = l - dl - 7;
        int named = 0;
        for (int k = 0; k < nfiles && !named; k++) {
            const char *e = files[k].dest;
            size_t el = strlen(e);
            if (strncmp(e, d, dl) || strchr(e + dl, '/') || el < 7 || strcmp(e + el - 7, ".numatb") || strcmp(files[k].mod, files[f].mod) ||
                files[k].size > (64u << 20))
                continue;
            u8 *mat = malloc(files[k].size + 1);
            if (mat && B->io->mod(B->io->ctx, k, 0, mat, files[k].size))
                for (u64 i = 0; i + nl < files[k].size && !named; i++)
                    named = !memcmp(mat + i, slash + 1, nl) && mat[i + nl] == 0 && (i == 0 || mat[i - 1] < 0x20 || mat[i - 1] >= 0x7F);
            free(mat);
        }
        if (!named) continue;
        int dropped = 0;
        for (int k = 0; k < nfiles; k++)
            if (!strncmp(files[k].dest, d, dl) && !strchr(files[k].dest + dl, '/') && !strcmp(files[k].mod, files[f].mod) && pidx[k] >= 0)
                pidx[k] = -1, B->skipped++, dropped++;
        if (dropped)
            fprintf(stderr, "[mods] %s: %.*s: its material needs %s, a new file config.json does not add; the game's model is kept "
                            "there (%d files not loaded)\n", files[f].mod, (int)(dl - 1), d, slash + 1, dropped);
    }

    for (int n = 0; n < nfiles; n++) {
        int f = order[n];
        if (pidx[f] < 0) continue;
        u32 pi = (u32)pidx[f];
        for (u32 x = pstart[pi]; x < pstart[pi + 1]; x++) {
            u32 info = pinfo[x];
            for (u32 c = B->cstart[info]; c < B->cstart[info + 1]; c++) {
                u32 d = B->child[c].d, k = B->child[c].k, folder, own;
                u8 *i2d;
                if (!place(B, info, d, k, &folder, &own, &i2d)) continue;
                u32 f2 = rd32(i2d), dd = rd32(i2d + 4), fl = rd32(i2d + 8);
                if (fl >> 24 == 1 && f2 < T->folder_offsets.count && dd < T->datas.count) {
                    folder = f2, own = dd;   /* (its own file, where it is: Wii Fit c01 keeps 407 not at start + k, its voice too) */
                    if (B->sounds_on && is_sound(files[f].dest)) {   /* (a regional voice: each language's in its folder, region 0 an
                                                                       * empty placeholder; the mod's, one for all, is not regional, as in unshare_to) */
                        u8 *ifl = E(T, infos, info) + 12;
                        wr32(ifl, rd32(ifl) & ~0x8000u);
                    }
                }
                if (f2 != folder || dd != own) {
                    if (fl >> 24 != 0 && fl >> 24 != 3) {
                        fprintf(stderr, "[mods] %s: %s: child %u of directory %u loads (%u, %u) with load type %u; skipped\n",
                                files[f].mod, files[f].dest, k, d, f2, dd, fl >> 24);
                        continue;
                    }
                    if (!take_slot(B, own)) {
                        if (B->fresh_on && (B->sounds_on || !is_sound(files[f].dest))) {   /* a new FileData of its own, where its slot would be */
                            u32 nd = fresh_data(B);
                            if (nd == ~0u || !unshare_to(B, info, d, k, pi, nd)) goto fail;
                            spread_regions(B, info);
                            if (!job_put_at(B, folder, nd, SEG_MOD, (u64)f, file_frame_size(fsize(a, f)), fsize(a, f), 3, 1, own)) goto fail;
                            continue;
                        }
                        fprintf(stderr, "[mods] %s: %s: child %u of directory %u has no free FileData to take; skipped\n",
                                files[f].mod, files[f].dest, k, d);
                        continue;
                    }
                    if (!unshare(B, info, d, k, pi)) goto fail;
                }
                spread_regions(B, info);
                if (!job_put(B, folder, own, SEG_MOD, (u64)f, file_frame_size(fsize(a, f)), fsize(a, f), 3, 1)) goto fail;
            }
        }
    }

    /* the colors that read a replaced FileData directly (load type 0) keep the original */
    u64 *modded = malloc(sizeof *modded * (B->njobs ? B->njobs : 1));
    u32 nmodded = 0;
    if (!modded) goto fail;
    for (u32 i = 0; i < B->njobs; i++) modded[nmodded++] = (u64)B->jobs[i].folder << 32 | B->jobs[i].own;
    qsort(modded, nmodded, sizeof *modded, cmp_u64);
    u32 ni2d = T->i2d.count;
    u32 *istart = calloc((u64)ni2d + 2, sizeof *istart), *iinfo = malloc(((u64)ninfo + 1) * sizeof *iinfo);
    if (!istart || !iinfo) { free(modded); free(istart); free(iinfo); goto fail; }
    for (u32 i = 0; i < ninfo; i++) {
        u32 x = rd32(E(T, infos, i) + 8);
        if (B->cstart[i + 1] > B->cstart[i] && x < ni2d) istart[x + 1]++;
    }
    for (u32 i = 0; i < ni2d; i++) istart[i + 1] += istart[i];
    {
        u32 *ifill = malloc(((u64)ni2d + 1) * sizeof *ifill);
        if (!ifill) { free(modded); free(istart); free(iinfo); goto fail; }
        memcpy(ifill, istart, (u64)ni2d * sizeof *ifill);
        for (u32 i = 0; i < ninfo; i++) {
            u32 x = rd32(E(T, infos, i) + 8);
            if (B->cstart[i + 1] > B->cstart[i] && x < ni2d) iinfo[ifill[x]++] = i;
        }
        free(ifill);
    }
    for (u32 i = 0; i < ni2d; i++) {
        u32 f2 = rd32(E(T, i2d, i)), dd = rd32(E(T, i2d, i) + 4), fl = rd32(E(T, i2d, i) + 8);
        if (fl >> 24 != 0 || !in_sorted(modded, nmodded, (u64)f2 << 32 | dd)) continue;
        for (u32 x = istart[i]; x < istart[i + 1]; x++) {
            u32 info = iinfo[x], dpi = rd32(E(T, infos, info));
            for (u32 c = B->cstart[info]; c < B->cstart[info + 1]; c++) {
                u32 d = B->child[c].d, k = B->child[c].k, folder, own;
                u8 *i2d;
                if (!place(B, info, d, k, &folder, &own, &i2d) || in_sorted(modded, nmodded, (u64)folder << 32 | own)) continue;
                int fresh = !take_slot(B, own);
                int sound = 0;   /* (the replaced file a sound: no new FileData) */
                for (u32 jj = 0; jj < B->njobs && !sound; jj++)
                    if (B->jobs[jj].folder == f2 && B->jobs[jj].own == dd && B->jobs[jj].kind == SEG_MOD) sound = is_sound(files[B->jobs[jj].src].dest);
                if (fresh && (!B->fresh_on || (sound && !B->sounds_on))) {
                    fprintf(stderr, "[mods] directory %u child %u: reads a replaced file and has no free FileData; it gets the mod's\n", d, k);
                    continue;
                }
                Where w;
                if (!data_where(T, B->file_section, i, &w)) continue;   /* (the source's bytes, before this child changes) */
                u32 target = ~0u, slot = own;
                if (fresh && ((target = fresh_data(B)) == ~0u)) { free(modded); free(istart); free(iinfo); goto fail; }
                if (!unshare_to(B, info, d, k, dpi, target)) { free(modded); free(istart); free(iinfo); goto fail; }
                if (target != ~0u) own = target;
                int ok;
                if (w.flags & 3) {   /* compressed already: served from data.arc as it is */
                    ok = job_put_at(B, folder, own, SEG_ARC, w.off, w.stored, w.size, w.flags & 3, 0, slot);
                } else {             /* stored: as a frame (the game takes no uncompressed FileData) */
                    u8 *orig = read_original(B->io, &w);
                    u64 fs = file_frame_size(w.size), cap = B->nmem;
                    ok = orig && grow((void **)&B->mem, B->nmem + fs, &cap, 1);
                    if (ok) {
                        static const u8 head6[6] = {0x28, 0xB5, 0x2F, 0xFD, 0x00, 0x58};
                        memcpy(B->mem + B->nmem, head6, 6);
                        put_blocks(B->mem + B->nmem + 6, orig, w.size);
                        ok = job_put_at(B, folder, own, SEG_MEM, B->nmem, fs, w.size, 3, 0, slot);
                        B->nmem += fs;
                    }
                    free(orig);
                }
                if (!ok) { free(modded); free(istart); free(iinfo); goto fail; }
                B->reshared++;
            }
        }
    }
    free(modded);
    free(istart);
    free(iinfo);
    free(order), order = NULL;
    free(pidx), pidx = NULL;
    free(same), same = NULL;
    free(added), added = NULL;
    free(pstart), pstart = NULL;
    free(pinfo), pinfo = NULL;
    if (B->nadds && !plan_adds(B)) goto fail;
    if (!B->njobs) return 0;

    /* every folder a FileData is filled in: rebuilt past data.arc's end */
    qsort(B->jobs, B->njobs, sizeof *B->jobs, cmp_job);
    u64 base = align(arc_size, 0x1000);
    u32 nextra = B->nmoved > B->nfresh ? B->nmoved : B->nfresh;   /* (FileInfo and FileData: one count sizes both) */
    u64 new_size = T->len + 8ull * B->nnew_ii + 40ull * B->nnew_paths + (16ull + 16) * nextra + 12ull * B->nnew_i2d;
    u64 at = base + align(16 + table_frame_size(new_size), 0x1000);
    u64 *users = NULL, nusers = 0, capusers = 0;   /* (folder << 32 | FileData) read through a rebuilt folder */
    {
        u8 *rebuilt = calloc((u64)T->folder_offsets.count + 1, 1);
        if (!rebuilt) goto fail;
        for (u32 j = 0; j < B->njobs; j++) rebuilt[B->jobs[j].folder] = 1;
        for (u32 i = 0; i < ni2d; i++) {
            u32 f2 = rd32(E(T, i2d, i)), dd = rd32(E(T, i2d, i) + 4);
            if (f2 < T->folder_offsets.count && rebuilt[f2] && dd < T->datas.count + B->nfresh) {
                if (!grow((void **)&users, nusers + 1, &capusers, sizeof *users)) { free(rebuilt); goto fail; }
                users[nusers++] = (u64)f2 << 32 | dd;
            }
        }
        free(rebuilt);
        qsort(users, nusers, sizeof *users, cmp_u64);
    }
    u32 *folder_dir = malloc(sizeof *folder_dir * ((u64)T->folder_offsets.count + 1));   /* (the directory loading each folder) */
    if (!folder_dir) { free(users); goto fail; }
    memset(folder_dir, 0xFF, sizeof *folder_dir * ((u64)T->folder_offsets.count + 1));
    for (u32 d = 0; d < T->dirs.count; d++) {
        u32 f = (u32)(rd64(E(T, dirs, d)) >> 40);
        if (f < T->folder_offsets.count && folder_dir[f] == ~0u) folder_dir[f] = d;
    }
    ModArcSeg *seg = NULL;
    u64 nseg = 1, capseg = 0;   /* (the table's goes first) */
    if (!grow((void **)&seg, 1, &capseg, sizeof *seg)) { free(users); free(folder_dir); goto fail; }
    u32 nfolders = 0;
    for (u32 j0 = 0, j1; j0 < B->njobs; j0 = j1) {
        u32 folder = B->jobs[j0].folder;
        for (j1 = j0; j1 < B->njobs && B->jobs[j1].folder == folder; j1++) {}
        u8 *fo = E(T, folder_offsets, folder);
        u64 f_off = rd64(fo), src = B->file_section + f_off;
        u32 decomp = rd32(fo + 8), size = rd32(fo + 12);
        /* the FileData this folder owns: those an info-to-data reads through it, and the ones filled */
        u64 lo = 0, hi = nusers;
        while (lo < hi) { u64 mid = (lo + hi) / 2; if (users[mid] >> 32 < folder) lo = mid + 1; else hi = mid; }
        u64 cap = 0, nmine = 0;
        u32 *mine = NULL;
        for (u64 u = lo; u < nusers && users[u] >> 32 == folder; u++)
            if (!nmine || mine[nmine - 1] != (u32)users[u]) {
                if (!grow((void **)&mine, nmine + 1, &cap, sizeof *mine)) goto fail_layout;
                mine[nmine++] = (u32)users[u];
            }
        for (u32 j = j0; j < j1; j++) {
            if (!grow((void **)&mine, nmine + 1, &cap, sizeof *mine)) goto fail_layout;
            mine[nmine++] = B->jobs[j].own;
        }
        {
            /* sorted, once each */
            u64 *tmp = malloc(sizeof *tmp * (nmine ? nmine : 1));
            if (!tmp) { free(mine); goto fail_layout; }
            for (u64 i = 0; i < nmine; i++) tmp[i] = mine[i];
            qsort(tmp, nmine, sizeof *tmp, cmp_u64);
            u64 w = 0;
            for (u64 i = 0; i < nmine; i++) if (!w || tmp[w - 1] != tmp[i]) tmp[w++] = tmp[i];
            for (u64 i = 0; i < w; i++) mine[i] = (u32)tmp[i];
            nmine = w;
            free(tmp);
        }
        /* their old places: offset -> the longest stored length there (FileData sharing an offset are one file) */
        u64 *real = malloc(sizeof *real * 2 * (nmine ? nmine : 1)), nreal = 0;
        u64 *newat = malloc(sizeof *newat * (nmine ? nmine : 1));
        u64 *place_of = malloc(sizeof *place_of * (j1 - j0 ? j1 - j0 : 1));
        if (!real || !newat || !place_of) { free(real); free(newat); free(place_of); free(mine); goto fail_layout; }
        for (u64 i = 0; i < nmine; i++) {
            const u8 *e = DATA(B, mine[i]);
            if (rd32(e + 4)) real[nreal * 2] = (u64)rd32(e) * 4, real[nreal * 2 + 1] = rd32(e + 4), nreal++;
        }
        qsort(real, nreal, 16, cmp_u64);   /* (by offset: the first of each pair) */
        {
            u64 w = 0;
            for (u64 i = 0; i < nreal; i++) {
                if (w && real[(w - 1) * 2] == real[i * 2]) {
                    if (real[i * 2 + 1] > real[(w - 1) * 2 + 1]) real[(w - 1) * 2 + 1] = real[i * 2 + 1];
                } else real[w * 2] = real[i * 2], real[w * 2 + 1] = real[i * 2 + 1], w++;
            }
            nreal = w;
        }
        /* The order the game reads them in: its loader walks the folder's directory child by child
         * (FileInfo start + k, then each one's info-to-data), reading the folder forward in chunks, so each
         * file must come after the ones the children before it read (in data.arc, offsets increase with
         * the FileData index in every directory, which is also the children's order but for 9). A file
         * behind what it read already makes it copy a size that wrapped around (Wii Fit's crash with the
         * new files laid out before the last file; Inkling's and Min Min's hang), and one past the
         * children's last is never read (Splatsune Inku's copy_inkling_gun, 3.11). So: the directory's
         * children in order, the new ones (plan_adds) after them, then what other directories read here. */
        u32 *visit = malloc(sizeof *visit * (nmine ? nmine : 1));
        u8 *seen = calloc(nmine + 1, 1), *rdone = calloc(nreal + 1, 1), *jdone = calloc((u64)(j1 - j0) + 1, 1);
        u64 nvisit = 0;
        if (!visit || !seen || !rdone || !jdone) { free(visit); free(seen); free(rdone); free(jdone); free(real); free(newat); free(place_of); free(mine); goto fail_layout; }
#define VISIT(idx) do { \
            u64 l_ = 0, h_ = nmine, k_ = (idx); \
            while (l_ < h_) { u64 m_ = (l_ + h_) / 2; if (mine[m_] < k_) l_ = m_ + 1; else h_ = m_; } \
            if (l_ < nmine && mine[l_] == k_ && !seen[l_]) seen[l_] = 1, visit[nvisit++] = (u32)k_; \
        } while (0)
        u32 dir = folder_dir[folder];
        if (dir != ~0u) {
            u32 cs = rd32(E(T, dirs, dir) + 0x20), cn = rd32(E(T, dirs, dir) + 0x24);
            for (u32 k = 0; k < cn && (u64)cs + k < T->infos.count; k++) {
                const u8 *info = E(T, infos, cs + k);
                u32 x = rd32(info + 8), nr = rd32(info + 12) & 0x8000 ? T->regions : 1;
                for (u32 r = 0; r < nr && (u64)x + r < T->i2d.count; r++)
                    if (rd32(E(T, i2d, x + r)) == folder) VISIT(rd32(E(T, i2d, x + r) + 4));
            }
            for (u32 x = 0; x < B->nadds; x++)
                if (B->adds[x].d == dir && B->adds[x].data) VISIT(B->adds[x].data);
        }
        for (u64 i = 0; i < nmine; i++) VISIT(mine[i]);
#undef VISIT
        u64 pos = 0;
#define PUT(j) do { \
            const Job *J_ = &B->jobs[(j)]; \
            if (!grow((void **)&seg, nseg + 1, &capseg, sizeof *seg)) goto fail_folder; \
            seg[nseg++] = (ModArcSeg){at + pos, J_->body, J_->src, J_->kind, 0}; \
            place_of[(j) - j0] = pos; \
            pos = align(pos + J_->body, 0x10); \
        } while (0)
        for (u64 v = 0; v < nvisit; v++) {
            u32 idx = visit[v];
            long long jj = -1;
            for (u32 x = j0; x < j1 && jj < 0; x++) if (B->jobs[x].own == idx) jj = x;
            u64 o = (u64)rd32(DATA(B, idx)) * 4;   /* (its old place: the FileData are written after) */
            int stored = rd32(DATA(B, idx) + 4) != 0;
            if (jj < 0 && !stored) continue;
            u64 l2 = 0, h2 = nreal;
            while (l2 < h2) { u64 m = (l2 + h2) / 2; if (real[m * 2] < o) l2 = m + 1; else h2 = m; }
            int has_r = stored && l2 < nreal && real[l2 * 2] == o;
            if (jj < 0 && has_r && !rdone[l2])   /* (a kept file whose bytes a filled FileData shared: the mod's now) */
                for (u32 x = j0; x < j1 && jj < 0; x++)
                    if (rd32(DATA(B, B->jobs[x].own) + 4) && (u64)rd32(DATA(B, B->jobs[x].own)) * 4 == o) jj = x;
            if (jj >= 0) {
                if (!jdone[jj - j0]) { PUT((u32)jj); jdone[jj - j0] = 1; }
                if (has_r && !rdone[l2] && rd32(DATA(B, B->jobs[jj].own) + 4) && (u64)rd32(DATA(B, B->jobs[jj].own)) * 4 == o)
                    newat[l2] = place_of[jj - j0], rdone[l2] = 1;
            } else if (has_r && !rdone[l2]) {
                newat[l2] = pos, rdone[l2] = 1;
                if (!grow((void **)&seg, nseg + 1, &capseg, sizeof *seg)) goto fail_folder;
                seg[nseg++] = (ModArcSeg){at + pos, real[l2 * 2 + 1], src + o, SEG_ARC, 0};
                pos = align(pos + real[l2 * 2 + 1], 0x10);
            }
        }
        for (u32 x = j0; x < j1; x++) if (!jdone[x - j0]) { PUT(x); jdone[x - j0] = 1; }   /* (none: each is a FileData of mine) */
#undef PUT
        /* the FileData: the filled ones at their new places, the others moved with their bytes */
        for (u64 i = 0; i < nmine; i++) {
            u8 *e = DATA(B, mine[i]);
            long long j = -1;
            for (u32 x = j0; x < j1; x++) if (B->jobs[x].own == mine[i]) j = x;
            if (j >= 0) {
                const Job *J = &B->jobs[j];
                decomp += (u32)J->real - rd32(e + 8);
                wr32(e, (u32)(place_of[j - j0] / 4)), wr32(e + 4, (u32)J->body), wr32(e + 8, (u32)J->real);
                wr32(e + 12, (rd32(e + 12) & ~3u) | J->comp);
            } else if (rd32(e + 4)) {
                u64 o = (u64)rd32(e) * 4, l2 = 0, h2 = nreal;
                while (l2 < h2) { u64 m = (l2 + h2) / 2; if (real[m * 2] < o) l2 = m + 1; else h2 = m; }
                wr32(e, (u32)(newat[l2] / 4));
            }
        }
        wr64(fo, at - B->file_section);
        wr32(fo + 8, decomp);
        wr32(fo + 12, (u32)pos);
        fprintf(stderr, "[mods] folder %u: %u files, %u bytes at %#llx rebuilt at %#llx, %llu bytes now\n", folder, j1 - j0,
                size, (unsigned long long)src, (unsigned long long)at, (unsigned long long)pos);
        at = align(at + pos, 0x1000);
        nfolders++;
        free(visit); free(seen); free(rdone); free(jdone); free(real); free(newat); free(place_of); free(mine);
        continue;
    fail_folder:
        free(visit); free(seen); free(rdone); free(jdone); free(real); free(newat); free(place_of); free(mine);
    fail_layout:
        free(users);
        free(seg);
        free(folder_dir);
        goto fail;
    }
    free(users);
    free(folder_dir);

    /* the new paths go into the hash lookup too: each in its bucket, in hash order */
    u64 h_n = T->hash_to_path.count;
    u64 *lookup = malloc(sizeof *lookup * (h_n + B->nnew_paths + 1));
    if (!lookup) { free(seg); goto fail; }
    for (u64 i = 0; i < h_n; i++) lookup[i] = rd64(E(T, hash_to_path, i));
    for (u32 n = 0; n < B->nnew_paths; n++) {
        u64 h = rd64(B->new_paths + (u64)n * 32) & LOW40, b = h % T->buckets;
        u8 *bk = T->t + T->bucket_table + b * 8;
        u32 start = rd32(bk), count = rd32(bk + 4), k = start;
        while (k < start + count && (lookup[k] & LOW40) < h) k++;
        memmove(lookup + k + 1, lookup + k, (h_n + n - k) * sizeof *lookup);
        lookup[k] = h | (u64)(T->paths.count + n) << 40;
        wr32(bk + 4, count + 1);
        for (u64 bb = b + 1; bb < T->buckets; bb++) {
            u8 *o = T->t + T->bucket_table + bb * 8;
            wr32(o, rd32(o) + 1);
        }
    }
    wr32(T->t + T->bucket_table - 8, (u32)(h_n + B->nnew_paths));   /* the lookup's count */
    u64 h_o = T->hash_to_path.o, p_o = T->paths.o, p_end = p_o + 32ull * T->paths.count;
    u64 ii_end = T->info_indices.o + 8ull * T->info_indices.count;
    u64 tlen = 16 + table_frame_size(new_size);
    u8 *blob = malloc(tlen + B->nmem), *nt = malloc(new_size);
    u64 fo_end = T->folder_offsets.o + 28ull * T->folder_offsets.count, inf_end = T->infos.o + 16ull * T->infos.count;
    u64 x_end = T->i2d.o + 12ull * T->i2d.count, d_end = T->datas.o + 16ull * T->datas.count;
    if (!blob || !nt || h_o + 8 * h_n != p_o || p_end != T->info_indices.o || !(ii_end <= fo_end && fo_end <= inf_end && inf_end <= x_end &&
                                                                              x_end <= d_end && d_end <= T->len)) {
        free(blob); free(nt); free(lookup); free(seg);
        if (blob && nt) fprintf(stderr, "[mods] data.arc's table is not laid out as expected\n");
        goto fail;
    }
    relocate(B);
    u64 w = 0;
#define CAT(p, n) (memcpy(nt + w, (p), (n)), w += (n))
    CAT(T->t, h_o);
    CAT(lookup, 8 * (h_n + B->nnew_paths));
    CAT(T->t + p_o, p_end - p_o);
    CAT(B->new_paths, 32ull * B->nnew_paths);
    CAT(T->t + p_end, ii_end - p_end);
    CAT(B->new_ii, 8ull * B->nnew_ii);
    CAT(T->t + ii_end, inf_end - ii_end);
    memset(nt + w, 0, 16ull * nextra);   /* (the moved and new FileInfo, then unused ones: the same count sizes both tables) */
    relocate_infos(B, nt + w), w += 16ull * nextra;
    CAT(T->t + inf_end, x_end - inf_end);
    CAT(B->new_i2d, 12ull * B->nnew_i2d);
    CAT(T->t + x_end, d_end - x_end);
    CAT(B->fresh_datas, 16ull * B->nfresh);
    memset(nt + w, 0, 16ull * (nextra - B->nfresh)), w += 16ull * (nextra - B->nfresh);
    CAT(T->t + d_end, T->len - d_end);
#undef CAT
    free(lookup);
    wr32(nt, (u32)new_size);
    wr32(nt + 4, rd32(nt + 4) + B->nnew_paths);   /* file_info_path_count */
    wr32(nt + 8, rd32(nt + 8) + B->nnew_ii);      /* file_info_index_count */
    wr32(nt + 68, rd32(nt + 68) + nextra);        /* the extra FileInfo and FileData, and its twin (the experiment of task 1) */
    wr32(nt + 76, rd32(nt + 76) + nextra);
    wr32(nt + 80, rd32(nt + 80) + B->nnew_i2d);   /* the extra info-to-data */
    u64 frame = table_frame_size(new_size);
    wr32(blob, 16), wr32(blob + 4, (u32)new_size), wr32(blob + 8, (u32)frame), wr32(blob + 12, (u32)(16 + frame));
    u8 *fh = blob + 16;
    fh[0] = 0x28, fh[1] = 0xB5, fh[2] = 0x2F, fh[3] = 0xFD, fh[4] = 0xE0;   /* single segment, 8-byte content size */
    wr64(fh + 5, new_size);
    put_blocks(fh + 13, nt, new_size);
    free(nt);
    if (B->nmem) memcpy(blob + tlen, B->mem, B->nmem);
    for (u64 i = 1; i < nseg; i++) if (seg[i].kind == SEG_MEM) seg[i].src += tlen;
    seg[0] = (ModArcSeg){base, tlen, 0, SEG_MEM, 0};
    qsort(seg, nseg, sizeof *seg, cmp_seg);

    memcpy(a->header, head, 0x38);
    wr64(a->header + 0x20, base);   /* the file system table, now past data.arc's end */
    a->n = arc_size, a->total = at;
    a->seg = seg, a->nseg = (u32)nseg;
    a->blob = blob, a->blob_size = tlen + B->nmem;
    if (B->nfresh > B->nadd_infos + B->ncopied)
        fprintf(stderr, "[mods] %u new FileData for children with no slot of their own \n", B->nfresh - B->nadd_infos - B->ncopied);
    if (B->nadd_infos) fprintf(stderr, "[mods] %u new files added to %u directories\n", B->nadd_infos, B->nrelocs);
    fprintf(stderr, "[mods] %d files: %u not used, %u equal to the original; %u FileData filled in %u folders, %u unshared "
                    "(%u other colors given the original), %u source paths kept; data.arc %llu bytes, %llu now\n",
            nfiles, B->skipped, B->identical, B->njobs, nfolders, B->unshared, B->reshared, B->nnew_paths,
            (unsigned long long)arc_size, (unsigned long long)at);
    return 1;
fail:
    free(order);
    free(pidx);
    free(same);
    free(added);
    free(pstart);
    free(pinfo);
    return 0;
}

/* ---- the cache -------------------------------------------------------------------------------- */
/* "SSBUMODS", u32 version, u32 fingerprint length, the fingerprint, u64 n, u64 total, the 0x38-byte
 * header, u32 segment count, u32 0, u64 blob size, the segments, the blob */
static char *fingerprint(const u8 head[0x38], const u8 table_head[16], u64 arc_size, const ModArcFile *files, int n, u64 *len) {
    u64 cap = 256, w;
    for (int i = 0; i < n; i++) cap += strlen(files[i].dest) + strlen(files[i].src) + (files[i].dir ? strlen(files[i].dir) : 0) + 56;
    char *s = malloc(cap);
    if (!s) return NULL;
    const char *fresh = getenv("SSBU_MODS_FRESH");
    const char *voices = getenv("SSBU_MODS_VOICES"), *newf = getenv("SSBU_MODS_NEW");
    w = (u64)snprintf(s, cap, "fresh %d voices %d new %d data.arc %llu ", !(fresh && !strcmp(fresh, "0")), !(voices && !strcmp(voices, "0")),
                      !(newf && !strcmp(newf, "0")), (unsigned long long)arc_size);
    for (int i = 0; i < 0x38; i++) w += (u64)snprintf(s + w, cap - w, "%02x", head[i]);
    for (int i = 0; i < 16; i++) w += (u64)snprintf(s + w, cap - w, "%02x", table_head[i]);
    s[w++] = '\n';
    for (int i = 0; i < n; i++)
        w += (u64)snprintf(s + w, cap - w, "%s\t%s\t%llu\t%llu\t%s\n", files[i].dest, files[i].src,
                           (unsigned long long)files[i].size, (unsigned long long)files[i].mtime, files[i].dir ? files[i].dir : "");
    *len = w;
    return s;
}

static int patches_alloc(ModArc *a) {
    a->over = calloc((size_t)(a->nfiles ? a->nfiles : 1), sizeof *a->over);
    a->over_size = calloc((size_t)(a->nfiles ? a->nfiles : 1), sizeof *a->over_size);
    return a->over && a->over_size;
}
static void patches_free(ModArc *a) {
    for (int i = 0; a->over && i < a->nfiles; i++) free(a->over[i]);
    free(a->over), free(a->over_size);
    a->over = NULL, a->over_size = NULL;
}

static int cache_load(ModArc *a, const char *file, const char *fp, u64 fplen) {
    FILE *f = fopen(file, "rb");
    if (!f) return 0;
    char magic[8];
    u32 v[2];
    int ok = fread(magic, 8, 1, f) == 1 && !memcmp(magic, CACHE_MAGIC, 8) && fread(v, 8, 1, f) == 1 && v[0] == CACHE_VERSION && v[1] == fplen;
    char *had = ok ? malloc(fplen + 1) : NULL;
    ok = ok && had && fread(had, 1, fplen, f) == fplen && !memcmp(had, fp, fplen);
    free(had);
    u64 nt[2], bs;
    u32 ns[2];
    ok = ok && fread(nt, 16, 1, f) == 1 && fread(a->header, 0x38, 1, f) == 1 && fread(ns, 8, 1, f) == 1 && fread(&bs, 8, 1, f) == 1;
    ModArcSeg *seg = ok ? malloc(sizeof *seg * (ns[0] ? ns[0] : 1)) : NULL;
    u8 *blob = ok ? malloc(bs ? bs : 1) : NULL;
    ok = ok && seg && blob && fread(seg, sizeof *seg, ns[0], f) == ns[0] && fread(blob, 1, bs, f) == bs;
    u32 np = 0;   /* (then the files served changed: u32 count, and u32 file, u32 0, u64 size, the bytes each) */
    ok = ok && patches_alloc(a) && fread(&np, 4, 1, f) == 1;
    for (u32 i = 0; ok && i < np; i++) {
        u32 fv[2];
        u64 w;
        ok = fread(fv, 8, 1, f) == 1 && fread(&w, 8, 1, f) == 1 && fv[0] < (u32)a->nfiles && w <= (64u << 20) && !a->over[fv[0]];
        if (ok) ok = (a->over[fv[0]] = malloc(w ? w : 1)) && fread(a->over[fv[0]], 1, w, f) == w;
        if (ok) a->over_size[fv[0]] = w;
    }
    fclose(f);
    for (u32 i = 0; ok && i < ns[0]; i++)
        ok = seg[i].at >= nt[0] && seg[i].at + seg[i].size <= nt[1] && (seg[i].kind != SEG_MEM || seg[i].src + seg[i].size <= bs) &&
             (seg[i].kind != SEG_MOD || seg[i].src < (u64)a->nfiles) && seg[i].kind <= SEG_MOD;
    if (!ok) { free(seg); free(blob); patches_free(a); return 0; }
    a->n = nt[0], a->total = nt[1];
    a->seg = seg, a->nseg = ns[0];
    a->blob = blob, a->blob_size = bs;
    return 1;
}

static void cache_save(const ModArc *a, const char *file, const char *fp, u64 fplen) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.new", file);
    FILE *f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "[mods] cannot write %s\n", tmp); return; }
    u32 v[2] = {CACHE_VERSION, (u32)fplen}, ns[2] = {a->nseg, 0};
    u64 nt[2] = {a->n, a->total};
    fwrite(CACHE_MAGIC, 8, 1, f);
    fwrite(v, 8, 1, f);
    fwrite(fp, 1, fplen, f);
    fwrite(nt, 16, 1, f);
    fwrite(a->header, 0x38, 1, f);
    fwrite(ns, 8, 1, f);
    fwrite(&a->blob_size, 8, 1, f);
    fwrite(a->seg, sizeof *a->seg, a->nseg, f);
    fwrite(a->blob, 1, a->blob_size, f);
    u32 np = 0;
    for (int i = 0; i < a->nfiles; i++) np += a->over && a->over[i];
    fwrite(&np, 4, 1, f);
    for (int i = 0; i < a->nfiles && np; i++)
        if (a->over[i]) {
            u32 fv[2] = {(u32)i, 0};
            fwrite(fv, 8, 1, f);
            fwrite(&a->over_size[i], 8, 1, f);
            fwrite(a->over[i], 1, a->over_size[i], f);
        }
    int ok = !ferror(f);
    ok &= fclose(f) == 0;
    remove(file);   /* (Windows: rename does not replace) */
    if (!ok || rename(tmp, file)) { remove(tmp); fprintf(stderr, "[mods] cannot write %s\n", file); }
}

/* ---- setup and reading ----------------------------------------------------------------------- */
int mod_arc_setup(ModArc *a, uint64_t arc_size, const ModArcIO *io, const ModArcFile *files, int n, const char *cache_file) {
    memset(a, 0, sizeof *a);
    a->files = files, a->nfiles = n;
    mod_arc_from_cache = 0;
    if (n <= 0) return 0;
    u8 head[0x38], th[16];
    if (!io->arc(io->ctx, 0, head, sizeof head) || rd64(head) != 0xABCDEF9876543210ull) {
        fprintf(stderr, "[mods] data.arc: cannot read its header; no mods\n");
        return 0;
    }
    u64 toff = rd64(head + 0x20);
    if (!io->arc(io->ctx, toff, th, sizeof th)) { fprintf(stderr, "[mods] data.arc: cannot read its table; no mods\n"); return 0; }
    u64 fplen;
    char *fp = fingerprint(head, th, arc_size, files, n, &fplen);
    if (!fp) return 0;
    if (cache_file && cache_load(a, cache_file, fp, fplen)) {
        free(fp);
        mod_arc_from_cache = 1;
        a->on = 1;
        a->read_once = calloc((size_t)n, 1);
        fprintf(stderr, "[mods] %d files; the table from %s; data.arc %llu bytes, %llu now\n", n, cache_file,
                (unsigned long long)a->n, (unsigned long long)a->total);
        return 1;
    }
    Build B;
    memset(&B, 0, sizeof B);
    B.io = io, B.files = files, B.file_section = rd64(head + 0x10);
    const char *fresh = getenv("SSBU_MODS_FRESH");   /* (on unless SSBU_MODS_FRESH=0) */
    B.fresh_on = !(fresh && !strcmp(fresh, "0"));
    const char *voices = getenv("SSBU_MODS_VOICES");   /* (on unless SSBU_MODS_VOICES=0) */
    B.sounds_on = !(voices && !strcmp(voices, "0"));
    const char *newf = getenv("SSBU_MODS_NEW");   /* (on unless SSBU_MODS_NEW=0) */
    B.new_on = !(newf && !strcmp(newf, "0"));
    u32 data_start = rd32(th), decomp = rd32(th + 4), comp = rd32(th + 8);
    u8 *ct = malloc(comp ? comp : 1);
    B.T.t = malloc(decomp ? decomp : 1);
    B.T.len = decomp;
    int ok = ct && B.T.t && io->arc(io->ctx, toff + data_start, ct, comp);
    if (ok) {
        size_t r = ZSTD_decompress(B.T.t, decomp, ct, comp);
        ok = !ZSTD_isError(r) && r == decomp;
    }
    free(ct);
    if (!ok || !parse(&B.T)) {
        fprintf(stderr, "[mods] data.arc: cannot read its file system table; no mods\n");
        free(B.T.t);
        free(fp);
        return 0;
    }
    B.A = a;
    ok = patches_alloc(a) && build(a, &B, arc_size, head);
    free(B.T.t); free(B.cstart); free(B.child); free(B.referenced); free(B.jobs);
    free(B.new_paths); free(B.new_ii); free(B.mem);
    free(B.fresh_datas);
    free(B.adds); free(B.relocs); free(B.add_infos); free(B.new_i2d); free(B.moved_folder);
    if (ok) {
        a->on = 1;
        a->read_once = calloc((size_t)n, 1);
        if (cache_file) cache_save(a, cache_file, fp, fplen);
    } else {
        mod_arc_free(a);
        a->files = files, a->nfiles = n;
        fprintf(stderr, "[mods] nothing to replace in data.arc\n");
    }
    free(fp);
    return ok;
}

int mod_arc_read(const ModArc *a, uint64_t at, void *buf, uint64_t size, const ModArcIO *io) {
    u8 *p = buf;
    while (size) {
        u64 n;
        if (at < 0x38) {
            n = 0x38 - at < size ? 0x38 - at : size;
            memcpy(p, a->header + at, n);
        } else if (at < a->n) {
            n = a->n - at < size ? a->n - at : size;
            if (!io->arc(io->ctx, at, p, n)) return 0;
        } else {
            u32 lo = 0, hi = a->nseg;   /* the first segment ending past `at` */
            while (lo < hi) {
                u32 mid = (lo + hi) / 2;
                if (a->seg[mid].at + a->seg[mid].size <= at) lo = mid + 1; else hi = mid;
            }
            const ModArcSeg *g = lo < a->nseg ? &a->seg[lo] : NULL;
            if (!g || at < g->at) {   /* a gap: zeros */
                u64 gap = (g ? g->at : a->total) - at;
                if (!gap || at >= a->total) return 0;
                n = gap < size ? gap : size;
                memset(p, 0, n);
            } else {
                n = g->at + g->size - at < size ? g->at + g->size - at : size;
                u64 o = at - g->at;
                int ok;
                if (g->kind == SEG_ARC) ok = io->arc(io->ctx, g->src + o, p, n);
                else if (g->kind == SEG_MEM) ok = g->src + o + n <= a->blob_size && (memcpy(p, a->blob + g->src + o, n), 1);
                else {
                    ok = g->src < (u64)a->nfiles && read_file_frame(io, (int)g->src, fsize(a, (int)g->src), o, p, n,
                                                                    a->over ? a->over[g->src] : NULL);
                    if (ok && a->read_once && !a->read_once[g->src]) {   /* (which mod file the game is loading: a hang's last line) */
                        a->read_once[g->src] = 1;
                        fprintf(stderr, "[mods] loading %s: %s\n", a->files[g->src].mod, a->files[g->src].dest);
                    }
                }
                if (!ok) return 0;
            }
        }
        p += n, at += n, size -= n;
    }
    return 1;
}

void mod_arc_free(ModArc *a) {
    free(a->seg);
    free(a->blob);
    patches_free(a);
    free((void *)a->read_once);
    memset(a, 0, sizeof *a);
}
