# DSU (cemuhook) server for Wii U remote play: virtual pads Cemu reads over UDP. Merge note: wiiu-stream adds its own
# add_subdirectory(src/stream); keep this file's targets (dsuserver, dsu_test, dsu_feed) when combining.
add_library(dsuserver STATIC dsu_server.cpp)
target_include_directories(dsuserver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(dsuserver PUBLIC cxx_std_17)
target_link_libraries(dsuserver PUBLIC ws2_32)

add_executable(dsu_test test_dsu.cpp)
target_link_libraries(dsu_test PRIVATE dsuserver)
add_executable(dsu_feed dsu_feed.cpp)
target_link_libraries(dsu_feed PRIVATE dsuserver)
