add_library(hublogic STATIC logic.cpp netplay_3ds.cpp)
target_include_directories(hublogic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_compile_features(hublogic PUBLIC cxx_std_17)

# Console-only unit test for the pure logic.
add_executable(hub_logic_test test_logic.cpp)
target_link_libraries(hub_logic_test PRIVATE hublogic)

set(RETROHUB_UI_SOURCES
    main.cpp app.cpp util.cpp lib_state.cpp save_jobs.cpp launch.cpp launch_online.cpp lobby.cpp
    ui_common.cpp tiles.cpp shelves.cpp page_library.cpp match_ui.cpp page_online.cpp friends_ui.cpp
    page_controllers.cpp page_saves.cpp page_settings.cpp settings_games_ui.cpp emu_dialogs.cpp page_about.cpp frame.cpp shot.cpp)
add_executable(RetroHub WIN32 ${RETROHUB_UI_SOURCES} ui_kit.cpp art.cpp art_match.cpp)
target_include_directories(RetroHub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../net ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(RetroHub PRIVATE hublogic imgui hubnet hubcommon hubsaves SDL2::SDL2
    ole32 shell32 uuid winhttp)
# main.cpp defines SDL_MAIN_HANDLED and provides WinMain itself, so SDL2main is not needed.
# Optional icon: add hub.rc (with an ICON resource) to the sources above.

add_custom_command(TARGET RetroHub POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:RetroHub>)
if (DEFINED HUB_VERSION)
    target_compile_definitions(RetroHub PRIVATE HUB_VERSION_STR="${HUB_VERSION}")
endif()

# Cover art: pure matching logic + console test (live network check with --live <cache> [rom]).
add_executable(hub_art_test test_art.cpp art.cpp art_match.cpp)
target_include_directories(hub_art_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(hub_art_test PRIVATE hubcommon SDL2::SDL2 winhttp)
add_custom_command(TARGET hub_art_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:hub_art_test>)

# Auto-updater (GitHub releases of fatty8me2/smash-classic). Console test: version/JSON/trust/markdown
# cases + a live release check (never downloads or installs).
target_sources(RetroHub PRIVATE updater.cpp update_apply.cpp)
target_link_libraries(RetroHub PRIVATE bcrypt)
add_executable(hub_updater_test test_updater.cpp updater.cpp update_apply.cpp)
target_include_directories(hub_updater_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(hub_updater_test PRIVATE hubcommon bcrypt shell32)
if (DEFINED HUB_VERSION)
    target_compile_definitions(hub_updater_test PRIVATE HUB_VERSION_STR="${HUB_VERSION}")
endif()

# ---- Branding: icon + version resources, Windows shell integration (assets/brand/retrohub.ico) ----
string(REGEX MATCH "([0-9]+)\.([0-9]+)\.([0-9]+)" _rh_ver "${HUB_VERSION}")
if (NOT _rh_ver)
    set(CMAKE_MATCH_1 0)
    set(CMAKE_MATCH_2 0)
    set(CMAKE_MATCH_3 0)
endif()
set(RH_VER_MAJOR ${CMAKE_MATCH_1})
set(RH_VER_MINOR ${CMAKE_MATCH_2})
set(RH_VER_PATCH ${CMAKE_MATCH_3})
set(RH_VER_STR "${RH_VER_MAJOR}.${RH_VER_MINOR}.${RH_VER_PATCH}.0")
file(WRITE "${CMAKE_BINARY_DIR}/brand_gen/hub_version_rc.h.tmp"
    "#define RH_VER_MAJOR ${RH_VER_MAJOR}\n#define RH_VER_MINOR ${RH_VER_MINOR}\n#define RH_VER_PATCH ${RH_VER_PATCH}\n#define RH_VER_STR \"${RH_VER_STR}\"\n")
configure_file("${CMAKE_BINARY_DIR}/brand_gen/hub_version_rc.h.tmp" "${CMAKE_BINARY_DIR}/brand_gen/hub_version_rc.h" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")
set(RH_BRAND_RC_DIRS ${CMAKE_BINARY_DIR}/brand_gen ${CMAKE_CURRENT_SOURCE_DIR})
set_source_files_properties(retrohub.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/assets/brand/retrohub.ico")
target_sources(RetroHub PRIVATE shell_integration.cpp retrohub.rc)
target_include_directories(RetroHub PRIVATE ${RH_BRAND_RC_DIRS})
target_link_libraries(RetroHub PRIVATE propsys)

add_executable(hub_shell_test test_shell.cpp shell_integration.cpp)
target_link_libraries(hub_shell_test PRIVATE SDL2::SDL2 ole32 shell32 uuid propsys user32)
add_custom_command(TARGET hub_shell_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:hub_shell_test>)

# ---- 3DS (Azahar) / Wii U (Cemu): external emulators, downloaded on demand (docs/EMULATORS.md) ----
target_sources(RetroHub PRIVATE emulators.cpp netplay_3ds_run.cpp)
target_link_libraries(RetroHub PRIVATE ws2_32)
target_link_libraries(RetroHub PRIVATE xinput setupapi)
target_link_libraries(RetroHub PRIVATE hubminiz)
add_executable(hub_emu_test test_emulators.cpp emulators.cpp)
target_include_directories(hub_emu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(hub_emu_test PRIVATE hublogic hubcommon hubminiz bcrypt winhttp xinput setupapi)

# GameCube online via Dolphin NetPlay
target_sources(RetroHub PRIVATE netplay_dolphin.cpp)
add_executable(hub_gcnp_test test_netplay_dolphin.cpp netplay_dolphin.cpp)
add_executable(hub_gcnp_live gcnp_live.cpp netplay_dolphin.cpp netplay_3ds_run.cpp)
target_include_directories(hub_gcnp_live PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../net)
target_link_libraries(hub_gcnp_live PRIVATE hublogic hubcommon hubnet user32 ws2_32 winhttp)

# Live launch driver: runs the 3DS / Wii U launch path without the UI (manual tool, not a ctest)
add_executable(hub_emu_live emu_live.cpp emulators.cpp)
target_include_directories(hub_emu_live PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_SOURCE_DIR}/../saves)
target_link_libraries(hub_emu_live PRIVATE hublogic hubcommon hubsaves hubminiz bcrypt winhttp user32 gdi32 xinput setupapi)

# 3DS local wireless over rooms (np_3ds): pure-logic test (no network, no emulator)
add_executable(hub_np3ds_test test_netplay_3ds.cpp)
target_link_libraries(hub_np3ds_test PRIVATE hublogic hubcommon)

# Native update applier: unit tests + integration dry-run of `RetroHub.exe --apply-update` in scratch dirs.
add_executable(hub_apply_test test_update_apply.cpp update_apply.cpp)
target_link_libraries(hub_apply_test PRIVATE shell32)
add_dependencies(hub_apply_test RetroHub)
# ---- Clickable invite links (retrohub://join/<CODE>, --join <CODE>, single-instance handoff) ----
add_library(hubinvite STATIC invite.cpp)
target_include_directories(hubinvite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hubinvite PUBLIC shell32 user32)
target_compile_features(hubinvite PUBLIC cxx_std_17)
target_link_libraries(RetroHub PRIVATE hubinvite advapi32)
target_link_libraries(hub_shell_test PRIVATE hubinvite advapi32)

add_executable(hub_invite_test test_invite.cpp)
target_link_libraries(hub_invite_test PRIVATE hubinvite)

# Per-game settings launch wiring helpers (pure).
add_executable(hub_launchset_test test_launch_settings.cpp)
target_link_libraries(hub_launchset_test PRIVATE hublogic hubcommon)
