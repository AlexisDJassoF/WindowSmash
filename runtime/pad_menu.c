/* The start-up menu's Controls page: its model and controllers.ini's [device default] (see pad_menu.h).
 * No window and no pads here: gpu_menu.c draws it and feeds it input, linux/window.c reads the pads. */
#define _CRT_SECURE_NO_WARNINGS
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_menu.h"
#include "pad_menu.h"

const char *pad_default_ini(void);   /* (hle_pad.c) controllers.ini as it is first written */

/* hle_pad.c's names: its raw controls (RAW_BUTTON_NAMES, then the triggers) and the console's buttons */
static const char *const CONTROL_KEY[PC_COUNT] = {
    "south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder", "rightshoulder",
    "dpup", "dpdown", "dpleft", "dpright", "misc1", "rightpaddle1", "leftpaddle1", "rightpaddle2", "leftpaddle2", "touchpad",
    "misc2", "misc3", "misc4", "misc5", "misc6", "lefttrigger", "righttrigger", "none"};
static const char *const BUTTON_KEY[NB_COUNT] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS", "MINUS",
                                                 "LEFT", "UP", "RIGHT", "DOWN"};
/* The button rows, top to bottom, and what the page calls them */
static const int ROW_BUTTONS[12] = {NB_A, NB_B, NB_X, NB_Y, NB_L, NB_R, NB_ZL, NB_ZR, NB_PLUS, NB_MINUS, NB_LSTICK, NB_RSTICK};
static const char *const BUTTON_LABEL[NB_COUNT] = {"Button A", "Button B", "Button X", "Button Y", "Left stick press", "Right stick press",
                                                   "L", "R", "ZL", "ZR", "Plus (+)", "Minus (-)", "Left", "Up", "Right", "Down"};
/* The console's names, short, for the picture's tags */
static const char *const BUTTON_TAG[NB_COUNT] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "+", "-", "<", "^", ">", "v"};
/* < > on a button row: the controls offered, in this order */
static const int OFFERED[] = {PC_SOUTH, PC_EAST, PC_WEST, PC_NORTH, PC_LSHOULDER, PC_RSHOULDER, PC_LTRIGGER, PC_RTRIGGER, PC_START, PC_BACK,
                              PC_LSTICK, PC_RSTICK, PC_DPUP, PC_DPDOWN, PC_DPLEFT, PC_DPRIGHT, PC_NONE};
#define NOFFERED (int)(sizeof OFFERED / sizeof OFFERED[0])

static void defaults(PadSettings *s) {
    memset(s, 0, sizeof *s);
    s->layout = 0;
    s->deadzone_left = s->deadzone_right = 0;
    s->snapback_left = s->snapback_right = 0;
    s->rumble = 100;
    for (int b = 0; b < NB_COUNT; b++) s->map[b] = PAD_DEFAULT;
}

int pad_menu_effective(const PadSettings *s, int kind, int b) {
    if (b < 0 || b >= NB_COUNT) return PC_NONE;
    if (s->map[b] != PAD_DEFAULT) return s->map[b];
    /* (hle_pad.c's dev_configure) positional: A the right face button, B the bottom, X the top, Y the left;
     * labels, on a pad with A B X Y printed as an Xbox pad has them: by the letter */
    static const int POSITIONAL[NB_COUNT] = {PC_EAST, PC_SOUTH, PC_NORTH, PC_WEST, PC_LSTICK, PC_RSTICK, PC_LSHOULDER, PC_RSHOULDER,
                                             PC_LTRIGGER, PC_RTRIGGER, PC_START, PC_BACK, PC_DPLEFT, PC_DPUP, PC_DPRIGHT, PC_DPDOWN};
    if (s->layout == 1 && kind == PK_XBOX && b <= NB_Y) {
        static const int LABELS[4] = {PC_SOUTH, PC_EAST, PC_WEST, PC_NORTH};
        return LABELS[b];
    }
    return POSITIONAL[b];
}

const char *pad_control_name(int kind, int c) {
    static const char *const XBOX[PC_COUNT] = {"A", "B", "X", "Y", "View", "Guide", "Menu", "Left stick (L3)", "Right stick (R3)", "LB", "RB",
                                               "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Share", "Paddle 1", "Paddle 2",
                                               "Paddle 3", "Paddle 4", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "LT", "RT", "None"};
    static const char *const PS[PC_COUNT] = {"Cross", "Circle", "Square", "Triangle", "Share / Create", "PS", "Options", "L3", "R3", "L1", "R1",
                                             "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Mute", "Paddle 1", "Paddle 2",
                                             "Paddle 3", "Paddle 4", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "L2", "R2", "None"};
    static const char *const NINTENDO[PC_COUNT] = {"B", "A", "Y", "X", "Minus (-)", "Home", "Plus (+)", "Left stick press", "Right stick press",
                                                   "L", "R", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Capture", "Paddle 1",
                                                   "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5",
                                                   "Misc 6", "ZL", "ZR", "None"};
    if (c < 0 || c >= PC_COUNT) c = PC_NONE;
    return kind == PK_PS ? PS[c] : kind == PK_NINTENDO ? NINTENDO[c] : XBOX[c];
}

/* ---- the model ------------------------------------------------------------------------------ */
void pad_menu_init(PadMenu *m, const PadSettings *s) {
    memset(m, 0, sizeof *m);
    m->s = m->initial = *s;
    m->row = CROW_LAYOUT;
}

int pad_menu_row_button(int row) { return row >= CROW_BUTTON && row <= CROW_BUTTON_END ? ROW_BUTTONS[row - CROW_BUTTON] : -1; }

int pad_menu_visible(const PadMenu *m, int row) {
    if (row < 0 || row >= CROWS) return 0;
    if (row == CROW_PLAY) return !m->no_play;
    if (row == CROW_PAD || row == CROW_PICTURE) return 1;
    int sub = row >= CROW_DZ_LEFT && row <= CROW_DZ_BACK;
    return m->dead_zones ? sub : !sub;
}

int pad_menu_selectable(const PadMenu *m, int row) { return pad_menu_visible(m, row) && row != CROW_PAD && row != CROW_PICTURE; }

int pad_menu_takes_back(const PadMenu *m) { return m->listening || m->dead_zones; }

const char *pad_menu_label(int row) {
    int b = pad_menu_row_button(row);
    if (b >= 0) return BUTTON_LABEL[b];
    switch (row) {
    case CROW_PAD: return "Controller";
    case CROW_LAYOUT: return "Face buttons";
    case CROW_DEADZONE: return "Stick dead zones";
    case CROW_DZ_LEFT: return "Left stick";   /* (the dead zones' page: each stick's) */
    case CROW_DZ_RIGHT: return "Right stick";
    case CROW_SNAP_LEFT: return "Left snapback";
    case CROW_SNAP_RIGHT: return "Right snapback";
    case CROW_DZ_TEST: return "Test the sticks";
    case CROW_DZ_BACK: return "Back";
    case CROW_RUMBLE: return "Rumble";
    case CROW_RESET: return "Reset to defaults";
    case CROW_PICTURE: return "Layout";
    case CROW_PLAY: return "Play";
    default: return "";
    }
}

/* The picture ('\16'): its fields, '\t' between them - the kind (PK_*), the control lit (PC_*, or -1),
 * listening (0/1), the left and right sticks' dead zones (percent), the sticks (lx,ly,rx,ry), the console
 * button each control presses (PC_SOUTH.. PC_RTRIGGER, ',' between them, "" none), a line under the
 * picture, and 1 for the sticks' test instead of the pad */
static void picture_value(const PadMenu *m, char *out, unsigned n) {
    int b = pad_menu_row_button(m->row), lit = b >= 0 ? pad_menu_effective(&m->s, m->kind, b) : -1;
    char tags[PC_NONE * 12 + 1] = "";
    size_t l = 0;
    for (int c = 0; c < PC_NONE; c++) {   /* (two console buttons on one control: "R/ZR") */
        char tag[24] = "";
        for (int k = 0; k < NB_COUNT; k++)
            if (pad_menu_effective(&m->s, m->kind, k) == c && strlen(tag) + strlen(BUTTON_TAG[k]) + 2 < sizeof tag)
                snprintf(tag + strlen(tag), sizeof tag - strlen(tag), "%s%s", *tag ? "/" : "", BUTTON_TAG[k]);
        l += (size_t)snprintf(tags + l, sizeof tags - l, "%s%s", c ? "," : "", tag);
        if (l >= sizeof tags) l = sizeof tags - 1;
    }
    char line[128];
    if (m->listening && b >= 0) snprintf(line, sizeof line, "Press a button for %s", BUTTON_LABEL[b]);
    else if (b >= 0) snprintf(line, sizeof line, "%s  <-  %s", BUTTON_LABEL[b], pad_control_name(m->kind, lit));
    else if (m->testing) snprintf(line, sizeof line, "Green: what the game gets");
    else if (m->row == CROW_SNAP_LEFT || m->row == CROW_SNAP_RIGHT) snprintf(line, sizeof line, "Let go of a stick fast: the bounce back is ignored");
    else if (m->row == CROW_DEADZONE || m->dead_zones) snprintf(line, sizeof line, "Move the sticks: inside the ring is ignored");
    else snprintf(line, sizeof line, "%s", m->pad[0] ? m->pad : "No controller: plug one in");
    snprintf(out, n, "\16%d\t%d\t%d\t%d\t%d\t%d,%d,%d,%d\t%s\t%s\t%d\t%d,%d,%d,%d\t%d,%d\t%d,%d", m->kind, lit, m->listening && b >= 0,
             m->s.deadzone_left, m->s.deadzone_right, m->axes[0], m->axes[1], m->axes[2], m->axes[3], tags, line, m->testing, m->game_axes[0],
             m->game_axes[1], m->game_axes[2], m->game_axes[3], m->s.snapback_left, m->s.snapback_right, m->snap_flash[0], m->snap_flash[1]);
}

void pad_menu_value(const PadMenu *m, int row, char *out, unsigned n) {
    int b = pad_menu_row_button(row);
    if (b >= 0) {
        int c = pad_menu_effective(&m->s, m->kind, b);
        if (m->listening && row == m->row) snprintf(out, n, "Press a button...");
        else snprintf(out, n, "%s%s", pad_control_name(m->kind, c), m->s.map[b] == PAD_DEFAULT ? "" : "  *");
        return;
    }
    switch (row) {
    case CROW_PAD: snprintf(out, n, "\1%s", m->pad[0] ? m->pad : "none connected - the keyboard plays"); return;
    case CROW_LAYOUT: snprintf(out, n, "%s", m->s.layout ? "By the printed letters" : "By position (as a Switch)"); return;
    case CROW_DEADZONE: snprintf(out, n, "\21Left %d%%, right %d%%", m->s.deadzone_left, m->s.deadzone_right); return;   /* ('\21': it opens a page) */
    case CROW_DZ_LEFT: case CROW_DZ_RIGHT: {
        int dz = row == CROW_DZ_LEFT ? m->s.deadzone_left : m->s.deadzone_right;
        snprintf(out, n, dz ? "%d%%" : "Off (the game's own)", dz);
        return;
    }
    case CROW_SNAP_LEFT: case CROW_SNAP_RIGHT:
        snprintf(out, n, "%s", (row == CROW_SNAP_LEFT ? m->s.snapback_left : m->s.snapback_right) ? "On - the bounce ignored" : "Off");
        return;
    case CROW_DZ_TEST: snprintf(out, n, "\21%s", m->testing ? "Testing - move the sticks" : "Start"); return;
    case CROW_DZ_BACK: snprintf(out, n, "\21Controls"); return;
    case CROW_RUMBLE: snprintf(out, n, m->s.rumble ? "%d%%" : "Off", m->s.rumble); return;
    case CROW_RESET: snprintf(out, n, "\5Enter: every button as the layout has it"); return;
    case CROW_PICTURE: picture_value(m, out, n); return;
    default: if (n) out[0] = 0; return;
    }
}

static void move(PadMenu *m, int dir) {
    for (int i = 0; i < CROWS; i++) {
        m->row = (m->row + dir + CROWS) % CROWS;
        if (pad_menu_selectable(m, m->row)) return;
    }
}

static void assign(PadMenu *m, int b, int c) {   /* c to console button b, PAD_DEFAULT when the layout has it so: no line in the file */
    PadSettings plain;
    defaults(&plain);
    plain.layout = m->s.layout;
    m->s.map[b] = c == pad_menu_effective(&plain, m->kind, b) ? PAD_DEFAULT : c;
}

static void open_dead_zones(PadMenu *m) { m->dead_zones = 1; m->row = CROW_DZ_LEFT; }
static void close_dead_zones(PadMenu *m) { m->dead_zones = m->testing = 0; m->row = CROW_DEADZONE; }

static void step(PadMenu *m, int dir) {
    int b = pad_menu_row_button(m->row);
    if (b >= 0) {   /* the next control offered, from the one it has now */
        int now = pad_menu_effective(&m->s, m->kind, b), i = 0;
        while (i < NOFFERED && OFFERED[i] != now) i++;
        if (i == NOFFERED) i = dir > 0 ? -1 : 0;
        assign(m, b, OFFERED[((i + dir) % NOFFERED + NOFFERED) % NOFFERED]);
        return;
    }
    switch (m->row) {
    case CROW_LAYOUT: m->s.layout = !m->s.layout; break;
    case CROW_DZ_LEFT: case CROW_DZ_RIGHT: {
        int *dz = m->row == CROW_DZ_LEFT ? &m->s.deadzone_left : &m->s.deadzone_right;
        *dz += dir * 5;
        if (*dz < 0) *dz = 0;
        if (*dz > 50) *dz = 50;
        break;
    }
    case CROW_DEADZONE: if (dir > 0) open_dead_zones(m); break;
    case CROW_SNAP_LEFT: m->s.snapback_left = !m->s.snapback_left; break;
    case CROW_SNAP_RIGHT: m->s.snapback_right = !m->s.snapback_right; break;
    case CROW_RUMBLE: m->s.rumble += dir * 10; if (m->s.rumble < 0) m->s.rumble = 0; if (m->s.rumble > 100) m->s.rumble = 100; break;
    }
}

int pad_menu_event(PadMenu *m, int event, int c) {
    int b = pad_menu_row_button(m->row);
    if (m->listening) {   /* (only a control, or going back, while it waits) */
        if (event == MENU_PADKEY && b >= 0 && c >= 0 && c < PC_NONE) assign(m, b, c);
        if (event == MENU_PADKEY || event == MENU_BACK || event == MENU_ACCEPT) m->listening = 0;
        return PAD_EV_NONE;
    }
    if (event == MENU_BACK) {   /* (the dead zones' page: the test stopped, then back to the rest) */
        if (m->testing) m->testing = 0; else if (m->dead_zones) close_dead_zones(m);
        return PAD_EV_NONE;
    }
    switch (event) {
    case MENU_UP: move(m, -1); break;
    case MENU_DOWN: move(m, 1); break;
    case MENU_LEFT: step(m, -1); break;
    case MENU_RIGHT: step(m, 1); break;
    case MENU_ACCEPT:
        if (b >= 0) { m->listening = 1; break; }
        if (m->row == CROW_DEADZONE) { open_dead_zones(m); break; }
        if (m->row == CROW_DZ_TEST) { m->testing = !m->testing; break; }
        if (m->row == CROW_SNAP_LEFT || m->row == CROW_SNAP_RIGHT) { step(m, 1); break; }
        if (m->row == CROW_DZ_BACK) { close_dead_zones(m); break; }
        if (m->row == CROW_RESET) {   /* (the buttons only: the rest as it is) */
            PadSettings keep = m->s;
            defaults(&m->s);
            m->s.rumble = keep.rumble, m->s.deadzone_left = keep.deadzone_left, m->s.deadzone_right = keep.deadzone_right, m->s.layout = keep.layout;
            m->s.snapback_left = keep.snapback_left, m->s.snapback_right = keep.snapback_right;
            break;
        }
        if (m->row == CROW_PLAY) return PAD_EV_PLAY;
        move(m, 1);
        break;
    }
    return PAD_EV_NONE;
}

/* ---- controllers.ini ------------------------------------------------------------------------ */
static const char *ini_path(void) {
    const char *p = getenv("SSBU_CONTROLLERS");
    return p && *p ? p : "controllers.ini";
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r' || s[l - 1] == '\n')) s[--l] = 0;
    return s;
}

static int is_default_section(const char *line) {   /* "[device default]" or "[device]" (hle_pad.c reads both so) */
    char buf[128];
    snprintf(buf, sizeof buf, "%s", line);
    char *s = trim(buf), *end;
    if (*s != '[' || !(end = strchr(s, ']'))) return -1;   /* (not a section) */
    *end = 0;
    s = trim(s + 1);
    if (_strnicmp(s, "device", 6) || (s[6] && s[6] != ' ' && s[6] != '\t')) return 0;
    s = trim(s + 6);
    return !*s || !_stricmp(s, "default");
}

static int control_of(const char *name) {
    for (int c = 0; c < PC_COUNT; c++) if (!_stricmp(name, CONTROL_KEY[c])) return c;
    return -1;
}

void pad_settings_load(PadSettings *s) {
    defaults(s);
    FILE *f = fopen(ini_path(), "r");
    if (!f) return;
    char line[512];
    int in = 0;
    while (fgets(line, sizeof line, f)) {
        int sec = is_default_section(line);
        if (sec >= 0) { in = sec; continue; }
        char *t = trim(line), *eq;
        if (!in || *t == ';' || *t == '#' || !(eq = strchr(t, '='))) continue;
        for (char *p = eq; *p; p++) if ((*p == ';' || *p == '#') && (p[-1] == ' ' || p[-1] == '\t')) { *p = 0; break; }
        *eq = 0;
        char *key = trim(t), *val = trim(eq + 1);
        if (!_stricmp(key, "layout")) s->layout = !_stricmp(val, "labels");
        else if (!_stricmp(key, "deadzone")) s->deadzone_left = s->deadzone_right = atoi(val) < 0 ? 0 : atoi(val) > 50 ? 50 : atoi(val);
        else if (!_stricmp(key, "deadzone_left")) s->deadzone_left = atoi(val) < 0 ? 0 : atoi(val) > 50 ? 50 : atoi(val);
        else if (!_stricmp(key, "deadzone_right")) s->deadzone_right = atoi(val) < 0 ? 0 : atoi(val) > 50 ? 50 : atoi(val);
        else if (!_stricmp(key, "snapback_left") || !_stricmp(key, "snapback_right")) {
            int on = !_stricmp(val, "on") || !_stricmp(val, "1") || !_stricmp(val, "true") || !_stricmp(val, "yes");
            if (!_stricmp(key, "snapback_left")) s->snapback_left = on; else s->snapback_right = on;
        }
        else if (!_stricmp(key, "rumble")) s->rumble = atoi(val) < 0 ? 0 : atoi(val) > 100 ? 100 : atoi(val);
        else
            for (int b = 0; b < NB_COUNT; b++)
                if (!_stricmp(key, BUTTON_KEY[b])) {   /* (the first control of a list: the page offers one) */
                    char first[32];
                    snprintf(first, sizeof first, "%.*s", (int)strcspn(val, ", \t"), val);
                    int c = control_of(first);
                    s->map[b] = c >= 0 ? c : PAD_DEFAULT;
                }
    }
    fclose(f);
}

/* The keys the page writes in [device default] (the others there, and every other section, kept) */
static int managed(const char *key) {
    if (!_stricmp(key, "layout") || !_stricmp(key, "deadzone") || !_stricmp(key, "deadzone_left") || !_stricmp(key, "deadzone_right") ||
        !_stricmp(key, "snapback_left") || !_stricmp(key, "snapback_right") || !_stricmp(key, "rumble"))
        return 1;
    for (int i = 0; i < 12; i++) if (!_stricmp(key, BUTTON_KEY[ROW_BUTTONS[i]])) return 1;
    return 0;
}

static void write_settings(FILE *w, const PadSettings *s) {
    fprintf(w, "layout = %s\ndeadzone_left = %d\ndeadzone_right = %d\nsnapback_left = %s\nsnapback_right = %s\nrumble = %d\n",
            s->layout ? "labels" : "positional", s->deadzone_left, s->deadzone_right, s->snapback_left ? "on" : "off",
            s->snapback_right ? "on" : "off", s->rumble);
    for (int i = 0; i < 12; i++) {
        int b = ROW_BUTTONS[i];
        if (s->map[b] != PAD_DEFAULT) fprintf(w, "%s = %s\n", BUTTON_KEY[b], CONTROL_KEY[s->map[b]]);
    }
}

int pad_settings_save(const PadSettings *s) {
    const char *path = ini_path();
    char *old = NULL;
    size_t len = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        old = malloc((size_t)(size > 0 ? size : 0) + 1);
        len = old ? fread(old, 1, (size_t)(size > 0 ? size : 0), f) : 0;
        if (old) old[len] = 0;
        fclose(f);
    } else {
        const char *d = pad_default_ini();
        old = malloc(strlen(d) + 1);
        if (old) strcpy(old, d);
    }
    if (!old) return 0;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *w = fopen(tmp, "w");
    if (!w) { free(old); return 0; }
    int in = 0, written = 0;
    for (char *line = old, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        int sec = is_default_section(line);
        if (sec >= 0) {
            if (in && !written) { write_settings(w, s); written = 1; }   /* (at the end of [device default]) */
            in = sec;
            fprintf(w, "%s\n", line);
            continue;
        }
        if (in) {
            char buf[512];
            snprintf(buf, sizeof buf, "%s", line);
            char *t = trim(buf), *eq = strchr(t, '=');
            if (*t != ';' && *t != '#' && eq) {
                *eq = 0;
                if (managed(trim(t))) continue;   /* (written anew, below) */
            }
            if (!*t && !written) { write_settings(w, s); written = 1; }   /* (before the blank line that ends it) */
        }
        fprintf(w, "%s\n", line);
    }
    if (!written) {
        if (!in) fprintf(w, "\n[device default]\n");
        write_settings(w, s);
    }
    free(old);
    if (fclose(w)) return 0;
    if (rename(tmp, path)) { remove(path); if (rename(tmp, path)) return 0; }
    return 1;
}
