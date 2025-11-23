/*
 * (c) Copyright 2025 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */

/*

  BruteFIR was first concieved ~2000 and was originally designed with fork() to
  make use of more than one CPU core, as pthreads could only use one core at the time.

  Modern sound servers doesn't work well with forking though, so this concurrency lib
  was created to make it "easy" to port the legacy BruteFIR design into using threads.

  Still the fork code stays intact, although can only be enabled compile time.

 */
#ifndef BFCONCURRENCY_H_
#define BFCONCURRENCY_H_

#include <pthread.h>
#include <stdbool.h>
#include <inttypes.h>

union bf_sem_t_ {
    struct {
        pthread_mutex_t mutex;
        pthread_cond_t cond;
        unsigned int count;
        uint8_t msg_data[16];
        int msg_offset;
    } sem;
    struct {
        int fd[2];
    } pipe;
};

union bf_pid_t_ {
    pthread_t pthread;
    pid_t process_id;
};

typedef union bf_sem_t_ bf_sem_t;
typedef union bf_pid_t_ bf_pid_t;

bool
bf_is_fork_mode(void);

/*
  Multi-process BruteFIR used pipe() as its main synchronization primitive,
  this has been translated into a semaphore.
*/
void
bf_sem_init(bf_sem_t *init);

void
bf_sem_post(bf_sem_t *sem);

void
bf_sem_postmany(bf_sem_t *sem, int count);

void
bf_sem_postmsg(bf_sem_t *sem, const void *msg, int msg_size);

void
bf_sem_wait(bf_sem_t *sem);

void
bf_sem_waitmany(bf_sem_t *sem, int count);

void
bf_sem_waitmsg(bf_sem_t *sem, void *msg, int msg_size);

void
bf_sem_never_post(bf_sem_t *sem);

void
bf_sem_never_wait(bf_sem_t *sem);

bf_pid_t
bf_fork(void (*child_func)(void *arg),
        void *arg);

bf_pid_t
bf_getpid(void);

bool
bf_pid_equal(bf_pid_t a, bf_pid_t b);

bf_pid_t
bf_process_id_to_bf_pid(pid_t pid);

int
bf_set_sched_fifo(int priority,
                  const char name[]);

void
bf_terminate(bf_pid_t bf_pid);

// Global thread lock for special situations, does nothing in fork mode.
void
bf_global_thread_lock(bool lock);

// Only use shared memory if fork mode, otherwise normal malloc().
void *
maybe_shmalloc(size_t size);

#endif
