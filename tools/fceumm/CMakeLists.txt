# SPDX-License-Identifier: GPL-3.0-or-later
# Builds FCEUmm (libretro NES core) as fceumm_libretro.dll for UWP x64.
# The source list mirrors Makefile.common of the pinned commit (vendor/fceumm/BASE_COMMIT):
# the boards and input folders, the core files, libretro-common's non-static set,
# and the NTSC filter. HD packs are left out (they need the extra image/codec files
# and the NES core here only has to run games). -DFCEUMM_SRC points at the checkout.
cmake_minimum_required(VERSION 3.20)
project(fceumm_libretro C)

if(NOT FCEUMM_SRC)
    message(FATAL_ERROR "Pass -DFCEUMM_SRC=<libretro-fceumm checkout>")
endif()
set(CORE ${FCEUMM_SRC}/src)
set(LRC ${CORE}/drivers/libretro/libretro-common)

file(GLOB BOARD_SOURCES ${CORE}/boards/*.c)
file(GLOB INPUT_SOURCES ${CORE}/input/*.c)

set(CORE_SOURCES
    ${CORE}/ntsc/nes_ntsc.c
    ${CORE}/drivers/libretro/libretro.c
    ${CORE}/drivers/libretro/libretro_dipswitch.c
    ${CORE}/cart.c ${CORE}/cheat.c ${CORE}/crc32.c ${CORE}/fceu-endian.c
    ${CORE}/fceu-memory.c ${CORE}/fceu.c ${CORE}/fds.c ${CORE}/fds_apu.c
    ${CORE}/file.c ${CORE}/filter.c ${CORE}/general.c ${CORE}/input.c
    ${CORE}/md5.c ${CORE}/nsf.c ${CORE}/palette.c ${CORE}/ppu.c ${CORE}/sound.c
    ${CORE}/state.c ${CORE}/video.c ${CORE}/vsuni.c ${CORE}/ines.c ${CORE}/unif.c
    ${CORE}/x6502.c)

set(COMMON_SOURCES
    ${LRC}/streams/memory_stream.c
    ${LRC}/compat/compat_posix_string.c
    ${LRC}/compat/compat_snprintf.c
    ${LRC}/compat/compat_strcasestr.c
    ${LRC}/compat/compat_strl.c
    ${LRC}/compat/fopen_utf8.c
    ${LRC}/encodings/encoding_utf.c
    ${LRC}/file/file_path.c
    ${LRC}/file/file_path_io.c
    ${LRC}/streams/file_stream.c
    ${LRC}/streams/file_stream_transforms.c
    ${LRC}/string/stdstring.c
    ${LRC}/time/rtime.c
    ${LRC}/vfs/vfs_implementation.c)

add_library(fceumm_libretro SHARED
    ${BOARD_SOURCES} ${INPUT_SOURCES} ${CORE_SOURCES} ${COMMON_SOURCES})

target_include_directories(fceumm_libretro PRIVATE
    ${CORE}/drivers/libretro ${LRC}/include ${CORE} ${CORE}/input ${CORE}/boards ${CORE}/ntsc)

# XRGB8888 output (WANT_32BPP=1, what upstream's own UWP build uses) plus the NTSC filter.
target_compile_definitions(fceumm_libretro PRIVATE
    __LIBRETRO__ PATH_MAX=1024 FCEU_VERSION_NUMERIC=9900
    FRONTEND_SUPPORTS_RGB888 HAVE_NTSC_FILTER)

if(MSVC)
    target_compile_definitions(fceumm_libretro PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    target_compile_options(fceumm_libretro PRIVATE /W1 /utf-8 /FS)
else()
    set_target_properties(fceumm_libretro PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(fceumm_libretro PRIVATE -w)
    target_link_libraries(fceumm_libretro PRIVATE m)
endif()
set_target_properties(fceumm_libretro PROPERTIES PREFIX "")
