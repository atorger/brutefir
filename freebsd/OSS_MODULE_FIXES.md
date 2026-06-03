# OSS I/O module restore — bug analysis and fixes

This document is written for the BruteFIR author/maintainer to review the
changes made to `src/bfio_oss.c` when the OSS I/O module was restored for the
FreeBSD port. It explains **where** the restored code was wrong, **why** it was
wrong, and **how** the v1.1.0 changes (the ALSA-modernisation and the
fork → pthreads rewrite) silently changed the module contract so that a
verbatim restore of the old OSS module produces corrupted audio.

The intent is to make the changes easy to review and accept: each fix is small,
local to `set_params()`, and motivated below from first principles.

---

## Background: how the OSS module came back

The OSS I/O module was removed in v1.1.0:

```
BruteFIR v1.1.0                                                November 23, 2025
        ...
        * Remove Sparc/SunOS and FreeBSD-specific code.
        * Remove obsolete OSS I/O module.
```

For the FreeBSD port it was restored essentially verbatim from a pre-1.1.0
revision, with only the obvious mechanical edits needed to compile against the
current tree (`defs.h` no longer exists, `bool_t` → `bool`, etc.).

The problem: **v1.1.0 did not just delete the OSS module, it also changed two
contracts that the OSS module depends on.** The old OSS code was correct against
the *old* framework, but the framework moved underneath it. A verbatim restore
therefore compiles and runs, but the audio stream is mangled. The two issues
below are the cause.

---

## The module/framework contract that matters here

When an I/O module is initialised, the framework calls `bfio_init()`, which in
turn calls the module-private `set_params()`. Two values cross this boundary:

* **in:** `period_size` — the software block size the framework wants.
* **out:** `*hardware_period_size` (a.k.a. `device_period_size`) — the block
  size the device actually settled on, reported back to the framework.

`dai.h` states the unit of the input explicitly:

```c
dai_init(int period_size, /* in samples, must be a power of two */
         ...
```

"samples" here means **frames** (one sample per channel), i.e. it is *not* a
byte count and *not* multiplied by the channel count. The output value must use
the **same unit** — frames — because the framework multiplies it straight back
up by channels and sample size (see Bug 2).

---

## Bug 1 — `SNDCTL_DSP_SETFRAGMENT` was given a byte count instead of `log2(bytes)`

### The restored (broken) code

```c
int format, n;

/* Set the fragment size */
n = period_size * open_channels * bf_sampleformat_size(sample_format);
n = (0x7FFF << 16) | n;
/* we check the actual result later (n does not get the true value) */
if (ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &n) == -1) {
    ...
}
```

### Why this is wrong

`SNDCTL_DSP_SETFRAGMENT` does **not** take a fragment size in bytes. Its `int`
argument is bit-packed into two 16-bit halves:

```
 31                    16 15                     0
+------------------------+------------------------+
|   max number of frags  |  log2(fragment bytes)  |
+------------------------+------------------------+
        high 16 bits             low 16 bits
```

* **High 16 bits** = the maximum number of fragments the driver may allocate.
  `0x7FFF` means "as many as you like" — an idiom for "don't cap the buffer
  count, just give me the fragment size I asked for".
* **Low 16 bits** = the fragment size expressed as a **power of two**, i.e. the
  *exponent*. If you want 4096-byte fragments you must pass `12`, because
  2¹² = 4096. You do **not** pass `4096`.

This encoding exists because OSS fragments are required to be powers of two, so
the API transmits only the exponent. Packing the count and the (log2) size into
one `int` lets a single `ioctl` configure the whole buffer geometry.

The restored code computes the fragment size **in bytes** and ORs that byte
count straight into the low 16 bits. That has two failure modes, both bad:

1. **Truncation / collision with the high half.** With BruteFIR's normal
   partitioned filter sizes the byte count is large. Example: a 32768-frame
   partition, 2 channels, 4 bytes/sample:

   ```
   bytes = 32768 * 2 * 4 = 262144 = 0x40000
   ```

   `0x40000` does not fit in 16 bits. `(0x7FFF << 16) | 0x40000` lets bit 18
   bleed into the fragment-count field, and the low 16 bits are `0x0000`.

2. **Absurd fragment size.** The driver reads the low 16 bits as an exponent.
   `0x0000` → 2⁰ = **1-byte fragments**. The device is now driven one byte at a
   time. Even when the byte count happens to fit in 16 bits, interpreting e.g.
   `4096` as an exponent asks for 2⁴⁰⁹⁶ bytes, which the driver clamps to
   something arbitrary. Either way the fragment geometry is garbage, which
   wrecks timing and buffering and produces the distorted/wrong-pitch/noisy
   output observed in testing.

The tell-tale sign that this is a *restore* mistake rather than original
behaviour: the file still `#include`d `"log2.h"` but never called it. The
original code used `log2_get()` here; the conversion was dropped during the
port.

### What `log2_get()` does

`log2.h` provides an exact integer base-2 logarithm for power-of-two inputs:

```c
static inline int
log2_get(uint32_t x)
{
    int lg;
    for (lg = 0; (x & 1) == 0 && lg < 32; x = x >> 1, lg++);
    if (lg == 32 || (x & ~1) != 0) {
        return -1;          /* not a power of two */
    }
    return lg;
}
```

It counts how many low zero bits `x` has (that count *is* the exponent for a
power of two) and returns `-1` if `x` is not a clean power of two. This is
exactly the value the low 16 bits of `SNDCTL_DSP_SETFRAGMENT` want, and the
`-1` return doubles as a validity check. It is safe to use here because BruteFIR
guarantees `period_size` is a power of two (`dai.h`), and `open_channels *
bytes-per-sample` is likewise a power of two for the supported formats, so the
product is always a power of two.

### The fix

```c
int format, n, frame_bytes, lg;

/* Set the fragment size.  The low 16 bits of the SNDCTL_DSP_SETFRAGMENT
   argument encode the fragment size as a power of two (log2 of the size in
   bytes), not the byte count itself; the high 16 bits are the maximum
   number of fragments. */
frame_bytes = open_channels * bf_sampleformat_size(sample_format);
if ((lg = log2_get((uint32_t)(period_size * frame_bytes))) == -1) {
    sprintf(errstr, "  Period size (%d bytes) is not a power of two.\n",
            period_size * frame_bytes);
    return false;
}
n = (0x7FFF << 16) | lg;
/* we check the actual result later (n does not get the true value) */
if (ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &n) == -1) {
    ...
}
```

`frame_bytes` (bytes per frame = channels × sample size) is factored out because
Bug 2 needs it as well.

---

## Bug 2 — `device_period_size` returned in bytes, but v1.1.0 expects frames

This is the contract that the v1.1.0 rewrite changed. The old OSS code was
correct *before* 1.1.0; the new framework made it wrong.

### The restored (broken) code

```c
/* Get the fragment size */
if (ioctl(fd, SNDCTL_DSP_GETBLKSIZE, &n) == -1) {
    sprintf(errstr, "  Could not get fragment size: %s.\n",
            strerror(errno));
    return false;
}
*hardware_period_size = n;
```

`SNDCTL_DSP_GETBLKSIZE` reports the fragment size **in bytes**. The old module
returned that byte count directly, and the old (fork-based) framework consumed
it as bytes. Consistent — back then.

### How v1.1.0 broke the contract

Two independent v1.1.0 changes combine to redefine this value as **frames**:

**(a) The ALSA module became the reference implementation, and ALSA speaks in
frames.** ALSA's period size is intrinsically a frame count, so the modernised
`bfio_alsa.c` returns frames:

```c
/* src/bfio_alsa.c */
snd_pcm_hw_params_get_period_size(params, &hw_period_size, NULL);
*hardware_period_size = (int)hw_period_size;   /* frames */
```

**(b) The threaded rewrite's `dai.c` now treats the returned value as frames and
multiplies it back up to bytes itself.** In `init_input()` / `init_output()` the
module's output lands in `sd->block_size_frames`, and then:

```c
/* src/dai.c */
if (sd->uses_clock && glob.period_size % sd->block_size_frames != 0) {
    sd->bad_alignment = true;                 /* (1) alignment in frames    */
}
...
sd->block_size = sd->block_size_frames
               * sd->channels.open_channels
               * sd->channels.sf.bytes;       /* (2) frames → bytes here    */
```

The variable was even renamed to `block_size_frames` to make the unit explicit.

So the framework now expects **frames** and does the `× channels × bytes`
conversion on its own. An OSS module that hands back **bytes** is effectively
pre-multiplied, and the framework multiplies again:

* `block_size` ends up inflated by `open_channels × bytes-per-sample`
  (e.g. **8×** for stereo S32_LE) — over-sized buffers and wrong read/write
  sizing.
* the alignment test `glob.period_size % block_size_frames` compares a frame
  count against a byte count, so it almost always reports `bad_alignment`,
  which silently forces input **poll mode** on (and explains why
  `allow_poll_mode: true` was needed just to start up).

This is the "old code made incompatible by changes since 1.1.0" case in its
purest form: no line of the OSS module changed, but its meaning did.

### The fix

```c
/* Get the fragment size.  SNDCTL_DSP_GETBLKSIZE reports it in bytes, but
   the framework expects the device period size in frames (samples per
   channel) — see dai.c, which multiplies this value back up by
   open_channels * sample size.  Convert here. */
if (ioctl(fd, SNDCTL_DSP_GETBLKSIZE, &n) == -1) {
    sprintf(errstr, "  Could not get fragment size: %s.\n",
            strerror(errno));
    return false;
}
*hardware_period_size = n / frame_bytes;
```

Dividing the byte count by `frame_bytes` (channels × sample size) yields frames,
matching both the documented `dai.h` unit and the ALSA module's behaviour.

---

## Housekeeping: dead includes removed

With `log2_get()` now correctly in use, `log2.h` is needed. Two other includes
were left over from the original file and are unused against the current tree
(clangd flags both); `BF_IN`, `BF_OUT`, `BF_MAXCHANNELS`, and the
`BF_SAMPLE_FORMAT_*` constants all come from `bfmod.h`:

```diff
 #include "log2.h"
-#include "bit.h"
 #include "emalloc.h"
 #define IS_BFIO_MODULE
 #include "bfmod.h"
-#include "inout.h"
```

---

## Items reviewed and deliberately left unchanged

For completeness, these looked suspicious during the audit but are **not** bugs:

* **`bfio_preinit()` parameter order** (`version_major` before `version_minor`).
  The names appear swapped relative to the `bfmod.h` prototype, but the
  in-tree ALSA module declares it the same way and only the `int *` types are
  significant at the ABI level — harmless.
* **Optional module symbols.** OSS omits `iscallback`, `command`, `start`,
  `stop`, and `message`. `bfconf.c` loads these with `required == false`
  (only `preinit`/`init` are mandatory), and a missing `iscallback` correctly
  defaults the module to non-callback. The module loads cleanly.
* **`read`/`write` errno handling.** Returning `-1` with `errno = EAGAIN` when
  `GETISPACE`/`GETOSPACE` report zero bytes is what `dai_input`/`dai_output`
  expect for poll-mode retry; real ioctl errors propagate their `errno`.

---

## Net diff

```diff
@@ set_params() @@
-    int format, n;
+    int format, n, frame_bytes, lg;

-    /* Set the fragment size */
-    n = period_size * open_channels * bf_sampleformat_size(sample_format);
-    n = (0x7FFF << 16) | n;
+    /* Set the fragment size.  The low 16 bits ... encode log2(bytes);
+       the high 16 bits are the maximum number of fragments. */
+    frame_bytes = open_channels * bf_sampleformat_size(sample_format);
+    if ((lg = log2_get((uint32_t)(period_size * frame_bytes))) == -1) {
+        sprintf(errstr, "  Period size (%d bytes) is not a power of two.\n",
+                period_size * frame_bytes);
+        return false;
+    }
+    n = (0x7FFF << 16) | lg;

@@ set_params(), after SNDCTL_DSP_GETBLKSIZE @@
-    *hardware_period_size = n;
+    *hardware_period_size = n / frame_bytes;   /* bytes → frames */
```

Both fixes are confined to `set_params()`; the rest of the module is the
faithful pre-1.1.0 implementation.
