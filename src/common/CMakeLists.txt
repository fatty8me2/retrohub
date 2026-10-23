add_library(hubcommon STATIC controls.cpp games.cpp library.cpp emu_formats.cpp emu_saves.cpp)
target_include_directories(hubcommon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hubcommon PUBLIC cxx_std_17)
target_link_libraries(hubcommon PRIVATE bcrypt hubminiz)   # hubminiz: defined in src/saves (zipped ROMs)

# Console-only unit test (no WIN32 flag, so it stays a console app).
add_executable(hubcommon_test test_games.cpp)
target_link_libraries(hubcommon_test PRIVATE hubcommon)

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
