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
target_link_libraries(hubnet PUBLIC hubbase ws2_32 winhttp bcrypt crypt32)

add_executable(hublink_test test_link.cpp)
target_link_libraries(hublink_test PRIVATE hubnet)

add_executable(hubnet_test test_session.cpp)
target_link_libraries(hubnet_test PRIVATE hubnet)

add_executable(hubfriends_dedupe_test test_friends_dedupe.cpp)
target_link_libraries(hubfriends_dedupe_test PRIVATE hubnet)

add_executable(hubreconnect_test test_reconnect.cpp)
target_link_libraries(hubreconnect_test PRIVATE hubnet)

add_executable(hubws_close_test test_ws_close.cpp)
target_link_libraries(hubws_close_test PRIVATE hubnet)

add_executable(hubws_error_text_test test_ws_error_text.cpp)
target_link_libraries(hubws_error_text_test PRIVATE hubnet)

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

# Offline outbox + socket liveness rules (pure logic, no network).
add_executable(hubfriends_outbox_test test_outbox.cpp)
target_include_directories(hubfriends_outbox_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_executable(hubadmin_test test_admin.cpp)
target_include_directories(hubadmin_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hubadmin_test PRIVATE hubbase)
add_executable(hubfriends_live_test test_liveness.cpp)
target_include_directories(hubfriends_live_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# QA driver, not a ctest: invite codes end to end against a LOCAL worker (qa\invite_e2e.ps1).
add_executable(qa_invite_e2e qa_invite_e2e.cpp)
target_include_directories(qa_invite_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../hub)
target_link_libraries(qa_invite_e2e PRIVATE hubnet)

# Per-Windows-user identity backup (offline).
add_executable(hubidbackup_test test_identity_backup.cpp)
target_link_libraries(hubidbackup_test PRIVATE hubnet)
