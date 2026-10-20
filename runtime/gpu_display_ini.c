/* display.ini rewritten with new values (the start-up menu's, gpu_menu.c), as text: each key's
 * value replaced on its own line - its indentation, the spaces around '=' and a comment after
 * the value kept - and every other line as it was, the user's comments and keys this does not
 * know included. A key the file lacks (one from an older version) is added at the end, after
 * the lines of display.ini's default text that describe it. No file I/O here: gpu_display.c
 * reads and writes, and tests/test_display_save.py drives this directly. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdlib.h>
#include <string.h>

char *display_ini_rewrite(const char *text, const char *defaults, int n, const char *const keys[], const char *const values[]);

typedef struct Buf { char *d; size_t n, cap; } Buf;

static void put(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        while (b->n + n + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 4096;
        b->d = realloc(b->d, b->cap);
    }
    memcpy(b->d + b->n, s, n);
    b->n += n;
    b->d[b->n] = 0;
}
static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* The key a line sets (its span in [line, end)), or 0 for a comment, a section or no '='. */
static int line_key(const char *line, const char *end, const char **k0, const char **k1, const char **eq) {
    const char *p = line;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p == end || *p == ';' || *p == '#' || *p == '[') return 0;
    const char *e = memchr(p, '=', (size_t)(end - p));
    if (!e) return 0;
    const char *q = e;
    while (q > p && (q[-1] == ' ' || q[-1] == '\t')) q--;
    *k0 = p, *k1 = q, *eq = e;
    return q > p;
}

static int key_is(const char *k0, const char *k1, const char *key) {
    size_t n = strlen(key);
    if ((size_t)(k1 - k0) != n) return 0;
    for (size_t i = 0; i < n; i++) if (lower(k0[i]) != lower(key[i])) return 0;
    return 1;
}

/* The lines of the default text that describe key: its comment lines just above it, and it. */
static void describe(Buf *out, const char *defaults, const char *key, const char *value) {
    const char *line = defaults, *start = NULL;
    while (*line) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        const char *k0, *k1, *eq;
        if (*line == ';') { if (!start) start = line; }
        else if (line_key(line, end, &k0, &k1, &eq) && key_is(k0, k1, key)) {
            if (start) put(out, start, (size_t)(line - start));
            break;
        } else start = NULL;   /* (a blank line or another key: the comments above were not this key's) */
        line = *end ? end + 1 : end;
    }
    puts_(out, key);
    puts_(out, " = ");
    puts_(out, value);
    puts_(out, "\n");
}

char *display_ini_rewrite(const char *text, const char *defaults, int n, const char *const keys[], const char *const values[]) {
    Buf out = {0};
    int seen[16] = {0};
    if (n > 16) n = 16;
    const char *line = text ? text : "";
    while (*line) {
        const char *nl = strchr(line, '\n'), *end = nl ? nl : line + strlen(line);
        const char *body_end = end > line && end[-1] == '\r' ? end - 1 : end;   /* (CRLF kept) */
        const char *k0, *k1, *eq;
        int which = -1;
        if (line_key(line, body_end, &k0, &k1, &eq))
            for (int i = 0; i < n && which < 0; i++) if (key_is(k0, k1, keys[i])) which = i;
        if (which < 0) put(&out, line, (size_t)((nl ? nl + 1 : end) - line));
        else {
            seen[which] = 1;
            /* the value starts after '=' and its spaces, ends before a comment (a ';' or '#'
             * after a space, as gpu_display.c reads it) or the line's end, less trailing spaces */
            const char *v0 = eq + 1;
            while (v0 < body_end && (*v0 == ' ' || *v0 == '\t')) v0++;
            const char *v1 = v0;
            while (v1 < body_end && !((*v1 == ';' || *v1 == '#') && v1 > v0 && (v1[-1] == ' ' || v1[-1] == '\t'))) v1++;
            while (v1 > v0 && (v1[-1] == ' ' || v1[-1] == '\t')) v1--;
            put(&out, line, (size_t)(v0 - line));
            puts_(&out, values[which]);
            put(&out, v1, (size_t)((nl ? nl + 1 : end) - v1));
        }
        line = nl ? nl + 1 : end;
    }
    int added = 0;
    for (int i = 0; i < n; i++) {
        if (seen[i]) continue;
        if (!added++ && out.n && out.d[out.n - 1] != '\n') puts_(&out, "\n");
        if (added == 1) puts_(&out, "\n");
        describe(&out, defaults, keys[i], values[i]);
    }
    if (!out.d) puts_(&out, "");
    return out.d;
}
