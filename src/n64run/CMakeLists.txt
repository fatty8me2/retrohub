# n64run: mupen64plus frontend + in-exe input plugin + lockstep netplay.

# Emulator-independent, network-independent lockstep scheduling (unit tested).
add_library(n64run_lockstep_core STATIC lockstep_core.cpp)
target_include_directories(n64run_lockstep_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(n64run_lockstep_core PUBLIC cxx_std_17)

# Controller Pak policy (the save-sync protocol itself lives in src/net, library hubsavesync).
add_library(n64run_save_sync STATIC mempak.cpp)
target_include_directories(n64run_save_sync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(n64run_save_sync PUBLIC cxx_std_17)
target_link_libraries(n64run_save_sync PUBLIC hubsavesync hubnet)

# --match VS-battle jump driver (pure RDRAM state machine, unit tested).
add_library(n64run_match STATIC match.cpp)
target_include_directories(n64run_match PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(n64run_match PUBLIC cxx_std_17)
target_link_libraries(n64run_match PUBLIC hubcommon)

add_executable(n64run
    frontend.cpp
    input_plugin.cpp
    local_input.cpp
    test_input.cpp
    lockstep.cpp
    headless.cpp)
target_link_libraries(n64run PRIVATE n64run_match n64run_lockstep_core n64run_save_sync hubnet hubcommon hubcontrols_sdl hubclip m64p_api SDL2::SDL2 opengl32)
# Console subsystem on purpose (logs to the console too); the hub launches it hidden/with a console as needed.
set_target_properties(n64run PROPERTIES WIN32_EXECUTABLE OFF)
# The input plugin entry points are __declspec(dllexport) from the exe; keep them even with /OPT:REF.
target_link_options(n64run PRIVATE /OPT:NOREF)

# SDL2.dll next to the exe (shared by the exe and the mupen64plus plugins).
add_custom_command(TARGET n64run POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:n64run>)

# Console unit test: lockstep over a simulated lossy network + BUTTONS layout check.
add_executable(n64run_lockstep_test test_lockstep.cpp)
target_link_libraries(n64run_lockstep_test PRIVATE n64run_lockstep_core m64p_api)


# Console test: Controller Pak policy and .mpk naming (pure logic).
add_executable(n64run_mempak_test test_mempak.cpp)
target_link_libraries(n64run_mempak_test PRIVATE n64run_save_sync)

# Branding: same icon + version resources as RetroHub.exe (see src/hub/CMakeLists.txt for the generated header).
target_sources(n64run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../hub/n64run.rc)
add_dependencies(n64run hubversion)   # generates version_gen/hub_version_rc.h (src/hub)
target_include_directories(n64run PRIVATE ${CMAKE_BINARY_DIR}/version_gen ${CMAKE_CURRENT_SOURCE_DIR}/../hub)

add_executable(n64run_match_test test_match.cpp)
target_link_libraries(n64run_match_test PRIVATE n64run_match)

add_executable(n64run_testinput_test test_testinput.cpp test_input.cpp)
target_include_directories(n64run_testinput_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(n64run_testinput_test PRIVATE cxx_std_17)

# SDL_GameControllerDB (zlib, see THIRD_PARTY.md) next to the exe: <exe>/controls/gamecontrollerdb.txt
add_custom_command(TARGET n64run POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/assets/controls/gamecontrollerdb.txt $<TARGET_FILE_DIR:n64run>/controls/gamecontrollerdb.txt)

# --gs-code GameShark RAM writer (QA).
add_executable(n64run_gs_cheat_test test_gs_cheat.cpp)
target_compile_features(n64run_gs_cheat_test PRIVATE cxx_std_17)

# Mid-match guest drop: cutoff agreement + LockstepCore neutralisation.
add_executable(n64run_dropagree_test test_dropagree.cpp)
target_link_libraries(n64run_dropagree_test PRIVATE n64run_lockstep_core)
