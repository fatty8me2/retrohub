# retrorun: minimal libretro frontend (GBA / GB / GBC cores; software rendering only).
# libretro.h (MIT) and stb_image_write.h (public domain) are vendored in third_party/.
# Netplay reuses n64run's tested lockstep glue (lockstep.cpp + lockstep_core), compiled in directly
# so n64run's targets stay untouched; one "sample" = one retro_run() frame here.

add_executable(retrorun
    main.cpp
    core.cpp
    image.cpp
    link_sync.cpp
    rollback.cpp
    present_fx.cpp
    rewind.cpp
    runahead.cpp
    features.cpp
    cheevos.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../n64run/lockstep.cpp)
target_include_directories(retrorun PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../n64run)
target_link_libraries(retrorun PRIVATE hubcheevos hubsavesync n64run_lockstep_core hubnet hubcommon hubcontrols_sdl hubminiz SDL2::SDL2)
set_target_properties(retrorun PROPERTIES WIN32_EXECUTABLE OFF)

add_custom_command(TARGET retrorun POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:retrorun>)

# Nintendo DS helpers (layout fit, stylus mapping, core option defaults): pure logic, console test.
add_executable(retrorun_ds_test test_dsview.cpp)
target_include_directories(retrorun_ds_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# DS local wireless packet framing (ds_link.h).
add_executable(retrorun_dslink_test test_dslink.cpp)
target_include_directories(retrorun_dslink_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Branding: same icon + version resources as RetroHub.exe (see src/hub/CMakeLists.txt for the generated header).
target_sources(retrorun PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../hub/retrorun.rc)
add_dependencies(retrorun hubversion)   # generates version_gen/hub_version_rc.h (src/hub)
target_include_directories(retrorun PRIVATE ${CMAKE_BINARY_DIR}/version_gen ${CMAKE_CURRENT_SOURCE_DIR}/../hub)

# Link-cable save exchange (retrorun --link): pure logic, console test with simulated loss/reorder.
add_executable(retrorun_link_test test_link_sync.cpp link_sync.cpp)
target_include_directories(retrorun_link_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../net)
target_link_libraries(retrorun_link_test PRIVATE hubnet)

# SDL_GameControllerDB (zlib, see THIRD_PARTY.md) next to the exe: <exe>/controls/gamecontrollerdb.txt
add_custom_command(TARGET retrorun POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/assets/controls/gamecontrollerdb.txt $<TARGET_FILE_DIR:retrorun>/controls/gamecontrollerdb.txt)

# Rollback netplay engine (rollback.h): console test with two peers over a simulated laggy/lossy link.
add_executable(retrorun_rollback_test test_rollback.cpp rollback.cpp)
target_include_directories(retrorun_rollback_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../n64run)

# Rollback under long network stalls (2 and 4 peers, virtual clock): never a desync, no false drops, a dropped peer learns it.
add_executable(retrorun_rollback_stall_test test_rollback_stall.cpp rollback.cpp)
target_include_directories(retrorun_rollback_stall_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../n64run)

# Screen filters + bezels (present_fx.h): pure CPU, console test + benchmark.
add_executable(retrorun_fx_test test_present_fx.cpp present_fx.cpp)
target_include_directories(retrorun_fx_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Rewind ring + run-ahead + scripted rewind/run-ahead checks against a deterministic fake core (no SDL, no real core).
add_executable(retrorun_rewind_test test_rewind.cpp rewind.cpp runahead.cpp features.cpp)
target_include_directories(retrorun_rewind_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Desync recovery (rollback.h resync + state_xfer.h): host state transfer over a lossy virtual link, give-up after 3.
add_executable(retrorun_resync_test test_resync.cpp rollback.cpp)
target_include_directories(retrorun_resync_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../n64run)
