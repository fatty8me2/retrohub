add_library(hubnet STATIC
    log.cpp
    udp.cpp
    ws.cpp
    util.cpp
    lobby.cpp
    session.cpp)
target_include_directories(hubnet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hubnet PUBLIC ws2_32 winhttp)

add_executable(hubnet_test test_session.cpp)
target_link_libraries(hubnet_test PRIVATE hubnet)
