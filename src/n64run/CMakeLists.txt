# n64run: mupen64plus frontend + in-exe input plugin + lockstep netplay.

# Emulator-independent, network-independent lockstep scheduling (unit tested).
add_library(n64run_lockstep_core STATIC lockstep_core.cpp)
target_include_directories(n64run_lockstep_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(n64run_lockstep_core PUBLIC cxx_std_17)

add_executable(n64run
    frontend.cpp
    input_plugin.cpp
    local_input.cpp
    lockstep.cpp)
target_link_libraries(n64run PRIVATE n64run_lockstep_core hubnet hubcommon m64p_api SDL2::SDL2)
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

# Branding: same icon + version resources as RetroHub.exe (see src/hub/CMakeLists.txt for the generated header).
target_sources(n64run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../hub/n64run.rc)
target_include_directories(n64run PRIVATE ${CMAKE_BINARY_DIR}/brand_gen ${CMAKE_CURRENT_SOURCE_DIR}/../hub)
