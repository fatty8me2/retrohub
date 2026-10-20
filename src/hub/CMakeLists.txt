add_library(hublogic STATIC logic.cpp)
target_include_directories(hublogic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Console-only unit test for the pure logic.
add_executable(hub_logic_test test_logic.cpp)
target_link_libraries(hub_logic_test PRIVATE hublogic)

add_executable(RetroHub WIN32 main.cpp ui_kit.cpp art.cpp art_match.cpp)
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
target_sources(RetroHub PRIVATE updater.cpp)
target_link_libraries(RetroHub PRIVATE bcrypt)
add_executable(hub_updater_test test_updater.cpp updater.cpp)
target_include_directories(hub_updater_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(hub_updater_test PRIVATE hubcommon bcrypt shell32)
if (DEFINED HUB_VERSION)
    target_compile_definitions(hub_updater_test PRIVATE HUB_VERSION_STR="${HUB_VERSION}")
endif()
