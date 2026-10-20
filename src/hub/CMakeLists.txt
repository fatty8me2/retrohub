add_library(hublogic STATIC logic.cpp)
target_include_directories(hublogic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Console-only unit test for the pure logic.
add_executable(hub_logic_test test_logic.cpp)
target_link_libraries(hub_logic_test PRIVATE hublogic)

add_executable(ClassicHub WIN32 main.cpp)
target_include_directories(ClassicHub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../net ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(ClassicHub PRIVATE hublogic imgui hubnet hubcommon hubsaves SDL2::SDL2
    ole32 shell32 uuid winhttp)
# main.cpp defines SDL_MAIN_HANDLED and provides WinMain itself, so SDL2main is not needed.
# Optional icon: add hub.rc (with an ICON resource) to the sources above.

add_custom_command(TARGET ClassicHub POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:ClassicHub>)
if (DEFINED HUB_VERSION)
    target_compile_definitions(ClassicHub PRIVATE HUB_VERSION_STR="${HUB_VERSION}")
endif()
