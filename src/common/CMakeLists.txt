# Shared JSON reader/writer + Windows helpers (UTF-8/UTF-16, exe dir, quoting, file I/O). Tiny static lib, no deps.
add_library(hubbase STATIC json.cpp winutil.cpp cheats.cpp runner_ctl.cpp start_hint.cpp hint_popup.cpp)
target_link_libraries(hubbase PUBLIC user32 gdi32)
target_include_directories(hubbase PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hubbase PUBLIC cxx_std_17)

add_executable(json_test test_json.cpp)
target_link_libraries(json_test PRIVATE hubbase)
add_executable(winutil_test test_winutil.cpp)
target_link_libraries(winutil_test PRIVATE hubbase shell32)

add_library(hubcommon STATIC ps1.cpp ${PS1_DB_CPP} disc_id.cpp ps2.cpp ps2_db.cpp kanto.cpp kanto_pad.cpp controls.cpp game_settings.cpp games.cpp library.cpp emu_formats.cpp emu_saves.cpp wiiu_meta.cpp findgames.cpp addgames.cpp cheats.cpp cheats_db.cpp cheat_store.cpp dolphin_cheats.cpp gameinfo.cpp)
target_include_directories(hubcommon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(hubcommon PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hubcommon PUBLIC cxx_std_17)
target_link_libraries(hubcommon PUBLIC hubbase ws2_32 PRIVATE bcrypt hubminiz chdr-static zstd)   # hubminiz: defined in src/saves (zipped ROMs)
target_include_directories(hubcommon PRIVATE ${libchdr_SOURCE_DIR}/deps/zstd-1.5.7)   # wiiu_meta.cpp: .wua (ZArchive) blocks are zstd

# Console-only unit test (no WIN32 flag, so it stays a console app).
add_executable(hubcommon_test test_games.cpp)
target_link_libraries(hubcommon_test PRIVATE hubcommon)

# Per-game settings: defaults/merge/serialization/online filtering/emitters.
add_executable(hubgamesettings_test test_game_settings.cpp)
target_link_libraries(hubgamesettings_test PRIVATE hubcommon)

# Library (multi-system identification, scan, cache, state) test. Optional argv[1]: a real
# folder to scan and print.
add_executable(hublibrary_test test_library.cpp)
target_link_libraries(hublibrary_test PRIVATE hubcommon hubminiz)

# SDL half of the controller profiles (pad list, capture, gamecontrollerdb); the runners and the Controllers page link this.
add_library(hubcontrols_sdl STATIC controls_sdl.cpp)
target_include_directories(hubcontrols_sdl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hubcontrols_sdl PUBLIC hubcommon SDL2::SDL2)

# Profile model, defaults-vs-legacy equivalence, storage, generated emulator configs' inputs.
add_executable(hubcontrols_test test_controls.cpp)
target_include_directories(hubcontrols_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../n64run ${CMAKE_CURRENT_SOURCE_DIR}/../retrorun)
target_link_libraries(hubcontrols_test PRIVATE hubcommon)

# NES / SNES: header detection, scan, cheat formats, profiles, settings.
add_executable(nes_snes_test test_nes_snes.cpp)
target_link_libraries(nes_snes_test PRIVATE hubcommon hubminiz)

# "Find my games": folder ranking / collapsing, emulator config parsers, bounded scan of a scratch tree.
add_executable(findgames_test test_findgames.cpp)
target_link_libraries(findgames_test PRIVATE hubcommon)

# Kanto Together (friend's FireRed co-op hack): package detection/install, ROM pick, launcher output parsing (scratch dirs only).
add_executable(hubkantopad_test test_kanto_pad.cpp)
target_link_libraries(hubkantopad_test PRIVATE hubcommon hubminiz)
add_executable(hubkanto_test test_kanto.cpp)
target_link_libraries(hubkanto_test PRIVATE hubcommon hubminiz)

# PlayStation 1: serials, catalog, synthetic discs (cue/bin, m3u, iso), PS1-vs-PS2, BIOS discovery, DualShock profile.
add_executable(ps1_test test_ps1.cpp)
target_link_libraries(ps1_test PRIVATE hubcommon)

# PlayStation disc archives (.7z/.rar): name/title-DB identification, truncated archives, extraction + SYSTEM.CNF confirm.
add_executable(psarchive_test test_psarchive.cpp)
target_link_libraries(psarchive_test PRIVATE hubcommon)

# "Add games": sorting / destination / dedupe / copy job (scratch dirs only).
add_executable(addgames_test test_addgames.cpp)
target_link_libraries(addgames_test PRIVATE hubcommon)

# Cheat choices per game (vault), custom codes, online rules, save backup/restore (scratch dirs only).
add_executable(hubcheatstore_test test_cheat_store.cpp)
target_link_libraries(hubcheatstore_test PRIVATE hubcommon)

# Cheat database (libretro cht bundle + curated featured list): matching, regions, ranking, the user's real ROM names.
add_executable(cheats_db_test test_cheats_db.cpp)
target_link_libraries(cheats_db_test PRIVATE hubcommon hubminiz)
target_compile_definitions(cheats_db_test PRIVATE CHEATS_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
# GameCube cheats: gecko / ar_gc validation, Dolphin GameSettings ini patching (user codes kept), EnableCheats, file writer.
add_executable(hubdolphincheats_test test_dolphin_cheats.cpp)
target_link_libraries(hubdolphincheats_test PRIVATE hubcommon)
# Cheat model: JSON, per-format code validation (N64 mupen codes, libretro strings), netplay hash.
add_executable(cheats_test test_cheats.cpp)
target_link_libraries(cheats_test PRIVATE hubbase)

add_executable(runner_ctl_test test_runner_ctl.cpp)
target_link_libraries(runner_ctl_test PRIVATE hubbase)

# Game-start controller tips: timing, row text, rendered card (pure logic + GDI rasteriser).
add_executable(starthint_test test_start_hint.cpp)
target_link_libraries(starthint_test PRIVATE hubbase)

# Popup behaviour on a private desktop: no-activate, click-through, foreground unchanged, auto-closes (~6 s).
add_executable(hint_popup_test test_hint_popup.cpp)
target_link_libraries(hint_popup_test PRIVATE hubbase)
