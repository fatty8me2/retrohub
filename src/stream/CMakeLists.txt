add_library(rplaycore STATIC stream_core.cpp)
target_include_directories(rplaycore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(stream_core_test test_stream_core.cpp)
target_link_libraries(stream_core_test PRIVATE rplaycore)
