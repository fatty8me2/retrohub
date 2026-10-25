# beetle_psx_libretro.dll: libretro Beetle PSX (Mednafen PSX, GPL-2.0), software renderer, pure C, CHD built in.
#
# Why Beetle PSX (see docs/CORES.md): bit-exact software GPU (no GL/Vulkan, which retrorun refuses anyway), a plain interpreter CPU
# (no JIT: deterministic, so two PCs stay in step and states roll back), save states that cover everything (~4.5 MB, SPU/GPU/CD
# included), CHD/CUE/PBP/M3U/ISO/EXE content, multitap and DualShock analog, and the libretro tree ships OpenBIOS so it boots without a
# real BIOS. SwanStation was the other candidate: its software renderer is fine, but its C++/threaded GPU backend and larger,
# non-bit-stable states are a worse fit for rollback.
#
# The tree is fetched at a pinned commit (SHA-256 verified), patched in place for deterministic savestates (patch.cmake, called from fetch.cmake) and compiled with MinGW-w64 gcc (the source uses GNU C extensions MSVC lacks).
# Without gcc this target is skipped: PlayStation games stay listed but "Play" tells the user the core is missing.
# Also generates ps1_db_data.cpp (serial -> title, from the Redump-derived catalog in the same tree) for the hub's library scan.
# (FetchContent + database generation live in fetch.cmake, included from the top-level CMakeLists before hubcommon)

# ---- the core ----
set(BEETLE_GCC "" CACHE FILEPATH "MinGW-w64 gcc.exe used to build beetle_psx_libretro.dll")
if(NOT BEETLE_GCC)
    find_program(BEETLE_GCC_FOUND NAMES x86_64-w64-mingw32-gcc gcc
        HINTS $ENV{MSYS2_ROOT}/mingw64/bin C:/msys64/mingw64/bin C:/msys64/ucrt64/bin NO_DEFAULT_PATH)
    if(NOT BEETLE_GCC_FOUND)
        find_program(BEETLE_GCC_FOUND NAMES x86_64-w64-mingw32-gcc)
    endif()
    set(BEETLE_GCC "${BEETLE_GCC_FOUND}")
endif()
if(NOT BEETLE_GCC OR NOT EXISTS "${BEETLE_GCC}")
    message(STATUS "beetle_psx_libretro: no MinGW gcc found (set -DBEETLE_GCC=...), skipping; PlayStation games cannot be played")
    return()
endif()

get_filename_component(BG_DIR "${BEETLE_GCC}" DIRECTORY)
set(B ${BEETLE_SRC})
set(LC ${B}/libretro-common)
set(PSX ${B}/mednafen/psx)
set(CDR ${B}/mednafen/cdrom)
set(BEETLE_SOURCES
    # emulator
    ${PSX}/cpu.c ${PSX}/cdc.c ${PSX}/vcd.c ${PSX}/frontio.c ${PSX}/gpu.c ${PSX}/dma.c ${PSX}/gpu_polygon_sub.c ${PSX}/gte.c
    ${PSX}/irq.c ${PSX}/mdec.c ${PSX}/sio.c ${PSX}/spu.c ${PSX}/timer.c
    ${LC}/formats/mpeg1/rmpeg1_ps.c ${LC}/formats/mpeg1/rmpeg1_video.c ${LC}/formats/mp3/rmp3.c ${LC}/formats/vorbis/rvorbis.c
    # discs
    ${CDR}/CDAccess.c ${CDR}/CDAccess_CCD.c ${CDR}/CDAccess_Image.c ${CDR}/CDAccess_PBP.c ${CDR}/audioreader.c ${CDR}/cdromif.c
    ${CDR}/cdaccess_track.c ${CDR}/CDUtility.c ${CDR}/galois.c ${CDR}/l-ec.c ${CDR}/lec.c ${CDR}/recover-raw.c ${CDR}/edc_crc32.c
    ${CDR}/CDAccess_CHD.c
    # frontend glue
    ${B}/libretro.c ${B}/libretro_game_database.c ${B}/input.c ${B}/mc_async.c ${B}/input_compatibility.c
    ${B}/mednafen/mempatcher.c ${B}/mednafen/general.c ${B}/mednafen/error.c ${B}/mednafen/video/Deinterlacer.c
    ${B}/mednafen/cdstream.c ${B}/mednafen/settings.c ${B}/mednafen/state.c
    ${B}/rhi/rhi_intf.c ${B}/rhi/hdr_display.c ${B}/rhi/rhi_defer.c
    ${B}/pgxp/pgxp_cpu.c ${B}/pgxp/pgxp_gpu.c ${B}/pgxp/pgxp_gte.c ${B}/pgxp/pgxp_main.c ${B}/pgxp/pgxp_mem.c ${B}/pgxp/pgxp_value.c
    # CHD (libretro-common's clean-room reader and its codecs)
    ${LC}/formats/chd/rchd.c ${LC}/encodings/encoding_huffman.c ${LC}/encodings/encoding_rzstd.c ${LC}/streams/trans_stream_rzstd.c
    ${LC}/formats/7z/r7z_lzma.c ${LC}/formats/flac/rflac.c ${LC}/encodings/encoding_deflate.c ${LC}/features/features_cpu.c
    # libretro-common
    ${LC}/streams/file_stream.c ${LC}/streams/file_stream_transforms.c ${LC}/file/file_path.c ${LC}/file/file_path_io.c
    ${LC}/file/retro_dirent.c ${LC}/vfs/vfs_implementation.c ${LC}/lists/dir_list.c ${LC}/lists/string_list.c ${LC}/string/stdstring.c
    ${LC}/string/rstrtod.c ${LC}/compat/compat_strl.c ${LC}/compat/fopen_utf8.c ${LC}/compat/compat_strcasestr.c
    ${LC}/compat/compat_posix_string.c ${LC}/encodings/encoding_utf.c ${LC}/encodings/encoding_crc32.c ${LC}/memmap/memalign.c
    ${LC}/time/rtime.c ${LC}/hash/lrc_hash.c ${LC}/formats/data_transfer.c ${LC}/memmap/memmap.c
    ${LC}/rthreads/rthreads.c ${LC}/rthreads/retro_eventcount.c ${LC}/queues/retro_spsc.c ${LC}/queues/retro_waitable_spsc.c
    ${LC}/streams/trans_stream.c ${LC}/streams/trans_stream_pipe.c ${LC}/streams/trans_stream_deflate.c
    ${LC}/vfs/vfs_hybrid.c)
list(REMOVE_DUPLICATES BEETLE_SOURCES)

set(BEETLE_DLL ${CMAKE_CURRENT_BINARY_DIR}/beetle_psx_libretro.dll)
# Interpreter only (no HAVE_LIGHTREC): deterministic. Software renderer only (no HAVE_HW). -O2, static runtime.
add_custom_command(OUTPUT ${BEETLE_DLL}
    # PATH must contain only the gcc dir (see sameboy_rb): $ENV{PATH} entries with spaces break the VS custom-build command line.
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${BG_DIR}" ${BEETLE_GCC} -shared -w -O2 -std=gnu11 -fwrapv -fsigned-char -fomit-frame-pointer
            -DGIT_VERSION=\"\ ea14d6a\" -DARCH_X86 -DNEED_CD -DWANT_32BPP -DNEED_DEINTERLACER -DWANT_THREADING -DHAVE_THREADS -DHAVE_PBP
            -DHAVE_CHD -DHAVE_RCHD -DHAVE_RCHD_DEFLATE -DHAVE_RCHD_LZMA -DHAVE_RCHD_FLAC -DHAVE_RCHD_ZSTD -DHAVE_RFLAC -DHAVE_RZSTD
            -DMEDNAFEN_VERSION_NUMERIC=9386 -DSTDC_HEADERS -D__STDC_LIMIT_MACROS -D__LIBRETRO__ -D_LOW_ACCURACY_ -D_FILE_OFFSET_BITS=64
            -D__STDC_CONSTANT_MACROS -DNDEBUG
            -I${B} -I${B}/mednafen -I${B}/mednafen/include -I${LC}/include -I${B}/include -include ${B}/include/debug.h
            ${BEETLE_SOURCES}
            -s -Wl,--no-undefined -Wl,--version-script=${B}/link.T -static -static-libgcc -o ${BEETLE_DLL} -lwinmm
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt ${CMAKE_CURRENT_SOURCE_DIR}/patch.cmake
    COMMENT "Building beetle_psx_libretro.dll (Beetle PSX, MinGW)"
    VERBATIM)
add_custom_target(beetle_psx_libretro ALL DEPENDS ${BEETLE_DLL})
add_dependencies(retrorun beetle_psx_libretro)
add_custom_command(TARGET beetle_psx_libretro POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:retrorun>/cores
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BEETLE_DLL} $<TARGET_FILE_DIR:retrorun>/cores/beetle_psx_libretro.dll)
