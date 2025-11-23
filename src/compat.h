/*
 * (c) Copyright 2025 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef COMPAT_H_
#define COMPAT_H_

#include <math.h>
#include <alloca.h> // for alloca(), may need some other header/macro if not on GNU/Clang

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int
ascii_strcasecmp(const char s1[], const char s2[]);

int
compat_usleep(unsigned long usec);

int
number_of_cpu_cores(void);

void *
allocate_aligned_memory(size_t size, size_t alignment);

int
posix_kill(pid_t pid, int sig);

// set thread name, for debugging only
void
set_thread_name(const char name[]);

#endif
