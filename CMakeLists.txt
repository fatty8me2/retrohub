cmake_minimum_required(VERSION 3.24)
project(ClassicHub LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Classic Hub is x64-only")
endif()

file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" HUB_VERSION LIMIT_COUNT 1)
string(STRIP "${HUB_VERSION}" HUB_VERSION)

if (MSVC)
    add_compile_options(/W3 /permissive- /utf-8 /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# SDL2 (window, input, gamepads) -- shared, shipped next to the exes (the
# mupen64plus plugins load SDL2.dll too, so everything shares one copy).
FetchContent_Declare(SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2)

# Dear ImGui (hub UI)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.5.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL2::SDL2)

# mupen64plus-core API headers only (the core itself ships as the official
# 2.6.0 win64 DLL, fetched by scripts/fetch-emu.ps1)
FetchContent_Declare(m64p_core
    URL https://github.com/mupen64plus/mupen64plus-core/archive/refs/tags/2.6.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Populate(m64p_core)
add_library(m64p_api INTERFACE)
target_include_directories(m64p_api INTERFACE ${m64p_core_SOURCE_DIR}/src/api)

add_subdirectory(src/common)
add_subdirectory(src/net)
add_subdirectory(src/n64run)
add_subdirectory(src/hub)
