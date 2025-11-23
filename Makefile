SRCDIR := src
BUILDDIR := build

# Where to find libraries, and their includes
LIBPATHS	= #-L/usr/local/lib
INCLUDE		= #-I/usr/local/include -I/usr/include/pipewire-0.3
INCLUDE		+= -I/usr/include/pipewire-0.3 -I/usr/include/spa-0.2
INCLUDE 	+= -I$(SRCDIR)

# Package maintaners: it's recommended to activate SINGLE_MOD_PATH to disallow loading modules
# from any location
DEFINE		= #-DSINGLE_MOD_PATH=/usr/lib/brutefir

FFTW_LIB	= -lfftw3 -lfftw3f

BRUTEFIR_VERSION = 1.1.0
UNAME	= $(shell uname)
UNAME_M = $(shell uname -m)
FLEX	= flex
LD	= gcc
CC	= gcc
CHMOD	= chmod
GNUTAR	= tar
CC_WARNINGS	= -Wall -Wpointer-arith -Wshadow \
-Wcast-align -Wwrite-strings -Wstrict-prototypes -Wmissing-prototypes \
-Wmissing-declarations -Wnested-externs -Wredundant-decls \
-Wdisabled-optimization
CC_OPTIMISE	= -O2
CC_STD          = -std=c99 -D_POSIX_C_SOURCE=200809L
CC_FLAGS	= $(DEFINE) $(CC_STD) $(CC_OPTIMISE) -g
FPIC		= -fPIC
LDSHARED	= -shared
CHMOD_REMOVEX	= -x

BRUTEFIR_LIBS	= $(FFTW_LIB) -lm
BRUTEFIR_OBJS = \
    $(BUILDDIR)/brutefir.o \
    $(BUILDDIR)/fftw_convolver.o \
    $(BUILDDIR)/bfconcurrency.o \
    $(BUILDDIR)/bfconf.o \
    $(BUILDDIR)/bfrun.o \
    $(BUILDDIR)/compat.o \
    $(BUILDDIR)/firwindow.o \
    $(BUILDDIR)/emalloc.o \
    $(BUILDDIR)/shmalloc.o \
    $(BUILDDIR)/dai.o \
    $(BUILDDIR)/bfconf_lexical.o \
    $(BUILDDIR)/dither.o \
    $(BUILDDIR)/delay.o
#peak_limiter.o
BRUTEFIR_SSE_OBJS = $(BUILDDIR)/convolver_xmm.o
BFIO_FILE_OBJS	= $(BUILDDIR)/bfio_file.fpic.o
#BFIO_NOISE_OBJS	= $(BUILDDIR)/bfio_noise.fpic.o
BFIO_ALSA_LIBS	= -lasound
BFIO_ALSA_OBJS	= $(BUILDDIR)/bfio_alsa.fpic.o $(BUILDDIR)/compat.fpic.o
BFIO_JACK_LIBS	= -ljack
BFIO_JACK_OBJS	= $(BUILDDIR)/bfio_jack.fpic.o
BFIO_PIPEWIRE_LIBS = -lpipewire-0.3
BFIO_PIPEWIRE_OBJS = $(BUILDDIR)/bfio_pipewire.fpic.o $(BUILDDIR)/compat.fpic.o
#BFIO_FILECB_OBJS = $(BUILDDIR)/bfio_filecb.fpic.o $(BUILDDIR)/emalloc.fpic.o
BFLOGIC_CLI_OBJS = $(BUILDDIR)/bflogic_cli.fpic.o $(BUILDDIR)/compat.fpic.o
#BFLOGIC_TEST_OBJS = bflogic_test.fpic.o shmalloc.fpic.o
#BFLOGIC_XTC_OBJS = bflogic_xtc.fpic.o emalloc.fpic.o
BFLOGIC_EQ_OBJS = $(BUILDDIR)/bflogic_eq.fpic.o $(BUILDDIR)/emalloc.fpic.o $(BUILDDIR)/compat.fpic.o $(BUILDDIR)/shmalloc.fpic.o
#BFLOGIC_HRTF_OBJS = bflogic_hrtf.fpic.o emalloc.fpic.o shmalloc.fpic.o

BASE_TARGETS	= \
    $(BUILDDIR)/brutefir \
    $(BUILDDIR)/cli.bflogic \
    $(BUILDDIR)/file.bfio \
    $(BUILDDIR)/eq.bflogic
TARGETS		= $(BASE_TARGETS)

#
# Specific Linux settings
#
ifeq ($(UNAME),Linux)
LDMULTIPLEDEFS	= -Xlinker --allow-multiple-definition
ifeq ($(UNAME_M),i586)
BRUTEFIR_OBJS	+= $(BRUTEFIR_SSE_OBJS)
CC_FLAGS	+= -msse
endif
ifeq ($(UNAME_M),i686)
BRUTEFIR_OBJS	+= $(BRUTEFIR_SSE_OBJS)
CC_FLAGS	+= -msse
endif
ifeq ($(UNAME_M),x86_64)
BRUTEFIR_OBJS	+= $(BRUTEFIR_SSE_OBJS)
CC_FLAGS	+= -msse
endif
TARGETS	+= alsa.bfio
endif

TARGETS += $(BUILDDIR)/pipewire.bfio $(BUILDDIR)/jack.bfio $(BUILDDIR)/filecb.bfio

all: $(TARGETS)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.fpic.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) -o $@			-c $(INCLUDE) $(CC_WARNINGS) $(CC_FLAGS) $(FPIC) $<

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) -o $@			-c $(INCLUDE) $(CC_WARNINGS) $(CC_FLAGS) $<

# special rule to avoid some warnings
$(BUILDDIR)/bfconf_lexical.o: $(BUILDDIR)/bfconf_lexical.c | $(BUILDDIR)
	$(CC) -o $@			-c $(LDFLAGS) $(INCLUDE) $(CC_FLAGS) $<

$(BUILDDIR)/%.c: $(SRCDIR)/%.lex | $(BUILDDIR)
	$(FLEX) -o$@ $<

$(BUILDDIR)/brutefir: $(BRUTEFIR_OBJS)
	$(CC) $(LIBPATHS) $(LDMULTIPLEDEFS) -o $@ $(BRUTEFIR_OBJS) $(BRUTEFIR_LIBS)

$(BUILDDIR)/alsa.bfio: $(BFIO_ALSA_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFIO_ALSA_OBJS) $(BFIO_ALSA_LIBS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

$(BUILDDIR)/file.bfio: $(BFIO_FILE_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFIO_FILE_OBJS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

$(BUILDDIR)/noise.bfio: $(BFIO_NOISE_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFIO_NOISE_OBJS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

$(BUILDDIR)/filecb.bfio: $(BFIO_FILECB_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFIO_FILECB_OBJS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

$(BUILDDIR)/jack.bfio: $(BFIO_JACK_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFIO_JACK_OBJS) $(BFIO_JACK_LIBS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

$(BUILDDIR)/pipewire.bfio: $(BFIO_PIPEWIRE_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFIO_PIPEWIRE_OBJS) $(BFIO_PIPEWIRE_LIBS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

$(BUILDDIR)/cli.bflogic: $(BFLOGIC_CLI_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFLOGIC_CLI_OBJS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

$(BUILDDIR)/eq.bflogic: $(BFLOGIC_EQ_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFLOGIC_EQ_OBJS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

$(BUILDDIR)/test.bflogic: $(BFLOGIC_TEST_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFLOGIC_TEST_OBJS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

$(BUILDDIR)/hrtf.bflogic: $(BFLOGIC_HRTF_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFLOGIC_HRTF_OBJS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

distrib: all
	[ -d brutefir-$(BRUTEFIR_VERSION) ] || mkdir brutefir-$(BRUTEFIR_VERSION)
	[ -d brutefir-$(BRUTEFIR_VERSION)/src ] || mkdir brutefir-$(BRUTEFIR_VERSION)/src
	[ -d brutefir-$(BRUTEFIR_VERSION)/examples ] || mkdir brutefir-$(BRUTEFIR_VERSION)/examples
	cp brutefir.html brutefir-$(BRUTEFIR_VERSION)
	cp brutefir-archive.html brutefir-$(BRUTEFIR_VERSION)
	cp CHANGES brutefir-$(BRUTEFIR_VERSION)
	cp LICENSE brutefir-$(BRUTEFIR_VERSION)
	cp Makefile.dist brutefir-$(BRUTEFIR_VERSION)/Makefile
	cp README.dist brutefir-$(BRUTEFIR_VERSION)/README
	cp examples/bench1_config brutefir-$(BRUTEFIR_VERSION)/examples
	cp examples/bench2_config brutefir-$(BRUTEFIR_VERSION)/examples
	cp examples/bench3_config brutefir-$(BRUTEFIR_VERSION)/examples
	cp examples/bench4_config brutefir-$(BRUTEFIR_VERSION)/examples
	cp examples/bench5_config brutefir-$(BRUTEFIR_VERSION)/examples
	cp examples/massive_config brutefir-$(BRUTEFIR_VERSION)/examples
	cp examples/crosspath.txt brutefir-$(BRUTEFIR_VERSION)/examples
	cp examples/directpath.txt brutefir-$(BRUTEFIR_VERSION)/examples
	cp examples/xtc_config brutefir-$(BRUTEFIR_VERSION)/examples
	cp src/asmprot.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/bfconcurrency.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/bfconcurrency.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/bfconf.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/bfconf.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/bfconf_grammar.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/bfconf_lexical.lex brutefir-$(BRUTEFIR_VERSION)/src
	cp src/bfio_alsa.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/bfio_file.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/bfio_jack.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/bfio_pipewire.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/bflogic_cli.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/bflogic_eq.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/bfmod.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/bfrun.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/bfrun.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/bit.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/brutefir.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/compat.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/compat.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/convolver.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/convolver_xmm.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/dai.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/dai.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/delay.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/delay.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/dither.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/dither.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/dither_funs.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/emalloc.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/emalloc.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/fdrw.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/fftw_convfuns.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/fftw_convolver.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/firwindow.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/firwindow.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/inout.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/log2.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/numunion.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/pinfo.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/raw2real.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/real2raw.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/rendereq.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/shmalloc.c brutefir-$(BRUTEFIR_VERSION)/src
	cp src/shmalloc.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/swap.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/sysarch.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/timermacros.h brutefir-$(BRUTEFIR_VERSION)/src
	cp src/timestamp.h brutefir-$(BRUTEFIR_VERSION)/src
	$(MAKE) -C brutefir-$(BRUTEFIR_VERSION) all
	$(MAKE) -C brutefir-$(BRUTEFIR_VERSION) clean
	$(GNUTAR) czf brutefir-$(BRUTEFIR_VERSION).tar.gz brutefir-$(BRUTEFIR_VERSION)

#bflogic_xtc: C_FLAGS += $(FPIC)
#bflogic_xtc: $(BFLOGIC_XTC_OBJS)
#	$(LD) $(LD_FLAGS) $(LIBPATHS) -o xtc.bflogic $(BFLOGIC_XTC_OBJS)
xtc: $(BFLOGIC_XTC_OBJS)
	$(CC) $(LIBPATHS) -o $@ $(BFLOGIC_XTC_OBJS) $(MATH_LIB) $(GSL_LIB)

clean:
	rm -f $(BUILDDIR)/bfconf_lexical.c $(BRUTEFIR_OBJS) $(BFIO_OSS_OBJS) $(BFIO_JACK_OBJS) $(BFLOGIC_EQ_OBJS) $(BFLOGIC_XTC_OBJS) $(BFLOGIC_HRTF_OBJS) $(BFLOGIC_CLI_OBJS) $(BFIO_ALSA_OBJS) $(BFIO_FILE_OBJS) $(BFIO_FILECB_OBJS) $(BFIO_PIPEWIRE_OBJS) $(TARGETS)
	rmdir $(BUILDDIR)
