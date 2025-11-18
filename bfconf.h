/*
 * (c) Copyright 2001 - 2004, 2006, 2013 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef BFCONF_H_
#define BFCONF_H_

#include <stdlib.h>
#include <inttypes.h>

#include "dai.h"
#include "bfrun.h"
#include "bfmod.h"
#include "dither.h"
#include "timestamp.h"

#define DEFAULT_BFCONF_NAME "~/.brutefir_defaults"

struct bfconf {
    double cpu_mhz;
    int n_cpus;
    int sampling_rate;
    int filter_length;
    int n_blocks;
    int max_dither_table_size;
    int flowthrough_blocks;
    int realtime_maxprio;
    int realtime_midprio;
    int realtime_usermaxprio;
    int realtime_minprio;
    int realsize;
    bool callback_io;
    bool blocking_io;
    bool powersave;
    double analog_powersave;
    bool benchmark;
    bool debug;
    bool quiet;
    bool overflow_warnings;
    bool show_progress;
    bool realtime_priority;
    bool lock_memory;
    bool monitor_rate;
    bool synched_write;
    bool allow_poll_mode;
    struct dither_state **dither_state;
    int n_coeffs;
    struct bfcoeff *coeffs;
    void ***coeffs_data;
    int n_channels[2];
    struct bfchannel *channels[2];
    int n_physical_channels[2];
    int *n_virtperphys[2];
    int **phys2virt[2];
    int *virt2phys[2];
    int n_subdevs[2];
    struct dai_subdevice *subdevs[2];
    int *delay[2];
    int *maxdelay[2];
    bool *mute[2];
    int n_filters;
    struct bffilter *filters;
    struct bffilter_control *initfctrl;
    int n_processes;
    struct filter_process *fproc;
    int n_iomods;
    struct bfio_module *iomods;
    char **ionames;
    int n_logicmods;
    struct bflogic_module *logicmods;
    char **logicnames;
    bool use_subdelay[2];
    int *subdelay[2];
    int sdf_length;
    double sdf_beta;
    double safety_limit;
};

extern struct bfconf *bfconf;

void
bfconf_init(char filename[],
	    bool quiet,
            bool nodefault);

#endif
