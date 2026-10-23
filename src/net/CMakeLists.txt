add_library(hubnet STATIC
    log.cpp
    udp.cpp
    ws.cpp
    util.cpp
    lobby.cpp
    friends.cpp
    identity_link.cpp
    session.cpp)
target_include_directories(hubnet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hubnet PUBLIC ws2_32 winhttp bcrypt crypt32)

add_executable(hublink_test test_link.cpp)
target_link_libraries(hublink_test PRIVATE hubnet)

add_executable(hubnet_test test_session.cpp)
target_link_libraries(hubnet_test PRIVATE hubnet)

add_executable(hubfriends_test test_friends.cpp)
target_link_libraries(hubfriends_test PRIVATE hubnet)

# Host-save sharing (n64run and retrorun --share-host-save): protocol state machine + Session driver.
add_library(hubsavesync STATIC save_sync.cpp save_sync_net.cpp)
target_include_directories(hubsavesync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hubsavesync PUBLIC cxx_std_17)
target_link_libraries(hubsavesync PUBLIC hubnet)

# Console test: save sync over a simulated lossy network; with a server URL argument also runs two
# real Sessions through the live server (registered with CTest in the top-level CMakeLists.txt).
add_executable(n64run_save_sync_test test_save_sync.cpp)
target_link_libraries(n64run_save_sync_test PRIVATE hubsavesync)
