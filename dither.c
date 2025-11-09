/*
 * (c) Copyright 2001, 2003 - 2004, 2025 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "defs.h"
#include "dai.h"
#include "dither.h"
#include "shmalloc.h"
#include "emalloc.h"
#include "pinfo.h"

/* desired spacing between channels in random number table in seconds */
#define RANDTAB_SPACING 10
#define MIN_RANDTAB_SPACING 1

int8_t *dither_randtab;
int dither_randtab_size;
void *dither_randmap = NULL;

static int realsize;

/*
 * Maximally equidistributed combined Tausworthe generator by L’Ecuyer.
 *
 * Generates pseudorandom numbers between 0x0 - 0xFFFFFFFF
 *
 * Note 2025: there's an improved version of Tausworthe, this is the older
 * implementation, but for this context its plenty good enough.
 */
static inline uint32_t
tausrand(uint32_t state[3])
{
#define TAUSWORTHE(s,a,b,c,d) ((s & c) << d) ^ (((s <<a) ^ s) >> b)
    state[0] = TAUSWORTHE(state[0], 13U, 19U, 4294967294U, 12);
    state[1] = TAUSWORTHE(state[1], 2U, 25U, 4294967288U, 4);
    state[2] = TAUSWORTHE(state[2], 3U, 11U, 4294967280U, 17);
#undef TAUSWORTHE

    return (state[0] ^ state[1] ^ state[2]);
}

static void
tausinit(uint32_t state[3],
         uint32_t seed)
{
    if (seed == 0) {
        seed = 1;
    }

#define LCG(n) ((69069U * (n)) & 0xFFFFFFFFU)
    state[0] = LCG(seed);
    state[1] = LCG(state[0]);
    state[2] = LCG(state[1]);
#undef LCG

    tausrand(state);
    tausrand(state);
    tausrand(state);
    tausrand(state);
    tausrand(state);
    tausrand(state);
}

bool_t
dither_init(const int n_channels,
	    const int sample_rate,
            const int realsize_,
	    const int max_size,
	    const int max_samples_per_loop,
	    struct dither_state *dither_states[])
{
    realsize = realsize_;

    int spacing;
    { // calculate spacing
        int minspacing = MIN_RANDTAB_SPACING * sample_rate;
        if (minspacing > max_samples_per_loop) {
            minspacing = max_samples_per_loop;
        }
        spacing = RANDTAB_SPACING * sample_rate;
        if (spacing < minspacing) {
            spacing = minspacing;
        }
        if (max_size > 0 && n_channels * spacing > max_size) {
            spacing = max_size / n_channels;
        }
        if (spacing < minspacing) {
            fprintf(stderr, "Maximum dither table size %d bytes is too small, must at least be %d bytes.\n",
                    max_size, n_channels * sample_rate * minspacing);
            return false;
        }
    }

    dither_randtab_size = n_channels * spacing + 1;

    { // generate random number table
        uint32_t state[3];

        pinfo("Dither table size is %d bytes.\n"
              "Generating random numbers...", dither_randtab_size);
        tausinit(state, 0);
        dither_randtab = emallocaligned(dither_randtab_size);
        for (int n = 0; n < dither_randtab_size; n++) {
            dither_randtab[n] = (int8_t)(tausrand(state) & 0x000000FF);
        }
        pinfo("finished.\n");
    }

    /* make a map for conversion of integer dither random numbers to
       floating point ranging from -1.0 to +1.0, plus an offset of +0.5,
       used to make the sample truncation be mid-tread requantisation */
    dither_randmap = emallocaligned(realsize * 511);
    dither_randmap = &((uint8_t *)dither_randmap)[255 * realsize];
    if (realsize == 4) {
        for (int n = -255; n <= 255; n++) {
            ((float *)dither_randmap)[n] = 0.5 + 1.0 / 255.0 * (float)n;
        }
    } else {
        for (int n = -255; n <= 255; n++) {
            ((double *)dither_randmap)[n] = 0.5 + 1.0 / 255.0 * (double)n;
        }
    }

    for (int n = 0; n < n_channels; n++) {
	dither_states[n] = emalloc(sizeof(struct dither_state));
	memset(dither_states[n], 0, sizeof(struct dither_state));
	dither_states[n]->randtab_ptr = n * spacing + 1;
    }
    return true;
}
