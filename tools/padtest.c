/* Offline checks of the controller layer (runtime/hle_pad.c) without a pad, SDL or the game:
 * configuration, button mapping, sticks, player slots and rumble pacing.
 *
 *   python -c "import sys; sys.path.insert(0,'tools'); import cc, subprocess; subprocess.run([cc.CLANG, '/nologo', '/O1', *cc.CFLAGS, 'tools/padtest.c', '/Febuild/padtest.exe'], check=True)"
 *   build\padtest.exe
 *
 * Exit code 0 when every check passes. */
#include "../runtime/hle_pad.c"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void load_text(const char *text) {
    memset(&cfg, 0, sizeof cfg);
    cfg.keyboard_slot = 0; cfg.background = 1;
    cfg.def.layout = cfg.def.deadzone = cfg.def.rumble = cfg.def.trigger = -1;
    char path[MAX_PATH], dir[MAX_PATH];
    GetTempPathA(sizeof dir, dir);
    GetTempFileNameA(dir, "pad", 0, path);
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
    f = fopen(path, "r");
    config_parse(f);
    fclose(f);
    DeleteFileA(path);
}

static Dev make_dev(const char *guid, const char *serial, const char *path, const char *name, const char *type) {
    Dev d = {0};
    d.used = 1; d.xinput = -1; d.slot = -1;
    snprintf(d.guid, sizeof d.guid, "%s", guid);
    snprintf(d.serial, sizeof d.serial, "%s", serial);
    snprintf(d.path, sizeof d.path, "%s", path);
    snprintf(d.name, sizeof d.name, "%s", name);
    snprintf(d.type, sizeof d.type, "%s", type);
    return d;
}

static u64 press(const Dev *d, u32 raw_buttons, i32 lt, i32 rt) {
    Raw raw = {raw_buttons, {0, 0, 0, 0, lt, rt}};
    PadState o;
    dev_state(d, &raw, &o);
    return o.buttons;
}

/* Connect a device into devs[i] the way sdl_open does. */
static int plug(int i, Dev d) {
    devs[i] = d;
    devs[i].slot = dev_assign(&devs[i]);
    if (devs[i].slot >= 0) dev_remember(&devs[i]);
    return devs[i].slot;
}

static void test_config(void) {
    load_text("[general]\nkeyboard_player = 3\nbackground = 0\n[players]\n2 = DualSense Wireless Controller ; mine\n9 = x\n"
              "[device default]\nlayout = labels\ndeadzone = 80\nrumble = 40\ntrigger_threshold = 50\nZR = rightshoulder, misc1\n"
              "[device ps5]\nrumble = 70\nA = south\n[device 0300abcd]\nlayout = positional\n");
    CHECK(cfg.keyboard_slot == 2, "keyboard slot %d", cfg.keyboard_slot);
    CHECK(cfg.background == 0, "background");
    CHECK(!strcmp(cfg.pin[1], "DualSense Wireless Controller"), "pin '%s' (comment after a value)", cfg.pin[1]);
    CHECK(cfg.def.layout == 1 && cfg.def.deadzone == 50 && cfg.def.rumble == 40 && cfg.def.trigger == 50, "defaults (deadzone clamped)");
    CHECK(cfg.def.has_map[N_ZR] && cfg.def.nmap[N_ZR] == 2, "ZR remap with two controls");
    CHECK(cfg.ndev == 2 && !strcmp(cfg.dev[0].match, "ps5") && cfg.dev[0].rumble == 70 && cfg.dev[0].has_map[N_A], "[device ps5]");
    load_text("keyboard_player = 0\n[general]\nkeyboard_player = 0\n");
    CHECK(cfg.keyboard_slot == -1, "no keyboard");
}

static void test_mapping(void) {
    load_text("");
    Dev d = make_dev("g", "", "", "Xbox", "xboxone");
    int xbox_labels[4] = {N_A, N_B, N_X, N_Y};
    dev_configure(&d, xbox_labels);
    CHECK(press(&d, 1u << RB_SOUTH, 0, 0) == 1ull << N_B, "positional: bottom face button is B");
    CHECK(press(&d, 1u << RB_EAST, 0, 0) == 1ull << N_A, "positional: right is A");
    CHECK(press(&d, 1u << RB_NORTH, 0, 0) == 1ull << N_X, "positional: top is X");
    CHECK(press(&d, 1u << RB_WEST, 0, 0) == 1ull << N_Y, "positional: left is Y");
    CHECK(press(&d, 1u << RB_START | 1u << RB_BACK, 0, 0) == (1ull << N_PLUS | 1ull << N_MINUS), "start/back");
    CHECK(press(&d, 1u << RB_DPAD_UP | 1u << RB_DPAD_LEFT, 0, 0) == (1ull << N_UP | 1ull << N_LEFT), "d-pad");
    CHECK(press(&d, 0, 8000, 0) == 0 && press(&d, 0, 8200, 32767) == (1ull << N_ZL | 1ull << N_ZR), "triggers at 25%%");
    CHECK(press(&d, 1u << RB_GUIDE | 1u << RB_MISC1, 0, 0) == 0, "guide and misc1 unmapped");

    load_text("[device default]\nlayout = labels\n");
    dev_configure(&d, xbox_labels);
    CHECK(press(&d, 1u << RB_SOUTH, 0, 0) == 1ull << N_A, "labels: Xbox A is A");
    CHECK(press(&d, 1u << RB_NORTH, 0, 0) == 1ull << N_Y, "labels: Xbox Y is Y");
    int ps_labels[4] = {-1, -1, -1, -1};
    dev_configure(&d, ps_labels);
    CHECK(press(&d, 1u << RB_SOUTH, 0, 0) == 1ull << N_B, "labels on a PlayStation pad stay positional");
    int nintendo_labels[4] = {N_B, N_A, N_Y, N_X};
    dev_configure(&d, nintendo_labels);
    CHECK(press(&d, 1u << RB_SOUTH, 0, 0) == 1ull << N_B, "labels on a Nintendo pad = positional");

    load_text("[device default]\nZR = rightshoulder\n[device xboxone]\nA = south\nB = none\ntrigger_threshold = 90\n");
    dev_configure(&d, xbox_labels);
    CHECK(press(&d, 1u << RB_RIGHT_SHOULDER, 0, 0) == (1ull << N_R | 1ull << N_ZR), "ZR = rightshoulder (R keeps it too)");
    CHECK(press(&d, 1u << RB_SOUTH, 0, 0) == 1ull << N_A, "per-type remap A = south");
    CHECK(press(&d, 0, 0, 32767) == 0, "ZR remapped away from the trigger");
    CHECK(press(&d, 0, 26000, 0) == 0 && press(&d, 0, 30000, 0) == 1ull << N_ZL, "type's trigger threshold 90%%");
    Dev other = make_dev("g2", "", "", "Other", "ps4");
    dev_configure(&other, ps_labels);
    CHECK(press(&other, 1u << RB_SOUTH, 0, 0) == 1ull << N_B, "another type keeps positional");
}

static void test_sticks(void) {
    i32 x, y;
    stick(0, 0, -32768, &x, &y);
    CHECK(x == 0 && y == 32767, "up is +y, clamped: %d %d", x, y);
    stick(0, 0, 32767, &x, &y);
    CHECK(y == -32767, "down: %d", y);
    stick(0, 32767, -32768, &x, &y);
    CHECK(x > 23000 && x < 23300 && y > 23000 && y < 23300 && (double)x * x + (double)y * y <= 32767.0 * 32767.0 + 70000,
          "corner inside the circle: %d %d", x, y);
    stick(0, 1000, 0, &x, &y);
    CHECK(x == 1000 && y == 0, "no deadzone passes small values: %d", x);
    stick(10, 3000, 0, &x, &y);
    CHECK(x == 0, "inside a 10%% deadzone: %d", x);
    stick(10, 32767, 0, &x, &y);
    CHECK(x == 32767, "full travel with a deadzone: %d", x);
    stick(10, 6554, 0, &x, &y);
    CHECK(x > 3600 && x < 3700, "rescaled past the deadzone: %d", x);
    XPad xp = {0x1000 | 0x0008, 255, 0, -32768, -32768, 0, 32767};
    Raw raw;
    xinput_raw(&xp, &raw);
    CHECK(raw.buttons == (1u << RB_SOUTH | 1u << RB_DPAD_RIGHT) && raw.axis[RA_LEFTY] == 32767 && raw.axis[RA_RIGHTY] == -32767 &&
          raw.axis[RA_LEFT_TRIGGER] == 32767, "XInput report in SDL terms");
}

static void test_slots(void) {
    load_text("[players]\n4 = ps5\n");
    memset(devs, 0, sizeof devs);
    nknown = 0;
    CHECK(plug(0, make_dev("xbox", "", "\\\\?\\HID#1", "Xbox Controller", "xboxone")) == 0, "first pad: player 1");
    CHECK(plug(1, make_dev("xbox", "", "\\\\?\\HID#2", "Xbox Controller", "xboxone")) == 1, "second: player 2");
    CHECK(plug(2, make_dev("ds", "AA:BB", "\\\\?\\HID#3", "DualSense", "ps5")) == 3, "pinned type: player 4");
    CHECK(plug(3, make_dev("other", "", "\\\\?\\HID#4", "Pad", "ps4")) == 2, "next free skips the pinned player");
    devs[0].used = 0; devs[1].used = 0; devs[2].used = 0;   /* three unplugged */
    CHECK(plug(1, make_dev("xbox", "", "\\\\?\\HID#2", "Xbox Controller", "xboxone")) == 1, "same path: player 2 again");
    CHECK(plug(0, make_dev("xbox", "", "\\\\?\\HID#9", "Xbox Controller", "xboxone")) == 0, "same GUID, new path: the free one it had");
    CHECK(plug(2, make_dev("ds", "AA:BB", "\\\\?\\HID#7", "DualSense", "ps5")) == 3, "the pinned pad comes back to its player");
    CHECK(plug(4, make_dev("ds4", "S1", "\\\\?\\HID#10", "DS4", "ps4")) == 4, "player 5");
    CHECK(plug(5, make_dev("ds4", "S2", "\\\\?\\HID#11", "DS4", "ps4")) == 5, "player 6");
    devs[4].used = 0; devs[5].used = 0;
    CHECK(plug(5, make_dev("ds4", "S2", "\\\\?\\HID#12", "DS4", "ps4")) == 5, "same serial, new path: its own player, not the first free");
    CHECK(plug(4, make_dev("ds4", "S1", "\\\\?\\HID#13", "DS4", "ps4")) == 4, "and the other one");
    for (int i = 6; i < 9; i++) {
        char g[8]; snprintf(g, sizeof g, "n%d", i);
        plug(i, make_dev(g, "", "", "Pad", "ps4"));
    }
    CHECK(devs[6].slot == 6 && devs[7].slot == 7 && devs[8].slot == -1, "eight players, then none: %d %d %d", devs[6].slot, devs[7].slot, devs[8].slot);
}

static void test_rumble(void) {
    load_text("[device default]\nrumble = 50\n");
    Dev d = make_dev("g", "", "", "Pad", "ps5");
    dev_configure(&d, NULL);
    d.slot = 5;
    u16 lo, hi;
    ULONGLONG t = GetTickCount64();
    pad_rumble(5, 1, 1.0f);
    pad_rumble(5, 2, 0.25f);
    CHECK(rumble_due(&d, t, &lo, &hi) && lo == 32767 && hi == 8191, "half strength: %u %u", lo, hi);
    CHECK(!rumble_due(&d, t + 50, &lo, &hi), "no resend within 100 ms");
    CHECK(rumble_due(&d, t + 100, &lo, &hi), "refresh after 100 ms");
    pad_rumble(5, 1, 0.5f);
    CHECK(!rumble_due(&d, t + 105, &lo, &hi), "a change waits 10 ms");
    CHECK(rumble_due(&d, t + 110, &lo, &hi) && lo == 16383, "then goes out: %u", lo);
    pad_rumble(5, 1, 0); pad_rumble(5, 2, 0);
    CHECK(rumble_due(&d, t + 130, &lo, &hi) && !lo && !hi, "stop");
    CHECK(!rumble_due(&d, t + 400, &lo, &hi), "nothing while stopped");
    pad_rumble(5, 1, 1.0f);
    CHECK(rumble_due(&d, t + 500, &lo, &hi) && lo, "start again");
    CHECK(rumble_due(&d, t + 3600, &lo, &hi) && !lo, "stopped when the game goes quiet for 3 s");
    CHECK(pad_rumble_value(5, 1) == 1.0f && pad_rumble_value(9, 1) == 0, "last value kept");
}

int main(void) {
    test_config();
    test_mapping();
    test_sticks();
    test_slots();
    test_rumble();
    printf("%d of %d checks passed\n", checks - failures, checks);
    return failures != 0;
}
