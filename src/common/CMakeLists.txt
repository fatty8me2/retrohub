add_library(hubcommon STATIC game_settings.cpp games.cpp library.cpp emu_formats.cpp emu_saves.cpp)
target_include_directories(hubcommon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hubcommon PUBLIC cxx_std_17)
target_link_libraries(hubcommon PRIVATE bcrypt hubminiz)   # hubminiz: defined in src/saves (zipped ROMs)

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
