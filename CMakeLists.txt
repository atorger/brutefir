cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# Compiler selection
# Must be set before project() so CMake picks it up during compiler detection.
# Use:  cmake -DUSE_GCC=ON ..
#   or: cmake -DCMAKE_C_COMPILER=gcc ..
# ---------------------------------------------------------------------------
set(USE_GCC OFF CACHE BOOL "Use GCC instead of the system default compiler (clang on FreeBSD)")
if(USE_GCC)
    find_program(GCC_C_COMPILER NAMES gcc REQUIRED)
    set(CMAKE_C_COMPILER "${GCC_C_COMPILER}" CACHE FILEPATH "C compiler" FORCE)
    message(STATUS "Compiler: GCC (USE_GCC=ON)")
endif()

project(BruteFIR VERSION 1.1.2 LANGUAGES C)

# ---------------------------------------------------------------------------
# Audio backend options
# Defaults are platform-specific: on FreeBSD only OSS is on; on Linux
# ALSA, PipeWire, and JACK are on and OSS is off.  Override with e.g.
# -DWITH_ALSA=ON or -DWITH_OSS=OFF on the cmake command line.
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    option(WITH_ALSA      "Build ALSA audio I/O module"     OFF)
    option(WITH_PIPEWIRE  "Build PipeWire audio I/O module" OFF)
    option(WITH_JACK      "Build JACK audio I/O module"     OFF)
    option(WITH_OSS       "Build OSS audio I/O module"      ON)
else()
    option(WITH_ALSA      "Build ALSA audio I/O module"     ON)
    option(WITH_PIPEWIRE  "Build PipeWire audio I/O module" ON)
    option(WITH_JACK      "Build JACK audio I/O module"     ON)
    option(WITH_OSS       "Build OSS audio I/O module"      OFF)
endif()

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)   # -std=c99, not -std=gnu99

# ---------------------------------------------------------------------------
# Required dependencies
# ---------------------------------------------------------------------------
find_package(FLEX REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(FFTW3  REQUIRED IMPORTED_TARGET fftw3)
pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)

# ---------------------------------------------------------------------------
# Platform feature-test macros
# ---------------------------------------------------------------------------
add_compile_definitions(_POSIX_C_SOURCE=200809L)
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    # __BSD_VISIBLE: unlocks alloca, MAP_ANON, _SC_NPROCESSORS_ONLN, etc.
    # _XOPEN_SOURCE=700: unlocks gettimeofday (dropped from POSIX.1-2008 but
    #   kept as an XSI extension).
    add_compile_definitions(__BSD_VISIBLE=1 _XOPEN_SOURCE=700)
endif()

# ---------------------------------------------------------------------------
# SSE detection (FreeBSD reports amd64, Linux reports x86_64)
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|i686|i586)$")
    set(BF_HAS_SSE TRUE)
endif()

# ---------------------------------------------------------------------------
# Compiler warnings (shared by all targets)
# ---------------------------------------------------------------------------
set(BF_WARNINGS
    -Wall
    -Wpointer-arith
    -Wshadow
    -Wcast-align
    -Wwrite-strings
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wmissing-declarations
    -Wnested-externs
    -Wredundant-decls
    -Wdisabled-optimization
)

# ---------------------------------------------------------------------------
# Flex-generated config parser
# ---------------------------------------------------------------------------
FLEX_TARGET(bfconf_lexical
    src/bfconf_lexical.lex
    ${CMAKE_CURRENT_BINARY_DIR}/bfconf_lexical.c
)
# Suppress warnings in flex output (it is not our code)
set_source_files_properties(${FLEX_bfconf_lexical_OUTPUTS}
    PROPERTIES COMPILE_OPTIONS "-w"
)

# ---------------------------------------------------------------------------
# Common include paths
# ---------------------------------------------------------------------------
set(BF_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
set(BRUTEFIR_SOURCES
    src/brutefir.c
    src/fftw_convolver.c
    src/bfconcurrency.c
    src/bfconf.c
    src/bfrun.c
    src/compat.c
    src/firwindow.c
    src/emalloc.c
    src/shmalloc.c
    src/dai.c
    ${FLEX_bfconf_lexical_OUTPUTS}
    src/dither.c
    src/delay.c
)
if(BF_HAS_SSE)
    list(APPEND BRUTEFIR_SOURCES src/convolver_xmm.c)
endif()

add_executable(brutefir ${BRUTEFIR_SOURCES})
target_include_directories(brutefir PRIVATE ${BF_INCLUDES})
target_compile_options(brutefir PRIVATE ${BF_WARNINGS} -O2)
if(BF_HAS_SSE)
    target_compile_options(brutefir PRIVATE -msse)
endif()
# Some internal symbols are defined in multiple translation units via headers;
# this flag preserves the original build behaviour.
target_link_options(brutefir PRIVATE "LINKER:--allow-multiple-definition")
target_link_libraries(brutefir PRIVATE
    PkgConfig::FFTW3
    PkgConfig::FFTW3F
    Threads::Threads
    m
)

# ---------------------------------------------------------------------------
# Helper function: build a .bfio or .bflogic plugin module
# Usage: bf_add_plugin(<cmake-target> <output-name> <bfio|bflogic> [sources…])
# ---------------------------------------------------------------------------
function(bf_add_plugin target output_name suffix)
    add_library(${target} SHARED ${ARGN})
    set_target_properties(${target} PROPERTIES
        PREFIX      ""
        SUFFIX      ".${suffix}"
        OUTPUT_NAME "${output_name}"
    )
    target_include_directories(${target} PRIVATE ${BF_INCLUDES})
    target_compile_options(${target} PRIVATE ${BF_WARNINGS} -O2)
    if(BF_HAS_SSE)
        target_compile_options(${target} PRIVATE -msse)
    endif()
    # These files are loaded via dlopen(); strip the execute bit so they are
    # not accidentally run as executables.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND chmod -x $<TARGET_FILE:${target}>
    )
endfunction()

# ---------------------------------------------------------------------------
# Always-built plugin modules
# ---------------------------------------------------------------------------
bf_add_plugin(file_bfio   "file"   "bfio"    src/bfio_file.c)
bf_add_plugin(filecb_bfio "filecb" "bfio"    src/bfio_filecb.c src/emalloc.c)
bf_add_plugin(cli_bflogic "cli"    "bflogic" src/bflogic_cli.c src/compat.c)
bf_add_plugin(eq_bflogic  "eq"     "bflogic"
    src/bflogic_eq.c src/emalloc.c src/compat.c src/shmalloc.c)
target_link_libraries(eq_bflogic PRIVATE PkgConfig::FFTW3 PkgConfig::FFTW3F)

# ---------------------------------------------------------------------------
# Optional: ALSA
# ---------------------------------------------------------------------------
if(WITH_ALSA)
    pkg_check_modules(ALSA IMPORTED_TARGET alsa)
    if(ALSA_FOUND)
        bf_add_plugin(alsa_bfio "alsa" "bfio" src/bfio_alsa.c src/compat.c)
        target_link_libraries(alsa_bfio PRIVATE PkgConfig::ALSA)
        message(STATUS "  ALSA I/O:     enabled")
    else()
        message(STATUS "  ALSA I/O:     disabled (alsa library not found)")
    endif()
else()
    message(STATUS "  ALSA I/O:     disabled (WITH_ALSA=OFF)")
endif()

# ---------------------------------------------------------------------------
# Optional: PipeWire
# pkg-config for libpipewire-0.3 already exports the pipewire and spa include
# paths, so no manual -I flags are needed here.
# ---------------------------------------------------------------------------
if(WITH_PIPEWIRE)
    pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
    if(PIPEWIRE_FOUND)
        bf_add_plugin(pipewire_bfio "pipewire" "bfio"
            src/bfio_pipewire.c src/compat.c)
        target_link_libraries(pipewire_bfio PRIVATE PkgConfig::PIPEWIRE)
        message(STATUS "  PipeWire I/O: enabled")
    else()
        message(STATUS "  PipeWire I/O: disabled (libpipewire-0.3 not found)")
    endif()
else()
    message(STATUS "  PipeWire I/O: disabled (WITH_PIPEWIRE=OFF)")
endif()

# ---------------------------------------------------------------------------
# Optional: JACK
# ---------------------------------------------------------------------------
if(WITH_JACK)
    pkg_check_modules(JACK IMPORTED_TARGET jack)
    if(JACK_FOUND)
        bf_add_plugin(jack_bfio "jack" "bfio" src/bfio_jack.c)
        target_link_libraries(jack_bfio PRIVATE PkgConfig::JACK)
        message(STATUS "  JACK I/O:     enabled")
    else()
        message(STATUS "  JACK I/O:     disabled (jack library not found)")
    endif()
else()
    message(STATUS "  JACK I/O:     disabled (WITH_JACK=OFF)")
endif()

# ---------------------------------------------------------------------------
# Optional: OSS  (native on FreeBSD via /dev/dsp0)
# ---------------------------------------------------------------------------
if(WITH_OSS)
    if(EXISTS "/usr/include/sys/soundcard.h" OR
       EXISTS "/usr/local/include/sys/soundcard.h")
        bf_add_plugin(oss_bfio "oss" "bfio" src/bfio_oss.c src/emalloc.c)
        message(STATUS "  OSS I/O:      enabled")
    else()
        message(STATUS "  OSS I/O:      disabled (sys/soundcard.h not found)")
    endif()
else()
    message(STATUS "  OSS I/O:      disabled (WITH_OSS=OFF)")
endif()

# ---------------------------------------------------------------------------
# Install rules (FreeBSD, default prefix /usr/local)
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    include(GNUInstallDirs)

    # Resolve the plugins directory used in brutefir_defaults
    set(BRUTEFIR_MODULES_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/brutefir")

    # Binary
    install(TARGETS brutefir
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    # Plugins (.bfio and .bflogic) — always-built set
    install(TARGETS
            file_bfio
            filecb_bfio
            cli_bflogic
            eq_bflogic
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/brutefir
    )
    if(WITH_OSS AND TARGET oss_bfio)
        install(TARGETS oss_bfio
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/brutefir)
    endif()
    if(WITH_ALSA AND TARGET alsa_bfio)
        install(TARGETS alsa_bfio
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/brutefir)
    endif()
    if(WITH_PIPEWIRE AND TARGET pipewire_bfio)
        install(TARGETS pipewire_bfio
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/brutefir)
    endif()
    if(WITH_JACK AND TARGET jack_bfio)
        install(TARGETS jack_bfio
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/brutefir)
    endif()

    # brutefir_defaults: substitute the real modules_path at configure time
    configure_file(
        freebsd/brutefir_defaults.in
        ${CMAKE_CURRENT_BINARY_DIR}/brutefir_defaults
        @ONLY
    )

    # rc.d service script — only meaningful when installing system-wide
    # (the rc subsystem looks in /usr/local/etc/rc.d, not under ~/.local).
    # Skipped automatically for per-user installs; install manually if needed:
    #   sudo install -m 555 freebsd/rc.d/brutefir_loopback /usr/local/etc/rc.d/
    if(CMAKE_INSTALL_PREFIX STREQUAL "/usr/local" OR
       CMAKE_INSTALL_PREFIX STREQUAL "/usr")
        install(PROGRAMS freebsd/rc.d/brutefir_loopback
            DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/rc.d
        )
    endif()

    # Sample configs
    install(FILES
            freebsd/brutefir.conf
            freebsd/brutefir_passthrough.conf
            ${CMAKE_CURRENT_BINARY_DIR}/brutefir_defaults
        DESTINATION ${CMAKE_INSTALL_DATADIR}/brutefir
    )
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "BruteFIR ${PROJECT_VERSION} — configuration summary")
message(STATUS "  System:   ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  Compiler: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  SSE:      ${BF_HAS_SSE}")
message(STATUS "")
