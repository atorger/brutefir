/*
 * (c) Copyright 2001 - 2004, 2025 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/shm.h>

#include "sysarch.h"
#include "shmalloc.h"

static void
print_shmget_error(size_t size)
{
    switch (errno) {
    case EINVAL:
        fprintf(stderr, "shmget() failed when requesting %ld bytes.\n"
                "  The OS shmmax parameter must be increased.\n", (long)size);
        break;
    case ENOSPC:
        fprintf(stderr, "shmget() failed due to exceeded id limit.\n"
                "  The OS shmmni parameter must be increased.\n");
        break;
    case ENOMEM:
        fprintf(stderr, "shmget() failed when requesting %ld bytes.\n"
                "Out of memory, or the OS parameters shmmax and/or shmmni "
                "may be too small.\n", (long)size);
        break;
    default:
        fprintf(stderr, "shmget() failed when requesting %ld bytes: %s.\n",
                (long)size, strerror(errno));
        break;
    }
}

void *
shmalloc(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return NULL;
    }
    if (((uintptr_t)p & (ALIGNMENT - 1)) != 0) {
        fprintf(stderr, "alignment error\n"); // should not happen
        return NULL;
    }
    return p;
}

/*
  Note 2025: System V shared memory is old school, this can easily be converted to POSIX shm_open(),
  but we continue to use System V for now as it has working auto-removal of shared memory after exit,
  which POSIX does not provide.
 */
void *
shmalloc_id(int *shmid,
            size_t size)
{
    static key_t key = 1;
    struct shmid_ds shmid_ds;
    void *p;
    int n;

    if (shmid != NULL) {
        *shmid = -1;
    }

    while ((n = shmget(key, size, IPC_CREAT | IPC_EXCL | SHM_R | SHM_W)) == -1 && errno == EEXIST) {
        key++;
    }
    if (n == -1) {
        print_shmget_error(size);
	return NULL;
    }

    if  ((p = shmat(n, NULL, 0)) == (char *)-1) {
	fprintf(stderr, "Failed to attach to shared memory (shmid %d): %s.\n", n, strerror(errno));
	return NULL;
    }
    memset(&shmid_ds, 0, sizeof(shmid_ds));
    if (shmctl(n, IPC_RMID, &shmid_ds) == -1) {
	fprintf(stderr, "Failed to set IPC_RMID (shmid %d): %s.\n", n, strerror(errno));
	return NULL;
    }

    key++;

    if (shmid != NULL) {
        *shmid = n;
    }

    return p;
}

void *
shmalloc_attach(int shmid)
{
    void *p;
    if ((p = shmat(shmid, NULL, 0)) == (void *)-1) {
	fprintf(stderr, "Failed to attach to shared memory (shmid %d): %s.\n", shmid, strerror(errno));
        return NULL;
    }
    return p;
}
