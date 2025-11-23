
# Where to find libraries, and their includes
LIBPATHS	= #-L/usr/local/lib
INCLUDE		= #-I/usr/local/include -I/usr/include/pipewire-0.3
INCLUDE		+= -I/usr/include/pipewire-0.3 -I/usr/include/spa-0.2

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

BRUTEFIR_LIBS	= $(FFTW_LIB) -lm -ldl
BRUTEFIR_OBJS	= brutefir.o fftw_convolver.o bfconf.o bfrun.o compat.o emalloc.o bfconcurrency.o \
shmalloc.o dai.o bfconf_lexical.o dither.o delay.o firwindow.o \
#peak_limiter.o
BRUTEFIR_SSE_OBJS = convolver_xmm.o
BFIO_FILE_OBJS	= bfio_file.fpic.o
BFIO_NOISE_OBJS	= bfio_noise.fpic.o
BFIO_ALSA_LIBS	= -lasound
BFIO_ALSA_OBJS	= bfio_alsa.fpic.o compat.fpic.o
BFIO_JACK_LIBS	= -ljack
BFIO_JACK_OBJS	= bfio_jack.fpic.o
BFIO_PIPEWIRE_LIBS = -lpipewire-0.3
BFIO_PIPEWIRE_OBJS = bfio_pipewire.fpic.o compat.fpic.o
BFIO_FILECB_OBJS = bfio_filecb.fpic.o emalloc.fpic.o
BFLOGIC_CLI_OBJS = bflogic_cli.fpic.o compat.fpic.o
BFLOGIC_TEST_OBJS = bflogic_test.fpic.o shmalloc.fpic.o
BFLOGIC_XTC_OBJS = bflogic_xtc.fpic.o emalloc.fpic.o
BFLOGIC_EQ_OBJS = bflogic_eq.fpic.o emalloc.fpic.o compat.fpic.o shmalloc.fpic.o
BFLOGIC_HRTF_OBJS = bflogic_hrtf.fpic.o emalloc.fpic.o shmalloc.fpic.o

BASE_TARGETS	= brutefir cli.bflogic file.bfio eq.bflogic noise.bfio \
test.bflogic
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

TARGETS += pipewire.bfio jack.bfio filecb.bfio

all: $(TARGETS)

%.fpic.o: %.c
	$(CC) -o $@			-c $(INCLUDE) $(CC_WARNINGS) $(CC_FLAGS) $(FPIC) $<

%.o: %.c
	$(CC) -o $@			-c $(INCLUDE) $(CC_WARNINGS) $(CC_FLAGS) $<

# special rule to avoid some warnings
bfconf_lexical.o: bfconf_lexical.c
	$(CC) -o $@			-c $(CC_FLAGS) $<

%.c: %.lex
	$(FLEX) -o$@ $<

shared_stuff:

brutefir: $(BRUTEFIR_OBJS)
	$(CC) $(LIBPATHS) $(LDMULTIPLEDEFS) -o $@ $(BRUTEFIR_OBJS) $(BRUTEFIR_LIBS)

alsa.bfio: $(BFIO_ALSA_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFIO_ALSA_OBJS) $(BFIO_ALSA_LIBS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

file.bfio: $(BFIO_FILE_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFIO_FILE_OBJS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

noise.bfio: $(BFIO_NOISE_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFIO_NOISE_OBJS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

filecb.bfio: $(BFIO_FILECB_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFIO_FILECB_OBJS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

jack.bfio: $(BFIO_JACK_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFIO_JACK_OBJS) $(BFIO_JACK_LIBS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

pipewire.bfio: $(BFIO_PIPEWIRE_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFIO_PIPEWIRE_OBJS) $(BFIO_PIPEWIRE_LIBS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

cli.bflogic: $(BFLOGIC_CLI_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFLOGIC_CLI_OBJS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

eq.bflogic: $(BFLOGIC_EQ_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFLOGIC_EQ_OBJS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

test.bflogic: $(BFLOGIC_TEST_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFLOGIC_TEST_OBJS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

hrtf.bflogic: $(BFLOGIC_HRTF_OBJS)
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
	cp Makefile.brutefir brutefir-$(BRUTEFIR_VERSION)/Makefile
	cp README brutefir-$(BRUTEFIR_VERSION)
	cp bench1_config brutefir-$(BRUTEFIR_VERSION)/examples
	cp bench2_config brutefir-$(BRUTEFIR_VERSION)/examples
	cp bench3_config brutefir-$(BRUTEFIR_VERSION)/examples
	cp bench4_config brutefir-$(BRUTEFIR_VERSION)/examples
	cp bench5_config brutefir-$(BRUTEFIR_VERSION)/examples
	cp massive_config brutefir-$(BRUTEFIR_VERSION)/examples
	cp crosspath.txt brutefir-$(BRUTEFIR_VERSION)/examples
	cp directpath.txt brutefir-$(BRUTEFIR_VERSION)/examples
	cp xtc_config brutefir-$(BRUTEFIR_VERSION)/examples
	cp asmprot.h brutefir-$(BRUTEFIR_VERSION)/src
	cp bfconcurrency.c brutefir-$(BRUTEFIR_VERSION)/src
	cp bfconcurrency.h brutefir-$(BRUTEFIR_VERSION)/src
	cp bfconf.c brutefir-$(BRUTEFIR_VERSION)/src
	cp bfconf.h brutefir-$(BRUTEFIR_VERSION)/src
	cp bfconf_grammar.h brutefir-$(BRUTEFIR_VERSION)/src
	cp bfconf_lexical.lex brutefir-$(BRUTEFIR_VERSION)/src
	cp bfio_alsa.c brutefir-$(BRUTEFIR_VERSION)/src
	cp bfio_file.c brutefir-$(BRUTEFIR_VERSION)/src
	cp bfio_jack.c brutefir-$(BRUTEFIR_VERSION)/src
	cp bfio_pipewire.c brutefir-$(BRUTEFIR_VERSION)/src
	cp bflogic_cli.c brutefir-$(BRUTEFIR_VERSION)/src
	cp bflogic_eq.c brutefir-$(BRUTEFIR_VERSION)/src
	cp bfmod.h brutefir-$(BRUTEFIR_VERSION)/src
	cp bfrun.c brutefir-$(BRUTEFIR_VERSION)/src
	cp bfrun.h brutefir-$(BRUTEFIR_VERSION)/src
	cp bit.h brutefir-$(BRUTEFIR_VERSION)/src
	cp brutefir.c brutefir-$(BRUTEFIR_VERSION)/src
	cp compat.c brutefir-$(BRUTEFIR_VERSION)/src
	cp compat.h brutefir-$(BRUTEFIR_VERSION)/src
	cp convolver.h brutefir-$(BRUTEFIR_VERSION)/src
	cp convolver_xmm.c brutefir-$(BRUTEFIR_VERSION)/src
	cp dai.c brutefir-$(BRUTEFIR_VERSION)/src
	cp dai.h brutefir-$(BRUTEFIR_VERSION)/src
	cp delay.c brutefir-$(BRUTEFIR_VERSION)/src
	cp delay.h brutefir-$(BRUTEFIR_VERSION)/src
	cp dither.c brutefir-$(BRUTEFIR_VERSION)/src
	cp dither.h brutefir-$(BRUTEFIR_VERSION)/src
	cp dither_funs.h brutefir-$(BRUTEFIR_VERSION)/src
	cp emalloc.c brutefir-$(BRUTEFIR_VERSION)/src
	cp emalloc.h brutefir-$(BRUTEFIR_VERSION)/src
	cp fdrw.h brutefir-$(BRUTEFIR_VERSION)/src
	cp fftw_convfuns.h brutefir-$(BRUTEFIR_VERSION)/src
	cp fftw_convolver.c brutefir-$(BRUTEFIR_VERSION)/src
	cp firwindow.c brutefir-$(BRUTEFIR_VERSION)/src
	cp firwindow.h brutefir-$(BRUTEFIR_VERSION)/src
	cp inout.h brutefir-$(BRUTEFIR_VERSION)/src
	cp log2.h brutefir-$(BRUTEFIR_VERSION)/src
	cp numunion.h brutefir-$(BRUTEFIR_VERSION)/src
	cp pinfo.h brutefir-$(BRUTEFIR_VERSION)/src
	cp raw2real.h brutefir-$(BRUTEFIR_VERSION)/src
	cp real2raw.h brutefir-$(BRUTEFIR_VERSION)/src
	cp rendereq.h brutefir-$(BRUTEFIR_VERSION)/src
	cp shmalloc.c brutefir-$(BRUTEFIR_VERSION)/src
	cp shmalloc.h brutefir-$(BRUTEFIR_VERSION)/src
	cp swap.h brutefir-$(BRUTEFIR_VERSION)/src
	cp sysarch.h brutefir-$(BRUTEFIR_VERSION)/src
	cp timermacros.h brutefir-$(BRUTEFIR_VERSION)/src
	cp timestamp.h brutefir-$(BRUTEFIR_VERSION)/src
	$(MAKE) -C brutefir-$(BRUTEFIR_VERSION) all
	$(MAKE) -C brutefir-$(BRUTEFIR_VERSION) clean
	$(GNUTAR) czf brutefir-$(BRUTEFIR_VERSION).tar.gz brutefir-$(BRUTEFIR_VERSION)

#bflogic_xtc: C_FLAGS += $(FPIC)
#bflogic_xtc: $(BFLOGIC_XTC_OBJS)
#	$(LD) $(LD_FLAGS) $(LIBPATHS) -o xtc.bflogic $(BFLOGIC_XTC_OBJS)
xtc: $(BFLOGIC_XTC_OBJS)
	$(CC) $(LIBPATHS) -o $@ $(BFLOGIC_XTC_OBJS) $(MATH_LIB) $(GSL_LIB)

clean:
	rm -f *.core core bfconf_lexical.c $(BRUTEFIR_OBJS) $(BFIO_OSS_OBJS) $(BFIO_JACK_OBJS) $(BFLOGIC_EQ_OBJS) $(BFLOGIC_XTC_OBJS) $(BFLOGIC_HRTF_OBJS) $(BFLOGIC_CLI_OBJS) $(BFIO_ALSA_OBJS) $(BFIO_FILE_OBJS) $(BFIO_FILECB_OBJS) $(BFIO_PIPEWIRE_OBJS)
