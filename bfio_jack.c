/*
 * (c) Copyright 2003 - 2006, 2009, 2013, 2025 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>

#include <jack/jack.h>

#define IS_BFIO_MODULE
#include "bfmod.h"
#include "inout.h"

struct jack_state {
    int n_channels;
    jack_port_t *ports[BF_MAXCHANNELS];
    char *port_name[BF_MAXCHANNELS];
    char *local_port_name[BF_MAXCHANNELS];
    char *dest_name[BF_MAXCHANNELS];
};

#define DEFAULT_CLIENTNAME "brutefir"
#define DEFAULT_JACK_CB_THREAD_PRIORITY 5

typedef uintptr_t wordbool_t;

static struct {
    volatile wordbool_t stopped;
    volatile wordbool_t has_started;
    bool debug;
    struct jack_state *handles[2][BF_MAXCHANNELS];
    int expected_priority;
    void **states[2];
    int n_handles[2];
    bool hasio[2];
    jack_client_t *client;
    char *client_name;
    int (*process_cb)(void **_states[2],
                      int state_count[2],
                      void **bufs[2],
                      int count,
                      int event);
} glob = {
    .stopped = false,
    .has_started = false,
    .debug = false,
    .handles = {{NULL}},
    .expected_priority = -1,
    .states = {NULL, NULL},
    .n_handles = {0, 0},
    .hasio = {false, false},
    .client = NULL,
    .client_name = NULL,
    .process_cb = NULL
};

static void
error_callback(const char *msg)
{
    if (glob.stopped) {
        /* we don't care about errors after we have stopped */
        return;
    }

    if (msg[strlen(msg)-1] ==  '\n') {
        fprintf(stderr, "JACK I/O: JACK reported an error: %s", msg);
    } else {
        fprintf(stderr, "JACK I/O: JACK reported an error: %s\n", msg);
    }
    if (glob.has_started) {
        glob.stopped = true;
        glob.process_cb(NULL, 0, NULL, 0, BF_CALLBACK_EVENT_ERROR);
    }
}

static void
shutdown_callback(void *arg)
{
    fprintf(stderr, "JACK I/O: JACK daemon shut down.\n");
    glob.stopped = true;
    glob.process_cb(NULL, 0, NULL, 0, BF_CALLBACK_EVENT_ERROR);
}

static int
buffersize_callback(jack_nframes_t nframes,
                    void *arg)
{
    const int period_size = (intptr_t)arg;
    if (nframes == period_size) {
        return 0;
    }
    fprintf(stderr, "\
JACK I/O: JACK reported a changed buffer size to %d. Change is not supported.\n\
  Make sure JACK has a fixed buffer size (called quantum if using Pipewire)\n\
  that is the same as the BruteFIR partition size (currently %d).\n",
            (int)nframes, period_size);
    return -1;
}

static void
init_callback_(void)
{
    struct sched_param schp;
    int policy;

    pthread_getschedparam(pthread_self(), &policy, &schp);
    if (policy != SCHED_FIFO && policy != SCHED_RR) {
        fprintf(stderr, "JACK I/O: Warning: JACK is not running with "
                "SCHED_FIFO or SCHED_RR (realtime).\n");
    } else if (schp.sched_priority != glob.expected_priority) {
        fprintf(stderr, "\
JACK I/O: Warning: JACK thread has priority %d, but BruteFIR expected %d.\n\
  In order to make correct realtime scheduling BruteFIR must know what\n\
  priority JACK uses. At the time of writing the JACK API does not support\n\
  getting that information so BruteFIR must be manually configured with that.\n\
  Use the \"priority\" setting in the first \"jack\" device clause in your\n\
  BruteFIR configuration file.\n",
                (int)schp.sched_priority, (int)glob.expected_priority);
    }
}

static void
init_callback(void *arg)
{
    static pthread_once_t once_control = PTHREAD_ONCE_INIT;

    pthread_once(&once_control, init_callback_);
}

static void
latency_callback(jack_latency_callback_mode_t mode,
                 void *arg)
{
    const int period_size = (intptr_t)arg;

    // same latency for all ports, regardless of how they are connected
    if (mode == JackPlaybackLatency) {
        // do nothing
    } else if (mode == JackCaptureLatency) {
        for (int n = 0; n < glob.n_handles[OUT]; n++) {
            struct jack_state *js = glob.handles[OUT][n];
            for (int i = 0; i < js->n_channels; i++) {
                jack_latency_range_t range;
                range.min = period_size;
                range.max = period_size;
                jack_port_set_latency_range(js->ports[i], mode, &range);
            }
        }
    }
}

static int
process_callback(jack_nframes_t n_frames,
                 void *arg)
{
    static int frames_left = 0;

    void *in_bufs[BF_MAXCHANNELS], *out_bufs[BF_MAXCHANNELS], **iobufs[2];

    iobufs[IN] = glob.n_handles[IN] > 0 ? in_bufs : NULL;
    iobufs[OUT] = glob.n_handles[OUT] > 0 ? out_bufs : NULL;
    FOR_IN_AND_OUT {
        for (int n = 0; n < glob.n_handles[IO]; n++) {
            struct jack_state *js = glob.handles[IO][n];
            for (int i = 0; i < js->n_channels; i++) {
                iobufs[IO][i] = jack_port_get_buffer(js->ports[i], n_frames);
            }
        }
    }
    if (frames_left != 0) {
        glob.process_cb(glob.states, glob.n_handles, NULL, 0, BF_CALLBACK_EVENT_FINISHED);
        for (int n = 0; n < glob.n_handles[OUT]; n++) {
            struct jack_state *js = glob.handles[OUT][n];
            for (int i = 0; i < js->n_channels; i++) {
                void *buffer = jack_port_get_buffer(js->ports[i], n_frames);
                memset(buffer, 0, n_frames * sizeof(jack_default_audio_sample_t));
            }
        }
        glob.stopped = true;
        return -1;
    }
    frames_left = glob.process_cb(glob.states, glob.n_handles, iobufs, n_frames, BF_CALLBACK_EVENT_NORMAL);
    if (frames_left == -1) {
        glob.stopped = true;
        return -1;
    }

    return 0;
}

static bool
global_init(void)
{
    jack_status_t status;
    jack_set_error_function(error_callback);
    if ((glob.client = jack_client_open(glob.client_name, JackNoStartServer, &status)) == NULL) {
        fprintf(stderr, "\
JACK I/O: Could not become JACK client (status: 0x%2.0x). Error message(s):\n",
                status);
        if ((status & JackFailure) != 0) {
            fprintf(stderr, "\
  Overall operation failed.\n");
        }
        if ((status & JackInvalidOption) != 0) {
            fprintf(stderr, "\
  Likely bug in BruteFIR: the operation contained an invalid or unsupported\n\
  option.\n");
        }
        if ((status & JackNameNotUnique) != 0) {
            fprintf(stderr, "\
  Client name \"%s\" not unique, try another name.\n", glob.client_name);
        }
        if ((status & JackServerFailed) != 0) {
            fprintf(stderr, "\
  Unable to connect to the JACK server. Perhaps it is not running? BruteFIR\n\
  requires that a JACK server is started in advance.\n");
        }
        if ((status & JackServerError) != 0) {
            fprintf(stderr, "  Communication error with the JACK server.\n");
        }
        if ((status & JackNoSuchClient) != 0) {
            fprintf(stderr, "  Requested client does not exist.\n");
        }
        if ((status & JackLoadFailure) != 0) {
            fprintf(stderr, "  Unable to load internal client.\n");
        }
        if ((status & JackInitFailure) != 0) {
            fprintf(stderr, "  Unable initialize client.\n");
        }
        if ((status & JackShmFailure) != 0) {
            fprintf(stderr, "  Unable to access shared memory.\n");
        }
        if ((status & JackVersionError) != 0) {
            fprintf(stderr, "\
  The version of the JACK server is not compatible with the JACK client\n\
  library used by BruteFIR.\n");
        }
        return false;
    }
    jack_set_thread_init_callback(glob.client, init_callback, NULL);
    jack_set_process_callback(glob.client, process_callback, NULL);
    jack_on_shutdown(glob.client, shutdown_callback, NULL);


    return true;
}

int
bfio_iscallback(void)
{
    return true;
}

#define GET_TOKEN(token, errstr)                                               \
    if (get_config_token(&lexval) != token) {                                  \
        fprintf(stderr, "JACK I/O: Parse error: " errstr);                     \
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
             struct sched_param *callback_sched_param,
             int debug)
{
    struct jack_state *js;

    const int ver = *version_major;
    *version_major = BF_VERSION_MAJOR;
    *version_minor = BF_VERSION_MINOR;
    if (ver != BF_VERSION_MAJOR) {
        return NULL;
    }

    glob.debug = !!debug;

    if (*sample_format == BF_SAMPLE_FORMAT_AUTO) {
#ifdef ARCH_LITTLE_ENDIAN
        if (sizeof(jack_default_audio_sample_t) == 4) {
            *sample_format = BF_SAMPLE_FORMAT_FLOAT_LE;
        } else if (sizeof(jack_default_audio_sample_t) == 8) {
            *sample_format = BF_SAMPLE_FORMAT_FLOAT64_LE;
        }
#endif
#ifdef ARCH_BIG_ENDIAN
        if (sizeof(jack_default_audio_sample_t) == 4) {
            *sample_format = BF_SAMPLE_FORMAT_FLOAT_BE;
        } else if (sizeof(jack_default_audio_sample_t) == 8) {
            *sample_format = BF_SAMPLE_FORMAT_FLOAT64_BE;
        }
#endif
    }
    if (sizeof(jack_default_audio_sample_t) == 4) {
#ifdef ARCH_LITTLE_ENDIAN
        if (*sample_format != BF_SAMPLE_FORMAT_FLOAT_LE) {
            fprintf(stderr, "JACK I/O: Sample format must be %s or %s.\n",
                    bf_strsampleformat(BF_SAMPLE_FORMAT_FLOAT_LE),
                    bf_strsampleformat(BF_SAMPLE_FORMAT_AUTO));
            return NULL;
        }
#endif
#ifdef ARCH_BIG_ENDIAN
        if (*sample_format != BF_SAMPLE_FORMAT_FLOAT_BE) {
            fprintf(stderr, "JACK I/O: Sample format must be %s or %s.\n",
                    bf_strsampleformat(BF_SAMPLE_FORMAT_FLOAT_BE),
                    bf_strsampleformat(BF_SAMPLE_FORMAT_AUTO));
            return NULL;
        }
#endif
    } else if (sizeof(jack_default_audio_sample_t) == 8) {
#ifdef ARCH_LITTLE_ENDIAN
        if (*sample_format != BF_SAMPLE_FORMAT_FLOAT64_LE) {
            fprintf(stderr, "JACK I/O: Sample format must be %s or %s.\n",
                    bf_strsampleformat(BF_SAMPLE_FORMAT_FLOAT64_LE),
                    bf_strsampleformat(BF_SAMPLE_FORMAT_AUTO));
            return NULL;
        }
#endif
#ifdef ARCH_BIG_ENDIAN
        if (*sample_format != BF_SAMPLE_FORMAT_FLOAT64_BE) {
            fprintf(stderr, "JACK I/O: Sample format must be %s or %s.\n",
                    bf_strsampleformat(BF_SAMPLE_FORMAT_FLOAT64_BE),
                    bf_strsampleformat(BF_SAMPLE_FORMAT_AUTO));
            return NULL;
        }
#endif
    }
    js = calloc(1, sizeof(struct jack_state));
    js->n_channels = open_channels;

    int token;
    union bflexval lexval;
    while ((token = get_config_token(&lexval)) > 0) {
        if (token != BF_LEXVAL_FIELD) {
            fprintf(stderr, "JACK I/O: Parse error: expected field.\n");
            return NULL;
        }
        if (strcmp(lexval.field, "ports") == 0) {
            for (int n = 0; n < open_channels; n++) {
                GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
                if (lexval.string[0] != '\0') {
                    js->dest_name[n] = strdup(lexval.string);
                }
                if ((token = get_config_token(&lexval)) == BF_LEX_SLASH) {
                    GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
                    if (lexval.string[0] != '\0') {
                        js->local_port_name[n] = strdup(lexval.string);
                    }
                    token = get_config_token(&lexval);
                }
                if (n < open_channels - 1) {
                    if (token != BF_LEX_COMMA) {
                        fprintf(stderr, "JACK I/O: Parse error: "
                                "expected comma (,).\n");
                        return NULL;
                    }
                } else {
                    if (token != BF_LEX_EOS) {
                        fprintf(stderr, "JACK I/O: Parse error: "
                                "expected end of statement (;).\n");
                        return NULL;
                    }
                }
            }
        } else if (strcmp(lexval.field, "clientname") == 0) {
            GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
            if (glob.client != NULL && strcmp(lexval.string, glob.client_name) != 0) {
                fprintf(stderr, "JACK I/O: clientname setting is global and "
                        "must be set in the first jack device.\n");
                return NULL;
            }
            if (glob.client_name == NULL) {
                glob.client_name = strdup(lexval.string);
            }
            GET_TOKEN(BF_LEX_EOS, "expected end of statement (;).\n");
        } else if (strcmp(lexval.field, "priority") == 0) {
            GET_TOKEN(BF_LEXVAL_REAL, "expected integer.\n");
            if (glob.client != NULL && glob.expected_priority != (int)lexval.real) {
                fprintf(stderr, "JACK I/O: priority setting is global and "
                        "must be set in the first jack device.\n");
                return NULL;
            }
            glob.expected_priority = (int)lexval.real;
            GET_TOKEN(BF_LEX_EOS, "expected end of statement (;).\n");
        }
    }
    glob.hasio[io] = true;

    if (glob.expected_priority < 0) {
        glob.expected_priority = DEFAULT_JACK_CB_THREAD_PRIORITY;
    }
    if (glob.client == NULL) {
        if (glob.client_name == NULL) {
            glob.client_name = strdup(DEFAULT_CLIENTNAME);
        }
        if (!global_init()) {
            return NULL;
        }
    }

    memset(callback_sched_param, 0, sizeof(*callback_sched_param));
    callback_sched_param->sched_priority = glob.expected_priority;
    *callback_sched_policy = SCHED_FIFO;

    const int rate = (int)jack_get_sample_rate(glob.client);
    if (rate == 0) {
        *uses_sample_clock = 0;
    } else {
        *uses_sample_clock = 1;
    }
    if (rate != 0 && rate != sample_rate) {
        fprintf(stderr, "JACK I/O: JACK sample rate is %d, BruteFIR is %d, they must be same.\n", rate, sample_rate);
        return NULL;
    }
    return (void *)js;
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
          int (*bf_process_callback)(void **callback_states[2],
                                     int callback_state_count[2],
                                     void **buffers[2],
                                     int frame_count,
                                     int event))
{
    static void *states_[2][BF_MAXCHANNELS];
    static int io_idx[2] = { 0, 0 };

    glob.process_cb = bf_process_callback;
    *device_period_size = jack_get_buffer_size(glob.client);
    *isinterleaved = false;
    struct jack_state *js = (struct jack_state *)params;

    if (used_channels != open_channels) {
        fprintf(stderr, "JACK I/O: Open channels must be equal to used channels for this I/O module.\n");
        return -1;
    }

    for (int n = 0; n < used_channels; n++) {
        if (js->dest_name[n] != NULL) {
            jack_port_t *port = jack_port_by_name(glob.client, js->dest_name[n]);
            if (port == NULL) {
                fprintf(stderr, "JACK I/O: Failed to open JACK port \"%s\".\n", js->dest_name[n]);
                return -1;
            }
            if ((io == IN && (jack_port_flags(port) & JackPortIsOutput) == 0) ||
                (io == OUT && (jack_port_flags(port) & JackPortIsInput) == 0))
            {
                fprintf(stderr, "JACK I/O: JACK port \"%s\" is not an %s.\n",
                        js->dest_name[n], io == IN ? "Output" : "Input");
                return -1;
            }
        }

        char *name, _name[128];
        if (js->local_port_name[n] != NULL) {
            name = js->local_port_name[n];
        } else {
            name = _name;
            sprintf(name, "%s-%d", io == IN ? "input" : "output", io_idx[io]++);
        }

        jack_port_t *port = jack_port_register(glob.client, name, JACK_DEFAULT_AUDIO_TYPE,
                                               (io == IN ? JackPortIsInput : JackPortIsOutput), 0);
        if (port == NULL) {
            fprintf(stderr, "JACK I/O: Failed to open new JACK port.\n");
            return -1;
        }
//        if (io == OUT) {
//            jack_port_set_latency(port, period_size);
//        }
        js->ports[n] = port;
        char longname[1024];
        snprintf(longname, sizeof(longname), "%s:%s", glob.client_name, name);
        longname[sizeof(longname) - 1] = '\0';
        js->port_name[n] = strdup(longname);
    }

    states_[io][glob.n_handles[io]] = callback_state;
    glob.handles[io][glob.n_handles[io]++] = js;

    glob.states[IN] = glob.n_handles[IN] > 0 ? states_[IN] : NULL;
    glob.states[OUT] = glob.n_handles[OUT] > 0 ? states_[OUT] : NULL;

    if (io == OUT && glob.hasio[IN]) {
        jack_set_latency_callback(glob.client, latency_callback, (void *)(intptr_t)period_size);
    }
    jack_set_buffer_size_callback(glob.client, buffersize_callback, (void *)(intptr_t)period_size);

    return 0;
}

int
bfio_synch_start(void)
{
    if (glob.has_started) {
        return 0;
    }
    if (glob.client == NULL) {
        fprintf(stderr, "JACK I/O: client is NULL\n");
        return -1;
    }
    glob.has_started = true;
    /*
     * jack_activate() will start a new pthread. We block all signals before
     * calling to make sure that we get all signals to our thread. This is a
     * bit ugly, since it assumes that the JACK library does not mess up the
     * signal handlers later.
     */
    sigset_t signals;
    sigfillset(&signals);
    pthread_sigmask(SIG_BLOCK, &signals, NULL);
    const int ret = jack_activate(glob.client);
    pthread_sigmask(SIG_UNBLOCK, &signals, NULL);
    if (ret != 0) {
        fprintf(stderr, "JACK I/O: Could not activate local JACK client.\n");
        glob.has_started = false;
        return -1;
    }
    for (int n = 0; n < glob.n_handles[IN]; n++) {
        struct jack_state *js = glob.handles[IN][n];
        for (int i = 0; i < js->n_channels; i++) {
            if (js->dest_name[i] == NULL) {
                continue;
            }
            if (jack_connect(glob.client, js->dest_name[i], js->port_name[i]) != 0) {
                fprintf(stderr, "JACK I/O: Could not connect local port \"%s\" to \"%s\".\n",
                        js->port_name[i], js->dest_name[i]);
                glob.has_started = false;
                return -1;
            }
        }
    }
    for (int n = 0; n < glob.n_handles[OUT]; n++) {
        struct jack_state *js = glob.handles[OUT][n];
        for (int i = 0; i < js->n_channels; i++) {
            if (js->dest_name[i] == NULL) {
                continue;
            }
            if (jack_connect(glob.client, js->port_name[i], js->dest_name[i]) != 0) {
                fprintf(stderr, "JACK I/O: Could not connect local port \"%s\" to \"%s\".\n",
                        js->port_name[i], js->dest_name[i]);
                glob.has_started = false;
                return -1;
            }
        }
    }
    return 0;
}

void
bfio_synch_stop(void)
{
    if (!glob.stopped) {
        glob.stopped = true;
        jack_client_close(glob.client);
    }
}
