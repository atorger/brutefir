/*
 * (c) Copyright 1999, 2002, 2025 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef SWAP_H_
#define SWAP_H_

#include <inttypes.h>

static inline uint16_t
bit16_swap_generic(uint16_t v)
{
    return (((unsigned)v & 0xFF00u) >> 8u) | (((unsigned)v & 0x00FFu) << 8u);
}
static inline uint16_t
bit16_swap(uint16_t v)
{
#if __has_builtin(__builtin_bswap16)
    return __builtin_bswap16(v);
#else
    return bit16_swap_generic(v);
#endif
}

static inline uint32_t
bit32_swap_generic(uint32_t v)
{
    return bit16_swap((uint16_t)((v & 0xFFFF0000u ) >> 16u)) |
        ((uint32_t)bit16_swap((uint16_t)(v & 0x0000FFFFu)) << 16u);
}
static inline uint32_t
bit32_swap(uint32_t v)
{
#if __has_builtin(__builtin_bswap32)
    return __builtin_bswap32(v);
#else
    return bit32_swap_generic(v);
#endif
}

static inline uint64_t
bit64_swap_generic(uint64_t v)
{
    return bit32_swap((uint32_t)((v & 0xFFFFFFFF00000000u ) >> 32u)) |
        ((uint64_t)bit32_swap((uint32_t)(v & 0xFFFFFFFFu)) << 32u);
}
static inline uint64_t
bit64_swap(uint64_t v)
{
#if __has_builtin(__builtin_bswap64)
    return __builtin_bswap64(v);
#else
    return bit64_swap_generic(v);
#endif
}

#endif
