# mgba_link_libretro.dll: N linked GBAs in one deterministic libretro core (mGBA 0.10.5, MPL-2.0, built from source).
# Used by retrorun --link for Pokemon Ruby/Sapphire/Emerald/FireRed/LeafGreen trades and battles (docs/RETRORUN.md).
include(FetchContent)
FetchContent_Declare(mgba
    URL https://github.com/mgba-emu/mgba/archive/refs/tags/0.10.5.tar.gz
    URL_HASH SHA256=91d6fbd32abcbdf030d58d3f562de25ebbc9d56040d513ff8e5c19bee9dacf14
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(mgba)
if(NOT mgba_POPULATED)
    FetchContent_Populate(mgba)
endif()

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(LIBMGBA_ONLY ON CACHE BOOL "" FORCE)
set(M_CORE_GBA ON CACHE BOOL "" FORCE)
set(M_CORE_GB OFF CACHE BOOL "" FORCE)
set(USE_DEBUGGERS OFF CACHE BOOL "" FORCE)
set(USE_GDB_STUB OFF CACHE BOOL "" FORCE)
set(ENABLE_SCRIPTING OFF CACHE BOOL "" FORCE)
set(USE_LUA OFF CACHE BOOL "" FORCE)
set(USE_FFMPEG OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(USE_PNG OFF CACHE BOOL "" FORCE)
set(USE_LIBZIP OFF CACHE BOOL "" FORCE)
set(USE_MINIZIP OFF CACHE BOOL "" FORCE)
set(USE_SQLITE3 OFF CACHE BOOL "" FORCE)
set(USE_ELF OFF CACHE BOOL "" FORCE)
set(USE_LZMA OFF CACHE BOOL "" FORCE)
set(USE_EDITLINE OFF CACHE BOOL "" FORCE)
set(USE_DISCORD_RPC OFF CACHE BOOL "" FORCE)
set(BUILD_QT OFF CACHE BOOL "" FORCE)
set(BUILD_SDL OFF CACHE BOOL "" FORCE)
set(BUILD_GL OFF CACHE BOOL "" FORCE)
set(BUILD_GLES2 OFF CACHE BOOL "" FORCE)
set(BUILD_GLES3 OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)

# 565 video like mGBA's own libretro build; the flags apply to mGBA and to our wrapper alike.
add_compile_definitions(COLOR_16_BIT COLOR_5_6_5)
add_subdirectory(${mgba_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/mgba EXCLUDE_FROM_ALL)

add_library(mgba_link_libretro SHARED mgba_link.c)
target_include_directories(mgba_link_libretro PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../retrorun/third_party
    ${mgba_SOURCE_DIR}/include ${CMAKE_CURRENT_BINARY_DIR}/mgba/include)
target_link_libraries(mgba_link_libretro PRIVATE mgba)
set_target_properties(mgba_link_libretro PROPERTIES PREFIX "")
add_dependencies(retrorun mgba_link_libretro)   # ship mgba_link_libretro.dll next to the other cores (cores\ folder)
# Dev layout: <build>\bin\<cfg>\cores\ (find_core looks in <exe_dir>\cores\ first). Packaged: copied by package-windows.ps1.
add_custom_command(TARGET mgba_link_libretro POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:retrorun>/cores
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:mgba_link_libretro> $<TARGET_FILE_DIR:retrorun>/cores/$<TARGET_FILE_NAME:mgba_link_libretro>)
