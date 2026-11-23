/* runtime/pad_menu.c without a window: tests/test_pad_menu.py builds this with it.
 *   pad_menu_test <script>   one letter each: u d l r a b (up down left right accept back),
 *                            k<0-9> (a pad's control pressed: 0-9 as PC_SOUTH.. PC_RSHOULDER, t/T the triggers),
 *                            P (a PlayStation pad drawn), W (saved). controllers.ini: SSBU_CONTROLLERS.
 * Prints the page, '|' between its values. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_menu.h"
#include "pad_menu.h"

const char *pad_default_ini(void) {
    return "; controllers.ini\n\n[general]\nkeyboard_player = 1\n\n[device default]\n; layout = positional\nlayout = positional\n"
           "deadzone = 0\nrumble = 100\ntrigger_threshold = 25\n; ZR = rightshoulder\n\n[device ps5]\nrumble = 60\n";
}

int main(int argc, char **argv) {
    PadSettings s;
    pad_settings_load(&s);
    PadMenu m;
    pad_menu_init(&m, &s);
    int plays = 0;
    for (const char *p = argc > 1 ? argv[1] : ""; *p; p++) {
        switch (*p) {
        case 'u': pad_menu_event(&m, MENU_UP, 0); break;
        case 'd': pad_menu_event(&m, MENU_DOWN, 0); break;
        case 'l': pad_menu_event(&m, MENU_LEFT, 0); break;
        case 'r': pad_menu_event(&m, MENU_RIGHT, 0); break;
        case 'a': plays += pad_menu_event(&m, MENU_ACCEPT, 0) == PAD_EV_PLAY; break;
        case 'b': pad_menu_event(&m, MENU_BACK, 0); break;
        case 'k': if (p[1]) { p++; pad_menu_event(&m, MENU_PADKEY, *p == 't' ? PC_LTRIGGER : *p == 'T' ? PC_RTRIGGER : *p - '0'); } break;
        case 'P': m.kind = PK_PS; break;
        case 'N': m.no_play = 1; break;   /* (as the start-up menu has it: no Play row) */
        case 'W': pad_settings_save(&m.s); break;
        }
    }
    char v[CROWS][512];
    for (int r = 0; r < CROWS; r++) {
        pad_menu_value(&m, r, v[r], sizeof v[r]);
        if (v[r][0] && v[r][0] < 32 && r != CROW_PICTURE) memmove(v[r], v[r] + 1, strlen(v[r]));
    }
    printf("row=%s|plays=%d|listening=%d|layout=%s|deadzone=%s|rumble=%s|dead_zones=%d|testing=%d|left=%s|right=%s|test=%s|takes_back=%d|"
           "dz_shown=%d|a_shown=%d", pad_menu_label(m.row), plays, m.listening, v[CROW_LAYOUT], v[CROW_DEADZONE], v[CROW_RUMBLE], m.dead_zones,
           m.testing, v[CROW_DZ_LEFT], v[CROW_DZ_RIGHT], v[CROW_DZ_TEST], pad_menu_takes_back(&m), pad_menu_visible(&m, CROW_DZ_LEFT),
           pad_menu_visible(&m, CROW_BUTTON));
    printf("|snap_left=%s|snap_right=%s", v[CROW_SNAP_LEFT], v[CROW_SNAP_RIGHT]);
    for (int r = CROW_BUTTON; r <= CROW_BUTTON_END; r++) printf("|%s=%s", pad_menu_label(r), v[r]);
    printf("\n");
    return 0;
}
