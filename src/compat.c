/*
 * (c) Copyright 2025 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/prctl.h>

#include "compat.h"

int
ascii_strcasecmp(const char s1[], const char s2[])
{
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1++);
        int c2 = tolower((unsigned char)*s2++);
        if (c1 != c2) {
            return c1 - c2;
        }
    }
    return (int)((unsigned char)*s1 - (unsigned char)*s2);
}

int
compat_usleep(unsigned long usec)
{
    struct timespec ts, rem;

    ts.tv_sec  = usec / 1000000UL;
    ts.tv_nsec = (usec % 1000000UL) * 1000UL;

    return nanosleep(&ts, &rem); // may be interrupted with EINTR
}

int
number_of_cpu_cores(void)
{
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs <= 0) {
        return 1;
    }
    return (int)nprocs;
}

void *
allocate_aligned_memory(size_t size, size_t alignment)
{
    if (size == 0) {
        return NULL;
    }
    void *p;
    int err = posix_memalign(&p, alignment, size < alignment ? alignment : size);
    if (err != 0) {
        return NULL;
    }
    return p;
}

void
set_thread_name(const char name[])
{
    prctl(PR_SET_NAME, (unsigned long)name, 0, 0, 0);
}
