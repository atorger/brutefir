/*
 * (c) Copyright 2003, 2004 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

#define IS_BFIO_MODULE
#include "bfmod.h"
#include "inout.h"

struct file {
    int io;
    int fd;
    int frame_size;
    int offset;
    int size;
    uint8_t *buf;
    bool complete;
    void *state;
};

static struct file *files[FD_SETSIZE];
static int n_files;
static bool debug = false;
static pthread_t pthread;
static int period_size;
static int (*process_cb)(void **states[2],
                         int state_count[2],
                         void **bufs[2],
                         int count,
                         int event);

struct settings {
    off_t skipbytes;
    bool append;
    char *path;
};

static void *
process_thread(void *arg)
{
    int n, i, n_poll, ret, state_count[2], all_state_count[2], frames_left;
    void **all_states[2], *_all_states[2][BF_MAXCHANNELS];
    void **states[2], *_states[2][BF_MAXCHANNELS];
    void *bufs[2][BF_MAXCHANNELS], **iobufs[2];
    int last_input_frames, filemap[n_files];
    struct pollfd pollfd[n_files];

    frames_left = 0;
    last_input_frames = -1;
    all_state_count[IN] = 0;
    all_state_count[OUT] = 0;
    for (n = 0; n < n_files; n++) {
        _all_states[files[n]->io][all_state_count[files[n]->io]] =
            files[n]->state;
        all_state_count[files[n]->io]++;
    }
    all_states[IN] = all_state_count[IN] > 0 ? _all_states[IN] : NULL;
    all_states[OUT] = all_state_count[OUT] > 0 ? _all_states[OUT] : NULL;
    while (true) {
        state_count[IN] = 0;
        state_count[OUT] = 0;
        for (n = 0, n_poll = 0; n < n_files; n++) {
            if (frames_left != 0 && files[n]->io == IN) {
                if (files[n]->fd != -1) {
                    close(files[n]->fd);
                }
                files[n]->fd = -1;
                continue;
            }
            if (files[n]->offset == files[n]->size || files[n]->complete) {
                bufs[files[n]->io][state_count[files[n]->io]] = files[n]->buf;
                _states[files[n]->io][state_count[files[n]->io]] =
                    files[n]->state;
                state_count[files[n]->io]++;
                continue;
            }
            filemap[n_poll] = n;
            pollfd[n_poll].fd = files[n]->fd;
            pollfd[n_poll].events = files[n]->io == IN ? POLLIN : POLLOUT;
            pollfd[n_poll].revents = 0;
            n_poll++;            
        }
        if (n_poll == 0) {
            if (frames_left != 0) {
                /* finished */
                process_cb(all_states, all_state_count, NULL, 0,
                           BF_CALLBACK_EVENT_FINISHED);
                return NULL;
            }
            iobufs[IN] = state_count[IN] > 0 ? bufs[IN] : NULL;
            iobufs[OUT] = state_count[OUT] > 0 ? bufs[OUT] : NULL;
            states[IN] = state_count[IN] > 0 ? _states[IN] : NULL;
            states[OUT] = state_count[OUT] > 0 ? _states[OUT] : NULL;
            if (last_input_frames != -1) {
                process_cb(all_states, all_state_count, NULL, last_input_frames,
                           BF_CALLBACK_EVENT_LAST_INPUT);
            }
            frames_left = process_cb(states, state_count, iobufs, period_size,
                                     BF_CALLBACK_EVENT_NORMAL);
            for (n = 0; n < n_files; n++) {
                files[n]->offset = 0;
                if (frames_left != 0 && files[n]->io == OUT) {
                    files[n]->size = frames_left * files[n]->frame_size;
                }
            }
            continue;
        }

        if (poll(pollfd, n_poll, -1) == -1) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "filecb I/O: Select failed: %s.\n",
                    strerror(errno));
            process_cb(all_states, all_state_count, NULL, 0,
                       BF_CALLBACK_EVENT_ERROR);
            return NULL;
        }
        for (n = 0; n < n_poll; n++) {
            i = filemap[n];
            if (files[i]->io == IN && (pollfd[n].revents & POLLIN) != 0) {
                if ((ret = read(files[i]->fd, &files[i]->buf[files[i]->offset],
                                files[i]->size - files[i]->offset)) == -1)
                {
                    fprintf(stderr, "filecb I/O: Read failed: %s.\n",
                            strerror(errno));
                    process_cb(all_states, all_state_count, NULL, 0,
                               BF_CALLBACK_EVENT_ERROR);
                    return NULL;
                }
                files[i]->offset += ret;
                if (ret == 0) {
                    files[i]->complete = true;
                    close(files[i]->fd);
                    files[i]->fd = -1;
                    if (last_input_frames == -1 ||
                        files[i]->offset / files[i]->frame_size <
                        last_input_frames)
                    {
                        last_input_frames = files[i]->offset /
                            files[i]->frame_size;
                    }
                }
            }
            if (files[i]->io == OUT && (pollfd[n].revents & POLLOUT) != 0) {
                if ((ret = write(files[i]->fd, &files[i]->buf[files[i]->offset],
                                 files[i]->size - files[i]->offset)) == -1)
                {
                    fprintf(stderr, "filecb I/O: Write failed: %s.\n",
                            strerror(errno));
                    process_cb(all_states, all_state_count, NULL, 0,
                               BF_CALLBACK_EVENT_ERROR);
                    return NULL;
                }
                files[i]->offset += ret;
            }
        }
    }
    
}

int
bfio_iscallback(void)
{
    return true;
}

#define GET_TOKEN(token, errstr)                                               \
    if (get_config_token(&lexval) != token) {                                  \
        fprintf(stderr, "filecb I/O: Parse error: " errstr);                   \
        return NULL;                                                           \
    }

void *
bfio_preinit(int *version_minor,
             int *version_major,
             int (*get_config_token)(union bflexval *lexval),
             int io,
             int *sample_format,
             int sample_rate,
             int open_channels,
             int *uses_sample_clock,
             int *callback_sched_policy,
             struct sched_param *callback_sched,
             int _debug)
{
    struct settings *settings;
    union bflexval lexval;
    int token, ver;

    ver = *version_major;
    *version_major = BF_VERSION_MAJOR;
    *version_minor = BF_VERSION_MINOR;
    if (ver != BF_VERSION_MAJOR) {
        return NULL;
    }
    debug = !!_debug;
    settings = malloc(sizeof(struct settings));
    memset(settings, 0, sizeof(struct settings));
    while ((token = get_config_token(&lexval)) > 0) {
        if (token == BF_LEXVAL_FIELD) {
            if (strcmp(lexval.field, "path") == 0) {
                if (settings->path != NULL) {
                    fprintf(stderr, "filecb I/O: Parse error: path already "
                            "set.\n");
                    return NULL;
                }
                GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
                settings->path = strdup(lexval.string);                
            } else if (strcmp(lexval.field, "skip") == 0) {
                GET_TOKEN(BF_LEXVAL_REAL, "expected integer.\n");
                settings->skipbytes = (off_t)lexval.real;
            } else if (strcmp(lexval.field, "append") == 0) {
                if (io == BF_IN) {
                    fprintf(stderr, "filecb I/O: Append on input makes no "
                            "sense.\n");
                    return NULL;
                }
                GET_TOKEN(BF_LEXVAL_BOOLEAN, "expected boolean value.\n");
                settings->append = lexval.boolean;
            } else {
                fprintf(stderr, "filecb I/O: Parse error: unknown field.\n");
                return NULL;
            }
            GET_TOKEN(BF_LEX_EOS, "expected end of statement (;).\n");
        } else {
            fprintf(stderr, "filecb I/O: Parse error: expected field.\n");
            return NULL;
        }
    }
    if (settings->path == NULL) {
        fprintf(stderr, "filecb I/O: Parse error: path not set.\n");
        return NULL;
    }
    if (*sample_format == BF_SAMPLE_FORMAT_AUTO) {
        fprintf(stderr, "filecb I/O: No support for AUTO sample format.\n");
        return NULL;
    }
    *uses_sample_clock = 0;
    return settings;
}

#undef GET_TOKEN

int
bfio_init(void *params,
	  int io,
	  int sample_format,
	  int sample_rate,
	  int open_channels,
	  int used_channels,
	  const int channel_selection[],
	  int _period_size,
	  int *device_period_size,
	  int *isinterleaved,
          void *callback_state,
          int (*process_callback)(void **callback_states[2],
                                  int callback_state_count[2],
                                  void **buffers[2],
                                  int frame_count,
                                  int event))
{
    struct settings *settings;
    int fd, mode;

    settings = (struct settings *)params;
    process_cb = process_callback;    
    *device_period_size = _period_size; 
    *isinterleaved = 1;
    period_size = _period_size;
        
    if (io == BF_IN) {
	if ((fd = open(settings->path, O_RDONLY | O_NONBLOCK |
		       O_LARGEFILE)) == -1)
	{
	    fprintf(stderr, "filecb I/O: Could not open file \"%s\" for "
                    "reading: %s.\n", settings->path, strerror(errno));
	    return -1;
	}
	if (settings->skipbytes > 0) {
	    if (lseek(fd, settings->skipbytes, SEEK_SET) == -1) {
		fprintf(stderr, "filecb I/O: File seek failed.\n");
		return -1;
	    }
	}
    } else {
	if (settings->append) {
	    mode = O_APPEND;
	} else {
	    mode = O_TRUNC;
	}
	if ((fd = open(settings->path, O_WRONLY | O_CREAT | mode |
		       O_NONBLOCK | O_LARGEFILE, S_IRUSR | S_IWUSR |
		       S_IRGRP | S_IROTH)) == -1)
	{
	    fprintf(stderr, "filecb I/O: Could not create file \"%s\" for "
                    "writing: %s.\n", settings->path, strerror(errno));
	    return -1;
	}
    }
    free(settings->path);
    free(settings);

    files[n_files] = malloc(sizeof(struct file));
    memset(files[n_files], 0, sizeof(struct file));
    files[n_files]->io = io;
    files[n_files]->fd = fd;
    files[n_files]->frame_size = open_channels *
        bf_sampleformat_size(sample_format);
    files[n_files]->size = period_size * files[n_files]->frame_size;
    files[n_files]->buf = malloc(files[n_files]->size);
    files[n_files]->state = callback_state;
    n_files++;
    
    return 0;
}

int
bfio_synch_start(void)
{
    sigset_t signals;

    /* no signals to child thread */
    sigfillset(&signals);
    pthread_sigmask(SIG_BLOCK, &signals, NULL);
    if (pthread_create(&pthread, NULL, process_thread, NULL) != 0) {
        fprintf(stderr, "filecb I/O: Could not start thread: %s",
                strerror(errno));
        return -1;
    }
    pthread_sigmask(SIG_UNBLOCK, &signals, NULL);
    
    return 0;
}

void
bfio_synch_stop(void)
{
    pthread_cancel(pthread);
}
