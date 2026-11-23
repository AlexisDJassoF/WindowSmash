/* Snapback: a stick pushed far out and let go springs back past the centre, for a few tens of
 * milliseconds, before it settles - and the game reads a flick the other way (a turn, a dash back).
 * The fix (per stick, controllers.ini's snapback_left / snapback_right, off by default): once a stick
 * has fallen fast from far out (from 60% or more to half of that within SNAP_FALL_MS), it reads as
 * centred for SNAP_HOLD_MS, unless it is pushed out again the way it came, or pushed far (80%) the
 * other way on purpose. Used by hle_pad.c for the game, and by the start-up menu's stick test
 * (linux/window.c), the same, so the test shows what the game gets. */
#pragma once
#include <math.h>
#include <stdint.h>

#define SNAP_FALL_MS 40
#define SNAP_HOLD_MS 60

typedef struct PadSnap {
    double peak, px, py;    /* the farthest out lately (its vector), and when */
    uint64_t peak_at;
    double dx, dy;          /* the way it came back from (a unit vector) while held */
    uint64_t until;         /* held centred until then (ms) */
} PadSnap;

/* x, y (-32768..32767, either sign convention) filtered in place at now (ms); 1 while it is held centred */
static inline int pad_snapback(PadSnap *s, int32_t *x, int32_t *y, uint64_t now) {
    double fx = *x / 32767.0, fy = *y / 32767.0, m = sqrt(fx * fx + fy * fy);
    if (m >= s->peak || now - s->peak_at > SNAP_FALL_MS) { s->peak = m; s->px = fx; s->py = fy; s->peak_at = now; }
    if (s->peak >= 0.6 && m <= s->peak * 0.5 && now - s->peak_at <= SNAP_FALL_MS) {   /* let go: the bounce comes next */
        s->dx = s->px / s->peak;
        s->dy = s->py / s->peak;
        s->until = now + SNAP_HOLD_MS;
        s->peak = m, s->px = fx, s->py = fy, s->peak_at = now;
    }
    if (now >= s->until) return 0;
    double along = fx * s->dx + fy * s->dy;
    if (along > 0.5 || along < -0.8) { s->until = 0; return 0; }   /* (pushed again: out the same way, or far the other way) */
    *x = *y = 0;
    return 1;
}
