/*
 * (c) Copyright 2002 - 2004, 2013, 2025 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef SYSARCH_H_
#define SYSARCH_H_

#include <limits.h>
#include <stdint.h>

#define ALIGNMENT 32

// CPU architecture
#if defined(__i386__) || defined(_M_IX86)
#define ARCH_X86 1
#elif defined(__x86_64__) || defined(_M_X64)
#define ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define ARCH_ARM64 1
#endif


// Byte order
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define ARCH_LITTLE_ENDIAN 1
#elif __BYTE_ORDER == __BIG_ENDIAN
#define ARCH_BIG_ENDIAN 1
#else
 #error unknown byte order
#endif

// Datatype sizes
#ifndef ARCH_SIZEOF_INT
#if INT_MAX == 2147483647LL
#define ARCH_SIZEOF_INT 4
#define ARCH_LOG2_SIZEOF_INT 2
#elif INT_MAX == 9223372036854775807LL
#define ARCH_SIZEOF_INT 8
#define ARCH_LOG2_SIZEOF_INT 3
#else
 #error "Unsupported size of INT_MAX"
#endif
#endif

#ifndef ARCH_SIZEOF_PTR
#if UINTPTR_MAX == 4294967295ULL
#define ARCH_SIZEOF_PTR 4
#define ARCH_LOG2_SIZEOF_PTR 2
#elif UINTPTR_MAX == 18446744073709551615ULL
#define ARCH_SIZEOF_PTR 8
#define ARCH_LOG2_SIZEOF_PTR 3
#else
 #error "Unsupported size of UINTPTR_MAX"
#endif
#endif

// OS
#ifdef __linux__
#define ARCH_OS_LINUX 1
#endif

static __inline void
arch_compile_time_macro_testing_(void)
{
    switch(0){case 0:break;case ARCH_SIZEOF_PTR==sizeof(void *):break;} // NOLINT
}

#endif
