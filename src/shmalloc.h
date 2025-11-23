/*
 * (c) Copyright 2001, 2003, 2025 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef SHMALLOC_H_
#define SHMALLOC_H_

#include <stdlib.h>

void *
shmalloc(size_t size);

void *
shmalloc_id(int *shmid,
            size_t size);

void *
shmalloc_attach(int shmid);

#endif
