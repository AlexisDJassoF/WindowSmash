/* runtime/gpu_scale.c driven from tests/test_gpu_scale.py: prints "ok" or what failed. */
#include <math.h>
#include <stdio.h>

#include "gpu_scale.h"

static int bad;
#define CHECK(cond, ...) do { if (!(cond)) { printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)

int main(void) {
    /* which render targets: (w, h, screen w, h, scale) -> expected */
    static const struct { unsigned w, h, sw, sh; float s, want; } rule[] = {
        {1920, 1080, 1920, 1080, 0.75f, 0.75f},   /* docked scene */
        {1920, 1080, 1280, 720, 0.75f, 0.75f},    /* handheld: the game's 1080p buffer, cropped */
        {1280, 720, 1280, 720, 0.5f, 0.5f},
        {960, 540, 1920, 1080, 0.75f, 1.0f},      /* bloom levels */
        {1024, 1024, 1920, 1080, 0.75f, 1.0f},    /* a shadow map */
        {2048, 512, 1920, 1080, 0.75f, 1.0f},     /* wide but short */
        {1920, 1080, 1920, 1080, 1.0f, 1.0f},
        {2560, 1440, 1920, 1080, 2.0f, 2.0f},
    };
    for (unsigned i = 0; i < sizeof rule / sizeof *rule; i++)
        CHECK(scale_for(rule[i].w, rule[i].h, rule[i].sw, rule[i].sh, rule[i].s) == rule[i].want, "rule %u", i);
    CHECK(scaled_size(1920, 0.75f) == 1440 && scaled_size(1080, 0.75f) == 810, "75%% of 1080p");
    CHECK(scaled_size(1280, 0.5f) == 640 && scaled_size(720, 0.5f) == 360, "50%% of 720p");
    CHECK(scaled_size(1, 0.5f) == 1 && scaled_size(0, 0.5f) == 1, "at least 1");
    CHECK(scaled_size(1080, 1.5f) == 1620 && scaled_size(777, 1.0f) == 777, "150%%, 100%%");
    long r[4] = {1, 1, 3, 3};
    scale_rect(r, 0.75f);
    CHECK(r[0] == 0 && r[1] == 0 && r[2] == 3 && r[3] == 3, "scissor outwards: %ld %ld %ld %ld", r[0], r[1], r[2], r[3]);
    long u[4] = {10, 20, 30, 40};
    scale_rect(u, 1.0f);
    CHECK(u[0] == 10 && u[1] == 20 && u[2] == 30 && u[3] == 40, "scissor unscaled");
    /* unscaled, the shaders' fix-ups change nothing, anywhere */
    for (int y = -2; y < 2200; y += 7)
        for (int x = 0; x < 4000; x += 13) {
            int p[2] = {x, y};
            scale_texel(p, 0.0f);
            if (p[0] != x || p[1] != y) { CHECK(0, "texel %d,%d unscaled -> %d,%d", x, y, p[0], p[1]); x = 99999; y = 99999; }
            float c = (float)x + 0.5f;
            if (scale_position(c, 0.0f) != c) { CHECK(0, "position %g unscaled", c); x = 99999; y = 99999; }
        }
    /* scaled, a texel lands inside the texture and in order */
    int last = -1;
    for (int x = 0; x < 1920; x++) {
        int p[2] = {x, 0};
        scale_texel(p, -0.25f);
        if (p[0] < last || p[0] < 0 || p[0] >= 1440) { CHECK(0, "75%% texel %d -> %d", x, p[0]); break; }
        last = p[0];
    }
    CHECK(last == 1439, "75%%: the last texel is the last one (%d)", last);
    /* the window position: physical pixel centres back to logical ones */
    CHECK(fabsf(scale_position(1439.5f, 1.0f / 0.75f - 1.0f) - 1919.33f) < 0.01f, "75%% position");
    if (!bad) printf("ok\n");
    return bad != 0;
}
