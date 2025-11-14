/*
 * (c) Copyright 2001 - 2005, 2009, 2013, 2025 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#define index _index /* hack to avoid shadowing of index() */
#define ALSA_PCM_NEW_HW_PARAMS_API
#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>
#undef index

#include "defs.h"
#include "log2.h"
#include "bit.h"
#include "compat.h"
#include "emalloc.h"
#define IS_BFIO_MODULE
#include "bfmod.h"
#include "inout.h"

#define ERRORSIZE 1024

struct alsa_access_state {
    snd_pcm_t *handle;
    bool_t isinterleaved;
    bool_t ismmap;
    bool_t ignore_xrun;
    int sw_period_size;
    int device_period_size;
    int sample_size;
    int used_channels;
    int open_channels;
    void **bufs;
    int *channel_selection;
    char *device;
    bool_t restart;

    struct timespec frame_ts;
    int64_t frame_count;
};

struct settings {
    snd_pcm_access_t forced_access_mode;
    bool_t force_access;
    bool_t ignore_xrun;
    char *device;
    char *timer_source;
};

static struct {
    snd_pcm_t *handles[2][BF_MAXCHANNELS];
    int n_handles[2];
    struct alsa_access_state fd2as[FD_SETSIZE];
    snd_pcm_t *base_handle;
    bool_t debug;
    bool_t link_handles;
    snd_output_t *out;
} glob = {
    .handles = {},
    .n_handles = { 0, 0 },
    .fd2as = {},
    .base_handle = NULL,
    .debug = false,
    .link_handles = true,
    .out = NULL
};

/*
static void
debug_dump(snd_pcm_t *handle)
{
    snd_output_t *log;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);


    if (snd_output_stdio_attach(&log, stderr, 0) < 0) {
        fprintf(stderr, "Failed to attach ALSA log to stderr.\n");
        return;
    }

    snd_pcm_dump(handle, log);

    snd_pcm_status_t *status;
    snd_pcm_status_alloca(&status);
    if (snd_pcm_status(handle, status) >= 0) {
        snd_pcm_status_dump(status, log);
    }
    snd_output_close(log);

    fprintf(stderr, "Current time  : %ld.%06ld (MONOTONIC)\n", (long)ts.tv_sec, (long)ts.tv_nsec / 1000);
}
*/

static bool_t
set_params(snd_pcm_t *handle,
           const struct settings *settings,
	   int sample_format,
	   int sample_rate,
	   int open_channels,
	   int period_size,
	   int *hardware_period_size,
	   int *sample_size,
	   int *isinterleaved,
           int *ismmap,
	   char errstr[])
{
    if (log2_get(period_size) == -1) {
	sprintf(errstr, "  Invalid software period size (%d): must be a power of 2.\n", period_size);
	return false;
    }

    int format;
    switch (sample_format) {
    case BF_SAMPLE_FORMAT_S8:
	format = SND_PCM_FORMAT_S8;
	*sample_size = 1;
	break;
    case BF_SAMPLE_FORMAT_S16_LE:
	format = SND_PCM_FORMAT_S16_LE;
	*sample_size = 2;
	break;
    case BF_SAMPLE_FORMAT_S16_BE:
	format = SND_PCM_FORMAT_S16_BE;
	*sample_size = 2;
	break;
    case BF_SAMPLE_FORMAT_S24_LE:
	format = SND_PCM_FORMAT_S24_3LE;
	*sample_size = 3;
	break;
    case BF_SAMPLE_FORMAT_S24_BE:
	format = SND_PCM_FORMAT_S24_3BE;
	*sample_size = 3;
	break;
    case BF_SAMPLE_FORMAT_S24_4LE:
	format = SND_PCM_FORMAT_S24_LE;
	*sample_size = 4;
	break;
    case BF_SAMPLE_FORMAT_S24_4BE:
	format = SND_PCM_FORMAT_S24_BE;
	*sample_size = 4;
	break;
    case BF_SAMPLE_FORMAT_S32_LE:
	format = SND_PCM_FORMAT_S32_LE;
	*sample_size = 4;
	break;
    case BF_SAMPLE_FORMAT_S32_BE:
	format = SND_PCM_FORMAT_S32_BE;
	*sample_size = 4;
	break;
    case BF_SAMPLE_FORMAT_FLOAT_LE:
	format = SND_PCM_FORMAT_FLOAT_LE;
	*sample_size = 4;
	break;
    case BF_SAMPLE_FORMAT_FLOAT_BE:
	format = SND_PCM_FORMAT_FLOAT_BE;
	*sample_size = 4;
	break;
    case BF_SAMPLE_FORMAT_FLOAT64_LE:
	format = SND_PCM_FORMAT_FLOAT64_LE;
	*sample_size = 8;
	break;
    case BF_SAMPLE_FORMAT_FLOAT64_BE:
	format = SND_PCM_FORMAT_FLOAT64_BE;
	*sample_size = 8;
	break;
    default:
	sprintf(errstr, "  Unsupported sample format.\n");
	return false;
    }

    snd_pcm_sw_params_t *swparams;
    snd_pcm_hw_params_t *params;
    int err;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_sw_params_alloca(&swparams);
    if ((err = snd_pcm_hw_params_any(handle, params)) < 0) {
	sprintf(errstr, "  Could not get any hardware configuration: %s.\n", snd_strerror(err));
	return false;
    }

    if (settings->force_access) {
	if ((err = snd_pcm_hw_params_set_access(handle, params, settings->forced_access_mode)) < 0) {
	    sprintf(errstr, "  Failed to set forced access mode: %s.\n", snd_strerror(err));
	    return false;
	}
        *isinterleaved =
            settings->forced_access_mode == SND_PCM_ACCESS_MMAP_INTERLEAVED ||
            settings->forced_access_mode == SND_PCM_ACCESS_RW_INTERLEAVED;
        *ismmap =
            settings->forced_access_mode == SND_PCM_ACCESS_MMAP_INTERLEAVED ||
            settings->forced_access_mode == SND_PCM_ACCESS_MMAP_NONINTERLEAVED ||
            settings->forced_access_mode == SND_PCM_ACCESS_MMAP_COMPLEX;
    } else {
        const struct {
            snd_pcm_access_t mode;
            bool_t is_interleaved;
            bool_t is_mmap;
        } modes[4] = {
            { SND_PCM_ACCESS_MMAP_INTERLEAVED, true, true },
            { SND_PCM_ACCESS_MMAP_NONINTERLEAVED, false, true },
            { SND_PCM_ACCESS_RW_INTERLEAVED, true, false },
            { SND_PCM_ACCESS_RW_NONINTERLEAVED, false, true }
        };
        bool_t found_mode = false;
        for (int i = 0; i < 4; i++) {
            if (snd_pcm_hw_params_set_access(handle, params, modes[i].mode) >= 0) {
                *isinterleaved = modes[i].is_interleaved;
                *ismmap = modes[i].is_mmap;
                found_mode = true;
                break;
            }
        }
        if (!found_mode) {
	    sprintf(errstr, "  Failed to set interleaved and non-interleaved access mode, with and without mmap: %s.\n",
                    snd_strerror(err));
	    return false;
	}
    }


    unsigned int un;
    /* It seems like it is best to set_rate_near instead of exact, have had problems with ens1371 */
    un = sample_rate;
    if ((err = snd_pcm_hw_params_set_rate_near(handle, params, &un, NULL)) < 0) {
	sprintf(errstr, "  Failed to set sample rate to %d Hz: %s.\n", sample_rate, snd_strerror(err));
	return false;
    }
    /* accept a minor variation in sample rate */
    if (un != sample_rate && !((int)((double)sample_rate * 0.99) < un &&
                               (int)((double)sample_rate / 0.99) > un))
    {
        sprintf(errstr, "  Failed to set sample rate to %d Hz, device suggested %u Hz instead.\n", sample_rate, un);
        return false;
    }

    if ((err = snd_pcm_hw_params_set_format(handle, params, format)) < 0) {
	sprintf(errstr, "  Failed to set sample format to %s: %s.\n", bf_strsampleformat(sample_format), snd_strerror(err));
	return false;
    }
    if ((err = snd_pcm_hw_params_set_channels(handle, params, open_channels)) < 0) {
	sprintf(errstr, "  Failed to set channel count to %d: %s.\n", open_channels, snd_strerror(err));
	return false;
    }

    snd_pcm_hw_params_get_periods_max(params, &un, NULL);
    if (un < 2) {
	/* really strange hardware if this happens */
	sprintf(errstr,
"  Hardware does not support enough periods. At least 2 is required, but the\n\
  hardware supports only %u.\n", un);
	return false;
    }

    /* try to get a hardware fragment size close to the software size */
    snd_pcm_uframes_t hw_period_size = period_size;
    snd_pcm_hw_params_set_period_size_near(handle, params, &hw_period_size, NULL);
    /* if the number of periods is only one, decrease the period size until we get at least two periods */
    snd_pcm_hw_params_get_periods(params, &un, NULL);
    snd_pcm_uframes_t try_hw_period_size = hw_period_size;
    while (un == 1 && try_hw_period_size != 0) {
	try_hw_period_size /= 2;
	hw_period_size = try_hw_period_size;
	snd_pcm_hw_params_set_period_size_near(handle, params, &hw_period_size, NULL);
        snd_pcm_hw_params_get_periods(params, &un, NULL);
    }
    if (hw_period_size == 0) {
	/* this should never happen, since we have checked that the hardware supports at least two periods */
	sprintf(errstr, "  Could not set period size.\n");
	return false;
    }
    if (snd_pcm_hw_params(handle, params) < 0) {
	sprintf(errstr, "  Unable to install hw params.\n");
	return false;
    }
    /* configure to start when explicitly told so */
    snd_pcm_sw_params_current(handle, swparams);
    if ((err = snd_pcm_sw_params_set_start_threshold(handle, swparams, ~0U)) < 0) {
	sprintf(errstr, "  Failed to set start threshold: %s.\n", snd_strerror(err));
	return false;
    }

    /* configure to stop when buffer underflow is detected */
    snd_pcm_uframes_t frames;
    snd_pcm_hw_params_get_buffer_size(params, &frames);
    if ((err = snd_pcm_sw_params_set_stop_threshold(handle, swparams, frames)) < 0) {
        sprintf(errstr, "  Failed to set stop threshold: %s.\n", snd_strerror(err));
        return false;
    }

    snd_pcm_hw_params_get_period_size(params, &hw_period_size, NULL);
    *hardware_period_size = (int)hw_period_size;
    if ((err = snd_pcm_sw_params_set_avail_min(handle, swparams, 1)) < 0) {
        sprintf(errstr, "  Failed to set min avail to 1: %s.\n", snd_strerror(err));
        return false;
    }

    if ((err = snd_pcm_sw_params(handle, swparams)) < 0) {
	sprintf(errstr, "  Unable to install sw params: %s.\n", snd_strerror(err));
	return false;
    }
    if ((err = snd_pcm_prepare(handle)) < 0) {
	sprintf(errstr, "  Unable to prepare audio: %s.\n", snd_strerror(err));
	return false;
    }

    if (glob.debug) {
        snd_pcm_dump(handle, glob.out);
    }

    return true;
}

#define GET_TOKEN(token, errstr)                                               \
    if (get_config_token(&lexval) != token) {                                  \
        fprintf(stderr, "ALSA I/O: Parse error: " errstr);                     \
        return NULL;                                                           \
    }

void *
bfio_preinit(int *version_major,
             int *version_minor,
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
    static bool_t has_been_called = false;

    const int ver = *version_major;
    *version_major = BF_VERSION_MAJOR;
    *version_minor = BF_VERSION_MINOR;
    if (ver != BF_VERSION_MAJOR) {
        return NULL;
    }
    glob.debug = !!_debug;

    int err;
    if (!has_been_called && (err = snd_output_stdio_attach(&glob.out, stderr, 0)) != 0) {
	fprintf(stderr, "ALSA I/O: Unable to attach output: %s.\n", snd_strerror(err));
        return NULL;
    }

    struct settings *settings = malloc(sizeof(struct settings));
    memset(settings, 0, sizeof(struct settings));

    union bflexval lexval;
    int token;
    while ((token = get_config_token(&lexval)) > 0) {
        if (token != BF_LEXVAL_FIELD) {
            fprintf(stderr, "ALSA I/O: Parse error: expected field.\n");
            return NULL;
        }
        if (strcmp(lexval.field, "param") == 0 || /* param for compability */
            strcmp(lexval.field, "device") == 0)
        {
            if (settings->device != NULL) {
                fprintf(stderr, "ALSA I/O: Parse error: device already set.\n");
                return NULL;
            }
            GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
            settings->device = estrdup(lexval.string);
        } else if (strcmp(lexval.field, "ignore_xrun") == 0) {
            GET_TOKEN(BF_LEXVAL_BOOLEAN, "expected boolean value.\n");
            settings->ignore_xrun = lexval.boolean;
        } else if (strcmp(lexval.field, "access") == 0) {
            GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
            settings->force_access = true;
            if (ascii_strcasecmp(lexval.string, "MMAP_INTERLEAVED") == 0) {
                settings->forced_access_mode = SND_PCM_ACCESS_MMAP_INTERLEAVED;
            } else if (ascii_strcasecmp(lexval.string, "MMAP_NONINTERLEAVED") == 0) {
                settings->forced_access_mode = SND_PCM_ACCESS_MMAP_NONINTERLEAVED;
            } else if (ascii_strcasecmp(lexval.string, "MMAP_COMPLEX") == 0) {
                // Note 2025: as far as I know MMAP_COMPLEX is not used by any driver
                fprintf(stderr, "ALSA I/O: MMAP_COMPLEX not supported");
                return NULL;
            } else if (ascii_strcasecmp(lexval.string, "RW_INTERLEAVED") == 0) {
                settings->forced_access_mode = SND_PCM_ACCESS_RW_INTERLEAVED;
            } else if (ascii_strcasecmp(lexval.string, "RW_NONINTERLEAVED") == 0) {
                settings->forced_access_mode = SND_PCM_ACCESS_RW_NONINTERLEAVED;
            } else {
                fprintf(stderr, "ALSA I/O: unknown access mode: %s.\n", lexval.string);
                return NULL;
            }
        } else if (strcmp(lexval.field, "link") == 0) {
            GET_TOKEN(BF_LEXVAL_BOOLEAN, "expected boolean value.\n");
            if (has_been_called && lexval.boolean != glob.link_handles) {
                fprintf(stderr, "ALSA I/O: \"link\" is a global setting, if set on more than one device, the\n"
                        "  value must be the same.\n");
                return NULL;
            }
            glob.link_handles = lexval.boolean;
        } else {
            fprintf(stderr, "ALSA I/O: Parse error: unknown field.\n");
            return NULL;
        }
        GET_TOKEN(BF_LEX_EOS, "expected end of statement (;).\n");
    }
    if (settings->device == NULL) {
        fprintf(stderr, "ALSA I/O: Parse error: device not set.\n");
        return NULL;
    }
    if (*sample_format == BF_SAMPLE_FORMAT_AUTO) {
        fprintf(stderr, "ALSA I/O: No support for AUTO sample format.\n");
        return NULL;
    }

    *uses_sample_clock = 1;

    has_been_called = true;
    return settings;
}

int
bfio_init(void *params,
	  int io,
	  int sample_format,
	  int sample_rate,
	  int open_channels,
	  int used_channels,
	  const int channel_selection[],
	  int period_size,
	  int *device_period_size,
	  int *isinterleaved,
          void *callback_state,
          int (*process_callback)(void **callback_states[2],
                                  int callback_state_count[2],
                                  void **buffers[2],
                                  int frame_count,
                                  int event))
{
    int err;
    struct settings *settings = (struct settings *)params;
    if ((err = snd_pcm_open(&glob.handles[io][glob.n_handles[io]], settings->device,
			    (io == BF_IN) ? SND_PCM_STREAM_CAPTURE :
			    SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0)
    {
	fprintf(stderr, "ALSA I/O: Could not open audio %s \"%s\": %s.\n",
		io == BF_IN ? "input" : "output", settings->device, snd_strerror(err));
	return -1;
    }

    snd_pcm_t *handle = glob.handles[io][glob.n_handles[io]];

    int ismmap;
    int sample_size;
    char errstr[ERRORSIZE];
    if (!set_params(handle, settings, sample_format, sample_rate, open_channels,
		    period_size, device_period_size, &sample_size, isinterleaved, &ismmap, errstr))
    {
	fprintf(stderr, "ALSA I/O: Could not set audio %s parameters for \"%s\":\n%s",
		io == BF_IN ? "input" : "output", settings->device, errstr);
	snd_pcm_close(handle);
	return -1;
    }
    struct pollfd pollfd;
    if (snd_pcm_poll_descriptors(handle, &pollfd, 1) != 1) {
	fprintf(stderr, "ALSA I/O: Could not get file descriptor.\n");
	snd_pcm_close(handle);
	return -1;
    }
    if (glob.base_handle == NULL) {
        glob.base_handle = handle;
    } else if (glob.link_handles) {
	if ((err = snd_pcm_link(glob.base_handle, handle)) < 0) {
	    fprintf(stderr, "ALSA I/O: Could not link alsa devices: %s.\n", snd_strerror(err));
	    snd_pcm_close(handle);
	    return -1;
	}
    }
    glob.n_handles[io]++;

    struct alsa_access_state *as = &glob.fd2as[pollfd.fd];
    as->handle = handle;
    as->isinterleaved = !!*isinterleaved;
    as->ignore_xrun = settings->ignore_xrun;
    as->ismmap = !!ismmap;
    as->sw_period_size = period_size;
    as->device_period_size = *device_period_size;
    as->sample_size = sample_size;
    as->open_channels = open_channels;
    as->used_channels = used_channels;
    as->device = estrdup(settings->device);
    as->frame_count = -1;

    if (*isinterleaved) {
        as->bufs = NULL;
    } else {
        as->bufs = emalloc(open_channels * sizeof(void *));
        memset(as->bufs, 0, open_channels * sizeof(void *));
        as->channel_selection = emalloc(used_channels * sizeof(int));
	memcpy(as->channel_selection, channel_selection, used_channels * sizeof(int));
    }
    efree(settings->device);
    efree(settings);
    return pollfd.fd;
}

int
bfio_synch_start(void)
{
    if (glob.base_handle == NULL) {
        return 0;
    }

    /* FIXME: the SND_PCM_STATE_RUNNING code would not be needed if the
       bfio_write autostart hack was not there */
    snd_pcm_status_t *status;
    snd_pcm_status_alloca(&status);

    if (glob.link_handles) {
        int err;
        if ((err = snd_pcm_status(glob.base_handle, status)) < 0) {
            fprintf(stderr, "ALSA I/O: Could not get status: %s.\n", snd_strerror(err));
            return -1;
        }
        if (snd_pcm_status_get_state(status) == SND_PCM_STATE_RUNNING) {
            return 0;
        }
        if ((err = snd_pcm_start(glob.base_handle)) < 0) {
            fprintf(stderr, "ALSA I/O: Could not start linked audio: %s.\n", snd_strerror(err));
            return -1;
        }
        return 0;
    }

    FOR_IN_AND_OUT {
        for (int n = 0; n < glob.n_handles[IO]; n++) {
            int err;
            if ((err = snd_pcm_status(glob.handles[IO][n], status)) < 0) {
                fprintf(stderr, "ALSA I/O: Could not get status for %s: %s.\n",
                        IO == IN ? "input" : "output", snd_strerror(err));
                return -1;
            }
            if (snd_pcm_status_get_state(status) == SND_PCM_STATE_RUNNING) {
                continue;
            }
            if ((err = snd_pcm_start(glob.handles[IO][n])) < 0) {
                fprintf(stderr, "ALSA I/O: Could not start audio %s: %s.\n",
                        IO == IN ? "input" : "output", snd_strerror(err));
                return -1;
            }
        }
    }
    return 0;
}

void
bfio_synch_stop(void)
{
    if (glob.base_handle == NULL) {
        return;
    }
    FOR_IN_AND_OUT {
        for (int n = 0; n < glob.n_handles[IO]; n++) {
            snd_pcm_close(glob.handles[IO][n]);
        }
    }
}

int
bfio_read(int fd,
	  void *buf,
	  int offset,
	  int count)
{
    struct alsa_access_state *as = &glob.fd2as[fd];
    int frame_count, frame_size, err;

 bfio_read_restart:

    if (as->isinterleaved) {
	frame_size = as->sample_size * as->open_channels;
        if (as->ismmap) {
            frame_count = snd_pcm_mmap_readi(as->handle, &((uint8_t *)buf)[offset], count / frame_size);
        } else {
            frame_count = snd_pcm_readi(as->handle, &((uint8_t *)buf)[offset], count / frame_size);
        }
	if (frame_count < 0) {
            err = frame_count;
            goto bfio_read_error;
	}
    } else {
        uint8_t *ptr = (uint8_t *)buf + offset / as->used_channels;
        for (int n = 0; n < as->used_channels; n++) {
            as->bufs[as->channel_selection[n]] = ptr;
            ptr += as->sw_period_size * as->sample_size;
        }
        frame_size = as->sample_size * as->used_channels;
        if (as->ismmap) {
            frame_count = snd_pcm_mmap_readn(as->handle, as->bufs, count / frame_size);
        } else {
            frame_count = snd_pcm_readn(as->handle, as->bufs, count / frame_size);
        }
        if (frame_count < 0) {
            err = frame_count;
            goto bfio_read_error;
        }
    }

    return frame_count * frame_size;

 bfio_read_error:
    switch (err) {
    case -EPIPE:
        if (as->ignore_xrun) {
            fprintf(stderr, "ALSA I/O: overflow! (read on %s)\n", as->device);
            if ((err = snd_pcm_prepare(as->handle)) < 0) {
                fprintf(stderr, "ALSA I/O: Unable to prepare audio: %s.\n", snd_strerror(err));
                errno = EPIPE;
                return -1;
            }
            if ((err = snd_pcm_start(as->handle)) < 0) {
                fprintf(stderr, "ALSA I/O: Could not restart audio: %s.\n", snd_strerror(err));
                errno = EPIPE;
                return -1;
            }
            goto bfio_read_restart;
        }
        // assume and indicate buffer overflow
        errno = EPIPE;
        break;
    case -EAGAIN:
        /* hack 2025: normal ALSA devices with connection to hardware normally only sets the fd ready for
           reading at each hardware interrupt, which happens at period cycle. To support polling mode the
           device is opened non-blocking though. However, virtual devices like Pipewire ALSA compatibility
           layer will have the fd always ready for reading even if there is no data, and even if polling we
           only get data at each period size.

           This behavior from Pipewire maybe should be considered to be a bug, but this is how it behaves
           at time of writing.

           To work around this, we detect this case, and immediately switch the device to blocking mode.
           This only makes sense if the device period size is not larger than the BruteFIR period though.

           Another solution would be to simply always open the devices in blocking mode which would work
           as long as the device block size is sane (ie same as or divisable by the BruteFIR period),
           but for now we keep non-blocking to keep the support of devices having weird period sizes.

           If the device period size is larger than the BruteFIR period we however have to keep
           non-blocking and hope for the best.
         */
        if (as->device_period_size <= as->sw_period_size) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
            if (poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN) != 0) {
                snd_pcm_sframes_t avail = snd_pcm_avail_update(as->handle);
                if (avail == 0) {
                    if ((err = snd_pcm_nonblock(as->handle, 0)) < 0) {
                        fprintf(stderr, "ALSA I/O: Could not set likely virtual device to blocking mode: %s.\n", snd_strerror(err));
                        errno = err;
                        return -1;
                    }
                }
            }
        }

        errno = EAGAIN;
        return -1;
    default:
        errno = 0;
        break;
    }
    fprintf(stderr, "ALSA I/O: Could not read audio: %s.\n", snd_strerror(err));
    return -1;
}

int
bfio_write(int fd,
	   const void *buf,
	   int offset,
	   int count)
{
    struct alsa_access_state *as = &glob.fd2as[fd];
    int frame_count, frame_size, err;

    if (as->isinterleaved) {
	frame_size = as->sample_size * as->open_channels;
        if (as->ismmap) {
            frame_count = snd_pcm_mmap_writei(as->handle, &((const uint8_t *)buf)[offset], count / frame_size);
        } else {
            frame_count = snd_pcm_writei(as->handle, &((const uint8_t *)buf)[offset], count / frame_size);
        }
	if (frame_count < 0) {
            err = frame_count;
            goto bfio_write_error;
	}
    } else {
        const uint8_t *ptr = (const uint8_t *)buf + offset / as->used_channels;
        for (int n = 0; n < as->used_channels; n++) {
            as->bufs[as->channel_selection[n]] = DECONST(void *, ptr);
            ptr += as->sw_period_size * as->sample_size;
        }
        frame_size = as->sample_size * as->used_channels;
        if (as->ismmap) {
            frame_count = snd_pcm_writen(as->handle, as->bufs, count / frame_size);
        } else {
            frame_count = snd_pcm_mmap_writen(as->handle, as->bufs, count / frame_size);
        }
        if (frame_count < 0) {
            err = frame_count;
            goto bfio_write_error;
        }
    }
    if (as->restart) {
        as->restart = false;
        if ((err = snd_pcm_start(as->handle)) < 0) {
            fprintf(stderr, "ALSA I/O: Could not restart audio: %s.\n", snd_strerror(err));
            errno = EPIPE;
            return -1;
        }
    }

    return frame_count * frame_size;

 bfio_write_error:
    switch (err) {
    case -EPIPE:
        if (as->ignore_xrun) {
            fprintf(stderr, "ALSA I/O: underflow! (write on %s)\n",  as->device);
            if ((err = snd_pcm_prepare(as->handle)) < 0) {
                fprintf(stderr, "ALSA I/O: Unable to prepare audio: %s.\n", snd_strerror(err));
                errno = EPIPE;
                return -1;
            }
            as->restart = true;
            return count;
        }
        // assume and indicate buffer underflow
        errno = EPIPE;
        break;
    case -EAGAIN:
        errno = EAGAIN;
        return -1;
    default:
        errno = 0;
        break;
    }
    fprintf(stderr, "ALSA I/O: Could not write audio: %s.\n", snd_strerror(err));
    return -1;
}
