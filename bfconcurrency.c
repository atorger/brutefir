/*
 * (c) Copyright 2025 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <signal.h>

#include "shmalloc.h"
#include "emalloc.h"
#include "bfconcurrency.h"
#include "fdrw.h"
#include "bfrun.h"
#include "compat.h"

static bool fork_mode = false;

bool
bf_is_fork_mode(void)
{
    return fork_mode;
}

void
bf_sem_init(bf_sem_t *sem)
{
    memset(sem, 0, sizeof(*sem));
    if (fork_mode) {
        if (pipe(sem->pipe.fd) == -1) {
            fprintf(stderr, "Failed to create pipe: %s.\n", strerror(errno));
            bf_exit(BF_EXIT_OTHER);
        }
    } else {
        if (pthread_mutex_init(&sem->sem.mutex, NULL) != 0 ||
            pthread_cond_init(&sem->sem.cond, NULL) != 0)
        {
            fprintf(stderr, "Failed to create semaphore.\n");
            bf_exit(BF_EXIT_OTHER);
        }
        sem->sem.count = 0;
    }
}

void
bf_sem_postmany(bf_sem_t *sem, int count)
{
    uint8_t dummydata[count];
    int i;

    if (fork_mode) {
        memset(dummydata, 0, count);
        if (!writefd(sem->pipe.fd[1], dummydata, count)) {
            bf_exit(BF_EXIT_OTHER);
        }
    } else {
        pthread_mutex_lock(&sem->sem.mutex);
        sem->sem.count += count;
        for (i = 0; i < count; i++) {
            pthread_cond_signal(&sem->sem.cond);
        }
        pthread_mutex_unlock(&sem->sem.mutex);
    }
}

void
bf_sem_post(bf_sem_t *sem)
{
    bf_sem_postmany(sem, 1);
}

void
bf_sem_postmsg(bf_sem_t *sem, const void *msg, int msg_size)
{
    if (fork_mode) {
        if (!writefd(sem->pipe.fd[1], msg, msg_size)) {
            bf_exit(BF_EXIT_OTHER);
        }
    } else {
        pthread_mutex_lock(&sem->sem.mutex);
        if (sem->sem.msg_offset == sizeof(sem->sem.msg_data)) {
            fprintf(stderr, "Semaphore message buffer overflow.\n");
            bf_exit(BF_EXIT_OTHER);
        }
        memcpy(&sem->sem.msg_data[sem->sem.msg_offset], msg, msg_size);
        sem->sem.msg_offset += msg_size;
        sem->sem.count++;
        pthread_cond_signal(&sem->sem.cond);
        pthread_mutex_unlock(&sem->sem.mutex);
    }
}

void
bf_sem_waitmany(bf_sem_t *sem, int count)
{
    uint8_t dummydata[count];
    int i;

    if (fork_mode) {
        memset(dummydata, 0, count);
        if (!readfd(sem->pipe.fd[0], dummydata, count)) {
            bf_exit(BF_EXIT_OTHER);
        }
    } else {
        pthread_mutex_lock(&sem->sem.mutex);
        for (i = 0; i < count; i++) {
            while (sem->sem.count == 0) {
                pthread_cond_wait(&sem->sem.cond, &sem->sem.mutex);
            }
            sem->sem.count--;
        }
        pthread_mutex_unlock(&sem->sem.mutex);
    }
}

void
bf_sem_wait(bf_sem_t *sem)
{
    bf_sem_waitmany(sem, 1);
}

void
bf_sem_waitmsg(bf_sem_t *sem, void *msg, int msg_size)
{
    if (fork_mode) {
        if (!readfd(sem->pipe.fd[0], msg, msg_size)) {
            bf_exit(BF_EXIT_OTHER);
        }
    } else {
        pthread_mutex_lock(&sem->sem.mutex);
        while (sem->sem.count == 0) {
            pthread_cond_wait(&sem->sem.cond, &sem->sem.mutex);
        }
        if (sem->sem.msg_offset < msg_size) {
            fprintf(stderr, "Semaphore message buffer underflow.\n");
            bf_exit(BF_EXIT_OTHER);
        }
        memcpy(msg, sem->sem.msg_data, msg_size);
        sem->sem.msg_offset -= msg_size;
        if (sem->sem.msg_offset > 0) {
            memmove(sem->sem.msg_data, &sem->sem.msg_data[msg_size], sem->sem.msg_offset);
        }
        sem->sem.count--;
        pthread_mutex_unlock(&sem->sem.mutex);
    }
}

void
bf_sem_never_post(bf_sem_t *sem)
{
    if (fork_mode) {
        close(sem->pipe.fd[1]);
    }
}

void
bf_sem_never_wait(bf_sem_t *sem)
{
    if (fork_mode) {
        close(sem->pipe.fd[0]);
    }
}

struct wrap_child_func_arg {
    void (*child_func)(void *arg);
    void *arg;
};

static void *
wrap_child_func(void *arg)
{
    struct wrap_child_func_arg a = *(struct wrap_child_func_arg *)arg;
    efree(arg);
    a.child_func(a.arg);
    return NULL;
}

bf_pid_t
bf_fork(void (*child_func)(void *arg),
        void *arg)
{
    bf_pid_t bf_pid;
    int error;
    memset(&bf_pid, 0, sizeof(bf_pid));
    if (fork_mode) {
        bf_pid.process_id = fork();
        if (bf_pid.process_id == -1) {
            fprintf(stderr, "fork() failed: %s.\n", strerror(errno));
            bf_exit(BF_EXIT_OTHER);
        }
        if (bf_pid.process_id == 0) {
            child_func(arg);
        }
    } else {
        struct wrap_child_func_arg *wrap_arg = emalloc(sizeof(*wrap_arg));
        wrap_arg->child_func = child_func;
        wrap_arg->arg = arg;
        if ((error = pthread_create(&bf_pid.pthread, NULL, wrap_child_func, wrap_arg)) != 0) {
            fprintf(stderr, "pthread_create() failed: %s.\n", strerror(error));
            bf_exit(BF_EXIT_OTHER);
        }
    }
    return bf_pid;
}

bf_pid_t
bf_process_id_to_bf_pid(pid_t pid)
{
    if (!fork_mode) {
        fprintf(stderr, "Tried to convert pid while not in fork mode, aborting.\n");
        abort();
    }
    bf_pid_t bf_pid;
    memset(&bf_pid, 0, sizeof(bf_pid));
    bf_pid.process_id = pid;
    return bf_pid;
}

bf_pid_t
bf_getpid(void)
{
    bf_pid_t bf_pid;
    memset(&bf_pid, 0, sizeof(bf_pid));
    if (fork_mode) {
        bf_pid.process_id = getpid();
    } else {
        bf_pid.pthread = pthread_self();
    }
    return bf_pid;
}

bool
bf_pid_equal(bf_pid_t a, bf_pid_t b)
{
    if (fork_mode) {
        return a.process_id == b.process_id;
    }
    return pthread_equal(a.pthread, b.pthread);
}

int
bf_set_sched_fifo(int priority,
                  const char name[])
{
    struct sched_param schp;
    int error;

    memset(&schp, 0, sizeof(schp));
    schp.sched_priority = priority;

    if (fork_mode) {
        if (sched_setscheduler(0, SCHED_FIFO, &schp) != 0) {
            error = errno;
        } else {
            error = 0;
        }
    } else {
        error = pthread_setschedparam(pthread_self(), SCHED_FIFO, &schp);
    }
    return error;
}

void
bf_terminate(bf_pid_t bf_pid)
{
    if (fork_mode) {
        kill(bf_pid.process_id, SIGTERM);
    } else {
        pthread_kill(bf_pid.pthread, SIGTERM);
    }
}

void
bf_global_thread_lock(bool lock)
{
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

    if (fork_mode) {
        return; // do nothing
    }
    if (lock) {
        pthread_mutex_lock(&mutex);
    } else {
        pthread_mutex_unlock(&mutex);
    }
}

void *
maybe_shmalloc(size_t size)
{
    if (fork_mode) {
        return shmalloc(size);
    } else {
        return emalloc(size);
    }
}
