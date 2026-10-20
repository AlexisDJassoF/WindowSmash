/* runtime/gpu_precompile.c without the game: tests/test_gpu_precompile.py builds this with it.
 *
 *   gpu_precompile_test missing <keys> <cache dir>   prints how many keys lack a file, then them
 *   gpu_precompile_test done <line>                   prints the line's three counts, or "no"
 *   gpu_precompile_test run <data dir>                precompile_run with <data dir> as the module
 *                                                     folder (SSBU_SHADERAOT: a stand-in compiler) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_precompile.h"

const char *rt_data_dir;
int shader_dispatch_forced(void) { return getenv("SSBU_SHADER_DISPATCH") && atoi(getenv("SSBU_SHADER_DISPATCH")); }
int gfx_progress(const char *title, unsigned done, unsigned total, const char *status) {
    static unsigned frames;
    if (getenv("TEST_SKIP_AT") && done >= (unsigned)atoi(getenv("TEST_SKIP_AT"))) precompile_skip = 1;   /* (Esc, as the window's thread would) */
    if (frames++ < 3) printf("screen: %s %u/%u %s\n", title, done, total, status);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "missing")) {
        long n = precompile_missing(argv[2], argv[3], stdout);
        printf("%ld\n", n);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "done")) {
        unsigned long c[3];
        if (precompile_done_line(argv[2], c)) printf("%lu %lu %lu\n", c[0], c[1], c[2]);
        else printf("no\n");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "run")) {
        rt_data_dir = argv[2];
        precompile_run();
        return 0;
    }
    return 2;
}
