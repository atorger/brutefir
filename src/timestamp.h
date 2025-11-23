/*
 * (c) Copyright 2000, 2002, 2004, 2006, 2013, 2025 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef TIMESTAMP_H_
#define TIMESTAMP_H_

#include <inttypes.h>
#include <time.h>

#include "sysarch.h"

#if defined(ARCH_X86) || defined(ARCH_X86_64)
#include <x86intrin.h>
static inline void
timestamp(volatile uint64_t *ts)
{
    *ts = __rdtsc();
}
#else
#define TIMESTAMP_NOT_CLOCK_CYCLE_COUNTER
static inline void
timestamp(volatile uint64_t *ts)
{
    struct timespec timespec;
    clock_gettime(CLOCK_MONOTONIC, &timespec);
    *ts = (uint64_t)timespec.tv_sec * 1000000000 + timespec.tv_nsec;
}
#endif

#endif
