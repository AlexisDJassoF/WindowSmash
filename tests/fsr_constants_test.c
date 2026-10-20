/* runtime/gpu_fsr.c's constants against AMD's own (third_party/fidelityfx-fsr1, compiled here for
 * the CPU): tests/test_fsr_constants.py builds and runs this. Prints "ok <cases>" or each case that
 * differs. */
#include <math.h>     /* (ffx_a.h's CPU code uses these without including them) */
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

#include "gpu_fsr.h"

int main(void) {
    static const float sizes[][8] = {   /* viewport, texture, output, offset */
        {1280, 720, 1920, 1080, 1920, 1080, 0, 0},     /* handheld, its crop of the docked-size buffer, to 1080p */
        {1280, 720, 1280, 720, 2560, 1440, 0, 0},
        {1920, 1080, 1920, 1080, 2560, 1440, 0, 0},    /* docked to 1440p */
        {1280, 720, 1920, 1080, 3840, 2160, 0, 0},     /* to 4K */
        {1280, 720, 1920, 1080, 1366, 768, 0, 0},
        {1152, 648, 1920, 1080, 2560, 1440, 16, 8},    /* a crop elsewhere in the buffer */
    };
    int cases = 0, bad = 0;
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        const float *s = sizes[k];
        AU1 c0[4], c1[4], c2[4], c3[4];
        FsrEasuConOffset(c0, c1, c2, c3, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
        uint32_t ref[16], mine[16];
        memcpy(ref, c0, 16); memcpy(ref + 4, c1, 16); memcpy(ref + 8, c2, 16); memcpy(ref + 12, c3, 16);
        fsr_easu_constants(mine, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
        cases++;
        for (int i = 0; i < 16; i++)
            if (ref[i] != mine[i]) { printf("easu case %u word %d: %08x, AMD's %08x\n", k, i, mine[i], ref[i]); bad++; }
    }
    for (int t = 0; t <= 40; t++) {   /* sharpness 0.0 to 2.0 */
        float sharp = t * 0.05f;
        AU1 ref[4];
        uint32_t mine[4];
        FsrRcasCon(ref, sharp);
        fsr_rcas_constants(mine, sharp);
        cases++;
        for (int i = 0; i < 4; i++)
            if (ref[i] != mine[i]) { printf("rcas %.2f word %d: %08x, AMD's %08x\n", sharp, i, mine[i], ref[i]); bad++; }
    }
    if (!bad) printf("ok %d\n", cases);
    return bad != 0;
}
