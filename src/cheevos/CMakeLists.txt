# RetroAchievements: pure data/cache/config (hub + runner), one-time login, vendored rcheevos in third_party/rcheevos.
add_library(hubcheevos STATIC cheevos_data.cpp cheevos_login.cpp)
target_include_directories(hubcheevos PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hubcheevos PUBLIC cxx_std_17)
target_link_libraries(hubcheevos PUBLIC hubbase rcheevos hubgameinfo_net)

# Console test: config/cache round trips, hardcore rules, login against a mocked server, rc_client session against canned
# server responses (src/retrorun/cheevos.cpp compiled in; no network, no real RA account).
add_executable(cheevos_test test_cheevos.cpp ${CMAKE_SOURCE_DIR}/src/retrorun/cheevos.cpp)
target_include_directories(cheevos_test PRIVATE ${CMAKE_SOURCE_DIR}/src/retrorun)
target_link_libraries(cheevos_test PRIVATE hubcheevos)
