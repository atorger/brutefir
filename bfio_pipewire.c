/*
 * (c) Copyright 2025 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdbool.h>
#include <pthread.h>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <spa/pod/builder.h>
#include <spa/param/latency-utils.h>

#include "compat.h"
#define IS_BFIO_MODULE
#include "bfmod.h"
#include "inout.h"

#define DEFAULT_CLIENTNAME "brutefir"
#define DEFAULT_PIPEWIRE_CB_THREAD_PRIORITY 84
#define DEFAULT_AUDIO_FORMAT "32 bit float mono audio"

struct pipewire_state {
    int n_channels;
    struct {
        void *handle;
        char *name;
        uint32_t dest_port_id;
        uint32_t id;
    } port[BF_MAXCHANNELS];
    struct {
        char *local_port_name;
        char *dest_name;
    } conf[BF_MAXCHANNELS];
};

enum registry_item_type {
    REGISTRY_ITEM_CLIENT,
    REGISTRY_ITEM_NODE,
    REGISTRY_ITEM_PORT
};

struct registry_item {
    enum registry_item_type type;
    struct registry_item *next;
    union {
        struct {
            uint32_t id;
        } client;
        struct {
            uint32_t id;
            uint32_t client_id;
            char *name;
        } node;
        struct {
            uint32_t id;
            uint32_t node_id;
            int io;
            char *name;
            char *alias;
        } port;
    } u;
};

static struct {
    bool waiting_for_own_objects_being_registered;
    bool debug;
    int sample_format_size;
    int expected_priority;
    struct pipewire_state *handles[2][BF_MAXCHANNELS];
    void **states[2];
    int n_handles[2];
    struct pw_core *pw_core;
    struct pw_main_loop *pw_loop;
    struct pw_filter *pw_filter;
    struct pw_registry *pw_registry;
    struct registry_item *startup_registry;
    char *client_name;
    int (*process_cb)(void **_states[2],
                      int state_count[2],
                      void **bufs[2],
                      int count,
                      int event);
    void *zerobuf;
} glob = {
    .waiting_for_own_objects_being_registered = false,
    .debug = false,
    .sample_format_size = 0,
    .expected_priority = -1,
    .handles = {{NULL}},
    .states = {NULL, NULL},
    .n_handles = {0, 0},
    .pw_core = NULL,
    .pw_loop = NULL,
    .pw_filter = NULL,
    .pw_registry = NULL,
    .startup_registry = NULL,
    .client_name = NULL,
    .process_cb = NULL,
    .zerobuf = NULL
};

static void
pipewire_filter_process_callback(void *arg,
                                 struct spa_io_position *position)
{

    static bool finished = false;

    const uint32_t n_samples = position->clock.duration;

    void *in_bufs[BF_MAXCHANNELS], *out_bufs[BF_MAXCHANNELS], **iobufs[2];
    iobufs[IN] = glob.n_handles[IN] > 0 ? in_bufs : NULL;
    iobufs[OUT] = glob.n_handles[OUT] > 0 ? out_bufs : NULL;

    FOR_IN_AND_OUT {
        for (int n = 0; n < glob.n_handles[IO]; n++) {
            struct pipewire_state *pws = glob.handles[IO][n];
            for (int i = 0; i < pws->n_channels; i++) {
                void *buffer = pw_filter_get_dsp_buffer(pws->port[i].handle, n_samples);
                if (buffer != NULL) {
                    iobufs[IO][i] = buffer;
                } else {
                    // this happens when there's no link to the input port
                    iobufs[IO][i] = glob.zerobuf;
                }
            }
        }
    }
    if (finished) {
        glob.process_cb(glob.states, glob.n_handles, NULL, 0, BF_CALLBACK_EVENT_FINISHED);
        for (int n = 0; n < glob.n_handles[OUT]; n++) {
            struct pipewire_state *pws = glob.handles[OUT][n];
            for (int i = 0; i < pws->n_channels; i++) {
                void *buffer = pw_filter_get_dsp_buffer(pws->port[i].handle, n_samples);
                if (buffer != NULL) {
                    memset(buffer, 0, n_samples * glob.sample_format_size);
                }
            }
        }
    } else {
        int frames_left = glob.process_cb(glob.states, glob.n_handles, iobufs, n_samples, BF_CALLBACK_EVENT_NORMAL);
        finished = frames_left != 0;
    }
}

static void
free_startup_registry(void)
{
    struct registry_item *item = glob.startup_registry;
    while (item != NULL) {
        if (item->type == REGISTRY_ITEM_NODE) {
            free(item->u.node.name);
        } else if (item->type == REGISTRY_ITEM_PORT) {
            free(item->u.port.name);
            free(item->u.port.alias);
        }
        struct registry_item *next_item = item->next;
        free(item);
        item = next_item;
    }
    glob.startup_registry = NULL;
}

static const char *
find_node_name_by_id(uint32_t node_id)
{
    for (const struct registry_item *item = glob.startup_registry; item != NULL; item = item->next) {
        if (item->type == REGISTRY_ITEM_NODE && item->u.node.id == node_id) {
            return item->u.node.name;
        }
    }
    return NULL;
}

static bool
find_port_by_name(const char name[],
                  int *io,
                  uint32_t *port_id)
{
    // First look for the combined full length name, then if no match, look for alias

    for (const struct registry_item *item = glob.startup_registry; item != NULL; item = item->next) {
        const char *node_name;
        if (item->type == REGISTRY_ITEM_PORT && (node_name = find_node_name_by_id(item->u.port.node_id)) != NULL) {
            char combined_name[1024];
            snprintf(combined_name, sizeof(combined_name), "%s:%s", node_name, item->u.port.name);
            if (strcmp(combined_name, name) == 0) {
                *io = item->u.port.io;
                *port_id = item->u.port.id;
                return true;
            }
        }
    }
    for (const struct registry_item *item = glob.startup_registry; item != NULL; item = item->next) {
        if (item->type == REGISTRY_ITEM_PORT && strcmp(item->u.port.alias, name) == 0) {
            *io = item->u.port.io;
            *port_id = item->u.port.id;
            return true;
        }
    }
    return false;
}

static uint32_t
find_own_filter_node_id(void)
{
    for (const struct registry_item *item = glob.startup_registry; item != NULL; item = item->next) {
        if (item->type == REGISTRY_ITEM_NODE) {
            for (const struct registry_item *item1 = glob.startup_registry; item1 != NULL; item1 = item1->next) {
                if (item1->type == REGISTRY_ITEM_CLIENT) {
                    if (item1->u.client.id == item->u.node.client_id) {
                        return item->u.node.id;
                    }
                }
            }
        }
    }
    return UINT32_MAX;
}

static bool
populate_own_port_ids_from_registry(bool silent)
{
    uint32_t filter_node_id = find_own_filter_node_id();
    if (filter_node_id == UINT32_MAX) {
        if (!silent) {
            fprintf(stderr, "Pipewire I/O: could not find own filter node in registry\n");
        }
        return false;
    }
    FOR_IN_AND_OUT {
        for (int n = 0; n < glob.n_handles[IO]; n++) {
            struct pipewire_state *pws = glob.handles[IO][n];
            for (int i = 0; i < pws->n_channels; i++) {
                pws->port[i].id = UINT32_MAX;
                for (const struct registry_item *item = glob.startup_registry; item != NULL; item = item->next) {
                    if (item->type == REGISTRY_ITEM_PORT && item->u.port.node_id == filter_node_id &&
                        strcmp(item->u.port.name, pws->port[i].name) == 0)
                    {
                        pws->port[i].id = item->u.port.id;
                        break;
                    }
                }
                if (pws->port[i].id == UINT32_MAX) {
                    if (!silent) {
                        fprintf(stderr, "Pipewire I/O: could not find own port \"%s\" in registry\n", pws->port[i].name);
                    }
                    return false;
                }
            }
        }
    }
    return true;
}

static void
pipewire_registry_event_callback(void *data,
                                 uint32_t id,
                                 uint32_t permissions,
                                 const char *type,
                                 uint32_t version,
                                 const struct spa_dict *props)
{
    if (props == NULL) {
        return;
    }

    /*
    if (0) {
        const struct spa_dict_item *item;
        printf("TYPE: %s | ID: %u\n", type, id);
        spa_dict_for_each(item, props) {
            printf("  %s = %s\n", item->key, item->value);
        }
        printf("\n");
        fflush(stdout);
    }
    */

    if (strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
        // There is more than one client with our pid, record them all
        const char *pid = spa_dict_lookup(props, PW_KEY_SEC_PID);
        if (pid == NULL) {
            // I don't think this could/should happen
            fprintf(stderr, "Pipewire I/O: missing pid in client registry callback\n");
            return;
        }
        if (atoll(pid) == (long long)getpid()) {
            struct registry_item *item = malloc(sizeof(*item));
            item->type = REGISTRY_ITEM_CLIENT;
            item->u.client.id = id;
            item->next = glob.startup_registry;
            glob.startup_registry = item;
        }
    } else if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char *client_id = spa_dict_lookup(props, PW_KEY_CLIENT_ID);
        if (node_name == NULL || client_id == NULL) {
            // I don't think this could/should happen
            fprintf(stderr, "Pipewire I/O: missing node information in registry callback\n");
            return;
        }
        struct registry_item *item = malloc(sizeof(*item));
        item->type = REGISTRY_ITEM_NODE;
        item->u.node.id = id;
        item->u.node.client_id = (uint32_t)atol(client_id);
        item->u.node.name = strdup(node_name);
        item->next = glob.startup_registry;
        glob.startup_registry = item;
    } else if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        const char *direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
        if (direction != NULL && (strcmp(direction, "in") == 0 || strcmp(direction, "out") == 0)) {
            const char *alias = spa_dict_lookup(props, PW_KEY_PORT_ALIAS);
            const char *node_id = spa_dict_lookup(props, PW_KEY_NODE_ID);
            const char *name = spa_dict_lookup(props, PW_KEY_PORT_NAME);
            if (node_id == NULL || name == NULL || alias == NULL) {
                // I don't think this could/should happen
                fprintf(stderr, "Pipewire I/O: missing port information in registry callback\n");
                return;
            }

            struct registry_item *item = malloc(sizeof(*item));
            item->type = REGISTRY_ITEM_PORT;
            item->u.port.id = id;
            item->u.port.node_id = (uint32_t)atol(node_id);
            item->u.port.io = strcmp(direction, "in") == 0 ? IN : OUT;
            item->u.port.name = strdup(name);
            item->u.port.alias = strdup(alias);
            item->next = glob.startup_registry;
            glob.startup_registry = item;
        }
    }
    if (glob.waiting_for_own_objects_being_registered) {
        if (populate_own_port_ids_from_registry(true)) {
            glob.waiting_for_own_objects_being_registered = false;
            pw_main_loop_quit(glob.pw_loop);
        }
    }
}

struct sync_arg {
    int sync_seq;
    struct pw_main_loop *loop;
};

static void
pipewire_core_done_callback(void *data,
                            uint32_t id,
                            int seq)
{
    struct sync_arg *arg = (struct sync_arg *)data;
    if (id == PW_ID_CORE && seq == arg->sync_seq) {
        pw_main_loop_quit(arg->loop);
    }
}

static void
sync_pipewire_core(struct pw_core *core,
                   struct pw_main_loop *loop)
{
    static const struct pw_core_events core_events = {
        PW_VERSION_CORE_EVENTS,
        .done = pipewire_core_done_callback,
    };

    struct sync_arg arg;
    struct spa_hook core_listener;
    pw_core_add_listener(core, &core_listener, &core_events, &arg);

    arg.loop = loop;
    arg.sync_seq = pw_core_sync(core, PW_ID_CORE, 0);

    pw_main_loop_run(loop);

    spa_hook_remove(&core_listener);
}

static struct pw_filter *
create_and_connect_pipewire_filter(int period_size,
                                   int sample_rate)
{
    static const struct pw_filter_events filter_events = {
        PW_VERSION_FILTER_EVENTS,
        .process = pipewire_filter_process_callback
    };
    char sample_rate_str[64];
    char quantum_str[64];
    snprintf(sample_rate_str, sizeof(sample_rate_str), "1/%u", sample_rate);
    snprintf(quantum_str, sizeof(quantum_str), "%u", period_size);
    struct pw_filter *filter = pw_filter_new_simple(
        pw_main_loop_get_loop(glob.pw_loop),
        glob.client_name,
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Filter",
            PW_KEY_MEDIA_ROLE, "DSP",
            PW_KEY_NODE_RATE, sample_rate_str,
            PW_KEY_NODE_FORCE_RATE, "0", // "0" means value of NODE_RATE is used
            PW_KEY_NODE_FORCE_QUANTUM, quantum_str, // Quantum == period_size
            NULL),
        &filter_events,
        NULL);

    if (filter == NULL) {
        fprintf(stderr, "Pipewire I/O: Failed to create pipewire filter.\n");
        return NULL;
    }

    uint8_t params_buffer[1024];
    const struct spa_pod *spa_params[1];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
    uint64_t io_delay_ns = ((uint64_t)period_size * 2 * SPA_NSEC_PER_SEC) / sample_rate;
    spa_params[0] = spa_process_latency_build(&b, SPA_PARAM_ProcessLatency,
                                              &SPA_PROCESS_LATENCY_INFO_INIT(.ns = io_delay_ns));

    if (pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, spa_params, 1) < 0) {
        fprintf(stderr, "Pipewire I/O: Failed to connect pipewire filter.\n");
        return NULL;
    }

    return filter;
}

static bool
pipewire_init(void)
{
    static const struct pw_registry_events registry_events = {
        PW_VERSION_REGISTRY_EVENTS,
        .global = pipewire_registry_event_callback
    };
    static struct spa_hook registry_listener;

    pw_init(NULL, NULL);
    glob.pw_loop = pw_main_loop_new(NULL);
    if (glob.pw_loop == NULL) {
        fprintf(stderr, "Pipewire I/O: Could not create pipewire main loop.\n");
        return false;
    }
    struct pw_context *context =  pw_context_new(pw_main_loop_get_loop(glob.pw_loop), NULL, 0);
    if (context == NULL) {
        fprintf(stderr, "Pipewire I/O: Could not create pipewire context.\n");
        return false;
    }
    glob.pw_core = pw_context_connect(context, NULL, 0);
    if (glob.pw_core == NULL) {
        fprintf(stderr, "Pipewire I/O: Could not connect context.\n");
        return false;
    }
    glob.pw_registry = pw_core_get_registry(glob.pw_core, PW_VERSION_REGISTRY, 0);
    if (glob.pw_registry == NULL) {
        fprintf(stderr, "Pipewire I/O: Could not get registry.\n");
        return false;
    }
    spa_zero(registry_listener);
    pw_registry_add_listener(glob.pw_registry, &registry_listener, &registry_events, NULL);

    // Sync with core, this way we get the registry filled with all objects existing at this moment.
    sync_pipewire_core(glob.pw_core, glob.pw_loop);

    return true;
}

int
bfio_iscallback(void)
{
    return true;
}

#define GET_TOKEN(token, errstr)                                        \
    if (get_config_token(&lexval) != token) {                           \
        fprintf(stderr, "Pipewire I/O: Parse error: " errstr);          \
        return NULL;                                                    \
    }

// This is called once for each input and output device in the configuration
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
    const int ver = *version_major;
    *version_major = BF_VERSION_MAJOR;
    *version_minor = BF_VERSION_MINOR;
    if (ver != BF_VERSION_MAJOR) {
        return NULL;
    }

    glob.debug = !!debug;

#ifdef ARCH_LITTLE_ENDIAN
    if (*sample_format == BF_SAMPLE_FORMAT_AUTO) {
        *sample_format = BF_SAMPLE_FORMAT_FLOAT_LE;
    } else if (*sample_format != BF_SAMPLE_FORMAT_FLOAT_LE) {
        fprintf(stderr, "Pipewire I/O: Sample format must be %s or %s.\n",
                bf_strsampleformat(BF_SAMPLE_FORMAT_FLOAT_LE),
                bf_strsampleformat(BF_SAMPLE_FORMAT_AUTO));
        return NULL;
    }
#endif
#ifdef ARCH_BIG_ENDIAN
    if (*sample_format == BF_SAMPLE_FORMAT_AUTO) {
        *sample_format = BF_SAMPLE_FORMAT_FLOAT_BE;
    } else if (*sample_format != BF_SAMPLE_FORMAT_FLOAT_BE) {
        fprintf(stderr, "Pipewire I/O: Sample format must be %s or %s.\n",
                bf_strsampleformat(BF_SAMPLE_FORMAT_FLOAT_BE),
                bf_strsampleformat(BF_SAMPLE_FORMAT_AUTO));
        return NULL;
    }
#endif
    glob.sample_format_size = bf_sampleformat_size(*sample_format);

    struct pipewire_state *pws = calloc(1, sizeof(struct pipewire_state));
    pws->n_channels = open_channels;

    int token;
    union bflexval lexval;
    while ((token = get_config_token(&lexval)) > 0) {
        if (token != BF_LEXVAL_FIELD) {
            fprintf(stderr, "Pipewire I/O: Parse error: expected field.\n");
            return NULL;
        }
        if (strcmp(lexval.field, "ports") == 0) {
            for (int n = 0; n < open_channels; n++) {
                GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
                if (lexval.string[0] != '\0') {
                    pws->conf[n].dest_name = strdup(lexval.string);
                }
                if ((token = get_config_token(&lexval)) == BF_LEX_SLASH) {
                    GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
                    if (lexval.string[0] != '\0') {
                        pws->conf[n].local_port_name = strdup(lexval.string);
                    }
                    token = get_config_token(&lexval);
                }
                if (n < open_channels - 1) {
                    if (token != BF_LEX_COMMA) {
                        fprintf(stderr, "Pipewire I/O: Parse error: expected comma (,).\n");
                        return NULL;
                    }
                } else {
                    if (token != BF_LEX_EOS) {
                        fprintf(stderr, "Pipewire I/O: Parse error: expected end of statement (;).\n");
                        return NULL;
                    }
                }
            }
        } else if (strcmp(lexval.field, "clientname") == 0) {
            GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
            if (glob.pw_loop != NULL && strcmp(lexval.string, glob.client_name) != 0) {
                fprintf(stderr, "Pipewire I/O: clientname setting is global and must be set in the first pipewire device.\n");
                return NULL;
            }
            if (glob.client_name == NULL) {
                glob.client_name = strdup(lexval.string);
            }
            GET_TOKEN(BF_LEX_EOS, "expected end of statement (;).\n");
        } else if (strcmp(lexval.field, "priority") == 0) {
            GET_TOKEN(BF_LEXVAL_REAL, "expected integer.\n");
            if (glob.pw_loop != NULL && glob.expected_priority != (int)lexval.real) {
                fprintf(stderr, "Pipewire I/O: priority setting is global and must be set in the first pipewire device.\n");
                return NULL;
            }
            glob.expected_priority = (int)lexval.real;
            GET_TOKEN(BF_LEX_EOS, "expected end of statement (;).\n");
        }
    }

    if (glob.expected_priority < 0) {
        glob.expected_priority = DEFAULT_PIPEWIRE_CB_THREAD_PRIORITY;
    }
    if (glob.pw_core == NULL) { // pipewire not yet initialized
        if (glob.client_name == NULL) {
            glob.client_name = strdup(DEFAULT_CLIENTNAME);
        }
        if (!pipewire_init()) {
            return NULL;
        }
    }

    memset(callback_sched_param, 0, sizeof(*callback_sched_param));
    callback_sched_param->sched_priority = glob.expected_priority;
    *callback_sched_policy = SCHED_FIFO;

    *uses_sample_clock = 1;
    return (void *)pws;
}

// This is called once for each input and output device in the configuration
// Many parameters are the same each call, like period_size.
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
    static void *callback_states_[2][BF_MAXCHANNELS];
    static int io_idx[2] = { 0, 0 };

    glob.process_cb = bf_process_callback;
    *isinterleaved = false;
    *device_period_size = period_size;
    struct pipewire_state *pws = (struct pipewire_state *)params;

    if (used_channels != open_channels) {
        // It does not make sense to have unused channels (ie ports) for a callback I/O module
        fprintf(stderr, "Pipewire I/O: Open channels must be equal to used channels for this I/O module.\n");
        return -1;
    }

    if (glob.pw_filter == NULL) {
        // create and connect filter object at first bf_init() call
        glob.pw_filter = create_and_connect_pipewire_filter(period_size, sample_rate);
        if (glob.pw_filter == NULL) {
            fprintf(stderr, "Pipewire I/O: Failed to init pipewire filter object.\n");
            return -1;
        }
        int bufsize = period_size * bf_sampleformat_size(sample_format);
        glob.zerobuf = calloc(1, bufsize);
    }

    for (int n = 0; n < used_channels; n++) {
        uint32_t dest_port_id = UINT32_MAX;
        if (pws->conf[n].dest_name != NULL) {
            int port_io;
            if (!find_port_by_name(pws->conf[n].dest_name, &port_io, &dest_port_id)) {
                fprintf(stderr, "Pipewire I/O: Failed to find port \"%s\".\n", pws->conf[n].dest_name);
                return -1;
            }
            if ((io == IN && port_io != OUT) || (io == OUT && port_io != IN)) {
                fprintf(stderr, "Pipewire I/O: port \"%s\" is not an %s.\n",
                        pws->conf[n].dest_name, io == IN ? "output" : "input");
                return -1;
            }
        }

        char *name, _name[128];
        if (pws->conf[n].local_port_name != NULL) {
            name = pws->conf[n].local_port_name;
        } else {
            name = _name;
            sprintf(name, "%s-%d", io == IN ? "input" : "output", io_idx[io]++);
        }

        void *port = pw_filter_add_port(glob.pw_filter,
                                        io == IN ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
                                        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                        0,
                                        pw_properties_new(
                                            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                                            PW_KEY_PORT_NAME, name,
                                            NULL),
                                        NULL, 0);
        if (port == NULL) {
            fprintf(stderr, "Pipewire I/O: Failed to add port \"%s\".\n", name);
            return -1;
        }
        pws->port[n].handle = port;
        pws->port[n].name = strdup(name);
        pws->port[n].dest_port_id = dest_port_id;
        pws->port[n].id = UINT32_MAX; // async api, don't know our own port id yet
    }

    callback_states_[io][glob.n_handles[io]] = callback_state;
    glob.handles[io][glob.n_handles[io]] = pws;
    glob.n_handles[io]++;

    glob.states[IN] = glob.n_handles[IN] > 0 ? callback_states_[IN] : NULL;
    glob.states[OUT] = glob.n_handles[OUT] > 0 ? callback_states_[OUT] : NULL;

    return 0;
}

static void *
run_pw_loop(void *arg)
{
    set_thread_name("pw-loop");
    pw_main_loop_run(glob.pw_loop);
    pw_filter_destroy(glob.pw_filter);
    pw_main_loop_destroy(glob.pw_loop);
    pw_deinit();
    glob.pw_filter = NULL;
    glob.pw_loop = NULL;
    return NULL;
}

// This is only called after successful init
int
bfio_synch_start(void)
{
    // Async API: we need to wait until our own filter and ports turn up in the registry to be able
    // to link them to other ports (pw_sync_core() doesn't work for this since the objects did not
    // already exist).
    glob.waiting_for_own_objects_being_registered = true;
    pw_main_loop_run(glob.pw_loop);
    pw_proxy_destroy((struct pw_proxy *)glob.pw_registry); // will remove registry listener
    glob.pw_registry = NULL;

    // should be populated, just a sanity check
    if (!populate_own_port_ids_from_registry(false)) {
        return -1;
    }
    free_startup_registry(); // registry no longer needed

    FOR_IN_AND_OUT {
        for (int n = 0; n < glob.n_handles[IO]; n++) {
            struct pipewire_state *pws = glob.handles[IO][n];
            for (int i = 0; i < pws->n_channels; i++) {
                if (pws->port[i].dest_port_id == UINT32_MAX) {
                    // no pre-linking of this port
                    continue;
                }
                char dest_port[32];
                char local_port[32];
                snprintf(dest_port, sizeof(dest_port), "%u", pws->port[i].dest_port_id);
                snprintf(local_port, sizeof(local_port), "%u", pws->port[i].id);
                struct pw_properties *props = pw_properties_new(
                    PW_KEY_LINK_INPUT_PORT,  IN ? local_port : dest_port,
                    PW_KEY_LINK_OUTPUT_PORT, IN ? dest_port : local_port,
                    NULL);
                struct pw_proxy *link = pw_core_create_object(
                    glob.pw_core,
                    "link-factory",
                    PW_TYPE_INTERFACE_Link,
                    PW_VERSION_LINK,
                    &props->dict,
                    0);
                (void)link; // we don't need to manage these
            }
        }
    }

    pthread_t pthread;
    int error;
    if ((error = pthread_create(&pthread, NULL, run_pw_loop, NULL)) != 0) {
        fprintf(stderr, "Pipewire I/O: pthread_create() failed: %s.\n", strerror(error));
        return -1;
    }
    return 0;
}

void
bfio_synch_stop(void)
{
    pw_main_loop_quit(glob.pw_loop);
}
