# retrorun: minimal libretro frontend (GBA / GB / GBC cores; software rendering only).
# libretro.h (MIT) and stb_image_write.h (public domain) are vendored in third_party/.
# Netplay reuses n64run's tested lockstep glue (lockstep.cpp + lockstep_core), compiled in directly
# so n64run's targets stay untouched; one "sample" = one retro_run() frame here.

add_executable(retrorun
    main.cpp
    core.cpp
    image.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../n64run/lockstep.cpp)
target_include_directories(retrorun PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../n64run)
target_link_libraries(retrorun PRIVATE n64run_lockstep_core hubnet hubcommon SDL2::SDL2)
set_target_properties(retrorun PROPERTIES WIN32_EXECUTABLE OFF)

add_custom_command(TARGET retrorun POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:retrorun>)
