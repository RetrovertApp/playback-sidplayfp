# libsidplayfp Playback Plugin
#
# This plugin uses libsidplayfp to decode Commodore 64 SID music formats.
# The plugin implements the RVPlaybackPlugin interface.
#
# Based on libsidplayfp 2.16.0 by Leandro Nini

cmake_minimum_required(VERSION 3.16)
project(playback_sidplayfp C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PlaybackPlugin)

# Download libsidplayfp 2.16.0
download_library(
    NAME sidplayfp
    URL "https://github.com/libsidplayfp/libsidplayfp/archive/refs/tags/v2.16.0.tar.gz"
    FILENAME "libsidplayfp-2.16.0.tar.gz"
    HASH "365c6381f4c43aebf47f1822cf242ef1666f10f9ba8efc23d238d47736532888"
    EXTRACT_DIR "libsidplayfp-2.16.0"
    PATCHES patches/scope-capture.patch patches/play-cycle-budget.patch
)

set(LIBSIDPLAYFP_ROOT "${sidplayfp_SOURCE_DIR}/src")

# Copy pre-assembled 6502 binary files (normally built by autoconf from .a65 sources)
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/psiddrv.bin" DESTINATION "${LIBSIDPLAYFP_ROOT}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/poweron.bin" DESTINATION "${LIBSIDPLAYFP_ROOT}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/sidplayer1.bin" DESTINATION "${LIBSIDPLAYFP_ROOT}/sidtune")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/sidplayer2.bin" DESTINATION "${LIBSIDPLAYFP_ROOT}/sidtune")

# Generate siddefs-fp.h from template (normally done by autoconf)
set(RESID_BRANCH_HINTS 1)
set(HAVE_BUILTIN_EXPECT 1)
set(RESID_INLINING 1)
set(RESID_INLINE inline)
configure_file(
    "${LIBSIDPLAYFP_ROOT}/builders/residfp-builder/residfp/siddefs-fp.h.in"
    "${LIBSIDPLAYFP_ROOT}/builders/residfp-builder/residfp/siddefs-fp.h"
)

# Generate sidversion.h from template
set(LIB_MAJOR 2)
set(LIB_MINOR 16)
set(LIB_LEVEL 0)
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

# ReSIDfp builder sources
file(GLOB RESIDFP_BUILDER_SOURCES "${LIBSIDPLAYFP_ROOT}/builders/residfp-builder/*.cpp")
file(GLOB RESIDFP_SOURCES "${LIBSIDPLAYFP_ROOT}/builders/residfp-builder/residfp/*.cpp")
file(GLOB RESIDFP_RESAMPLE_SOURCES "${LIBSIDPLAYFP_ROOT}/builders/residfp-builder/residfp/resample/*.cpp")
list(FILTER RESIDFP_RESAMPLE_SOURCES EXCLUDE REGEX "test\\.cpp$")

# Utility sources
file(GLOB UTILS_SOURCES "${LIBSIDPLAYFP_ROOT}/utils/*.cpp")
file(GLOB UTILS_MD5_SOURCES "${LIBSIDPLAYFP_ROOT}/utils/MD5/*.cpp")

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
    ${UTILS_MD5_SOURCES}
    ${LIBSIDPLAYFP_TOPLEVEL_SOURCES}
)

set_target_properties(sidplayfp_playback PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
)

# Include directories
target_include_directories(sidplayfp_playback PRIVATE
    ${LIBSIDPLAYFP_ROOT}
    ${LIBSIDPLAYFP_ROOT}/sidplayfp
    ${LIBSIDPLAYFP_ROOT}/builders/residfp-builder
    ${LIBSIDPLAYFP_ROOT}/builders/residfp-builder/residfp
    ${LIBSIDPLAYFP_ROOT}/utils/MD5
)

# Compile definitions
target_compile_definitions(sidplayfp_playback PRIVATE
    HAVE_CXX14
    PACKAGE="libsidplayfp"
    VERSION="2.16.0"
    PACKAGE_NAME="libsidplayfp"
    PACKAGE_VERSION="2.16.0"
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
