# libsidplayfp Playback Plugin
#
# This plugin uses libsidplayfp to decode Commodore 64 SID music formats.
# The plugin implements the RVPlaybackPlugin interface.
#
# Based on libsidplayfp 3.1.1 by Leandro Nini

cmake_minimum_required(VERSION 3.16)
project(playback_sidplayfp C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PlaybackPlugin)

# Download libsidplayfp 3.1.1
download_library(
    NAME sidplayfp
    URL "https://github.com/libsidplayfp/libsidplayfp/archive/refs/tags/v3.1.1.tar.gz"
    FILENAME "libsidplayfp-3.1.1.tar.gz"
    HASH "771591756a096c96c34f7fe850f0227088fe6fda840415e1b55780c96acdd721"
    EXTRACT_DIR "libsidplayfp-3.1.1"
    PATCHES patches/scope-capture.patch patches/play-cycle-budget.patch
)

# libsidplayfp 3.0.0 moved the reSIDfp engine out of its own tree into this
# separate library, so the SID emulation is a second download.
download_library(
    NAME residfp
    URL "https://github.com/libsidplayfp/libresidfp/archive/refs/tags/v1.2.2.tar.gz"
    FILENAME "libresidfp-1.2.2.tar.gz"
    HASH "bf1782056d524112034b0d642615ffde9242513bbb6b1b99e5461c4a1430cb2a"
    EXTRACT_DIR "libresidfp-1.2.2"
    PATCHES patches/residfp-scope-capture.patch
)

set(LIBSIDPLAYFP_ROOT "${sidplayfp_SOURCE_DIR}/src")
set(LIBRESIDFP_ROOT "${residfp_SOURCE_DIR}/src")

# Copy pre-assembled 6502 binary files (normally built by autoconf from .a65 sources)
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/psiddrv.bin" DESTINATION "${LIBSIDPLAYFP_ROOT}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/poweron.bin" DESTINATION "${LIBSIDPLAYFP_ROOT}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/sidplayer1.bin" DESTINATION "${LIBSIDPLAYFP_ROOT}/sidtune")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/sidplayer2.bin" DESTINATION "${LIBSIDPLAYFP_ROOT}/sidtune")

# Generate siddefs-fp.h from template (normally done by autoconf)
set(RESIDFP_BRANCH_HINTS 1)
set(HAVE_BUILTIN_EXPECT 1)
set(RESIDFP_INLINING 1)
set(RESIDFP_INLINE inline)
set(PACKAGE_VERSION 1.2.2)
configure_file(
    "${LIBRESIDFP_ROOT}/siddefs-fp.h.in"
    "${LIBRESIDFP_ROOT}/siddefs-fp.h"
)

# Generate the two sidversion.h headers from their templates
set(LIB_MAJOR 1)
set(LIB_MINOR 2)
set(LIB_LEVEL 2)
configure_file(
    "${LIBRESIDFP_ROOT}/residfp/sidversion.h.in"
    "${LIBRESIDFP_ROOT}/residfp/sidversion.h"
)

set(LIB_MAJOR 3)
set(LIB_MINOR 1)
set(LIB_LEVEL 1)
configure_file(
    "${LIBSIDPLAYFP_ROOT}/sidplayfp/sidversion.h.in"
    "${LIBSIDPLAYFP_ROOT}/sidplayfp/sidversion.h"
)

# Collect source files
file(GLOB SIDPLAYFP_API_SOURCES "${LIBSIDPLAYFP_ROOT}/sidplayfp/*.cpp")
file(GLOB SIDTUNE_SOURCES "${LIBSIDPLAYFP_ROOT}/sidtune/*.cpp")

# C64 emulation sources
file(GLOB C64_SOURCES "${LIBSIDPLAYFP_ROOT}/c64/*.cpp")
file(GLOB C64_CPU_SOURCES "${LIBSIDPLAYFP_ROOT}/c64/CPU/*.cpp")
file(GLOB C64_CIA_SOURCES "${LIBSIDPLAYFP_ROOT}/c64/CIA/*.cpp")
file(GLOB C64_VIC_SOURCES "${LIBSIDPLAYFP_ROOT}/c64/VIC_II/*.cpp")

# ReSIDfp builder sources — the builder stayed in libsidplayfp, the engine did not
file(GLOB RESIDFP_BUILDER_SOURCES "${LIBSIDPLAYFP_ROOT}/builders/residfp-builder/*.cpp")

# ReSIDfp engine sources, from libresidfp
file(GLOB RESIDFP_SOURCES "${LIBRESIDFP_ROOT}/*.cpp")
file(GLOB RESIDFP_RESAMPLE_SOURCES "${LIBRESIDFP_ROOT}/resample/*.cpp")
list(FILTER RESIDFP_RESAMPLE_SOURCES EXCLUDE REGEX "test\\.cpp$")
list(APPEND RESIDFP_SOURCES
    "${LIBRESIDFP_ROOT}/version.cc"
    "${LIBRESIDFP_ROOT}/residfp/residfp.cpp"
)

# Utility sources (MD5 now comes from the header-only hashlib under src/libs)
file(GLOB UTILS_SOURCES "${LIBSIDPLAYFP_ROOT}/utils/*.cpp")

# Top-level sources
file(GLOB LIBSIDPLAYFP_TOPLEVEL_SOURCES "${LIBSIDPLAYFP_ROOT}/*.cpp")

add_library(sidplayfp_playback SHARED
    sidplayfp_plugin.cpp
    ${SIDPLAYFP_API_SOURCES}
    ${SIDTUNE_SOURCES}
    ${C64_SOURCES}
    ${C64_CPU_SOURCES}
    ${C64_CIA_SOURCES}
    ${C64_VIC_SOURCES}
    ${RESIDFP_BUILDER_SOURCES}
    ${RESIDFP_SOURCES}
    ${RESIDFP_RESAMPLE_SOURCES}
    ${UTILS_SOURCES}
    ${LIBSIDPLAYFP_TOPLEVEL_SOURCES}
)

set_target_properties(sidplayfp_playback PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Include directories
target_include_directories(sidplayfp_playback PRIVATE
    ${LIBSIDPLAYFP_ROOT}
    ${LIBSIDPLAYFP_ROOT}/sidplayfp
    ${LIBSIDPLAYFP_ROOT}/builders/residfp-builder
    ${LIBRESIDFP_ROOT}
)

# Compile definitions
target_compile_definitions(sidplayfp_playback PRIVATE
    HAVE_CXX17
    PACKAGE="libsidplayfp"
    VERSION="3.1.1"
    PACKAGE_NAME="libsidplayfp"
    PACKAGE_VERSION="3.1.1"
    PACKAGE_URL="https://github.com/libsidplayfp/libsidplayfp"
)

# libsidplayfp's residfp filter tables spin up a std::thread, and with the
# harness's -static-libstdc++ the static libstdc++ pulls pthread_create and
# pthread_join in with it. On the glibc 2.28 release floor those live in
# libpthread rather than libc, so the link needs it named; a modern host
# links without it only because glibc has since folded them into libc.
find_package(Threads REQUIRED)
target_link_libraries(sidplayfp_playback PRIVATE Threads::Threads)

rv_set_plugin_properties(sidplayfp_playback)
