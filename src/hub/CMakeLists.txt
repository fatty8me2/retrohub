add_library(hublogic STATIC changelog.cpp osk_logic.cpp logic.cpp toast.cpp netplay_3ds.cpp smash_save.cpp state_slots.cpp)
target_link_libraries(hublogic PUBLIC hubbase)
target_include_directories(hublogic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_SOURCE_DIR}/../saves)
target_compile_features(hublogic PUBLIC cxx_std_17)

# Console-only unit test for the pure logic.
add_executable(hub_logic_test test_logic.cpp)
target_link_libraries(hub_logic_test PRIVATE hublogic)

add_executable(hub_states_test test_state_slots.cpp)
target_link_libraries(hub_states_test PRIVATE hublogic)

add_executable(hub_online_caps_test test_online_caps.cpp)
target_link_libraries(hub_online_caps_test PRIVATE hublogic hubcommon)
add_executable(hub_picker_test test_picker.cpp)
target_include_directories(hub_picker_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(hub_friend_games_test test_friend_games.cpp)
target_include_directories(hub_friend_games_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../net)
add_executable(hub_boot_art_test test_boot_art.cpp boot_art.cpp)
target_include_directories(hub_boot_art_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(hub_boot_art_test PRIVATE imgui)
target_compile_features(hub_boot_art_test PRIVATE cxx_std_17)

add_executable(hub_changelog_test test_changelog.cpp)
target_link_libraries(hub_changelog_test PRIVATE hublogic)

# Session-level rules: Smash save one-time copy (scratch dirs), post-exit touch/sync, queued friend join.
add_executable(hub_osk_test test_osk.cpp)
target_link_libraries(hub_osk_test PRIVATE hublogic)

add_executable(hub_username_test test_username.cpp)
target_link_libraries(hub_username_test PRIVATE hublogic)
target_include_directories(hub_username_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../net)

add_executable(hub_session_test test_session.cpp)
target_link_libraries(hub_session_test PRIVATE hublogic)

# Release version: hub_version.cpp (kHubVersion -> HUB_VERSION_STR) and hub_version_rc.h (RH_VER_*) are generated AT BUILD TIME
# from VERSION (cmake/gen_hub_version.cmake, writes a file only if its text changed). A VERSION bump therefore needs no
# reconfigure and recompiles just hub_version.cpp + retrohub.rc; it is not a compile definition on any target.
set(HUB_VERSION_GEN ${CMAKE_BINARY_DIR}/version_gen)
add_custom_command(OUTPUT ${HUB_VERSION_GEN}/hub_version.cpp ${HUB_VERSION_GEN}/hub_version_rc.h
    COMMAND ${CMAKE_COMMAND} -DVERSION_FILE=${CMAKE_SOURCE_DIR}/VERSION -DOUT_DIR=${HUB_VERSION_GEN}
            -P ${CMAKE_SOURCE_DIR}/cmake/gen_hub_version.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/VERSION ${CMAKE_SOURCE_DIR}/cmake/gen_hub_version.cmake
    COMMENT "Generating hub version files" VERBATIM)
add_library(hubversion STATIC ${HUB_VERSION_GEN}/hub_version.cpp ${HUB_VERSION_GEN}/hub_version_rc.h)
set(RETROHUB_UI_SOURCES
    main.cpp app.cpp util.cpp lib_state.cpp save_jobs.cpp launch.cpp launch_online.cpp lobby.cpp
    ui_common.cpp username_ui.cpp kanto_ui.cpp tiles.cpp shelves.cpp page_library.cpp states_ui.cpp match_ui.cpp page_online.cpp picker_ui.cpp friends_ui.cpp friend_games_ui.cpp group_ui.cpp
    page_controllers.cpp page_saves.cpp cloud_ui.cpp history_ui.cpp page_settings.cpp account_ui.cpp settings_games_ui.cpp emu_dialogs.cpp ps1_ui.cpp find_games_ui.cpp add_games_ui.cpp page_about.cpp page_changelog.cpp cheats_ui.cpp gameinfo_ui.cpp quickmenu.cpp shot_viewer.cpp osk.cpp onboarding.cpp frame.cpp shot.cpp autotest.cpp userflow.cpp boot_art_shot.cpp)
add_executable(RetroHub WIN32 ${RETROHUB_UI_SOURCES} ui_kit.cpp art.cpp art_match.cpp boot_anim.cpp boot_flow.cpp boot_art.cpp boot_sound.cpp boot_synth.cpp ui_sound.cpp real_boot_ui.cpp)
target_include_directories(RetroHub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../net ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(RetroHub PRIVATE hubversion hubgameinfo_net hublogic imgui hubnet hubcommon hubcontrols_sdl hubsaves SDL2::SDL2
    ole32 shell32 uuid winhttp)
# main.cpp defines SDL_MAIN_HANDLED and provides WinMain itself, so SDL2main is not needed.

add_custom_command(TARGET RetroHub POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:RetroHub>)

# Cheat database (data/cheats_db.bin + data/cheats_featured.json) ships next to RetroHub.exe in data\ (hub::cheats_db_set_dir default).
add_custom_command(TARGET RetroHub POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:RetroHub>/data
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/data/cheats_db.bin ${CMAKE_SOURCE_DIR}/data/cheats_featured.json ${CMAKE_SOURCE_DIR}/data/changelog.json ${CMAKE_SOURCE_DIR}/data/gameinfo.tsv $<TARGET_FILE_DIR:RetroHub>/data)

# Cover art: pure matching logic + console test (live network check with --live <cache> [rom]).
add_executable(hub_art_test test_art.cpp art.cpp art_match.cpp)
target_include_directories(hub_art_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(hub_art_test PRIVATE hubcommon SDL2::SDL2 winhttp)
add_custom_command(TARGET hub_art_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:hub_art_test>)

# Auto-updater (GitHub releases of fatty8me2/retrohub). Console test: version/JSON/trust/markdown
# cases + a live release check (never downloads or installs).
target_sources(RetroHub PRIVATE updater.cpp update_apply.cpp)
target_link_libraries(RetroHub PRIVATE bcrypt)
add_executable(hub_updater_test test_updater.cpp updater.cpp update_apply.cpp)
target_include_directories(hub_updater_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(hub_updater_test PRIVATE hubcommon bcrypt shell32 ws2_32)
target_link_libraries(hub_updater_test PRIVATE hubversion)

# ---- Branding: icon + version resources, Windows shell integration (assets/brand/retrohub.ico) ----
set(RH_BRAND_RC_DIRS ${HUB_VERSION_GEN} ${CMAKE_CURRENT_SOURCE_DIR})
set_source_files_properties(retrohub.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/assets/brand/retrohub.ico;${HUB_VERSION_GEN}/hub_version_rc.h")
target_sources(RetroHub PRIVATE shell_integration.cpp toast_win.cpp retrohub.rc)
target_include_directories(RetroHub PRIVATE ${RH_BRAND_RC_DIRS})
target_link_libraries(RetroHub PRIVATE propsys windowsapp)

add_executable(hub_cloud_ui_test test_cloud_ui.cpp)
target_link_libraries(hub_cloud_ui_test PRIVATE hublogic)

add_executable(hub_wizard_test test_wizard.cpp)
target_link_libraries(hub_wizard_test PRIVATE hublogic)
target_include_directories(hub_wizard_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../net)

add_executable(hub_group_test test_group.cpp)
target_link_libraries(hub_group_test PRIVATE hublogic)

add_executable(hub_toast_test test_toast.cpp)
target_link_libraries(hub_toast_test PRIVATE hublogic)

add_executable(hub_toast_win_test test_toast_win.cpp toast_win.cpp shell_integration.cpp)
target_link_libraries(hub_toast_win_test PRIVATE hublogic hubbase SDL2::SDL2 ole32 shell32 uuid propsys user32 windowsapp)
add_custom_command(TARGET hub_toast_win_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:hub_toast_win_test>)

add_executable(hub_shell_test test_shell.cpp shell_integration.cpp)
target_link_libraries(hub_shell_test PRIVATE hubbase SDL2::SDL2 ole32 shell32 uuid propsys user32)
add_custom_command(TARGET hub_shell_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:hub_shell_test>)

# ---- 3DS (Azahar) / Wii U (Cemu): external emulators, downloaded on demand (docs/EMULATORS.md) ----
target_sources(RetroHub PRIVATE emulators.cpp netplay_3ds_run.cpp netplay_3ds_join.cpp)
target_link_libraries(RetroHub PRIVATE ws2_32)
target_link_libraries(RetroHub PRIVATE xinput setupapi)
target_link_libraries(RetroHub PRIVATE hubminiz)
add_executable(hub_emu_test test_emulators.cpp emulators.cpp)
target_include_directories(hub_emu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(hub_emu_test PRIVATE hublogic hubcommon hubminiz bcrypt winhttp xinput setupapi)
add_executable(hub_wiiu_test test_wiiu_offline.cpp emulators.cpp art_match.cpp)   # offline Wii U: .wua/meta.xml, own-Cemu settings, profiles, saves, art names
target_include_directories(hub_wiiu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common ${libchdr_SOURCE_DIR}/deps/zstd-1.5.7)
target_link_libraries(hub_wiiu_test PRIVATE hublogic hubcommon hubminiz bcrypt winhttp xinput setupapi)

# GameCube online via Dolphin NetPlay
target_sources(RetroHub PRIVATE netplay_dolphin.cpp)
add_executable(hub_gcnp_test test_netplay_dolphin.cpp netplay_dolphin.cpp)
target_link_libraries(hub_gcnp_test PRIVATE hubbase)
add_executable(hub_gcnp_live gcnp_live.cpp netplay_dolphin.cpp netplay_3ds_run.cpp)
target_include_directories(hub_gcnp_live PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../net)
target_link_libraries(hub_gcnp_live PRIVATE hublogic hubcommon hubnet user32 ws2_32 winhttp)
add_executable(hub_n3ds_live n3ds_live.cpp netplay_3ds_run.cpp netplay_3ds_join.cpp)
target_include_directories(hub_n3ds_live PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../net)
target_link_libraries(hub_n3ds_live PRIVATE hublogic hubcommon hubnet user32 ws2_32 winhttp)

# Live launch driver: runs the 3DS / Wii U launch path without the UI (manual tool, not a ctest)
add_executable(hub_emu_live emu_live.cpp emulators.cpp)
target_include_directories(hub_emu_live PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_SOURCE_DIR}/../saves)
target_link_libraries(hub_emu_live PRIVATE hublogic hubcommon hubsaves hubminiz bcrypt winhttp user32 gdi32 xinput setupapi)

# 3DS local wireless over rooms (np_3ds): pure-logic test (no network, no emulator)
add_executable(hub_np3ds_test test_netplay_3ds.cpp netplay_3ds_join.cpp)
target_link_libraries(hub_np3ds_test PRIVATE hublogic hubcommon)

# Native update applier: unit tests + integration dry-run of `RetroHub.exe --apply-update` in scratch dirs.
add_executable(hub_apply_test test_update_apply.cpp update_apply.cpp)
target_link_libraries(hub_apply_test PRIVATE hubbase shell32)
add_dependencies(hub_apply_test RetroHub)
# ---- Clickable invite links (retrohub://join/<CODE>, --join <CODE>, single-instance handoff) ----
add_library(hubinvite STATIC invite.cpp)
target_include_directories(hubinvite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hubinvite PUBLIC hubbase shell32 user32 advapi32)
target_compile_features(hubinvite PUBLIC cxx_std_17)
target_link_libraries(RetroHub PRIVATE hubinvite advapi32)
target_link_libraries(hub_shell_test PRIVATE hubinvite advapi32)
target_link_libraries(hub_toast_win_test PRIVATE hubinvite advapi32)

add_executable(hub_invite_test test_invite.cpp)
target_link_libraries(hub_invite_test PRIVATE hubinvite)

# Per-game settings launch wiring helpers (pure).
add_executable(hub_launchset_test test_launch_settings.cpp)
target_link_libraries(hub_launchset_test PRIVATE hublogic hubcommon)

# Manual live driver for Kanto Together (hidden desktop, local relay). Not a ctest.
add_executable(kanto_live kanto_live.cpp)
target_link_libraries(kanto_live PRIVATE hubcommon hubminiz)

# PlayStation 2 via PCSX2 (docs/PS2.md): BIOS discovery, PCSX2.ini writers, pad bindings, card session; library scan of PS2 images
target_sources(RetroHub PRIVATE pcsx2.cpp)
add_executable(hub_ps2_test test_pcsx2.cpp pcsx2.cpp emulators.cpp)
target_include_directories(hub_ps2_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(hub_ps2_test PRIVATE hublogic hubcommon hubminiz bcrypt winhttp xinput setupapi)

# PS2 online (docs/PS2_ONLINE.md): readiness reasons, retrorun arguments, exit-code texts, the pinned LRPS2 download
add_executable(hub_ps2_online_test test_ps2_online.cpp emulators.cpp)
target_include_directories(hub_ps2_online_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(hub_ps2_online_test PRIVATE hublogic hubcommon hubminiz bcrypt winhttp xinput setupapi)

# Game info (details modal): WinHTTP + fixture layer, the fixture recorder (live network, never run by tests) and the offline test.
add_library(hubgameinfo_net STATIC gameinfo_net.cpp)
target_include_directories(hubgameinfo_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hubgameinfo_net PUBLIC hubcommon winhttp)
target_link_libraries(hubgameinfo_net PUBLIC hubversion)
add_executable(gameinfo_record gameinfo_record.cpp)
target_link_libraries(gameinfo_record PRIVATE hubgameinfo_net)
add_executable(hub_gameinfo_test test_gameinfo.cpp)
target_link_libraries(hub_gameinfo_test PRIVATE hubgameinfo_net)
target_compile_definitions(hub_gameinfo_test PRIVATE GAMEINFO_FIXTURES="${CMAKE_SOURCE_DIR}/src/hub/testdata/gameinfo" GAMEINFO_DB="${CMAKE_SOURCE_DIR}/data/gameinfo.tsv")
add_executable(hub_boot_anim_test test_boot_anim.cpp)
target_link_libraries(hub_boot_anim_test PRIVATE hublogic)
# Startup sounds: original procedural synth (docs/BOOT_SOUNDS.md). Test renders to memory + qa/out/boot_sounds/*.wav (never played).
add_executable(hub_uisound_test test_ui_sound.cpp boot_synth.cpp)
target_include_directories(hub_uisound_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(hub_uisound_test PRIVATE hubcommon)
add_executable(hub_bootsound_test test_boot_sound.cpp boot_synth.cpp)
target_include_directories(hub_bootsound_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(hub_bootsound_test PRIVATE hubcommon)
target_compile_definitions(hub_bootsound_test PRIVATE BOOT_SOUND_OUT_DIR="${CMAKE_SOURCE_DIR}/qa/out/boot_sounds")
target_include_directories(RetroHub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)   # boot_anim.h includes "common/library.h"
