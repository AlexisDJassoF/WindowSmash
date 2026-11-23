/* runtime/pad_snap.h's snapback fix, on made-up stick movements (tests/test_pad_menu.py builds and runs
 * this). Exit 0 when every check passes, else what failed. */
#include <stdio.h>

#include "pad_snap.h"

static int failures;
#define CHECK(cond, what) do { if (!(cond)) { failures++; printf("FAIL: %s\n", what); } } while (0)

/* The stick at (x, y) at t ms through the fix: what comes out */
static void at(PadSnap *s, int t, int x, int y, int *ox, int *oy) {
    int32_t fx = x, fy = y;
    pad_snapback(s, &fx, &fy, (uint64_t)t);
    *ox = fx, *oy = fy;
}

int main(void) {
    int x, y;
    {   /* let go from full right: the bounce left is ignored, then the stick reads again */
        PadSnap s = {0};
        for (int t = 0; t <= 20; t += 4) at(&s, 1000 + t, 32767, 0, &x, &y);
        CHECK(x == 32767, "held right passes");
        at(&s, 1024, 2000, 0, &x, &y);                 /* (let go: near the centre) */
        at(&s, 1028, -14000, 0, &x, &y);
        CHECK(x == 0 && y == 0, "the bounce past the centre is ignored");
        at(&s, 1040, -6000, 0, &x, &y);
        CHECK(x == 0, "still settling: ignored");
        at(&s, 1100, -6000, 0, &x, &y);
        CHECK(x == -6000, "after the hold, the stick reads again");
    }
    {   /* a flick far the other way on purpose comes through at once */
        PadSnap s = {0};
        for (int t = 0; t <= 20; t += 4) at(&s, 2000 + t, 32767, 0, &x, &y);
        at(&s, 2024, 1000, 0, &x, &y);
        at(&s, 2028, -30000, 0, &x, &y);
        CHECK(x == -30000, "pushed far the other way: through");
    }
    {   /* pushed out again the same way: through */
        PadSnap s = {0};
        for (int t = 0; t <= 20; t += 4) at(&s, 3000 + t, 0, -32767, &x, &y);
        at(&s, 3024, 0, -3000, &x, &y);
        at(&s, 3032, 0, -25000, &x, &y);
        CHECK(y == -25000, "out again the same way: through");
    }
    {   /* a slow return is no bounce */
        PadSnap s = {0};
        int v = 32767;
        for (int t = 0; t <= 400; t += 8, v -= 700) at(&s, 4000 + t, v > 0 ? v : 0, 0, &x, &y);
        at(&s, 4408, -5000, 0, &x, &y);
        CHECK(x == -5000, "back slowly, then the other way: through");
    }
    {   /* small movements near the centre: never held */
        PadSnap s = {0};
        at(&s, 5000, 12000, 0, &x, &y);
        at(&s, 5004, 1000, 0, &x, &y);
        at(&s, 5008, -9000, 0, &x, &y);
        CHECK(x == -9000, "small wiggles pass");
    }
    if (!failures) printf("all passed\n");
    return failures != 0;
}
