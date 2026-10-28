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
add_subdirectory(third_party/rcheevos)   # RetroAchievements rcheevos (vendored, see THIRD_PARTY.md)
add_subdirectory(src/cheevos)
add_subdirectory(src/retrorun)
add_subdirectory(src/mgba_link)
add_subdirectory(src/sameboy_rb)
add_subdirectory(src/beetle_psx)
add_subdirectory(src/stream)
add_subdirectory(src/hub)

# ---- Tests (ctest) ----
# Offline (no network, no emulator): ctest -C Release -LE online
# Online (hit the live netplay/friends server): ctest -C Release -L online
enable_testing()
# Online tests require RETROHUB_TEST_SERVER (a LOCAL worker) at run time; production is refused unless RETROHUB_ALLOW_PROD_TESTS=1.
# Nothing is baked into the command lines, so set the env var when running ctest; never configure a URL here.

add_test(NAME json_test COMMAND json_test)
add_test(NAME runner_ctl_test COMMAND runner_ctl_test)   # hub<->runner control file, ack, slot/shot paths
add_test(NAME cheevos_test COMMAND cheevos_test)   # RetroAchievements: config/cache, hardcore rules, mocked login + rc_client session (no network, no RA account)
add_test(NAME osd_test COMMAND osd_test)   # OSD toast queue + font renderer
add_test(NAME prefs_test COMMAND prefs_test)   # synced account prefs: field-wise merge, library favorites mirror
add_test(NAME winutil_test COMMAND winutil_test)
add_test(NAME starthint_test COMMAND starthint_test)
add_test(NAME hint_popup_test COMMAND hint_popup_test)
add_test(NAME cheats_test COMMAND cheats_test)                         # cheat file model + code parsing per format
add_test(NAME hubcommon_test COMMAND hubcommon_test)
add_test(NAME hublibrary_test COMMAND hublibrary_test)
add_test(NAME hubkanto_test COMMAND hubkanto_test)                       # Kanto Together: package, install, rom pick, output parsing
add_test(NAME hubkantopad_test COMMAND hubkantopad_test)               # Kanto Together mGBA gamepad config writer
add_test(NAME nes_snes_test COMMAND nes_snes_test)                 # NES/SNES headers, scan, cheats, profiles, settings
add_test(NAME nes_snes_smoke COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/qa/nes_snes_synctest.ps1)   # Nestopia + Snes9x headless run, .srm write, rollback SyncTest (skips without the cores)
add_test(NAME findgames_test COMMAND findgames_test)                   # Find my games: collapse/rank + scratch-tree scan
add_test(NAME hub_friend_games_test COMMAND hub_friend_games_test)   # greyed-out friend games: identity, ghosts, badge text, publish hash/debounce
add_test(NAME hub_gamepack_test COMMAND hub_gamepack_test)           # GamePack: Drive listing HTML parse, entity decode, safe names (no network)
add_test(NAME hub_boot_art_test COMMAND hub_boot_art_test)   # boot-animation art: every System has console + media art
add_test(NAME hub_theme_test COMMAND hub_theme_test)   # every theme preset defines every palette slot and meets text contrast
add_test(NAME hub_picker_test COMMAND hub_picker_test)   # room game picker: search, chips, sections, virtual layout
add_test(NAME hub_ps2_online_test COMMAND hub_ps2_online_test)   # PS2 online: readiness reasons, retrorun args, exit texts, pinned LRPS2 download
add_test(NAME hub_online_caps_test COMMAND hub_online_caps_test)   # supports_online <-> launch_online branches (table-driven)
add_test(NAME ps1_test COMMAND ps1_test)                                 # PlayStation: serials, catalog, synthetic discs, BIOS discovery (scratch dirs)
add_test(NAME cheats_db_test COMMAND cheats_db_test)
add_test(NAME psarchive_test COMMAND psarchive_test)                       # PS1/PS2 disc archives: identify by name/DB, truncated, extract+confirm (scratch dirs)
add_test(NAME archives_headless COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/qa/archives_headless.ps1)   # GBA .zip, NES/SNES/N64 .7z, PS1 cue/bin .zip unpacked + retrorun --headless
set_tests_properties(archives_headless PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME archives_all_test COMMAND archives_all_test)              # .zip/.7z of every system: keys, rescans, re-unpack, rom-cache (scratch dirs)
add_test(NAME addgames_test COMMAND addgames_test)                     # Add games: console sorting, destination, dedupe, copy job (scratch dirs)
add_test(NAME hubgamesettings_test COMMAND hubgamesettings_test)
add_test(NAME hubdolphincheats_test COMMAND hubdolphincheats_test)   # GameCube cheats: gecko/ar_gc + Dolphin GameSettings ini writer
add_test(NAME hubcheatstore_test COMMAND hubcheatstore_test)   # cheat choices: persistence/merge/custom codes/online rules/save backup
add_test(NAME hubcontrols_test COMMAND hubcontrols_test)             # controller profiles: defaults == old hardcoded maps
add_test(NAME retrorun_ds_test COMMAND retrorun_ds_test)
add_test(NAME retrorun_fx_test COMMAND retrorun_fx_test)
add_test(NAME retrorun_dslink_test COMMAND retrorun_dslink_test)
add_test(NAME retrorun_link_test COMMAND retrorun_link_test)
add_test(NAME retrorun_rollback_test COMMAND retrorun_rollback_test)
add_test(NAME retrorun_rewind_test COMMAND retrorun_rewind_test)   # rewind ring size/limits, rewind->replay hashes, run-ahead == normal shifted (fake core)
add_test(NAME retrorun_rollback_stall_test COMMAND retrorun_rollback_stall_test)
add_test(NAME ps1_synctest COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/qa/ps1_synctest.ps1)   # Beetle PSX rollback SyncTest on homebrew discs (skips without the core / python)
set_tests_properties(ps1_synctest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 ENVIRONMENT "RETROHUB_QA=1")
add_test(NAME ps1_discs COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/qa/ps1_discs.ps1)   # 2-disc homebrew .m3u: libretro disk-control swap + rollback SyncTest with swap events
set_tests_properties(ps1_discs PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600 ENVIRONMENT "RETROHUB_QA=1")
add_test(NAME hubhistory_test COMMAND hubhistory_test)
add_test(NAME hubsaves_test COMMAND hubsaves_test)
add_test(NAME hubsaves_cloud_test COMMAND hubsaves_cloud_test)   # e2e part runs only with RETROHUB_TEST_SAVES_URL (qa\saves_cloud.ps1)
add_test(NAME hubsaves_engine_test COMMAND hubsaves_engine_test)   # cloud saves v2 engine against an in-process mock Worker (loopback only)
add_test(NAME hub_logic_test COMMAND hub_logic_test)
add_test(NAME hub_homefun_test COMMAND hub_homefun_test)               # attract mode idle/slideshow, Surprise-me weighting + reel, Wrapped aggregation
add_test(NAME hub_states_test COMMAND hub_states_test)               # save-state slot listing, resume wording, library sort
add_test(NAME hub_customize_test COMMAND hub_customize_test)   # custom shelves ops, Home composition, renames, rename-aware search
add_test(NAME hub_osk_test COMMAND hub_osk_test)                        # OSK key layout / edit logic / folder browser (scratch dirs)
add_test(NAME hub_session_test COMMAND hub_session_test)               # Smash save migrate, post-exit rules, queued join (scratch dirs only)
add_test(NAME hub_qm_test COMMAND hub_qm_test)
add_test(NAME hub_launchset_test COMMAND hub_launchset_test)
add_test(NAME hub_boot_anim_test COMMAND hub_boot_anim_test)           # start animation: media map, timeline, launch rules, state machine, settings
add_test(NAME real_boot_test COMMAND real_boot_test)                       # real console startup: own IPL/BIOS detection, config edits, custom sound decode
add_test(NAME hub_art_test COMMAND hub_art_test)                       # offline: no --live
add_test(NAME hub_gameinfo_test COMMAND hub_gameinfo_test)   # game info: offline metadata, Wikidata matching on recorded fixtures, cache
add_test(NAME hub_updater_test COMMAND hub_updater_test --offline)     # unit checks only
add_test(NAME hub_apply_test COMMAND hub_apply_test $<TARGET_FILE:RetroHub>)   # scratch dirs only, never relaunches
add_test(NAME hub_shell_test COMMAND hub_shell_test)
add_test(NAME hub_changelog_test COMMAND hub_changelog_test ${CMAKE_SOURCE_DIR}/data/changelog.json)   # patch notes parser + show-once rule
add_test(NAME hub_gamenight_test COMMAND hub_gamenight_test)     # game night plans: DM tag, reminder timing, persistence
add_test(NAME hub_group_test COMMAND hub_group_test)             # recent groups + notification history persistence/expiry
add_test(NAME hub_rivals_test COMMAND hub_rivals_test)         # rivalry result parsing, ranking, storage union, head-to-head
add_test(NAME hub_popup_guard_test COMMAND hub_popup_guard_test)   # never pop up over a game: guard logic + lint of every raise/show/MessageBox site in src/hub
add_test(NAME hub_toast_test COMMAND hub_toast_test)                  # toast XML/activation/policy with a mock notifier (no real toasts)
add_test(NAME hub_wizard_test COMMAND hub_wizard_test)
add_test(NAME hub_cloud_ui_test COMMAND hub_cloud_ui_test)             # cloud saves v2 status/label/dialog logic
add_test(NAME hub_tv_test COMMAND hub_tv_test)   # TV mode scale, hero, Run-key value, clock, avatar plan
add_test(NAME hub_username_test COMMAND hub_username_test)        # username rules, suggestions, availability checker, wizard name step
add_test(NAME hub_toast_win_test COMMAND hub_toast_win_test)               # real WinRT apartment init + XML load, nothing shown
add_test(NAME hub_invite_test COMMAND hub_invite_test)                   # invite parsing + pipe handoff (no network, no windows)
add_test(NAME hub_uisound_test COMMAND hub_uisound_test)   # menu UI sounds + ambient render non-silent within limits; mute policy (not played)
add_test(NAME hub_bootsound_test COMMAND hub_bootsound_test)       # procedural startup sounds render to WAVs (not played)
add_test(NAME hub_gcnp_test COMMAND hub_gcnp_test)                     # GameCube netplay spec/config (no network)
add_test(NAME dsu_test COMMAND dsu_test)   # DSU packets + loopback server round trip
add_test(NAME hub_emu_test COMMAND hub_emu_test)                     # 3DS/Wii U parsers, save mapping, config (no network)
add_test(NAME hub_wiiu_test COMMAND hub_wiiu_test)               # offline Wii U: meta.xml/.wua, own-Cemu settings backup+revert, profiles, saves, archives, art names
add_test(NAME hub_ps2_test COMMAND hub_ps2_test)                   # PS2: images, BIOS, PCSX2.ini, pads, card session (no network)
add_test(NAME hub_np3ds_test COMMAND hub_np3ds_test)
add_test(NAME stream_core_test COMMAND stream_core_test)            # remote play: packetizer, FEC, NACK, jitter buffer, codecs, bitrate
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
                     retrorun_rollback_test retrorun_rollback_stall_test retrorun_dslink_test hub_bootsound_test PROPERTIES ENVIRONMENT "RETROHUB_QA=1")   # QA-only env switches (HUBNET_LOOPBACK, N64RUN_TEST_INPUT)
add_test(NAME hub_updater_live_test COMMAND hub_updater_test)         # checks GitHub releases
set_tests_properties(hubnet_test hubfriends_test hub_updater_live_test PROPERTIES LABELS online)
