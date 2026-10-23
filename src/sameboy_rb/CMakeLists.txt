# sameboy_rb_libretro.dll: libretro SameBoy (MIT) with two determinism patches, for rollback netplay (GB/GBC, incl. --link dual mode).
#
# Why: SameBoy's joypad-bounce emulation compares a new key state with gb->keys, which is NOT part of the savestate, and its power-on
# RAM comes from a global time-seeded RNG that is not saved either. After a rollback load the core therefore re-simulated a frame
# differently (CGB: "video differs after a load"; DMG: rare desyncs). The patch (see patch_libretro.cmake) turns both off. Everything
# else is the unmodified libretro/SameBoy tree.
#
# SameBoy uses GNU C extensions (statement expressions, __typeof__, packed structs), which MSVC cannot compile, so the core is built
# with a MinGW-w64 gcc (MSYS2 mingw64, or -DSAMEBOY_RB_GCC=<path>). Without one this target is skipped and retrorun stays on lockstep
# netplay for SameBoy (it refuses --rollback for any core it has not verified).
include(FetchContent)
set(SAMEBOY_RB_GCC "" CACHE FILEPATH "MinGW-w64 gcc.exe used to build sameboy_rb_libretro.dll")
if(NOT SAMEBOY_RB_GCC)
    find_program(SAMEBOY_RB_GCC_FOUND NAMES x86_64-w64-mingw32-gcc gcc
        HINTS $ENV{MSYS2_ROOT}/mingw64/bin C:/msys64/mingw64/bin C:/msys64/ucrt64/bin NO_DEFAULT_PATH)
    if(NOT SAMEBOY_RB_GCC_FOUND)
        find_program(SAMEBOY_RB_GCC_FOUND NAMES x86_64-w64-mingw32-gcc)
    endif()
    set(SAMEBOY_RB_GCC "${SAMEBOY_RB_GCC_FOUND}")
endif()
if(NOT SAMEBOY_RB_GCC OR NOT EXISTS "${SAMEBOY_RB_GCC}")
    message(STATUS "sameboy_rb_libretro: no MinGW gcc found (set -DSAMEBOY_RB_GCC=...), skipping; GB/GBC netplay stays on lockstep")
    return()
endif()

FetchContent_Declare(sameboy
    URL https://github.com/libretro/SameBoy/archive/aa158a889a48b538a0302873704a34577c8eb67d.tar.gz
    URL_HASH SHA256=6d80783ac470c15b5be1060e619eda01079f00c1c42fdfbcc65527b7dcd8b5b7
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(sameboy)
if(NOT sameboy_POPULATED)
    FetchContent_Populate(sameboy)
endif()

get_filename_component(SB_GCC_DIR "${SAMEBOY_RB_GCC}" DIRECTORY)   # cc1/as/ld and runtime DLLs live next to gcc
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN})
include(${CMAKE_CURRENT_SOURCE_DIR}/patch_libretro.cmake)
sameboy_rb_prepare(${sameboy_SOURCE_DIR} ${GEN})

set(SB ${sameboy_SOURCE_DIR})
set(LC ${SB}/libretro/libretro-common)
set(SB_SRC
    ${SB}/Core/gb.c ${SB}/Core/sgb.c ${SB}/Core/apu.c ${SB}/Core/memory.c ${SB}/Core/mbc.c ${SB}/Core/timing.c ${SB}/Core/display.c
    ${SB}/Core/symbol_hash.c ${SB}/Core/camera.c ${SB}/Core/sm83_cpu.c ${SB}/Core/joypad.c ${SB}/Core/save_state.c ${SB}/Core/random.c
    ${SB}/Core/rumble.c
    ${GEN}/agb_boot.c ${GEN}/cgb_boot.c ${GEN}/dmg_boot.c ${GEN}/sgb_boot.c ${GEN}/sgb2_boot.c ${GEN}/libretro.c
    ${LC}/compat/compat_posix_string.c ${LC}/compat/compat_snprintf.c ${LC}/compat/compat_strcasestr.c ${LC}/compat/compat_strl.c
    ${LC}/compat/fopen_utf8.c ${LC}/encodings/encoding_utf.c ${LC}/file/file_path.c ${LC}/file/file_path_io.c
    ${LC}/streams/file_stream.c ${LC}/streams/file_stream_transforms.c ${LC}/string/stdstring.c ${LC}/time/rtime.c
    ${LC}/vfs/vfs_implementation.c)

# retrorun's rollback gate recognises this core by its library name ("SameBoy (rollback)") and by this DLL name.
set(SB_DLL ${CMAKE_CURRENT_BINARY_DIR}/sameboy_rb_libretro.dll)
add_custom_command(OUTPUT ${SB_DLL}
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SB_GCC_DIR};$ENV{PATH}" ${SAMEBOY_RB_GCC} -shared -w -O2 -std=gnu11 -D__LIBRETRO__ -D_GNU_SOURCE -D_USE_MATH_DEFINES
            -DGB_DISABLE_TIMEKEEPING -DGB_DISABLE_REWIND -DGB_DISABLE_DEBUGGER -DGB_DISABLE_CHEATS -DGB_INTERNAL
            "-DGB_VERSION=\"1.0.4\"" "-DGIT_VERSION=\" aa158a8\""
            -I${SB} -I${SB}/libretro -I${LC}/include ${SB_SRC}
            -Wl,--version-script=${SB}/libretro/link.T -static -static-libgcc -o ${SB_DLL} -lm
    DEPENDS ${GEN}/libretro.c ${CMAKE_CURRENT_SOURCE_DIR}/patch_libretro.cmake
    COMMENT "Building sameboy_rb_libretro.dll (SameBoy + rollback determinism patch, MinGW)"
    VERBATIM)
add_custom_target(sameboy_rb_libretro ALL DEPENDS ${SB_DLL})
add_dependencies(retrorun sameboy_rb_libretro)
add_custom_command(TARGET sameboy_rb_libretro POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:retrorun>/cores
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SB_DLL} $<TARGET_FILE_DIR:retrorun>/cores/sameboy_rb_libretro.dll)
