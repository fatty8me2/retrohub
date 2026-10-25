cmake_minimum_required(VERSION 3.24)
project(RetroHub LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "RetroHub is x64-only")
endif()

file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" HUB_VERSION LIMIT_COUNT 1)
string(STRIP "${HUB_VERSION}" HUB_VERSION)

if (MSVC)
    add_compile_options(/W3 /permissive- /utf-8 /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# SDL2 (window, input, gamepads) -- shared, shipped next to the exes (the
# mupen64plus plugins load SDL2.dll too, so everything shares one copy).
FetchContent_Declare(SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2)

# Dear ImGui (hub UI)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.5.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL2::SDL2)

# mupen64plus-core API headers only (the core itself ships as the official
# 2.6.0 win64 DLL, fetched by scripts/fetch-emu.ps1)
FetchContent_Declare(m64p_core
    URL https://github.com/mupen64plus/mupen64plus-core/archive/refs/tags/2.6.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Populate(m64p_core)
add_library(m64p_api INTERFACE)
target_include_directories(m64p_api INTERFACE ${m64p_core_SOURCE_DIR}/src/api)

include(src/beetle_psx/fetch.cmake)   # pinned Beetle PSX tree + generated PS1 serial database (before hubcommon)
# libchdr (BSD-3-Clause, github.com/rtissera/libchdr) pinned to commit 47d7941: reads .chd disc images (PS1/PS2) in src/common/disc_id.cpp.
# It bundles lzma, zstd and miniz; its miniz symbols are renamed (third_party/chdr_miniz_rename.h, force-included) so they cannot
# clash with the repo's own miniz (hubminiz).
FetchContent_Declare(libchdr
    URL https://github.com/rtissera/libchdr/archive/47d794151b2b64f6aba60304e6480f4f5a2bc24b.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_LZMA_ASM OFF CACHE BOOL "" FORCE)
set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libchdr)
if(MSVC)
    foreach(t chdr-static miniz)
        target_compile_options(${t} PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/src/common/third_party/chdr_miniz_rename.h /W0)
    endforeach()
    target_compile_options(chdr-lzma PRIVATE /W0)
    target_compile_options(zstd PRIVATE /W0)
endif()

add_subdirectory(src/common)
add_subdirectory(src/saves)
add_subdirectory(src/net)
add_subdirectory(src/n64run)
add_subdirectory(src/retrorun)
add_subdirectory(src/mgba_link)
add_subdirectory(src/sameboy_rb)
add_subdirectory(src/beetle_psx)
add_subdirectory(src/hub)

# ---- Tests (ctest) ----
# Offline (no network, no emulator): ctest -C Release -LE online
# Online (hit the live netplay/friends server): ctest -C Release -L online
enable_testing()
# Online tests require RETROHUB_TEST_SERVER (a LOCAL worker) at run time; production is refused unless RETROHUB_ALLOW_PROD_TESTS=1.
# Nothing is baked into the command lines, so set the env var when running ctest; never configure a URL here.

add_test(NAME json_test COMMAND json_test)
add_test(NAME winutil_test COMMAND winutil_test)
add_test(NAME cheats_test COMMAND cheats_test)                         # cheat file model + code parsing per format
add_test(NAME hubcommon_test COMMAND hubcommon_test)
add_test(NAME hublibrary_test COMMAND hublibrary_test)
add_test(NAME hubkanto_test COMMAND hubkanto_test)                       # Kanto Together: package, install, rom pick, output parsing
add_test(NAME hubkantopad_test COMMAND hubkantopad_test)               # Kanto Together mGBA gamepad config writer
add_test(NAME findgames_test COMMAND findgames_test)                   # Find my games: collapse/rank + scratch-tree scan
add_test(NAME hub_online_caps_test COMMAND hub_online_caps_test)   # supports_online <-> launch_online branches (table-driven)
add_test(NAME ps1_test COMMAND ps1_test)                                 # PlayStation: serials, catalog, synthetic discs, BIOS discovery (scratch dirs)
add_test(NAME cheats_db_test COMMAND cheats_db_test)
add_test(NAME psarchive_test COMMAND psarchive_test)                       # PS1/PS2 disc archives: identify by name/DB, truncated, extract+confirm (scratch dirs)
add_test(NAME addgames_test COMMAND addgames_test)                     # Add games: console sorting, destination, dedupe, copy job (scratch dirs)
add_test(NAME hubgamesettings_test COMMAND hubgamesettings_test)
add_test(NAME hubdolphincheats_test COMMAND hubdolphincheats_test)   # GameCube cheats: gecko/ar_gc + Dolphin GameSettings ini writer
add_test(NAME hubcheatstore_test COMMAND hubcheatstore_test)   # cheat choices: persistence/merge/custom codes/online rules/save backup
add_test(NAME hubcontrols_test COMMAND hubcontrols_test)             # controller profiles: defaults == old hardcoded maps
add_test(NAME retrorun_ds_test COMMAND retrorun_ds_test)
add_test(NAME retrorun_dslink_test COMMAND retrorun_dslink_test)
add_test(NAME retrorun_link_test COMMAND retrorun_link_test)
add_test(NAME retrorun_rollback_test COMMAND retrorun_rollback_test)
add_test(NAME ps1_synctest COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/qa/ps1_synctest.ps1)   # Beetle PSX rollback SyncTest on homebrew discs (skips without the core / python)
set_tests_properties(ps1_synctest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 ENVIRONMENT "RETROHUB_QA=1")
add_test(NAME ps1_discs COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/qa/ps1_discs.ps1)   # 2-disc homebrew .m3u: libretro disk-control swap + rollback SyncTest with swap events
set_tests_properties(ps1_discs PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600 ENVIRONMENT "RETROHUB_QA=1")
add_test(NAME hubhistory_test COMMAND hubhistory_test)
add_test(NAME hubsaves_test COMMAND hubsaves_test)
add_test(NAME hubsaves_cloud_test COMMAND hubsaves_cloud_test)   # e2e part runs only with RETROHUB_TEST_SAVES_URL (qa\saves_cloud.ps1)
add_test(NAME hubsaves_engine_test COMMAND hubsaves_engine_test)   # cloud saves v2 engine against an in-process mock Worker (loopback only)
add_test(NAME hub_logic_test COMMAND hub_logic_test)
add_test(NAME hub_states_test COMMAND hub_states_test)               # save-state slot listing, resume wording, library sort
add_test(NAME hub_osk_test COMMAND hub_osk_test)                         # OSK key layout / edit logic / folder browser (scratch dirs)
add_test(NAME hub_session_test COMMAND hub_session_test)               # Smash save migrate, post-exit rules, queued join (scratch dirs only)
add_test(NAME hub_launchset_test COMMAND hub_launchset_test)
add_test(NAME hub_art_test COMMAND hub_art_test)                       # offline: no --live
add_test(NAME hub_updater_test COMMAND hub_updater_test --offline)     # unit checks only
add_test(NAME hub_apply_test COMMAND hub_apply_test $<TARGET_FILE:RetroHub>)   # scratch dirs only, never relaunches
add_test(NAME hub_shell_test COMMAND hub_shell_test)
add_test(NAME hub_changelog_test COMMAND hub_changelog_test ${CMAKE_SOURCE_DIR}/data/changelog.json)   # patch notes parser + show-once rule
add_test(NAME hub_group_test COMMAND hub_group_test)             # recent groups + notification history persistence/expiry
add_test(NAME hub_toast_test COMMAND hub_toast_test)                   # toast XML/activation/policy with a mock notifier (no real toasts)
add_test(NAME hub_wizard_test COMMAND hub_wizard_test)
add_test(NAME hub_cloud_ui_test COMMAND hub_cloud_ui_test)             # cloud saves v2 status/label/dialog logic
add_test(NAME hub_username_test COMMAND hub_username_test)         # username rules, suggestions, availability checker, wizard name step
add_test(NAME hub_toast_win_test COMMAND hub_toast_win_test)               # real WinRT apartment init + XML load, nothing shown
add_test(NAME hub_invite_test COMMAND hub_invite_test)                   # invite parsing + pipe handoff (no network, no windows)
add_test(NAME hub_gcnp_test COMMAND hub_gcnp_test)                     # GameCube netplay spec/config (no network)
add_test(NAME hub_emu_test COMMAND hub_emu_test)                     # 3DS/Wii U parsers, save mapping, config (no network)
add_test(NAME hub_ps2_test COMMAND hub_ps2_test)                   # PS2: images, BIOS, PCSX2.ini, pads, card session (no network)
add_test(NAME hub_np3ds_test COMMAND hub_np3ds_test)
add_test(NAME n64run_lockstep_test COMMAND n64run_lockstep_test)
add_test(NAME n64run_dropagree_test COMMAND n64run_dropagree_test)
add_test(NAME n64run_match_test COMMAND n64run_match_test)
add_test(NAME n64run_gs_cheat_test COMMAND n64run_gs_cheat_test)
add_test(NAME n64run_testinput_test COMMAND n64run_testinput_test)
add_test(NAME n64run_save_sync_test COMMAND n64run_save_sync_test)    # simulated network only
add_test(NAME n64run_mempak_test COMMAND n64run_mempak_test)          # controller pak policy (no network)

# Online: need the live server.
add_test(NAME hubnet_test COMMAND hubnet_test)                           # server: $RETROHUB_TEST_SERVER at run time
add_test(NAME hubfriends_test COMMAND hubfriends_test)                   # server: $RETROHUB_TEST_SERVER at run time
add_test(NAME hubfriends_dedupe_test COMMAND hubfriends_dedupe_test)   # friends list: one entry per uid (no network)
add_test(NAME hublink_test COMMAND hublink_test)                         # link-a-new-PC crypto/adopt/export (no network)
set_tests_properties(hubnet_test hublink_test n64run_save_sync_test n64run_lockstep_test n64run_testinput_test retrorun_link_test
                     retrorun_rollback_test retrorun_dslink_test PROPERTIES ENVIRONMENT "RETROHUB_QA=1")   # QA-only env switches (HUBNET_LOOPBACK, N64RUN_TEST_INPUT)
add_test(NAME hub_updater_live_test COMMAND hub_updater_test)         # checks GitHub releases
set_tests_properties(hubnet_test hubfriends_test hub_updater_live_test PROPERTIES LABELS online)
