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

add_library(hubsaves STATIC savestore.cpp cloud.cpp cloud_classify.cpp cloud_wire.cpp cloud_queue.cpp cloud_engine.cpp history.cpp playstats.cpp)
target_include_directories(hubsaves PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hubsaves PUBLIC cxx_std_17)
target_link_libraries(hubsaves PUBLIC hubcommon PRIVATE hubnet hubminiz bcrypt winhttp)

# Console-only test (no WIN32 flag).
add_executable(hubsaves_test test_saves.cpp)
target_link_libraries(hubsaves_test PRIVATE hubsaves)

# QA helper for qa\saves_two_pc.ps1 (key/dirs/sync from the command line). Console only.
add_executable(qa_saves_tool qa_tool.cpp)
target_link_libraries(qa_saves_tool PRIVATE hubsaves hubcommon)

# Cloud saves test: unit checks always; end-to-end against a local `wrangler dev` when RETROHUB_TEST_SAVES_URL +
# RETROHUB_TEST_SAVES_ACCOUNTS are set (qa\saves_cloud.ps1). Skips (passes) otherwise.
add_executable(hubsaves_cloud_test test_cloud.cpp)
target_link_libraries(hubsaves_cloud_test PRIVATE hubsaves)

# Cloud saves v2 engine: contract fixture (mock Worker over loopback HTTP), unit tests, standalone mock server and QA tool
# for qa\cloud_two_pc.ps1. None of these ship.
add_library(hubsaves_mock STATIC cloud_mock.cpp)
target_link_libraries(hubsaves_mock PUBLIC hubsaves ws2_32)

add_executable(hubsaves_engine_test test_cloud_engine.cpp)
target_link_libraries(hubsaves_engine_test PRIVATE hubsaves_mock)
target_compile_definitions(hubsaves_engine_test PRIVATE CLOUD_CLASSIFY_CASES="${CMAKE_SOURCE_DIR}/docs/cloud_classify_cases.txt")

add_executable(cloud_mock_server cloud_mock_server.cpp)
target_link_libraries(cloud_mock_server PRIVATE hubsaves_mock)

add_executable(qa_cloud_tool qa_cloud_tool.cpp)
target_link_libraries(qa_cloud_tool PRIVATE hubsaves_mock hubnet)

# Save history + play stats unit test.
add_executable(hubhistory_test test_history.cpp)
target_link_libraries(hubhistory_test PRIVATE hubsaves)
