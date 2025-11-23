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

#include "dither.h"
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
 * From "Tables of maximally equidistributed combined LSFR generators" by L’Ecuyer.
 *
 * Generates pseudorandom numbers between 0x0 - 0xFFFFFFFF
 */
static inline uint32_t
lfsr113(uint32_t z[4])
{
    uint32_t b;
    b = (((z[0] << 6u) ^ z[0]) >> 13u);
    z[0] = (((z[0] & 4294967294u) << 18u) ^ b);
    b = (((z[1] << 2u) ^ z[1]) >> 27u);
    z[1] = (((z[1] & 4294967288u) << 2u) ^ b);
    b = (((z[2] << 13u) ^ z[2]) >> 21u);
    z[2] = (((z[2] & 4294967280u) << 7u) ^ b);
    b = (((z[3] << 3u) ^ z[3]) >> 12u);
    z[3] = (((z[3] & 4294967168u) << 13u) ^ b);
    return (z[0] ^ z[1] ^ z[2] ^ z[3]);
}

static void
lfsr113_init(uint32_t state[4])
{
    // The initial seeds z[0] - z [3]  MUST be larger than 1, 7, 15, and 127 respectively.
    const uint32_t seed = 12345;
    state[0] = seed;
    state[1] = seed;
    state[2] = seed;
    state[3] = seed;
    lfsr113(state);
    lfsr113(state);
    lfsr113(state);
    lfsr113(state);
    lfsr113(state);
    lfsr113(state);
}

bool
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
        uint32_t state[4];

        pinfo("Dither table size is %d bytes.\n"
              "Generating random numbers...", dither_randtab_size);
        lfsr113_init(state);
        dither_randtab = emallocaligned(dither_randtab_size);
        for (int n = 0; n < dither_randtab_size; n++) {
            dither_randtab[n] = (int8_t)(lfsr113(state) & 0x000000FF);
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
