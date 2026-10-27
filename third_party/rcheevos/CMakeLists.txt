# RetroHub's build file for the vendored rcheevos (RetroAchievements/rcheevos v12.2.1, MIT; see VENDORED.txt).
# Not part of upstream. The rc_client_raintegration sources (RAIntegration DLL loader) are left out on purpose.
# rc_client_external.c is only used with -DRC_CLIENT_SUPPORTS_EXTERNAL (RAIntegration), so it is not built.
add_library(rcheevos STATIC
    src/rc_client.c
    src/rc_compat.c
    src/rc_libretro.c
    src/rc_util.c
    src/rc_version.c
    src/rapi/rc_api_common.c
    src/rapi/rc_api_editor.c
    src/rapi/rc_api_info.c
    src/rapi/rc_api_runtime.c
    src/rapi/rc_api_user.c
    src/rcheevos/alloc.c
    src/rcheevos/condition.c
    src/rcheevos/condset.c
    src/rcheevos/consoleinfo.c
    src/rcheevos/format.c
    src/rcheevos/lboard.c
    src/rcheevos/memref.c
    src/rcheevos/operand.c
    src/rcheevos/rc_validate.c
    src/rcheevos/richpresence.c
    src/rcheevos/runtime.c
    src/rcheevos/runtime_progress.c
    src/rcheevos/trigger.c
    src/rcheevos/value.c
    src/rhash/aes.c
    src/rhash/cdreader.c
    src/rhash/hash.c
    src/rhash/hash_disc.c
    src/rhash/hash_encrypted.c
    src/rhash/hash_rom.c
    src/rhash/hash_zip.c
    src/rhash/md5.c)
target_include_directories(rcheevos PUBLIC include src
    PRIVATE src ${CMAKE_SOURCE_DIR}/src/retrorun/third_party)   # rc_libretro.c: <libretro.h> (MIT, vendored for retrorun)
target_include_directories(rcheevos PUBLIC ${CMAKE_SOURCE_DIR}/src/retrorun/third_party)   # rc_libretro.h pulls in <libretro.h>
set_target_properties(rcheevos PROPERTIES C_STANDARD 99 LINKER_LANGUAGE C)
if (MSVC)
    target_compile_options(rcheevos PRIVATE /W0)
    target_compile_definitions(rcheevos PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
