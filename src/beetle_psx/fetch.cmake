# Included from the top-level CMakeLists.txt.
include(FetchContent)
FetchContent_Declare(beetle_psx
    URL https://github.com/libretro/beetle-psx-libretro/archive/ea14d6a877f89d3d4525c8426e63ac6b02405ad7.tar.gz
    URL_HASH SHA256=33574f23d3589422280b43cc426708ad58fa176449e4ca976e4108e6b1a91d08
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(beetle_psx)
if(NOT beetle_psx_POPULATED)
    FetchContent_Populate(beetle_psx)
endif()
set(BEETLE_SRC ${beetle_psx_SOURCE_DIR})

# ---- serial database -> C++ (used by hubcommon's library identification) ----
set(PS1_DB_CPP ${CMAKE_BINARY_DIR}/ps1_db_gen/ps1_db_data.cpp)
if(NOT EXISTS ${PS1_DB_CPP} OR ${BEETLE_SRC}/database/psx_games.inc IS_NEWER_THAN ${PS1_DB_CPP})
    include(${CMAKE_SOURCE_DIR}/src/beetle_psx/gen_ps1_db.cmake)
    ps1_db_generate(${BEETLE_SRC}/database/psx_games.inc ${PS1_DB_CPP})
endif()

