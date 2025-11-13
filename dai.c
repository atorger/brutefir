/*
 * (c) Copyright 2001 - 2006, 2013, 2016, 2025 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

#include "defs.h"
#include "emalloc.h"
#include "shmalloc.h"
#include "dai.h"
#include "bfrun.h"
#include "bit.h"
#include "inout.h"
#include "bfconf.h"
#include "timestamp.h"
#include "fdrw.h"
#include "pinfo.h"
#include "delay.h"
#include "compat.h"
#include "timermacros.h"
#include "numunion.h"

/* FIXME: use bfconf directly in more situations? */

#define CB_MSG_START 1
#define CB_MSG_STOP 2

struct dai_buffer_format *dai_buffer_format[2] = { NULL, NULL };

struct subdev {
    volatile bool_t finished;
    bool_t uses_callback;
    bool_t uses_clock;
    bool_t isinterleaved;
    bool_t bad_alignment;
    int index;
    int fd;
    int buf_size;
    int buf_offset;
    int buf_left;
    int block_size;
    int block_size_frames;
    struct dai_channels channels;
    delaybuffer_t **db;
    struct bfio_module *module;
    struct {
        int iodelay_fill;
        int curbuf;
        volatile int frames_left;
    } cb;
};

struct comarea {
    volatile bool_t blocking_stopped;
    volatile int lastbuf_index;
    volatile int frames_left;
    volatile int cb_lastbuf_index;
    volatile int cb_frames_left;
    volatile bool_t is_muted[2][BF_MAXCHANNELS];
    volatile int delay[2][BF_MAXCHANNELS];
    volatile bf_pid_t pid[2];
    volatile bf_pid_t callback_pid;
    struct subdev dev[2][BF_MAXCHANNELS];
    struct dai_buffer_format buffer_format[2];
    int buffer_id;
    volatile int cb_buf_index[2];
};

static struct comarea *ca = NULL;
static struct {
    void *iobuffers[2][2];
    int n_devs[2];
    int n_fd_devs[2];
    fd_set dev_fds[2];
    fd_set clocked_wfds;
    int n_clocked_devs;
    int dev_fdn[2];
    int min_block_size[2];
    int cb_min_block_size[2];
    bool_t input_poll_mode;
    struct subdev *dev[2][BF_MAXCHANNELS];
    struct subdev *fd2dev[2][FD_SETSIZE];
    struct subdev *ch2dev[2][BF_MAXCHANNELS];
    int period_size;
    int sample_rate;
    int monitor_rate_fd;
    bf_sem_t synchpipe[2];
    bf_sem_t cbmutex_pipe[2];
    bf_sem_t cbreadywait_pipe[2];
    bf_sem_t cbpipe_s;
    bf_sem_t cbpipe_r;
    int paramspipe_s[2][2];
    int paramspipe_r[2][2];
    volatile int callback_ready_waiting[2];
} glob = {
    .iobuffers = {},
    .n_devs = { 0, 0 },
    .n_fd_devs = { 0, 0 },
    .dev_fds = {},
    .clocked_wfds = {},
    .n_clocked_devs = 0,
    .dev_fdn = { 0, 0 },
    .min_block_size = { 0, 0 },
    .cb_min_block_size = { 0, 0 },
    .input_poll_mode = false,
    .dev = {},
    .fd2dev = {},
    .ch2dev = {},
    .period_size = 0,
    .sample_rate = 0,
    .monitor_rate_fd = -1,
    .synchpipe = {},
    .cbmutex_pipe = {},
    .cbreadywait_pipe = {},
    .cbpipe_s = {},
    .cbpipe_r = {},
    .paramspipe_s = {},
    .paramspipe_r = {},
    .callback_ready_waiting = { 0, 0 }
};

static int
process_callback(void **state[2],
                 int state_count[2],
                 void **buffers[2],
                 int frame_count,
                 int event);

static void
cbmutex(int io,
        bool_t lock)
{
    if (lock) {
        bf_sem_wait(&glob.cbmutex_pipe[io]);
    } else {
        bf_sem_post(&glob.cbmutex_pipe[io]);
    }
}

static bool_t
output_finish(void)
{
    cbmutex(OUT, true);
    bool_t finished = true;
    for (int n = 0; n < glob.n_devs[OUT]; n++) {
        if (!glob.dev[OUT][n]->finished) {
            finished = false;
            break;
        }
    }
    if (finished) {
	pinfo("\nFinished!\n");
        return true;
    }
    cbmutex(OUT, false);
    return false;
}

static void
update_devmap(const int idx,
	      const int io)
{
    struct subdev *sd = glob.dev[io][idx];
    if (sd->fd >= 0) {
        FD_SET(sd->fd, &glob.dev_fds[io]);
        if (sd->fd > glob.dev_fdn[io]) {
            glob.dev_fdn[io] = sd->fd;
        }
        glob.fd2dev[io][sd->fd] = sd;
    }
    for (int n = 0; n < sd->channels.used_channels; n++) {
	glob.ch2dev[io][sd->channels.channel_name[n]] = sd;
    }
}

/* if noninterleaved, update channel layout to fit the noninterleaved access
   mode (it is setup for interleaved layout per default). */
static void
noninterleave_modify(const int idx,
		     const int io)
{
    struct subdev *sd = glob.dev[io][idx];
    if (!sd->isinterleaved) {
	sd->channels.open_channels = sd->channels.used_channels;
	for (int n = 0; n < sd->channels.used_channels; n++) {
	    sd->channels.channel_selection[n] = n;
	}
    }
}

static void
update_delay(struct subdev *sd,
             const int io,
             uint8_t *buf)
{
    if (sd->db == NULL) {
        return;
    }
    for (int n = 0; n < sd->channels.used_channels; n++) {
        if (sd->db[n] == NULL) {
            continue;
        }
        const struct buffer_format *bf = &dai_buffer_format[io]->bf[sd->channels.channel_name[n]];
        const int virtch = bfconf->phys2virt[io][sd->channels.channel_name[n]][0];
        int newdelay = ca->delay[io][sd->channels.channel_name[n]];
        if (bfconf->use_subdelay[io] && bfconf->subdelay[io][virtch] == BF_UNDEFINED_SUBDELAY) {
            newdelay += bfconf->sdf_length;
        }
        delay_update(sd->db[n], (void *)&buf[bf->byte_offset], bf->sf.bytes, bf->sample_spacing, newdelay, NULL);
    }
}

static void
allocate_delay_buffers(int io,
                       struct subdev *sd)
{
    sd->db = emalloc(sd->channels.used_channels * sizeof(delaybuffer_t *));
    for (int n = 0; n < sd->channels.used_channels; n++) {
	/* check if we need a delay buffer here, that is if at least one
	   channel has a direct virtual to physical mapping */
	if (bfconf->n_virtperphys[io][sd->channels.channel_name[n]] == 1) {
            const int virtch = bfconf->phys2virt[io][sd->channels.channel_name[n]][0];
            int extra_delay = 0;
            if (bfconf->use_subdelay[io] && bfconf->subdelay[io][virtch] == BF_UNDEFINED_SUBDELAY) {
                extra_delay = bfconf->sdf_length;
            }
	    sd->db[n] = delay_allocate_buffer(glob.period_size,
					      bfconf->delay[io][virtch] +
                                              extra_delay,
					      bfconf->maxdelay[io][virtch] +
                                              extra_delay,
					      sd->channels.sf.bytes);
	} else {
	    /* this delay is taken care of previous to feeding the channel
	       output to this module */
	    sd->db[n] = NULL;
	}
    }
}

static void
do_mute(struct subdev *sd,
	const int io,
	const int wsize,
	void *buf_,
	const int offset)
{
    // Calculate which channels that should be cleared
    int ch[sd->channels.used_channels];
    int bsch[sd->channels.used_channels];
    int n_mute = 0;
    for (int n = 0; n < sd->channels.used_channels; n++) {
	if (ca->is_muted[io][sd->channels.channel_name[n]]) {
	    ch[n_mute] = sd->channels.channel_selection[n];
	    bsch[n_mute] = ch[n_mute] * sd->channels.sf.bytes;
	    n_mute++;
	}
    }
    if (n_mute == 0) {
	return;
    }

    numunion_t *buf = (numunion_t *)buf_;
    if (!sd->isinterleaved) {
        // non-interleaved case, trivial
        uint8_t *p = &buf->u8[offset / sd->channels.open_channels];
	for (int n = 0; n < n_mute; n++) {
	    memset(p + ch[n] * glob.period_size * sd->channels.sf.bytes, 0, wsize / sd->channels.open_channels);
	}
	return;
    }

    // interleaved case, a bit more messy
    uint8_t * const startp = &buf->u8[offset];
    uint8_t * const endp = &buf->u8[offset] + wsize;
    const int framesize = sd->channels.open_channels * sd->channels.sf.bytes;
    const int head = offset % framesize;
    int mid_offset = offset;
    if (head != 0) {
        int k;
	for (k = 0; k < n_mute && bsch[k] + sd->channels.sf.bytes <= head; k++);
        uint8_t *p = startp;
	for (int n = head; p < startp + framesize - head && p < endp; p++, n++) {
	    if (n >= bsch[k] && n < bsch[k] + sd->channels.sf.bytes) {
		*p = 0;
		if (n == bsch[k] + sd->channels.sf.bytes) {
		    if (++k == n_mute) {
			break;
		    }
		}
	    }
	}
	if (p == endp) {
	    return;
	}
	mid_offset += framesize - head;
    }
    switch (sd->channels.sf.bytes) {
    case 1:
	for (uint8_t *p = &buf->u8[mid_offset]; p < endp; p += sd->channels.open_channels) {
	    for (int n = 0; n < n_mute; n++) {
		p[ch[n]] = 0;
	    }
	}
	break;
    case 2:
	for (uint16_t *p16 = &buf->u16[mid_offset>>1]; (uint8_t *)p16 < endp; p16 += sd->channels.open_channels) {
	    for (int n = 0; n < n_mute; n++) {
		p16[ch[n]] = 0;
	    }
	}
	break;
    case 3:
	for (uint8_t *p = &buf->u8[mid_offset]; p < endp; p += sd->channels.open_channels) {
	    for (int n = 0; n < n_mute; n++) {
		p[bsch[n]+0] = 0;
		p[bsch[n]+1] = 0;
		p[bsch[n]+2] = 0;
	    }
	}
    case 4: {
	for (uint32_t *p32 = &buf->u32[mid_offset>>2]; (uint8_t *)p32 < endp; p32 += sd->channels.open_channels) {
	    for (int n = 0; n < n_mute; n++) {
		p32[ch[n]] = 0;
	    }
	}
	break;
    }
    case 8: {
	for (uint64_t *p64 = &buf->u64[mid_offset>>3]; (uint8_t *)p64 < endp; p64 += sd->channels.open_channels) {
	    for (int n = 0; n < n_mute; n++) {
		p64[ch[n]] = 0;
	    }
	}
	break;
    }
    default:
	fprintf(stderr, "Sample byte size %d not supported.\n", sd->channels.sf.bytes);
	bf_exit(BF_EXIT_OTHER);
	break;
    }

    const int tail = (offset + wsize) % framesize;
    if (tail != 0) {
        uint8_t *p = endp - tail;
	if (p >= startp) {
            for (int n = 0, k = 0; p < endp; p++, n++) {
                if (n >= bsch[k] && n < bsch[k] + sd->channels.sf.bytes) {
                    *p = 0;
                    if (n == bsch[k] + sd->channels.sf.bytes) {
                        if (++k == n_mute) {
                            break;
                        }
                    }
                }
            }
	}
    }
}

static bool_t
init_input(const struct dai_subdevice *dai_subdev,
	   const int idx)
{
    int fd;

    struct subdev *sd = glob.dev[IN][idx];
    sd->uses_callback = bfconf->iomods[dai_subdev->module].iscallback;
    sd->channels = dai_subdev->channels;
    sd->uses_clock = dai_subdev->uses_clock;
    sd->module = &bfconf->iomods[dai_subdev->module];
    sd->index = idx;

    if ((fd = sd->module->init(dai_subdev->params,
                               IN,
                               dai_subdev->channels.sf.format,
                               glob.sample_rate,
                               dai_subdev->channels.open_channels,
                               dai_subdev->channels.used_channels,
                               dai_subdev->channels.channel_selection,
                               glob.period_size,
                               &sd->block_size_frames,
                               &sd->isinterleaved,
                               sd->uses_callback ?
                               sd : NULL,
                               sd->uses_callback ?
                               process_callback : NULL)) == -1)
    {
        fprintf(stderr, "Failed to init input device.\n");
	return false;
    }
    if (sd->uses_callback) {
        sd->fd = -1;
        if (sd->block_size_frames == 0 || glob.period_size % sd->block_size_frames != 0) {
            fprintf(stderr, "Invalid block size for callback input. Got %d, expected multiple of %d.\n",
                    sd->block_size_frames, glob.period_size);
            return false;
        }
        if (sd->uses_clock && (sd->block_size_frames < glob.cb_min_block_size[IN] || glob.cb_min_block_size[IN] == 0)) {
            glob.cb_min_block_size[IN] = sd->block_size_frames;
        }
    } else {
        glob.n_fd_devs[IN]++;
        sd->fd = fd;
        if (bfconf->monitor_rate && glob.monitor_rate_fd == -1 && dai_subdev->uses_clock) {
            glob.monitor_rate_fd = sd->fd;
        }
        if (sd->uses_clock && sd->block_size_frames != 0 &&
            (sd->block_size_frames < glob.min_block_size[IN] ||
             glob.min_block_size[IN] == 0))
        {
            glob.min_block_size[IN] = sd->block_size_frames;
        }
    }
    sd->isinterleaved = !!sd->isinterleaved;
    if (sd->uses_clock && glob.period_size % sd->block_size_frames != 0) {
        sd->bad_alignment = true;
    }
    noninterleave_modify(idx, IN);
    sd->block_size = sd->block_size_frames * sd->channels.open_channels * sd->channels.sf.bytes;
    allocate_delay_buffers(IN, sd);
    update_devmap(idx, IN);
    return true;
}

static bool_t
init_output(const struct dai_subdevice *dai_subdev,
	    const int idx)
{
    int fd;

    struct subdev *sd = glob.dev[OUT][idx];
    sd->uses_callback = bfconf->iomods[dai_subdev->module].iscallback;
    sd->channels = dai_subdev->channels;
    sd->uses_clock = dai_subdev->uses_clock;
    sd->module = &bfconf->iomods[dai_subdev->module];
    sd->index = idx;

    if ((fd = sd->module->init(dai_subdev->params,
                               OUT,
                               dai_subdev->channels.sf.format,
                               glob.sample_rate,
                               dai_subdev->channels.open_channels,
                               dai_subdev->channels.used_channels,
                               dai_subdev->channels.channel_selection,
                               glob.period_size,
                               &sd->block_size_frames,
                               &sd->isinterleaved,
                               sd->uses_callback ?
                               sd : NULL,
                               sd->uses_callback ?
                               process_callback : NULL)) == -1)
    {
        fprintf(stderr, "Failed to init output device.\n");
	return false;
    }
    if (sd->uses_callback) {
        sd->fd = -1;
        if (sd->block_size_frames == 0 || glob.period_size % sd->block_size_frames != 0) {
            fprintf(stderr, "Invalid block size for callback output.\n");
            return false;
        }
        if (sd->uses_clock && (sd->block_size_frames < glob.cb_min_block_size[OUT] || glob.cb_min_block_size[OUT] == 0)) {
            glob.cb_min_block_size[OUT] = sd->block_size_frames;
        }
    } else {
        glob.n_fd_devs[OUT]++;
        sd->fd = fd;
        if (sd->uses_clock) {
            FD_SET(sd->fd, &glob.clocked_wfds);
            glob.n_clocked_devs++;
        }
        if (sd->uses_clock &&
            sd->block_size_frames != 0 &&
            (sd->block_size_frames < glob.min_block_size[OUT] ||
             glob.min_block_size[OUT] == 0))
        {
            glob.min_block_size[OUT] = sd->block_size_frames;
        }
    }
    sd->isinterleaved = !!sd->isinterleaved;
    noninterleave_modify(idx, OUT);
    sd->block_size = sd->block_size_frames * sd->channels.open_channels * sd->channels.sf.bytes;
    allocate_delay_buffers(OUT, sd);
    update_devmap(idx, OUT);
    return true;
}

static void
calc_buffer_format(const int fragsize,
		   const int io,
		   struct dai_buffer_format *format)
{
    format->n_samples = fragsize;
    format->n_channels = 0;
    format->n_bytes = 0;
    for (int n = 0; n < glob.n_devs[io]; n++) {
        struct subdev *sd = glob.dev[io][n];
	sd->buf_offset = format->n_bytes;
	format->n_channels += sd->channels.used_channels;
	for (int i = 0; i < sd->channels.used_channels; i++) {
	    int ch = sd->channels.channel_name[i];
	    format->bf[ch].sf = sd->channels.sf;
	    if (sd->isinterleaved) {
		format->bf[ch].byte_offset = format->n_bytes + sd->channels.channel_selection[i] * sd->channels.sf.bytes;
		format->bf[ch].sample_spacing = sd->channels.open_channels;
	    } else {
		format->bf[ch].byte_offset = format->n_bytes;
		format->bf[ch].sample_spacing = 1;
		format->n_bytes += sd->channels.sf.bytes * fragsize;
	    }
	}
	sd->buf_size = sd->buf_left = sd->channels.open_channels * sd->channels.sf.bytes * fragsize;

	if (sd->isinterleaved) {
	    format->n_bytes += sd->buf_size;
	}
	if (format->n_bytes % ALIGNMENT != 0) {
	    format->n_bytes += ALIGNMENT - format->n_bytes % ALIGNMENT;
	}
    }
}

static void
handle_params(const int io)
{
    char *params, *msgstr = NULL;
    int size, ans, subdev_index;

    if (!readfd(glob.paramspipe_s[io][0], &subdev_index, sizeof(int))) {
	fprintf(stderr, "Failed to read from pipe.\n");
	bf_exit(BF_EXIT_OTHER);
    }
    if (!readfd(glob.paramspipe_s[io][0], &size, sizeof(int))) {
        fprintf(stderr, "Failed to read from pipe.\n");
        bf_exit(BF_EXIT_OTHER);
    }
    if ((params = alloca(size)) == NULL) {
        fprintf(stderr, "Could not allocate %d bytes on stack.\n", size);
        bf_exit(BF_EXIT_OTHER);
    }
    if (!readfd(glob.paramspipe_s[io][0], params, size)) {
        fprintf(stderr, "Failed to read from pipe.\n");
        bf_exit(BF_EXIT_OTHER);
    }
    struct subdev *sd = glob.dev[io][subdev_index];
    if (sd->module->command == NULL) {
        ans = -1;
        msgstr = estrdup("Module does not support any commands");
    } else {
        ans = sd->module->command(sd->fd, params);
        msgstr = estrdup(sd->module->message());
    }
    if (!writefd(glob.paramspipe_r[io][1], &ans, sizeof(int))) {
        fprintf(stderr, "Failed to write to pipe.\n");
        bf_exit(BF_EXIT_OTHER);
    }
    size = strlen(msgstr) + 1;
    if (!writefd(glob.paramspipe_r[io][1], &size, sizeof(int)) ||
        !writefd(glob.paramspipe_r[io][1], msgstr, size))
    {
        fprintf(stderr, "Failed to write to pipe.\n");
        bf_exit(BF_EXIT_OTHER);
    }
    efree(msgstr);
}

static bool_t
callback_init(int n_subdevs[2],
              struct dai_subdevice *subdevs[2])
{
    bf_sem_init(&glob.cbreadywait_pipe[IN]);
    bf_sem_init(&glob.cbreadywait_pipe[OUT]);

    /* initialise inputs */
    for (int n = 0; n < n_subdevs[IN]; n++) {
        if (!bfconf->iomods[subdevs[IN][n].module].iscallback) {
            continue;
        }
	if (!init_input(&subdevs[IN][n], n)) {
	    return false;
	}
    }

    /* initialise outputs */
    for (int n = 0; n < n_subdevs[OUT]; n++) {
        if (!bfconf->iomods[subdevs[OUT][n].module].iscallback) {
            continue;
        }
	if (!init_output(&subdevs[OUT][n], n)) {
	    return false;
	}
    }

    FOR_IN_AND_OUT {
        for (int n = 0; n < n_subdevs[IO]; n++) {
            if (!bfconf->iomods[subdevs[IO][n].module].iscallback) {
                continue;
            }
            if (glob.dev[IO][n]->bad_alignment) {
                fprintf(stderr, "\
Error: currently no support for bad callback I/O block alignment.\n\
  BruteFIR's partition length must be divisable with the sound server's\n\
  buffer size.\n");
                return false;
            }
        }
    }
    return true;
}

static void
callback_process(int n_subdevs[2],
                 struct dai_subdevice *subdevs[2],
                 bf_pid_t *wpid)
{
    bf_sem_never_wait(&glob.cbpipe_r);
    bf_sem_never_post(&glob.cbpipe_s);
    uint8_t bool_msg = (uint8_t)!!callback_init(n_subdevs, subdevs);
    bf_sem_postmsg(&glob.cbpipe_r, &bool_msg, 1);
    if (!bool_msg) {
        // callback_init() failed, wait for exit
        while (true) sleep(1000);
    }
    bf_sem_waitmsg(&glob.cbpipe_s, &bool_msg, 1);
    if (bf_is_fork_mode()) {
        // attach I/O buffers
        uint8_t *buffer;
        if ((buffer = shmalloc_attach(ca->buffer_id)) == NULL) {
            fprintf(stderr, "Failed to attach to shared memory with id %d: %s.\n", ca->buffer_id, strerror(errno));
            bool_msg = false;
        } else {
            FOR_IN_AND_OUT {
                glob.iobuffers[IO][0] = buffer;
                buffer += ca->buffer_format[IO].n_bytes;
                glob.iobuffers[IO][1] = buffer;
                buffer += ca->buffer_format[IO].n_bytes;
            }
            bool_msg = true;
        }
    } else {
        // threaded mode, we already have iobuffers shared
        bool_msg = true;
    }
    bf_sem_postmsg(&glob.cbpipe_r, &bool_msg, 1);
    if (!bool_msg) {
        // could not get buffers, wait for exit
        while (true) sleep(1000);
    }
    if (bfconf->realtime_priority) {
        bf_make_realtime(bfconf->realtime_midprio, "callback");
    }
    while (true) {
#warning removal of this break problematic?
        uint8_t msg;
        bf_sem_waitmsg(&glob.cbpipe_s, &msg, 1);
        //if (!readfd(cbpipe_s[0], &msg, 1)) {
        //    break;
        //}
        switch ((int)msg) {
        case CB_MSG_START:
            for (int n = 0; n < bfconf->n_iomods; n++) {
                if (!bfconf->iomods[n].iscallback) {
                    continue;
                }
                if (bfconf->iomods[n].synch_start() != 0) {
                    fprintf(stderr, "Failed to start I/O module, aborting.\n");
                    bf_exit(BF_EXIT_OTHER);
                }
            }
            if (wpid != NULL) {
                bf_wait_for_process_end(*wpid);
            }
            break;
        case CB_MSG_STOP:
            for (int n = 0; n < bfconf->n_iomods; n++) {
                if (!bfconf->iomods[n].iscallback) {
                    continue;
                }
                bfconf->iomods[n].synch_stop();
            }
            msg = 1;
            bf_sem_postmsg(&glob.cbpipe_r, &msg, 1);
            break;
        default:
            fprintf(stderr, "Bug: invalid msg %d, aborting.\n", (int)msg);
            bf_exit(BF_EXIT_OTHER);
            break;
        }
    }
    while (true) sleep(1000);
}

struct callback_process_thread_args {
    int n_subdevs[2];
    struct dai_subdevice *subdevs[2];
    bf_pid_t wpid;
};

static void
callback_process_thread(void *arg)
{
    struct callback_process_thread_args a = *(struct callback_process_thread_args *)arg;
    ca->callback_pid = bf_getpid();
    callback_process(a.n_subdevs, a.subdevs, bfconf->blocking_io ? NULL : &a.wpid);
}

bool_t
dai_init(int _period_size,
	 int rate,
	 int n_subdevs[2],
	 struct dai_subdevice *subdevs[2],
         void *buffers[2][2])
{
    FD_ZERO(&glob.dev_fds[IN]);
    FD_ZERO(&glob.dev_fds[OUT]);
    FD_ZERO(&glob.clocked_wfds);
    memset(glob.fd2dev, 0, sizeof(glob.fd2dev));
    memset(glob.ch2dev, 0, sizeof(glob.ch2dev));

    glob.period_size = _period_size;
    glob.sample_rate = rate;

    /* allocate shared memory for interprocess communication */
    if ((ca = maybe_shmalloc(sizeof(struct comarea))) == NULL) {
        fprintf(stderr, "Failed to allocate shared memory.\n");
        return false;
    }
    memset(ca, 0, sizeof(struct comarea));
    ca->frames_left = -1;
    ca->cb_frames_left = -1;
    FOR_IN_AND_OUT {
        dai_buffer_format[IO] = &ca->buffer_format[IO];
        glob.n_devs[IO] = n_subdevs[IO];
        for (int n = 0; n < bfconf->n_physical_channels[IO]; n++) {
            if (bfconf->n_virtperphys[IO][n] == 1) {
                ca->delay[IO][n] = bfconf->delay[IO][bfconf->phys2virt[IO][n][0]];
                ca->is_muted[IO][n] = bfconf->mute[IO][bfconf->phys2virt[IO][n][0]];
            } else {
                ca->delay[IO][n] = 0;
                ca->is_muted[IO][n] = false;
            }
        }
        for (int n = 0; n < glob.n_devs[IO]; n++) {
            glob.dev[IO][n] = &ca->dev[IO][n];
        }
    }

    bf_sem_init(&glob.cbmutex_pipe[IN]);
    bf_sem_init(&glob.cbmutex_pipe[OUT]);
    bf_sem_init(&glob.cbpipe_s);
    bf_sem_init(&glob.cbpipe_r);

    bf_sem_post(&glob.cbmutex_pipe[IN]);
    bf_sem_post(&glob.cbmutex_pipe[OUT]);

    /* initialise callback io, if any */
    if (bfconf->callback_io) {
        /*
          Notes 2025: with the old fork() model the callback_process() is run in the calling process,
          and this init continues in the child process, this is out-of-necessity hack that was
          introduced to be able to support JACK callback I/O that (unsurprisingly) doesn't work if
          the API is called in a forked child, so it must happen in the parent process.

          This hack becomes really awkward for the threaded mode though, so we handle it separately.
        */
        if (bf_is_fork_mode()) {
            pid_t fork_pid = fork();
            if (fork_pid != 0) {
                bf_pid_t pid = bf_process_id_to_bf_pid(fork_pid);
                bf_register_process(pid);
                ca->callback_pid = bf_getpid();
                callback_process(n_subdevs, subdevs, bfconf->blocking_io ? NULL : &pid);
            }
        } else {
            struct callback_process_thread_args *cb_args = emalloc(sizeof(*cb_args));
            FOR_IN_AND_OUT {
                cb_args->n_subdevs[IO] = n_subdevs[IO];
                cb_args->subdevs[IO] = subdevs[IO];
            }
            cb_args->wpid = bf_getpid();
            bf_pid_t pid = bf_fork(callback_process_thread, cb_args);
            bf_register_process(pid);
        }

        bf_sem_never_post(&glob.cbpipe_r);
        bf_sem_never_wait(&glob.cbpipe_s);
        uint8_t bool_msg;
        bf_sem_waitmsg(&glob.cbpipe_r, &bool_msg, 1);
        if (!bool_msg) {
            // callback_init() in callback_process() failed
            return false;
        }
    }

    FOR_IN_AND_OUT {
	if (pipe(glob.paramspipe_s[IO]) == -1 ||
	    pipe(glob.paramspipe_r[IO]) == -1)
	{
	    fprintf(stderr, "Failed to create pipe: %s.\n", strerror(errno));
	    return false;
	}
        bf_sem_init(&glob.synchpipe[IO]);
        bf_sem_post(&glob.synchpipe[IO]);
    }

    /* initialise inputs */
    for (int n = 0; n < n_subdevs[IN]; n++) {
        if (bfconf->iomods[subdevs[IN][n].module].iscallback) {
            continue;
        }
	if (!init_input(&subdevs[IN][n], n)) {
	    return false;
	}
    }

    /* initialise outputs */
    for (int n = 0; n < n_subdevs[OUT]; n++) {
        if (bfconf->iomods[subdevs[OUT][n].module].iscallback) {
            continue;
        }
	if (!init_output(&subdevs[OUT][n], n)) {
	    return false;
	}
    }

    /* calculate buffer format, and allocate buffers */
    FOR_IN_AND_OUT {
	calc_buffer_format(glob.period_size, IO, &ca->buffer_format[IO]);
    }
    uint8_t *buffer;
    const size_t buffer_size = 2 * dai_buffer_format[IN]->n_bytes + 2 * dai_buffer_format[OUT]->n_bytes;
    if (bf_is_fork_mode()) {
        if ((buffer = shmalloc_id(&ca->buffer_id, buffer_size)) == NULL) {
            fprintf(stderr, "Failed to allocate shared memory.\n");
            return false;
        }
    } else {
        buffer = emalloc(buffer_size);
    }
    memset(buffer, 0, buffer_size);
    FOR_IN_AND_OUT {
        glob.iobuffers[IO][0] = buffer;
        buffer += dai_buffer_format[IO]->n_bytes;
        glob.iobuffers[IO][1] = buffer;
        buffer += dai_buffer_format[IO]->n_bytes;
        buffers[IO][0] = glob.iobuffers[IO][0];
        buffers[IO][1] = glob.iobuffers[IO][1];
    }
    if (bfconf->callback_io) {

        /* some magic callback I/O init values */
        for (int n = 0; n < glob.n_devs[OUT]; n++) {
            struct subdev *sd = glob.dev[OUT][n];
            if (sd->uses_callback) {
                sd->buf_left = 0;
                sd->cb.frames_left = -1;
                sd->cb.iodelay_fill = 2 * glob.period_size / sd->block_size_frames - 2;
            }
        }

        // let callback_process() attach shared mem buffers (dummy action if not fork mode)
        uint8_t bool_msg = true;
        bf_sem_postmsg(&glob.cbpipe_s, &bool_msg, 1);
        bf_sem_waitmsg(&glob.cbpipe_r, &bool_msg, 1);
        if (!bool_msg) {
            // callback_process() failed to init buffers
            return false;
        }
    }

    /* decide if to use input poll mode */
    glob.input_poll_mode = false;
    bool_t all_bad_alignment = true;
    bool_t none_clocked = true;
    for (int n = 0; n < n_subdevs[IN]; n++) {
        struct subdev *sd = glob.dev[IN][n];
        if (sd->uses_clock && !sd->uses_callback) {
            none_clocked = false;
            if (!sd->bad_alignment) {
                all_bad_alignment = false;
            }
        }
    }
    if (bfconf->blocking_io && all_bad_alignment && !none_clocked) {
        if (!bfconf->allow_poll_mode) {
            fprintf(stderr, "\
Error: sound input hardware requires poll mode to be activated but current\n\
  configuration does not allow it (allow_poll_mode: false;).\n");
            return false;
        }
        glob.input_poll_mode = true;
        pinfo("Input poll mode activated\n");
    }
    return true;
}

void
dai_trigger_callback_io(void)
{
    uint8_t msg = CB_MSG_START;
    bf_sem_postmsg(&glob.cbpipe_s, &msg, 1);
}

int
dai_minblocksize(void)
{
    int size = INT_MAX;

    if (bfconf->blocking_io) {
        FOR_IN_AND_OUT {
            if (glob.min_block_size[IO] != 0 && glob.min_block_size[IO] < size) {
                size = glob.min_block_size[IO];
            }
        }
    }
    if (bfconf->callback_io) {
        FOR_IN_AND_OUT {
            if (glob.cb_min_block_size[IO] != 0 && glob.cb_min_block_size[IO] < size) {
                size = glob.cb_min_block_size[IO];
            }
        }
    }
    return size;
}

bool_t
dai_input_poll_mode(void)
{
    return glob.input_poll_mode;
}

bool_t
dai_isinit(void)
{
    return (dai_buffer_format[IN] != NULL);
}

void
dai_toggle_mute(const int io,
		const int channel)
{
    if ((io != IN && io != OUT) || channel < 0 || channel >= BF_MAXCHANNELS) {
	return;
    }
    ca->is_muted[io][channel] = !ca->is_muted[io][channel];
}

int
dai_change_delay(const int io,
		 const int channel,
		 const int delay)
{
    if (delay < 0 || channel < 0 || channel >= BF_MAXCHANNELS ||
        (io != IN && io != OUT) ||
        bfconf->n_virtperphys[io][channel] != 1)
    {
	return -1;
    }
    ca->delay[io][channel] = delay;
    return 0;
}

int
dai_subdev_command(int io,
                   int subdev_index,
                   const char params[],
                   char **message)
{
    static char *msgstr = NULL;

    efree(msgstr);
    msgstr = NULL;
    if (io != IN && io != OUT) {
	if (message != NULL) {
	    msgstr = estrdup("Invalid io selection");
	    *message = msgstr;
	}
	return -1;
    }
    if (subdev_index < 0 || subdev_index >= glob.n_devs[io]) {
	if (message != NULL) {
	    msgstr = estrdup("Invalid device index");
	    *message = msgstr;
	}
	return -1;
    }
    if (params == NULL) {
	if (message != NULL) {
	    msgstr = estrdup("Missing parameters");
	    *message = msgstr;
	}
	return -1;
    }
    bf_sem_wait(&glob.synchpipe[io]);

    int n = strlen(params) + 1;
    if (!writefd(glob.paramspipe_s[io][1], &subdev_index, sizeof(int)) ||
	!writefd(glob.paramspipe_s[io][1], &n, sizeof(int)) ||
	!writefd(glob.paramspipe_s[io][1], params, n))
    {
	fprintf(stderr, "Failed to write to pipe.\n");
	bf_exit(BF_EXIT_OTHER);
    }

    int ans;
    if (!readfd(glob.paramspipe_r[io][0], &ans, sizeof(int))) {
	fprintf(stderr, "Failed to read from pipe.\n");
	bf_exit(BF_EXIT_OTHER);
    }
    if (!readfd(glob.paramspipe_r[io][0], &n, sizeof(int))) {
        fprintf(stderr, "Failed to read from pipe.\n");
        bf_exit(BF_EXIT_OTHER);
    }
    msgstr = emalloc(n);
    if (!readfd(glob.paramspipe_r[io][0], msgstr, n)) {
        fprintf(stderr, "Failed to read from pipe.\n");
	    bf_exit(BF_EXIT_OTHER);
    }
    if (message != NULL) {
        *message = msgstr;
    }
    bf_sem_post(&glob.synchpipe[io]);
    return ans;
}

void
dai_die(void)
{
    if (ca == NULL) {
	return;
    }

    const bool_t iscallback = bf_pid_equal(bf_getpid(), ca->callback_pid);

    if (iscallback) {
        for (int n = 0; n < bfconf->n_iomods; n++) {
            if (bfconf->iomods[n].iscallback) {
                bfconf->iomods[n].synch_stop();
            }
        }
        return;
    }
    if (ca->blocking_stopped) {
        return;
    }

    if (bf_pid_equal(bf_getpid(), ca->pid[OUT])) {
        for (int n = 0; n < bfconf->n_iomods; n++) {
            if (!bfconf->iomods[n].iscallback && bfconf->iomods[n].synch_stop != NULL) {
                bfconf->iomods[n].synch_stop();
            }
        }
    }
    FOR_IN_AND_OUT {
	if (bf_pid_equal(bf_getpid(), ca->pid[IO])) {
	    for (int n = 0; n < bfconf->n_iomods; n++) {
                if (!bfconf->iomods[n].iscallback && bfconf->iomods[n].stop != NULL) {
                    bfconf->iomods[n].stop(IO);
                }
	    }
	}
    }
}

void
dai_input(volatile struct debug_input dbg[],
          int dbg_len,
          volatile int *dbg_loops)
{
    static struct {
        bool_t isfirst;
        bool_t startmeasure;
        int buf_index;
        int frames;
        int curbuf;
        struct timeval starttv;
    } st = {
        .isfirst = true,
        .startmeasure = true,
        .buf_index = 0,
        .frames = 0,
        .curbuf = 0,
        .starttv = {0}
    };

    int dbg_pos = 0;
    *dbg_loops = 0;

    if ((ca->frames_left != -1 && st.buf_index == ca->lastbuf_index + 1) ||
        (ca->cb_frames_left != -1 && st.buf_index == ca->cb_lastbuf_index + 1))
    {
	for (int n = 0; n < bfconf->n_iomods; n++) {
            if (bfconf->iomods[n].stop != NULL) {
                bfconf->iomods[n].stop(IN);
            }
	}
	/* There is no more data to read, just sleep and let the output process end all processes. */
	while (true) sleep(1000);
    }

    if (st.isfirst) {
	ca->pid[IN] = bf_getpid();

        timestamp(&dbg[0].init.ts_start_call);
        dai_trigger_callback_io();
	for (int n = 0; n < bfconf->n_iomods; n++) {
            if (bfconf->iomods[n].iscallback) {
                continue;
            }
	    if ((bfconf->iomods[n].start != NULL && bfconf->iomods[n].start(IN) != 0) ||
                (bfconf->iomods[n].synch_start != NULL && bfconf->iomods[n].synch_start() != 0))
            {
		fprintf(stderr, "Failed to start I/O module, aborting.\n");
		bf_exit(BF_EXIT_OTHER);
	    }
	}
        timestamp(&dbg[0].init.ts_start_ret);

    }

    uint8_t *buf = (uint8_t *)glob.iobuffers[IN][st.curbuf];
    st.curbuf = !st.curbuf;

    int devsleft = glob.n_fd_devs[IN];
    fd_set rfds = glob.dev_fds[IN];
    int minleft = glob.period_size;
    bool_t firstloop = true;
    while (devsleft != 0) {
        fd_set readfds = rfds;
	FD_SET(glob.paramspipe_s[IN][0], &readfds);

        const int fdmax = (glob.dev_fdn[IN] > glob.paramspipe_s[IN][0]) ? glob.dev_fdn[IN] : glob.paramspipe_s[IN][0];

        dbg[dbg_pos].select.fdmax = fdmax;
        timestamp(&dbg[dbg_pos].select.ts_call);

        struct timeval *ptv, zerotv_;
        if (glob.input_poll_mode) {
            if (!firstloop) {
                int64_t usec = (int64_t)minleft * 1000000 / (int64_t)glob.sample_rate;
                if (glob.min_block_size[IN] > 0) {
                    int64_t usec2 = (int64_t)glob.min_block_size[IN] * 1000000 / (int64_t)glob.sample_rate;
                    if (usec2 < usec) {
                        usec = usec2;
                    }
                }
                /* nanosleep sleeps precise in maximum 2 ms (Note 2025: statement applies
                   to 2001 linux kernels, haven't checked current status) */
                struct timespec ts;
                if (usec > 40000) {
                    ts.tv_sec = 0;
                    ts.tv_nsec = usec * 1000;
                    nanosleep(&ts, NULL);
                } else if (usec > 20000) {
                    ts.tv_sec = 0;
                    ts.tv_nsec = 10000000;
                    nanosleep(&ts, NULL);
                } else if (usec > 2050) {
                    ts.tv_sec = 0;
                    ts.tv_nsec = 2000000;
                    nanosleep(&ts, NULL);
                } else if (usec > 50) {
                    ts.tv_sec = 0;
                    ts.tv_nsec = (usec - 50) * 1000;
                    nanosleep(&ts, NULL);
                }
            }
            ptv = &zerotv_;
            zerotv_ = (struct timeval){0};
        } else {
            ptv = NULL;
        }

        int fdn;
        while ((fdn = select(fdmax + 1, &readfds, NULL, NULL, ptv)) == -1 && errno == EINTR);

        timestamp(&dbg[dbg_pos].select.ts_ret);
        dbg[dbg_pos].select.retval = fdn;

	if (fdn == -1) {
	    fprintf(stderr, "Select failed: %s.\n", strerror(errno));
	    bf_exit(BF_EXIT_OTHER);
	}

        for (int n = 0; n < glob.n_devs[IN]; n++) {
            struct subdev *sd = glob.dev[IN][n];
            if (sd->uses_clock && !sd->uses_callback && !FD_ISSET(sd->fd, &readfds) &&
                (glob.input_poll_mode || sd->bad_alignment))
            {
                FD_SET(sd->fd, &readfds);
                fdn++;
            }
        }

	int fd = -1;
	while (fdn--) {
            for (fd++; !FD_ISSET(fd, &readfds) && fd <= fdmax; fd++);
	    if (fd == glob.paramspipe_s[IN][0]) {
		handle_params(IN);
		continue;
	    }
	    struct subdev *sd = glob.fd2dev[IN][fd];

            dbg[dbg_pos].read.fd = fd;
            dbg[dbg_pos].read.buf = buf + sd->buf_offset;
            dbg[dbg_pos].read.offset = sd->buf_size - sd->buf_left;
            dbg[dbg_pos].read.count = sd->buf_left;
            timestamp(&dbg[dbg_pos].read.ts_call);

	    const int byte_count = sd->module->read(fd, buf + sd->buf_offset, sd->buf_size - sd->buf_left, sd->buf_left);

            timestamp(&dbg[dbg_pos].read.ts_ret);
            dbg[dbg_pos].read.retval = byte_count;
            if (++dbg_pos == dbg_len) {
                dbg_pos = 0;
            }
            (*dbg_loops)++;

	    switch (byte_count) {
	    case -1: {
		switch (errno) {
		case EINTR:
		case EAGAIN:
		    /* try again later */
		    break;
		case EIO:
		    /* invalid input signal */
		    fprintf(stderr, "I/O module failed to read due to invalid input signal, aborting.\n");
		    bf_exit(BF_EXIT_INVALID_INPUT);
		    break;
		case EPIPE:
		    /* buffer underflow */

                    /* Actually, this should be overflow, but since we have
                       linked the devices, broken pipe on output will be
                       noted on the input as well, and it is more likely that
                       it is an underflow on the output than an overflow on
                       the input */
		    fprintf(stderr, "I/O module failed to read (probably) due to buffer underflow on output, aborting.\n");
		    bf_exit(BF_EXIT_BUFFER_UNDERFLOW);
		    break;
		default:
		    /* general error */
		    fprintf(stderr, "I/O module failed to read, aborting.\n");
		    bf_exit(BF_EXIT_OTHER);
		    break;
		}
		break;
            }
	    case 0: {
		if (sd->isinterleaved) {
		    memset(buf + sd->buf_offset + sd->buf_size - sd->buf_left, 0, sd->buf_left);
		} else {
		    const int i = sd->buf_size / sd->channels.open_channels;
		    const int k = sd->buf_left / sd->channels.open_channels;
		    for (int n = 1; n <= sd->channels.open_channels; n++) {
			memset(buf + sd->buf_offset + n * i - k, 0, k);
		    }
		}
		devsleft--;
		FD_CLR(fd, &rfds);

		const int frames_left = (sd->buf_size - sd->buf_left) / sd->channels.sf.bytes / sd->channels.open_channels;
		if (ca->frames_left == -1 || frames_left < ca->frames_left) {
		    ca->frames_left = frames_left;
		}
		ca->lastbuf_index = st.buf_index;
		break;
            }
	    default: {
		sd->buf_left -= byte_count;
		if (glob.monitor_rate_fd == fd) {
		    if (st.startmeasure) {
                        if (sd->buf_left == 0) {
                            st.startmeasure = false;
                            gettimeofday(&st.starttv, NULL);
                        }
		    } else {
			st.frames += byte_count / (sd->buf_size / glob.period_size);
                        if (st.frames >= glob.sample_rate && sd->buf_left == 0) {
                            struct timeval tv;
                            gettimeofday(&tv, NULL);
                            timersub(&tv, &st.starttv, &tv);
                            double measured_rate = 1000.0 * (double)st.frames / (double)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
                            if (bfconf->debug) {
                                fprintf(stderr, "measured rate: %.3f kHz (%d frames / %ld usecs)\n",
                                        measured_rate / 1000.0, st.frames, tv.tv_sec * 1000000 + tv.tv_usec);
                            }
                            if (measured_rate < (double)glob.sample_rate * 0.98 ||
                                measured_rate > (double)glob.sample_rate / 0.98)
                            {
                                fprintf(stderr, "Configured sample rate is %.1f kHz, but measured is %.1f kHz, aborting.\n",
                                        (double)glob.sample_rate / 1000.0, measured_rate / 1000.0);
                                bf_exit(BF_EXIT_INVALID_INPUT);
                            }
                            st.startmeasure = true;
                            st.frames = 0;
                        }
		    }
		}
                const int frames_left = sd->buf_left / (sd->buf_size / glob.period_size);
                if (sd->uses_clock && (frames_left < minleft || minleft == -1)) {
                    minleft = frames_left;
                }
		if (sd->buf_left == 0) {
		    sd->buf_left = sd->buf_size;
		    devsleft--;
		    FD_CLR(fd, &rfds);
		}
		break;
            }
	    }
	}
        firstloop = false;
    }

    for (int n = 0; n < glob.n_devs[IN]; n++) {
        struct subdev *sd = glob.dev[IN][n];
        if (!sd->uses_callback) {
            do_mute(sd, IN, sd->buf_size, (void *)(buf + sd->buf_offset), 0);
            update_delay(sd, IN, buf);
        }
    }

    st.isfirst = false;
    st.buf_index++;
}

void
dai_output(bool_t iodelay_fill,
           bf_sem_t *synch_fd,
           volatile struct debug_output dbg[],
           int dbg_len,
           volatile int *dbg_loops)
{
    static struct {
        bool_t isfirst;
        bool_t islast;
        int buf_index;
        int curbuf;
        fd_set readfds;
    } st = {
        .isfirst = true,
        .islast = false,
        .buf_index = 0,
        .curbuf = 0,
        .readfds = {}
    };

    int dbg_pos = 0;
    *dbg_loops = 0;

    if ((ca->frames_left != -1 && st.buf_index == ca->lastbuf_index) ||
        (ca->cb_frames_left != -1 && st.buf_index == ca->cb_lastbuf_index))
    {
	int frames_left = ca->frames_left;
        if (frames_left == -1 || ( ca->cb_frames_left != -1 &&  ca->cb_frames_left < frames_left)) {
            frames_left =  ca->cb_frames_left;
        }
	for (int n = 0; n < glob.n_devs[OUT]; n++) {
            struct subdev *sd = glob.dev[OUT][n];
            if (!sd->uses_callback) {
                sd->buf_left = sd->buf_size = frames_left * sd->channels.sf.bytes * sd->channels.open_channels;
            }
	}
        st.islast = true;
    }
    if (st.isfirst) {
	FD_ZERO(&st.readfds); // in theory FD_ZERO() can be different from just = {}
    }

    uint8_t *buf = (uint8_t *)glob.iobuffers[OUT][st.curbuf];
    st.curbuf = !st.curbuf;

    for (int n = 0; n < glob.n_devs[OUT]; n++) {
        struct subdev *sd = glob.dev[OUT][n];
        if (sd->uses_callback) {
            continue;
        }
        update_delay(sd, OUT, buf);
    }

    int devsleft;
    fd_set wfds;
    if (iodelay_fill) {
        wfds = glob.clocked_wfds;
        devsleft = glob.n_clocked_devs;
    } else {
        wfds = glob.dev_fds[OUT];
        devsleft = glob.n_fd_devs[OUT];
    }

    while (devsleft != 0) {
        fd_set writefds = wfds;
	FD_SET(glob.paramspipe_s[OUT][0], &st.readfds);
        int fdn = (glob.dev_fdn[OUT] > glob.paramspipe_s[OUT][0]) ? glob.dev_fdn[OUT] : glob.paramspipe_s[OUT][0];

        dbg[dbg_pos].select.fdmax = fdn + 1;
        timestamp(&dbg[dbg_pos].select.ts_call);

	while ((fdn = select(fdn + 1, &st.readfds, &writefds, NULL, NULL)) == -1 && errno == EINTR);

        timestamp(&dbg[dbg_pos].select.ts_ret);
        dbg[dbg_pos].select.retval = fdn;

	if (fdn == -1) {
	    fprintf(stderr, "Select failed: %s.\n", strerror(errno));
	    bf_exit(BF_EXIT_OTHER);
	}
        if (FD_ISSET(glob.paramspipe_s[OUT][0], &st.readfds)) {
            handle_params(OUT);
            fdn--;
        }

	int fd = -1;
	while (fdn--) {
            for (fd++; !FD_ISSET(fd, &writefds) && fd <= glob.dev_fdn[OUT]; fd++);
	    struct subdev *sd = glob.fd2dev[OUT][fd];
            int write_size;
	    if (sd->block_size > 0 && sd->buf_left > sd->block_size) {
		write_size = sd->block_size + sd->buf_left % sd->block_size;
	    } else {
		write_size = sd->buf_left;
	    }
	    do_mute(sd, OUT, write_size, (void *)(buf + sd->buf_offset), sd->buf_size - sd->buf_left);

            dbg[dbg_pos].write.fd = fd;
            dbg[dbg_pos].write.buf = buf + sd->buf_offset;
            dbg[dbg_pos].write.offset = sd->buf_size - sd->buf_left;
            dbg[dbg_pos].write.count = write_size;
            timestamp(&dbg[dbg_pos].write.ts_call);

	    int byte_count = sd->module->write(fd, buf + sd->buf_offset, sd->buf_size - sd->buf_left, write_size);

            timestamp(&dbg[dbg_pos].write.ts_ret);
            dbg[dbg_pos].write.retval = byte_count;
            if (++dbg_pos == dbg_len) {
                dbg_pos = 0;
            }
            (*dbg_loops)++;

	    switch (byte_count) {
	    case -1:
                switch (errno) {
                case EINTR:
                case EAGAIN:
                    /* try again later */
                    break;
                case EPIPE:
                    /* buffer underflow */
                    fprintf(stderr, "I/O module failed to write due to buffer underflow, aborting.\n");
                    bf_exit(BF_EXIT_BUFFER_UNDERFLOW);
                    break;
                default:
                    /* general error */
                    fprintf(stderr, "I/O module failed to write, aborting.\n");
                    bf_exit(BF_EXIT_OTHER);
                    break;
                }
		break;

	    default:
		sd->buf_left -= byte_count;
		break;
	    }
	    if (sd->buf_left == 0) {
		sd->buf_left = sd->buf_size;
		devsleft--;
		FD_CLR(fd, &wfds);
	    }
	}
        if (synch_fd != NULL) {
            timestamp(&dbg[0].init.ts_synchfd_call);
            bf_sem_post(synch_fd);
            sched_yield(); /* let input process start now */
            timestamp(&dbg[0].init.ts_synchfd_ret);
            synch_fd = NULL;
        }
	if (!iodelay_fill && st.isfirst) {
	    st.isfirst = false;

            timestamp(&dbg[0].init.ts_start_call);
	    for (int n = 0; n < bfconf->n_iomods; n++) {
                if (bfconf->iomods[n].iscallback) {
                    continue;
                }
		if (bfconf->iomods[n].start != NULL && bfconf->iomods[n].start(OUT) != 0) {
		    fprintf(stderr, "I/O module failed to start, aborting.\n");
		    bf_exit(BF_EXIT_OTHER);
		}
	    }
            timestamp(&dbg[0].init.ts_start_ret);
	    ca->pid[OUT] = bf_getpid();
	}
    }

    if (iodelay_fill) {
        return;
    }

    if (st.islast) {
	for (int n = 0; n < bfconf->n_iomods; n++) {
            if (bfconf->iomods[n].iscallback) {
                /* callback I/O is stopped elsewhere */
                continue;
            }
            if (bfconf->iomods[n].synch_stop != NULL) {
                bfconf->iomods[n].synch_stop();
            }
            if (bfconf->iomods[n].stop != NULL) {
                bfconf->iomods[n].stop(OUT);
            }
	}
	ca->blocking_stopped = true;
        for (int n = 0; n < glob.n_devs[OUT]; n++) {
            struct subdev *sd = glob.dev[OUT][n];
            if (!sd->uses_callback) {
                sd->finished = true;
            }
        }
        if (output_finish()) {
            bf_exit(BF_EXIT_OK);
        } else {
            while (true) sleep(1000);
        }
    }

    st.buf_index++;
}

static void
process_callback_input(struct subdev *sd,
                       void *cbbufs[],
                       int frame_count)
{
    uint8_t *buf = (uint8_t *)glob.iobuffers[IN][sd->cb.curbuf];

    const int count = frame_count * sd->channels.used_channels * sd->channels.sf.bytes;
    if (sd->isinterleaved) {
        memcpy(buf + sd->buf_offset + sd->buf_size - sd->buf_left, cbbufs[0], count);
    } else {
        const struct buffer_format *bf = &dai_buffer_format[IN]->bf[sd->channels.channel_name[0]];
        const int cnt = count / sd->channels.used_channels;
        uint8_t *copybuf = buf + sd->buf_offset + (sd->buf_size - sd->buf_left) / sd->channels.used_channels;
        for (int n = 0; n < sd->channels.used_channels; n++) {
            memcpy(copybuf, cbbufs[n], cnt);
            copybuf += glob.period_size * bf->sf.sbytes;
        }
    }
    sd->buf_left -= count;
    if (sd->buf_left == 0) {
        sd->cb.curbuf = !sd->cb.curbuf;
        do_mute(sd, IN, sd->buf_size, (void *)(buf + sd->buf_offset), 0);
        update_delay(sd, IN, buf);
    }
}

static void
process_callback_output(struct subdev *sd,
                        void *cbbufs[],
                        int frame_count,
                        bool_t iodelay_fill)
{
    uint8_t *buf = (uint8_t *)glob.iobuffers[OUT][sd->cb.curbuf];

    const int count = frame_count * sd->channels.used_channels * sd->channels.sf.bytes;

    if (iodelay_fill) {
        if (sd->isinterleaved) {
            memset(cbbufs[0], 0, count);
        } else {
            const int cnt = count / sd->channels.used_channels;
            for (int n = 0; n < sd->channels.used_channels; n++) {
                memset(cbbufs[n], 0, cnt);
            }
        }
        return;
    }

    if (sd->buf_left == sd->buf_size) {
        update_delay(sd, OUT, buf);
    }
    do_mute(sd, OUT, count, (void *)(buf + sd->buf_offset), sd->buf_size - sd->buf_left);
    if (sd->isinterleaved) {
        memcpy(cbbufs[0], buf + sd->buf_offset + sd->buf_size - sd->buf_left, count);
    } else {
        const struct buffer_format *bf = &dai_buffer_format[OUT]->bf[sd->channels.channel_name[0]];
        const int cnt = count / sd->channels.used_channels;
        const uint8_t *copybuf = buf + sd->buf_offset + (sd->buf_size - sd->buf_left) / sd->channels.used_channels;
        for (int n = 0; n < sd->channels.used_channels; n++) {
            memcpy(cbbufs[n], copybuf, cnt);
            copybuf += glob.period_size * bf->sf.sbytes;
        }
    }

    sd->buf_left -= count;
    if (sd->buf_left == 0) {
        sd->cb.curbuf = !sd->cb.curbuf;
    }
}

static void
trigger_callback_ready(int io)
{
    if (glob.callback_ready_waiting[io] > 0) {
        bf_sem_postmany(&glob.cbreadywait_pipe[io], glob.callback_ready_waiting[io]);
        glob.callback_ready_waiting[io] = 0;
    }
    cbmutex(io, false);
}

static void
wait_callback_ready(int io)
{
    glob.callback_ready_waiting[io]++;
    cbmutex(io, false);
    bf_sem_wait(&glob.cbreadywait_pipe[io]);
}

static int
process_callback(void **states[2],
                 int state_count[2],
                 void **buffers[2],
                 int frame_count,
                 int event)
{
    switch (event) {
    case BF_CALLBACK_EVENT_LAST_INPUT:
        if (ca->cb_frames_left == -1 || frame_count < ca->cb_frames_left) {
            ca->cb_frames_left = frame_count;
        }
        ca->cb_lastbuf_index = ca->cb_buf_index[IN];
        return 0;
    case BF_CALLBACK_EVENT_FINISHED:
        for (int n = 0; n < state_count[OUT]; n++) {
            struct subdev *sd = (struct subdev *)states[OUT][n];
            sd->finished = true;
        }
        cbmutex(IN, true);
        trigger_callback_ready(IN);
        cbmutex(OUT, true);
        trigger_callback_ready(OUT);
        if (output_finish()) {
            bf_exit(BF_EXIT_OK);
        }
        return -1;
    case BF_CALLBACK_EVENT_ERROR:
        fprintf(stderr, "An error occurred in a callback I/O module.\n");
        bf_exit(BF_EXIT_OTHER);
        break;
    case BF_CALLBACK_EVENT_NORMAL:
        break;
    default:
        fprintf(stderr, "Invalid event: %d\n", event);
        bf_exit(BF_EXIT_OTHER);
        break;
    }

    if (states == NULL || state_count == NULL || buffers == NULL || frame_count <= 0) {
        fprintf(stderr, "Invalid parameters: states %p; state_count %p; buffers: %p; frame_count: %d\n",
                states, state_count, buffers, frame_count);
        bf_exit(BF_EXIT_OTHER);
    }

    if (state_count[IN] > 0) {

        cbmutex(IN, true);

        for (int n = 0, i = 0; n < state_count[IN]; n++) {
            struct subdev *sd = (struct subdev *)states[IN][n];
            if (frame_count != sd->block_size_frames) {
                fprintf(stderr, "Error: unexpected callback I/O block alignment (got %d, expected %d)\n",
                        frame_count, sd->block_size_frames);
                bf_exit(BF_EXIT_OTHER);
            }
            process_callback_input(sd, &buffers[IN][i], frame_count);
            if (sd->isinterleaved) {
                i++;
            } else {
                i += sd->channels.used_channels;
            }
        }

        struct subdev *sd = (struct subdev *)states[IN][0];
        if (sd->buf_left == 0) {
            bool_t finished = true;
            for (int n = 0; n < glob.n_devs[IN]; n++) {
                sd = glob.dev[IN][n];
                if (sd->uses_callback && sd->buf_left != 0) {
                    finished = false;
                    break;
                }
            }
            if (finished) {
                for (int n = 0; n < glob.n_devs[IN]; n++) {
                    sd = glob.dev[IN][n];
                    if (sd->uses_callback) {
                        sd->buf_left = sd->buf_size;
                    }
                }
                bf_callback_ready(IN);
                ca->cb_buf_index[IN]++;
                trigger_callback_ready(IN);
            } else {
                wait_callback_ready(IN);
            }
        } else {
            cbmutex(IN, false);
        }
    }

    if (state_count[OUT] > 0) {
        struct subdev *sd;

        cbmutex(OUT, true);

        bool_t unlock_output = false;
        sd = (struct subdev *)states[OUT][0];
        if (sd->buf_left == 0 && sd->cb.iodelay_fill == 0) {
            bool_t finished = true;
            for (int n = 0; n < glob.n_devs[OUT]; n++) {
                sd = glob.dev[OUT][n];
                if (sd->uses_callback &&
                    (sd->buf_left != 0 || sd->cb.iodelay_fill != 0))
                {
                    finished = false;
                    break;
                }
            }
            if (finished) {
                for (int n = 0; n < glob.n_devs[OUT]; n++) {
                    sd = glob.dev[OUT][n];
                    if (sd->uses_callback) {
                        sd->buf_left = sd->buf_size;
                    }
                }
                bf_callback_ready(OUT);
                ca->cb_buf_index[OUT]++;
                trigger_callback_ready(OUT);
            } else {
                wait_callback_ready(OUT);
            }
        } else {
            unlock_output = true;
        }

        for (int n = 0, i = 0; n < state_count[OUT]; n++) {
            sd = (struct subdev *)states[OUT][n];
            if (frame_count != sd->block_size_frames) {
                fprintf(stderr, "Error: unexpected callback I/O block alignment (%d != %d)\n",
                        frame_count, sd->block_size_frames);
                bf_exit(BF_EXIT_OTHER);
            }
            process_callback_output(sd, &buffers[OUT][i], frame_count, sd->cb.iodelay_fill != 0);
            if (sd->cb.iodelay_fill != 0) {
                sd->cb.iodelay_fill--;
            }
            if (sd->isinterleaved) {
                i++;
            } else {
                i += sd->channels.used_channels;
            }
        }

        if (unlock_output) {
            cbmutex(OUT, false);
        }

        /* last buffer? */
        const int buf_index = ca->cb_buf_index[IN] < ca->cb_buf_index[OUT] ? ca->cb_buf_index[OUT] : ca->cb_buf_index[IN];
        sd = (struct subdev *)states[OUT][0];
        if (sd->cb.frames_left == -1 &&
            ((ca->frames_left != -1 && buf_index == ca->lastbuf_index + 1) ||
             (ca->cb_frames_left != -1 && buf_index == ca->cb_lastbuf_index + 1)))
        {
            if (ca->frames_left == -1 ||
                (ca->frames_left > ca->cb_frames_left && ca->cb_frames_left != -1))
            {
                ca->frames_left = ca->cb_frames_left;
            }
            sd->cb.frames_left = ca->frames_left;
        }

        if (sd->cb.frames_left != -1) {
            if (sd->cb.frames_left > sd->block_size_frames) {
                sd->cb.frames_left -= sd->block_size_frames;
                return 0;
            }
            if (sd->cb.frames_left == 0) {
                return -1;
            }
            return sd->cb.frames_left;
        }

    }

    return 0;
}
