# BruteFIR on FreeBSD

## Background

BruteFIR v1.1.0 removed all FreeBSD-specific code, including the OSS I/O
module. This document describes every change made to restore FreeBSD support,
the reasoning behind each one, and how to build on FreeBSD 15.

---

## Prerequisites

### Full build (all I/O modules)

```sh
pkg install cmake flex fftw3 pipewire alsa-lib
# Optional: pkg install jack
```

### OSS-only build

```sh
pkg install cmake flex fftw3
```

`pipewire`, `alsa-lib`, and `jack` are not required when building with
OSS only.

`cmake` is the build system used on FreeBSD (the legacy `Makefile` is retained
for Linux). `flex` is required to generate the configuration file parser.

---

## Building

```sh
cd /path/to/brutefir
mkdir build && cd build
cmake ..
cmake --build . -j$(nproc)
```

All `.bfio` and `.bflogic` plugin files are produced alongside the `brutefir`
binary in the build directory. Copy them wherever your `brutefir.conf`
references them. See [Plugin reference](#plugin-reference) below for a
description of each file.

### CMake Options

Defaults are platform-specific. Override any option with
`-DWITH_<NAME>=ON` or `OFF` on the `cmake` command line.

| Option          | FreeBSD default | Linux default | Description                                           |
|-----------------|-----------------|---------------|-------------------------------------------------------|
| `WITH_ALSA`     | `OFF`           | `ON`          | Build the ALSA audio I/O module (`alsa.bfio`)         |
| `WITH_PIPEWIRE` | `OFF`           | `ON`          | Build the PipeWire audio I/O module (`pipewire.bfio`) |
| `WITH_JACK`     | `OFF`           | `ON`          | Build the JACK audio I/O module (`jack.bfio`)         |
| `WITH_OSS`      | `ON`            | `OFF`         | Build the OSS audio I/O module (`oss.bfio`)           |
| `USE_GCC`       | `OFF`           | `OFF`         | Use GCC instead of the system default (clang)         |

### OSS-only build

On FreeBSD, `cmake ..` builds OSS only by default — no extra flags
needed. ALSA, PipeWire, and JACK are all `OFF` unless explicitly
requested.

```sh
cd /path/to/brutefir
mkdir build_oss && cd build_oss
cmake ..
cmake --build . -j$(nproc)
```

What gets produced in `build_oss/`:

| File          | Required? | Purpose                                        |
|---------------|-----------|------------------------------------------------|
| `brutefir`    | yes       | Main binary                                    |
| `oss.bfio`    | yes       | OSS audio I/O — reads/writes `/dev/dsp*`       |
| `file.bfio`   | no        | File audio I/O — useful for offline testing    |
| `filecb.bfio` | no        | Non-blocking file/pipe I/O                     |
| `cli.bflogic` | no        | Runtime control interface (TCP or serial port) |
| `eq.bflogic`  | no        | Runtime parametric equaliser                   |

Run directly from the build directory (plugins are looked up relative
to the binary by default):

```sh
cd build_oss
./brutefir ../freebsd/brutefir.conf
```

To add GCC instead of clang, append `-DUSE_GCC=ON` to the cmake
configure line (see [GCC vs. Clang](#gcc-vs-clang) below).

### Installing

#### System-wide (`/usr/local`, requires root)

```sh
sudo cmake --install build_oss
```

Installs to:

| Path | Contents |
|------|----------|
| `/usr/local/bin/brutefir` | Main binary |
| `/usr/local/lib/brutefir/*.bfio` / `*.bflogic` | Plugins |
| `/usr/local/etc/rc.d/brutefir_loopback` | Boot service script |
| `/usr/local/share/brutefir/` | Sample `brutefir.conf` and `brutefir_defaults` |

The installed `brutefir_defaults` has `modules_path` pre-set to
`/usr/local/lib/brutefir` — no manual editing needed.

#### Per-user (`~/.local`, no root required)

```sh
cmake --install build_oss --prefix ~/.local
```

Installs to:

| Path | Contents |
|------|----------|
| `~/.local/bin/brutefir` | Main binary |
| `~/.local/lib/brutefir/*.bfio` / `*.bflogic` | Plugins |
| `~/.local/share/brutefir/` | Sample configs |

`modules_path` in the installed `brutefir_defaults` is automatically
set to `~/.local/lib/brutefir`.

The rc.d service script is **not** installed for a per-user prefix
(the system rc subsystem only scans `/usr/local/etc/rc.d`). See
[Starting virtual_oss at boot](#starting-virtual_oss-at-boot) for the
manual install path, or simply start `virtual_oss` from `~/.profile`
before launching BruteFIR.

**Benefits of a per-user install:**
- No root needed to install, update, or remove.
- The system package tree (`/usr/local`) stays clean.
- Multiple versions can coexist: system-wide and per-user independently.
- Config and filter files live entirely under your home directory.

**One prerequisite regardless of install location:** your user must
have access to `/dev/dsp*`. Add yourself to the `operator` group:

```sh
sudo pw groupmod operator -m $(whoami)
```

Log out and back in for the group change to take effect.

### GCC vs. Clang

FreeBSD ships clang as its system compiler. The build defaults to clang.
If you prefer GCC for maximum fidelity with the original Linux/GCC build:

```sh
cmake -DUSE_GCC=ON ..
```

Both compilers implement IEEE 754 identically at `-O2` with standard flags.
The SSE convolver (`convolver_xmm.c`) uses explicit intrinsics rather than
auto-vectorisation, so the generated machine code is equivalent.

**Note:** changing `USE_GCC` after an initial configure requires deleting
`CMakeCache.txt` and re-running `cmake`.

### Plugin reference

BruteFIR loads plugins at startup via `dlopen()`. It looks for them in
the directory specified by `modules_path` in `brutefir_defaults` (or
the path you pass to `brutefir` directly). Plugins not present are
simply unavailable — BruteFIR will error only if a plugin listed in
`brutefir.conf` cannot be found.

#### `.bfio` — audio I/O modules

These handle reading and writing audio samples to and from BruteFIR's
processing pipeline.

| File          | Required for OSS setup | Description |
|---------------|------------------------|-------------|
| `oss.bfio`    | **yes**                | Reads and writes raw PCM samples via the OSS kernel interface (`/dev/dsp*`). This is the native audio path on FreeBSD. |
| `alsa.bfio`   | no                     | ALSA audio I/O (Linux). Not built in the default FreeBSD build. |
| `pipewire.bfio` | no                   | PipeWire audio I/O. Not built in the default FreeBSD build. |
| `jack.bfio`   | no                     | JACK audio I/O. Not built in the default FreeBSD build. |
| `file.bfio`   | no                     | Reads input from and writes output to regular files on disk. Useful for offline testing: pipe a WAV through BruteFIR without any soundcard. Supports looping and both binary and text sample formats. |
| `filecb.bfio` | no                     | Non-blocking variant of `file.bfio` that uses a callback model (`poll()`-based). Intended for use with pipes or FIFOs where the other end may not always be ready. |

#### `.bflogic` — logic and control modules

These run alongside the audio processing loop and provide runtime
control or additional DSP. They are all optional; BruteFIR runs
perfectly without them.

| File          | Description |
|---------------|-------------|
| `cli.bflogic` | Command-line control interface. Listens on a TCP port or serial device and accepts text commands at runtime: change filter attenuation, swap coefficient sets, adjust delay, mute/unmute inputs and outputs, print peak levels, and more. Useful for scripting and for integrating BruteFIR into a larger system. |
| `eq.bflogic`  | Runtime parametric equaliser. Uses FFTW to modify the frequency response of loaded filter coefficients on the fly — no restart needed. Supports up to 64 independent equalisers each with up to 128 bands. Communicated with via `cli.bflogic` or a dedicated socket. |

---

## Source Changes for FreeBSD Compatibility

### 1. `src/compat.h` — `alloca.h` not present on FreeBSD

FreeBSD does not ship a standalone `alloca.h`; `alloca()` is declared in
`<stdlib.h>`.

```c
// Before
#include <alloca.h>

// After
#if defined(__linux__)
#include <alloca.h>
#else
#include <stdlib.h>
#endif
```

`<unistd.h>` was also added to provide `pid_t` for the `posix_kill`
declaration in the same header.

---

### 2. `src/compat.c` — `sys/prctl.h` and `prctl()` are Linux-only

Thread naming via `prctl(PR_SET_NAME, …)` is a Linux kernel interface.
`set_thread_name()` is a debugging aid only; making it a no-op on FreeBSD
has no functional effect.

```c
// Before
#include <sys/prctl.h>
…
void set_thread_name(const char name[])
{
    prctl(PR_SET_NAME, (unsigned long)name, 0, 0, 0);
}

// After
#if defined(__linux__)
#include <sys/prctl.h>
#endif
…
void set_thread_name(const char name[])
{
#if defined(__linux__)
    prctl(PR_SET_NAME, (unsigned long)name, 0, 0, 0);
#else
    (void)name;
#endif
}
```

---

### 3. `src/shmalloc.c` — `MAP_ANONYMOUS` and `IPC_R`/`IPC_W`

**`MAP_ANONYMOUS`**: FreeBSD uses `MAP_ANON` (same bit value; `0x1000`).

```c
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
```

**`IPC_R` / `IPC_W`**: `SHM_R` and `SHM_W` expand to `IPC_R`/`IPC_W`, which
are BSD extensions hidden when `_POSIX_C_SOURCE=200809L` is defined. They are
defined as octal `0400` and `0200` respectively, so `SHM_R | SHM_W` equals
`0600` exactly. Replacing the symbols with the literal value is safe and
mathematically identical.

```c
// Before
shmget(key, size, IPC_CREAT | IPC_EXCL | SHM_R | SHM_W)

// After  (SHM_R=0400, SHM_W=0200 → 0400|0200 = 0600, same bits)
shmget(key, size, IPC_CREAT | IPC_EXCL | 0600)
```

`<sys/ipc.h>` was added explicitly because FreeBSD's `<sys/shm.h>` does not
always pull it in under strict POSIX mode.

---

### 4. Feature-test macros — BSD and XSI extensions hidden by `_POSIX_C_SOURCE`

With `-D_POSIX_C_SOURCE=200809L`, FreeBSD's (and glibc's) headers hide several
symbols needed by BruteFIR. Two additional defines are added for FreeBSD in
`CMakeLists.txt`:

| Define             | What it unlocks                                                |
|--------------------|----------------------------------------------------------------|
| `__BSD_VISIBLE=1`  | `alloca` in `<stdlib.h>`, `MAP_ANON`, `_SC_NPROCESSORS_ONLN` |
| `_XOPEN_SOURCE=700`| `gettimeofday` (removed from POSIX.1-2008, kept as XSI)      |

These are set as target-level defines for FreeBSD only and do not affect Linux
builds.

---

### 5. `amd64` vs. `x86_64` — SSE convolver not compiled

FreeBSD reports `uname -m` as `amd64`; Linux reports `x86_64`. The original
Makefile's SSE detection only matched `x86_64`, so `convolver_xmm.c` was never
compiled on FreeBSD.

Fixed in `CMakeLists.txt`:

```cmake
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|i686|i586)$")
    set(BF_HAS_SSE TRUE)
endif()
```

And in the legacy `Makefile`:

```makefile
ifeq ($(UNAME_M),amd64)
BRUTEFIR_OBJS += $(BRUTEFIR_SSE_OBJS)
CC_FLAGS      += -msse
endif
```

---

### 6. `src/bfio_pipewire.c` — `pipewire_init()` name conflict

A local `static` helper named `pipewire_init()` conflicted with a public API
function of the same name added in newer PipeWire versions. Renamed to
`bfio_pipewire_init()`.

---

### 7. PipeWire/SPA include paths

On Linux, headers are at `/usr/include/pipewire-0.3` and `/usr/include/spa-0.2`.
On FreeBSD (installed via `pkg`), they live under `/usr/local/include/`.
The `pkg-config` output for `libpipewire-0.3` on FreeBSD already includes the
correct `-I` flags for both pipewire and spa, so no manual path overrides are
needed in CMake.

---

### 8. `src/bfio_oss.c` — OSS I/O module restored

The OSS module was removed in v1.1.0. It has been restored for FreeBSD since
OSS (`/dev/dsp0`) is the native audio interface and the kernel driver is
always present.

#### Exact diff vs. the original `bfio_oss.c` (v1.0, commit `8a3dc5d~1`)

```diff
 5a6,10
 >  * Restored for FreeBSD by giacomo (2026): bfio_oss.c was removed in v1.1.0.
 >  * Changes from the original:
 >  *   - #include "defs.h" removed (file no longer exists); replaced with
 >  *     <stdbool.h> and <stdint.h>
 >  *   - bool_t replaced with bool throughout

 8a14,15
 > #include <stdbool.h>
 > #include <stdint.h>

 16d22
 < #include "defs.h"

 28,29c34,35
 <     bool_t dir[2];
 <     bool_t trigger;
 ---
 >     bool dir[2];
 >     bool trigger;

 43c49
 < static bool_t debug = false;
 ---
 > static bool debug = false;

 45c51
 < static bool_t
 ---
 > static bool

 (all other differences: trailing-whitespace removal only — zero logic changes)
```

Every other line in the file is byte-for-byte identical to v1.0.

#### Why `bool_t` → `bool` is safe

`bool_t` was a `typedef int` in the now-removed `defs.h`. C99 `bool`
(`_Bool`) from `<stdbool.h>` is also an integer type with identical 0 /
non-zero semantics. All assignments (`= true`, `= false`) and all tests
(`if (x)`, `if (!x)`) behave identically. The struct layout is unchanged
because the compiler pads `bool` and `int` the same way on x86-64/amd64.

The `bfio_preinit()` / `bfio_init()` / `bfio_read()` / `bfio_write()` /
`bfio_synch_start()` / `bfio_synch_stop()` signatures are unchanged and
compatible with the current `bfmod.h` ABI.

**brutefir.conf snippet for OSS:**

```
io "output" {
    device:        "oss";
    device:        "/dev/dsp0";
    sample_format: S32_LE;
    sample_rate:   192000;
    channels:      2;
};
```

---

## Bit-Perfect Audit

This section documents, function by function and file by file, why none of the
changes made in this port touch the audio signal path.

### What "bit-perfect" means here

BruteFIR processes audio as blocks of IEEE 754 floating-point samples. Bit-
perfect means that for a given input block, the output block is identical
regardless of OS or compiler, because:

1. The convolution arithmetic is deterministic IEEE 754.
2. The dithering PRNG is seeded deterministically.
3. Buffer pointers, sizes, and sample formats are negotiated during
   initialisation and then fixed for the lifetime of the process.

Any change that only affects initialisation, OS bookkeeping, or debug paths
cannot alter audio data.

---

### `src/fftw_convolver.c` — **UNMODIFIED**

Core FFT convolution engine. Calls FFTW for forward/inverse transforms and
performs complex multiply-accumulate in the frequency domain. Not touched.

---

### `src/convolver_xmm.c` — **UNMODIFIED**

SSE-accelerated inner loop for the convolution. Uses explicit x86 SSE
intrinsics (`_mm_*`). Not touched. The `-msse` flag is the same as the
original build; since the intrinsics are explicit, auto-vectorisation cannot
introduce non-determinism.

---

### `src/bfrun.c` — **UNMODIFIED**

Real-time processing thread. Owns the sample clock, calls the convolver,
manages the inter-process buffer exchange via shared memory, and invokes
dithering. Not touched.

---

### `src/dither.c` — **UNMODIFIED**

Dithering (TPDF noise shaping). PRNG state and all arithmetic are unchanged.
Not touched.

---

### `src/delay.c` — **UNMODIFIED**

Delay lines (sample-accurate, implemented as circular ring buffers). Not
touched.

---

### `src/shmalloc.c` — modified; **no effect on audio data**

**a) `MAP_ANONYMOUS` → `MAP_ANON` fallback**

```c
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
```

`MAP_ANONYMOUS` and `MAP_ANON` are aliases for the same kernel flag
(`0x1000` on FreeBSD). The kernel creates identical anonymous memory-mapped
pages in both cases. The bytes stored in that memory — audio sample buffers
— are unaffected.

**b) `SHM_R | SHM_W` → `0600`**

```c
// before
shmget(key, size, IPC_CREAT | IPC_EXCL | SHM_R | SHM_W)
// after
shmget(key, size, IPC_CREAT | IPC_EXCL | 0600)
```

This is the permission mask for the System V shared memory segment. On all
POSIX systems:

```
SHM_R = IPC_R = 0000400 octal  (owner read)
SHM_W = IPC_W = 0000200 octal  (owner write)
SHM_R | SHM_W = 0000600 octal = 0600
```

The substitution is algebraically identical — same bits, same kernel
behaviour, same memory content.

**c) `#include <sys/ipc.h>`** — header only; no runtime effect.

---

### `src/compat.c` — modified; **no effect on audio data**

**`set_thread_name()` made a no-op on non-Linux**

```c
void set_thread_name(const char name[])
{
#if defined(__linux__)
    prctl(PR_SET_NAME, (unsigned long)name, 0, 0, 0);
#else
    (void)name;
#endif
}
```

This sets the OS-visible thread name for debugging tools (`top`, `htop`).
It is called once at thread startup and has zero interaction with audio
processing. Thread names won't appear in process monitors on FreeBSD; audio
behaviour is unchanged.

`number_of_cpu_cores()`, `compat_usleep()`, and `allocate_aligned_memory()`
are byte-for-byte identical to upstream.

---

### `src/compat.h` — modified; **no effect on audio data**

Added `#include <unistd.h>` (provides `pid_t` for the `posix_kill`
prototype). Changed `alloca.h` to a platform guard so FreeBSD gets `alloca`
from `<stdlib.h>`. `alloca()` itself — used for stack-allocated scratch
buffers in `bfrun.c`, `bfconf.c`, `delay.c`, `dai.c` — has identical
runtime semantics on all platforms: it advances the stack pointer and returns
a pointer. The declaration source has no effect on the generated machine code.

---

### `src/bfio_pipewire.c` — one rename; **no effect on audio data**

Local `static` helper renamed from `pipewire_init()` to
`bfio_pipewire_init()`. It is called once during module initialisation
(before audio starts flowing), sets up the PipeWire registry listener, and
returns a bool indicating success. Its body is byte-for-byte identical to
upstream; only the name changed to avoid a symbol collision with the PipeWire
public API.

---

### `src/bfio_oss.c` — restored module; **I/O boundary only**

The OSS module sits at the I/O boundary. Audio data flows only through
`bfio_read()` and `bfio_write()`, which call the POSIX `read()`/`write()`
system calls on the `/dev/dsp` file descriptor. These calls move raw bytes
between the kernel ring buffer and the BruteFIR buffer pointer supplied by
`bfrun.c`. The bytes are never inspected, modified, or reinterpreted inside
`bfio_oss.c`.

The `bool_t` → `bool` change affects only flag variables in the device
bookkeeping struct (`dir[]`, `trigger`, `debug`) and the return type of the
internal helper `set_params()`. None of these variables appear in the data
path. `set_params()` configures the kernel driver once at startup via `ioctl`;
after that it is never called again during audio streaming.

---

### Summary table

| File                    | Changed?  | In audio data path?  | Verdict          |
|-------------------------|-----------|----------------------|------------------|
| `src/fftw_convolver.c`  | No        | Yes — core FFT       | Unmodified       |
| `src/convolver_xmm.c`   | No        | Yes — SSE inner loop | Unmodified       |
| `src/bfrun.c`           | No        | Yes — RT loop        | Unmodified       |
| `src/dither.c`          | No        | Yes                  | Unmodified       |
| `src/delay.c`           | No        | Yes                  | Unmodified       |
| `src/firwindow.c`       | No        | Yes — FIR design     | Unmodified       |
| `src/bfconf.c`          | No        | No — config only     | Unmodified       |
| `src/shmalloc.c`        | Yes       | No — IPC setup       | Safe (see above) |
| `src/compat.c`          | Yes       | No — OS glue         | Safe (see above) |
| `src/compat.h`          | Yes       | No — headers only    | Safe (see above) |
| `src/bfio_pipewire.c`   | Yes       | No — rename only     | Safe (see above) |
| `src/bfio_oss.c`        | Restored  | No — I/O boundary    | Safe (see above) |

---

## Running BruteFIR on FreeBSD

### Minimal configuration files

Two files are provided under `freebsd/`:

| File                          | Purpose                                              |
|-------------------------------|------------------------------------------------------|
| `freebsd/brutefir_defaults`   | Global defaults (filter length, sample rate, paths)  |
| `freebsd/brutefir.conf`       | Stereo passthrough via OSS (`/dev/dsp0`)              |

Copy `brutefir_defaults` to `~/.brutefir_defaults` (or
`$XDG_CONFIG_HOME/BruteFIR/brutefir_defaults.conf`):

```sh
cp freebsd/brutefir_defaults ~/.brutefir_defaults
```

Run from the build directory:

```sh
cd build_oss
./brutefir ../freebsd/brutefir.conf
```

The configuration uses identity (dirac pulse) filters, so the audio is
passed through unmodified. Replace the `"dirac pulse"` coefficient with a
real FIR file to apply room correction or crossfeed.

---

## Virtual loop device (FreeBSD equivalent of `snd_aloop`)

### Background

On Linux, `snd_aloop` is a kernel module that creates a virtual ALSA
loopback soundcard. Writing to its playback device exposes the audio on
its capture device, allowing applications to be connected without
a physical cable:

```
music player → ALSA loopback (write) → ALSA loopback (read) → BruteFIR → DAC
```

FreeBSD does not ship a kernel loopback soundcard, but the
`virtual_oss` package provides the same functionality as a userspace
daemon.

### Install

```sh
pkg install virtual_oss
```

`virtual_oss` requires the `cuse` kernel module. `cuse` (Character
device in Userspace) is a FreeBSD kernel module that lets a userspace
program create and serve `/dev/*` character device entries — the same
mechanism FUSE uses for filesystems, applied to devices. `virtual_oss`
uses it to publish `/dev/dsp.play` and `/dev/dsp.loop` as real device
nodes that any application can open with ordinary POSIX calls, while
the actual audio routing happens entirely in the `virtual_oss` process.

To load `cuse` automatically at boot, add to `/boot/loader.conf`:

```
cuse_load="YES"
```

### Loopback-only setup (no real hardware involved)

This creates `/dev/dsp.play` (playback) and `/dev/dsp.loop` (loopback
capture). Audio written to `/dev/dsp.play` is readable on
`/dev/dsp.loop`. No physical soundcard is used as the backend.

```sh
virtual_oss \
  -C 2 -c 2 \
  -r 192000 \
  -b 32 \
  -f /dev/null \
  -a 0 -d dsp.play \
  -a 0 -l dsp.loop &
```

| Flag | Meaning |
|------|---------|
| `-C 2 -c 2` | 2 output / 2 input channels |
| `-r 192000` | Sample rate for subsequent device commands |
| `-b 32` | Bit depth for subsequent device commands |
| `-f /dev/null` | Backend device for both playback and recording. `/dev/null` is magic: pure virtual routing, no real hardware opened. Use `-P` / `-R` separately for asymmetric setups (e.g. playback-only Bluetooth). |
| `-a 0` | Amplification: `log2_amp = 0` → gain = 2⁰ = 1 (unity). Set explicitly before each device; the default is undocumented. Non-zero values modify audio and break bit-accuracy. |
| `-d dsp.play` | Create `/dev/dsp.play` (playback side) |
| `-l dsp.loop` | Create `/dev/dsp.loop` (loopback capture side) |

### Full pipeline: player → BruteFIR → soundcard

```
music player ──► /dev/dsp.play ──► virtual_oss ──► /dev/dsp.loop ──► BruteFIR ──► /dev/dsp0
```

Use `-f /dev/null` so that `virtual_oss` only provides the virtual
loopback and never opens `/dev/dsp0`. Passing a real device here (e.g.
`-f /dev/dsp0`) would set that device as the playback *and* recording
backend: audio would be routed directly to the DAC by `virtual_oss`,
bypassing BruteFIR entirely, and would conflict with BruteFIR's own
open of the same device. BruteFIR must be the sole writer to the real
hardware:

```sh
virtual_oss \
  -C 2 -c 2 \
  -r 192000 \
  -b 32 \
  -f /dev/null \
  -a 0 -d dsp.play \
  -a 0 -l dsp.loop &
```

Then edit `brutefir.conf` to read from the loopback capture device and
write to the real soundcard:

```
input 0, 1 {
    device: "oss" {
        device: "/dev/dsp.loop";
    };
    sample: "S32_LE";
    channels: 2;
};

output 0, 1 {
    device: "oss" {
        device: "/dev/dsp0";
    };
    sample: "S32_LE";
    dither: false;
    channels: 2;
};
```

Configure your music player (e.g. `mpd`, `ffplay`, `sox`) to output to
`/dev/dsp.play`.

### Starting virtual_oss at boot

A dedicated rc.d service script is provided at `freebsd/rc.d/brutefir_loopback`
and installed by `cmake --install` to `/usr/local/etc/rc.d/`.

Enable it in `/etc/rc.conf`:

```sh
brutefir_loopback_enable="YES"
```

All settings have sensible defaults. Override only what you need:

```sh
brutefir_loopback_rate="192000"   # must match brutefir sampling_rate
brutefir_loopback_bits="32"       # must match sample: format (S32_LE -> 32)
brutefir_loopback_channels="2"
brutefir_loopback_play="dsp.play"
brutefir_loopback_loop="dsp.loop"
brutefir_loopback_priority="8"    # real-time priority; 0 to disable
```

Then enable and start:

```sh
service brutefir_loopback enable
service brutefir_loopback start
```

To install the script manually without running `cmake --install`:

```sh
install -m 555 freebsd/rc.d/brutefir_loopback /usr/local/etc/rc.d/
```

### Checking available devices

```sh
ls /dev/dsp*
cat /dev/sndstat
```

After starting virtual_oss, `/dev/dsp.play` and `/dev/dsp.loop` should
appear alongside the real `/dev/dsp0`.

### Notes

- **`-r`** must match `sampling_rate` in `brutefir.conf`. If you run
  BruteFIR at 192000 Hz, pass `-r 192000` to `virtual_oss`.
  The `-S` flag enables automatic rate resampling inside `virtual_oss`,
  but it is omitted here intentionally: BruteFIR requires sample-exact
  rates, so your player, `virtual_oss`, and BruteFIR must all be
  configured to the same rate with no resampling in between.
- **`-b`** is the PCM wire format on the OSS device — it must match the
  `sample:` field in `brutefir.conf` (`S32_LE` → `-b 32`, `S16_LE` →
  `-b 16`). It is unrelated to BruteFIR's `float_bits` setting, which
  controls internal processing precision only. You can use `-b 32` /
  `S32_LE` even when `float_bits: 64`.
- Because `virtual_oss` uses `-f /dev/null`, it never opens `/dev/dsp0`
  and does not interfere with other applications accessing the real
  soundcard. The service can safely run at boot and stay running when
  BruteFIR is stopped; other players can still use `/dev/dsp0` directly.
- **Bit-accurate routing**: `virtual_oss` is a software mixer and can
  modify audio in several ways. To keep the signal intact between player
  and BruteFIR:
  - `-a 0` before each device (unity gain, shown above). The
    amplification default is not documented; always set it explicitly.
  - Never add `-x` or `-g` (output/input compressors). They
    dynamically attenuate loud samples and are off by default, but
    adding them for any reason destroys bit accuracy.
  - Never add `-p 1` (polarity inversion; default is 0, normal).
  - Keep `-S` absent (no resampling, as covered above).
- **Real-time priority**: add `-i <priority>` (e.g. `-i 8`) to reduce
  the risk of buffer underruns under system load. This does not affect
  audio data, only scheduling reliability.
- If you use a JACK backend instead of OSS, `virtual_oss` is not needed:
  connect BruteFIR's JACK ports directly in `qjackctl` or via
  `jack_connect`.
