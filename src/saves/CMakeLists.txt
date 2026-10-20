# Save vault: portable per-game saves, two-way folder sync, zip export/import.
include(FetchContent)
FetchContent_Declare(miniz
    URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(miniz)
if (NOT miniz_POPULATED)
    FetchContent_Populate(miniz)
endif()

# miniz (MIT/unlicense) amalgamation: one .c + one .h
add_library(hubminiz STATIC ${miniz_SOURCE_DIR}/miniz.c)
target_include_directories(hubminiz PUBLIC ${miniz_SOURCE_DIR})
set_target_properties(hubminiz PROPERTIES LINKER_LANGUAGE C)
if (MSVC)
    target_compile_options(hubminiz PRIVATE /W0)
endif()

add_library(hubsaves STATIC savestore.cpp)
target_include_directories(hubsaves PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hubsaves PUBLIC cxx_std_17)
target_link_libraries(hubsaves PUBLIC hubcommon PRIVATE hubminiz bcrypt)

# Console-only test (no WIN32 flag).
add_executable(hubsaves_test test_saves.cpp)
target_link_libraries(hubsaves_test PRIVATE hubsaves)

# QA helper for qa\saves_two_pc.ps1 (key/dirs/sync from the command line). Console only.
add_executable(qa_saves_tool qa_tool.cpp)
target_link_libraries(qa_saves_tool PRIVATE hubsaves hubcommon)
