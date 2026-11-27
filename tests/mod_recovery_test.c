/* runtime/mod_recovery.c without the game, for tests/test_mod_recovery.py: one process is one session.
 *   mod_recovery_test <dir> <step>...
 * steps, in order:  read                     the last session (prints "failed=<kind> running=<0|1> suspect=<mod>
 *                                            path=<path> new=<a,b> fails=<n> no_mods=<0|1>")
 *                   chosen                   a choice made in the menu (the two-failures rule off)
 *                   begin <mod>:<hash>,...   the session's mods (prints "begin=<1: without mods>")
 *                   file <mod> <path>        the game read that mod file
 *                   running | end            20 s of frames | a normal close
 *                   crash | fatal | frozen   it failed so (the process then ends, as a crash's would)
 *                   good                     prints last_good.txt's mods ("good=a,b") */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod_recovery.h"

const char *rt_data_dir = ".";

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *dir = argv[1];
    static Recovery r;
    int have = 0;
    for (int i = 2; i < argc; i++) {
        const char *s = argv[i];
        if (!strcmp(s, "read")) {
            have = mod_recovery_read(dir, &r);
            printf("failed=%d running=%d suspect=%s path=%s new=", r.failed, r.was_running, r.suspect, r.suspect_path);
            for (int k = 0; k < r.nnews; k++) printf("%s%s", k ? "," : "", r.news[k]);
            printf(" fails=%d no_mods=%d suspect_new=%d\n", r.fails, r.no_mods, r.suspect_new);
        } else if (!strcmp(s, "chosen")) r.no_mods = 0;
        else if (!strcmp(s, "begin") && i + 1 < argc) {
            static RecoveryMod mods[64];
            int n = 0;
            char list[2048];
            snprintf(list, sizeof list, "%s", argv[++i]);
            for (char *t = strtok(list, ","); t && n < 64; t = strtok(NULL, ",")) {
                char *colon = strchr(t, ':');
                if (colon) *colon++ = 0;
                snprintf(mods[n].key, sizeof mods[n].key, "%s", t);
                mods[n++].hash = colon ? strtoull(colon, NULL, 16) : 0;
            }
            printf("begin=%d\n", mod_recovery_begin(dir, mods, n, have ? &r : NULL));
        } else if (!strcmp(s, "file") && i + 2 < argc) {
            mod_recovery_read_file(argv[i + 1], argv[i + 2]);
            i += 2;
        } else if (!strcmp(s, "running")) mod_recovery_running(dir);
        else if (!strcmp(s, "end")) mod_recovery_end(dir);
        else if (!strcmp(s, "crash") || !strcmp(s, "fatal") || !strcmp(s, "frozen")) {
            mod_recovery_failed(!strcmp(s, "crash") ? REC_CRASH : !strcmp(s, "fatal") ? REC_FATAL : REC_FROZEN);
            fflush(stdout);
            _Exit(5);
        } else if (!strcmp(s, "good")) {
            char keys[64][128];
            int n = mod_recovery_last_good(dir, keys, 64);
            printf("good=");
            for (int k = 0; k < n; k++) printf("%s%s", k ? "," : "", keys[k]);
            printf("\n");
        }
    }
    return 0;
}
