
# Where to find libraries, and their includes
LIBPATHS	= #-L/usr/local/lib
INCLUDE		= #-I/usr/local/include

FFTW_LIB	= -lfftw3 -lfftw3f

BRUTEFIR_VERSION = 1.0n
UNAME	= $(shell uname)
UNAME_M = $(shell uname -m)
FLEX	= flex
LD	= ld
CC	= gcc
CHMOD	= chmod
GNUTAR	= tar
CHECKER	= $(CC) #checkergcc
CC_WARNINGS	= -Wall -Wpointer-arith -Wshadow \
-Wcast-align -Wwrite-strings -Wstrict-prototypes -Wmissing-prototypes \
-Wmissing-declarations -Wnested-externs -Wredundant-decls \
-Wdisabled-optimization
CC_OPTIMISE	= -O2
CC_FLAGS	= $(DEFINE) $(CC_OPTIMISE) -g
FPIC		= -fPIC
LDSHARED	= -shared
CHMOD_REMOVEX	= -x

BRUTEFIR_LIBS	= $(FFTW_LIB) -lm -ldl
BRUTEFIR_OBJS	= brutefir.o fftw_convolver.o bfconf.o bfrun.o emalloc.o \
shmalloc.o dai.o bfconf_lexical.o inout.o dither.o delay.o merge.o firwindow.o \
#peak_limiter.o
BRUTEFIR_SSE_OBJS = convolver_xmm.o
BFIO_FILE_OBJS	= bfio_file.fpic.o
BFIO_NOISE_OBJS	= bfio_noise.fpic.o
BFIO_ALSA_LIBS	= -lasound
BFIO_ALSA_OBJS	= bfio_alsa.fpic.o emalloc.fpic.o inout.fpic.o
BFIO_OSS_OBJS	= bfio_oss.fpic.o emalloc.fpic.o
BFIO_JACK_LIBS	= -ljack
BFIO_JACK_OBJS	= bfio_jack.fpic.o emalloc.fpic.o inout.fpic.o
BFIO_FILECB_OBJS = bfio_filecb.fpic.o emalloc.fpic.o inout.fpic.o
BFLOGIC_CLI_OBJS = bflogic_cli.fpic.o inout.fpic.o
BFLOGIC_TEST_OBJS = bflogic_test.fpic.o shmalloc.fpic.o
BFLOGIC_XTC_OBJS = bflogic_xtc.fpic.o emalloc.fpic.o
BFLOGIC_EQ_OBJS = bflogic_eq.fpic.o emalloc.fpic.o shmalloc.fpic.o
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

TARGETS += oss.bfio jack.bfio filecb.bfio

all: $(TARGETS)

%.fpic.o: %.c
	$(CHECKER) -o $@			-c $(INCLUDE) $(CC_WARNINGS) $(CC_FLAGS) $(FPIC) $<

%.o: %.c
	$(CHECKER) -o $@			-c $(INCLUDE) $(CC_WARNINGS) $(CC_FLAGS) $<

# special rule to avoid some warnings
bfconf_lexical.o: bfconf_lexical.c
	$(CHECKER) -o $@			-c $(CC_FLAGS) $<

%.c: %.lex
	$(FLEX) -o$@ $<

shared_stuff:

brutefir: $(BRUTEFIR_OBJS)
	$(CHECKER) $(LIBPATHS) $(LDMULTIPLEDEFS) -o $@ $(BRUTEFIR_OBJS) $(BRUTEFIR_LIBS)

alsa.bfio: $(BFIO_ALSA_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFIO_ALSA_OBJS) $(BFIO_ALSA_LIBS) -lc
	$(CHMOD) $(CHMOD_REMOVEX) $@

oss.bfio: $(BFIO_OSS_OBJS)
	$(LD) $(LDSHARED) $(FPIC) $(LIBPATHS) -o $@ $(BFIO_OSS_OBJS) -lc
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
	cp CHANGES brutefir-$(BRUTEFIR_VERSION)
	cp LICENSE brutefir-$(BRUTEFIR_VERSION)
	cp GPL-2.0 brutefir-$(BRUTEFIR_VERSION)
	cp Makefile.brutefir brutefir-$(BRUTEFIR_VERSION)/Makefile
	cp README brutefir-$(BRUTEFIR_VERSION)
	cp asmprot.h brutefir-$(BRUTEFIR_VERSION)
	cp bench1_config brutefir-$(BRUTEFIR_VERSION)
	cp bench2_config brutefir-$(BRUTEFIR_VERSION)
	cp bench3_config brutefir-$(BRUTEFIR_VERSION)
	cp bench4_config brutefir-$(BRUTEFIR_VERSION)
	cp bench5_config brutefir-$(BRUTEFIR_VERSION)
	cp bfconf.c brutefir-$(BRUTEFIR_VERSION)
	cp bfconf.h brutefir-$(BRUTEFIR_VERSION)
	cp bfconf_grammar.h brutefir-$(BRUTEFIR_VERSION)
	cp bfconf_lexical.lex brutefir-$(BRUTEFIR_VERSION)
	cp bfio_alsa.c brutefir-$(BRUTEFIR_VERSION)
	cp bfio_file.c brutefir-$(BRUTEFIR_VERSION)
	cp bfio_jack.c brutefir-$(BRUTEFIR_VERSION)
	cp bfio_oss.c brutefir-$(BRUTEFIR_VERSION)
	cp bflogic_cli.c brutefir-$(BRUTEFIR_VERSION)
	cp bflogic_eq.c brutefir-$(BRUTEFIR_VERSION)
	cp bfmod.h brutefir-$(BRUTEFIR_VERSION)
	cp bfrun.c brutefir-$(BRUTEFIR_VERSION)
	cp bfrun.h brutefir-$(BRUTEFIR_VERSION)
	cp bit.h brutefir-$(BRUTEFIR_VERSION)
	cp brutefir.c brutefir-$(BRUTEFIR_VERSION)
	cp brutefir.html brutefir-$(BRUTEFIR_VERSION)
	cp convolver.h brutefir-$(BRUTEFIR_VERSION)
	cp convolver_xmm.c brutefir-$(BRUTEFIR_VERSION)
	cp crosspath.txt brutefir-$(BRUTEFIR_VERSION)
	cp dai.c brutefir-$(BRUTEFIR_VERSION)
	cp dai.h brutefir-$(BRUTEFIR_VERSION)
	cp defs.h brutefir-$(BRUTEFIR_VERSION)
	cp delay.c brutefir-$(BRUTEFIR_VERSION)
	cp delay.h brutefir-$(BRUTEFIR_VERSION)
	cp directpath.txt brutefir-$(BRUTEFIR_VERSION)
	cp dither.c brutefir-$(BRUTEFIR_VERSION)
	cp dither.h brutefir-$(BRUTEFIR_VERSION)
	cp dither_funs.h brutefir-$(BRUTEFIR_VERSION)
	cp emalloc.c brutefir-$(BRUTEFIR_VERSION)
	cp emalloc.h brutefir-$(BRUTEFIR_VERSION)
	cp fdrw.h brutefir-$(BRUTEFIR_VERSION)
	cp fftw_convfuns.h brutefir-$(BRUTEFIR_VERSION)
	cp fftw_convolver.c brutefir-$(BRUTEFIR_VERSION)
	cp firwindow.c brutefir-$(BRUTEFIR_VERSION)
	cp firwindow.h brutefir-$(BRUTEFIR_VERSION)
	cp inout.c brutefir-$(BRUTEFIR_VERSION)
	cp inout.h brutefir-$(BRUTEFIR_VERSION)
	cp log2.h brutefir-$(BRUTEFIR_VERSION)
	cp massive_config brutefir-$(BRUTEFIR_VERSION)
	cp numunion.h brutefir-$(BRUTEFIR_VERSION)
	cp pinfo.h brutefir-$(BRUTEFIR_VERSION)
	cp raw2real.h brutefir-$(BRUTEFIR_VERSION)
	cp real2raw.h brutefir-$(BRUTEFIR_VERSION)
	cp rendereq.h brutefir-$(BRUTEFIR_VERSION)
	cp shmalloc.c brutefir-$(BRUTEFIR_VERSION)
	cp shmalloc.h brutefir-$(BRUTEFIR_VERSION)
	cp swap.h brutefir-$(BRUTEFIR_VERSION)
	cp sysarch.h brutefir-$(BRUTEFIR_VERSION)
	cp timermacros.h brutefir-$(BRUTEFIR_VERSION)
	cp timestamp.h brutefir-$(BRUTEFIR_VERSION)
	cp xtc_config brutefir-$(BRUTEFIR_VERSION)
	$(MAKE) -C brutefir-$(BRUTEFIR_VERSION) all
	$(MAKE) -C brutefir-$(BRUTEFIR_VERSION) clean
	$(GNUTAR) czf brutefir-$(BRUTEFIR_VERSION).tar.gz brutefir-$(BRUTEFIR_VERSION)

#bflogic_xtc: C_FLAGS += $(FPIC)
#bflogic_xtc: $(BFLOGIC_XTC_OBJS)
#	$(LD) $(LD_FLAGS) $(LIBPATHS) -o xtc.bflogic $(BFLOGIC_XTC_OBJS)
xtc: $(BFLOGIC_XTC_OBJS)
	$(CHECKER) $(LIBPATHS) -o $@ $(BFLOGIC_XTC_OBJS) $(MATH_LIB) $(GSL_LIB)

clean:
	rm -f *.core core bfconf_lexical.c $(BRUTEFIR_OBJS) $(BFIO_OSS_OBJS) $(BFIO_JACK_OBJS) $(BFLOGIC_EQ_OBJS) $(BFLOGIC_XTC_OBJS) $(BFLOGIC_HRTF_OBJS) $(BFLOGIC_CLI_OBJS) $(BFIO_ALSA_OBJS) $(BFIO_FILE_OBJS)
