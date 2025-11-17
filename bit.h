/*
 * (c) Copyright 2001, 2004 - 2006, 2025 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef BIT_H_
#define BIT_H_

#include <stdbool.h>
#include <inttypes.h>

#include "sysarch.h"

static inline bool
bit32_isset(const uint32_t bits[],
            const unsigned position)
{
    const unsigned i = position >> 5u;
    return !!(bits[i] & (uint32_t)1u << (position & 0x1Fu));
}

static inline void
bit32_set(uint32_t bits[],
          const unsigned position)
{
    const unsigned i = position >> 5u;
    bits[i] = bits[i] | (uint32_t)1u << (position & 0x1Fu);
}

static inline void
bit32_clr(uint32_t bits[],
          const unsigned position)
{
    const unsigned i = position >> 5u;
    bits[i] = bits[i] & ~((uint32_t)1u << (position & 0x1Fu));
}

static inline bool
bit32_isset_volatile(volatile const uint32_t bits[],
                     const unsigned position)
{
    const unsigned i = position >> 5u;
    return !!(bits[i] & (uint32_t)1u << (position & 0x1Fu));
}

static inline void
bit32_set_volatile(volatile uint32_t bits[],
                     const unsigned position)
{
    const unsigned i = position >> 5u;
    bits[i] = bits[i] | (uint32_t)1u << (position & 0x1Fu);
}

static inline void
bit32_clr_volatile(volatile uint32_t bits[],
                   const unsigned position)
{
    const unsigned i = position >> 5u;
    bits[i] = bits[i] & ~((uint32_t)1u << (position & 0x1Fu));
}

static inline unsigned
bit32_bsf_generic(const uint32_t value)
{
    static const unsigned table[256] = {
        0, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
        4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
        5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
        4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
        6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
        4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
        5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
        4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
        7, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
        4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
        5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
        4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
        6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
        4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
        5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
        4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0
    };
    if ((value & 0x0000FFFFu) != 0) {
       if ((value & 0x000000FFu) != 0) {
           return table[value & 0x000000FFu];
       } else {
           return 8u + table[(value & 0x0000FF00u) >> 8u];
       }
    } else {
       if ((value & 0x00FF0000u) != 0) {
           return 16u + table[(value & 0x00FF0000u) >> 16u];
       } else {
           return 24u + table[(value & 0xFF000000u) >> 24u];
       }
    }
}

/* Note on bit scans below: if integer is zero the return value is undefined! */
static inline unsigned
bit32_bsf(const uint32_t value)
{
#if ARCH_SIZEOF_INT >= 4 && __has_builtin(__builtin_ctz)
    return (unsigned)__builtin_ctz((unsigned)value);
#else
    return bit32_bsf_generic(value);
#endif
}

static inline int
bit32_find(const uint32_t bits[],
           const unsigned from,
           const unsigned to)
{
    unsigned i;
    uint32_t bb;

    if ((const int)to < (const int)from) {
        return -1;
    }
    if ((bb = bits[from >> 5u] >> (from & 0x1Fu)) != 0) {
	if ((i = bit32_bsf(bb) + from) > to) {
	    return -1;
	}
	return (int)i;
    }
    for (i = (from >> 5u) + 1; i <= (to >> 5u); i++) {
	if (bits[i] != 0) {
	    if ((i = bit32_bsf(bits[i]) + (i << 5u)) > to) {
		return -1;
	    }
	    return (int)i;
	}
    }
    return -1;
}

#endif
