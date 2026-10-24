# Shared JSON reader/writer + Windows helpers (UTF-8/UTF-16, exe dir, quoting, file I/O). Tiny static lib, no deps.
add_library(hubbase STATIC json.cpp winutil.cpp)
target_include_directories(hubbase PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hubbase PUBLIC cxx_std_17)

add_executable(json_test test_json.cpp)
target_link_libraries(json_test PRIVATE hubbase)
add_executable(winutil_test test_winutil.cpp)
target_link_libraries(winutil_test PRIVATE hubbase shell32)

add_library(hubcommon STATIC kanto.cpp controls.cpp game_settings.cpp games.cpp library.cpp emu_formats.cpp emu_saves.cpp findgames.cpp addgames.cpp)
target_include_directories(hubcommon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hubcommon PUBLIC cxx_std_17)
target_link_libraries(hubcommon PUBLIC hubbase ws2_32 PRIVATE bcrypt hubminiz)   # hubminiz: defined in src/saves (zipped ROMs)

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

# "Find my games": folder ranking / collapsing, emulator config parsers, bounded scan of a scratch tree.
add_executable(findgames_test test_findgames.cpp)
target_link_libraries(findgames_test PRIVATE hubcommon)

# Kanto Together (friend's FireRed co-op hack): package detection/install, ROM pick, launcher output parsing (scratch dirs only).
add_executable(hubkanto_test test_kanto.cpp)
target_link_libraries(hubkanto_test PRIVATE hubcommon hubminiz)

# "Add games": sorting / destination / dedupe / copy job (scratch dirs only).
add_executable(addgames_test test_addgames.cpp)
target_link_libraries(addgames_test PRIVATE hubcommon)
