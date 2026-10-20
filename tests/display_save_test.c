/* runtime/gpu_display_ini.c without the game: tests/test_display_save.py builds this with it.
 *   display_save_test <file> <defaults file> key=value...   prints the file rewritten */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *display_ini_rewrite(const char *text, const char *defaults, int n, const char *const keys[], const char *const values[]);

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = calloc(1, (size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) n = 0;
    fclose(f);
    return s;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const char *keys[16], *values[16];
    int n = 0;
    for (int i = 3; i < argc && n < 16; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) return 2;
        *eq = 0;
        keys[n] = argv[i];
        values[n++] = eq + 1;
    }
    char *out = display_ini_rewrite(slurp(argv[1]), slurp(argv[2]), n, keys, values);
    fputs(out, stdout);
    free(out);
    return 0;
}
