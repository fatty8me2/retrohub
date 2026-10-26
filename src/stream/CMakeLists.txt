add_library(rplaycore STATIC stream_core.cpp)
target_include_directories(rplaycore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(stream_core_test test_stream_core.cpp)
target_link_libraries(stream_core_test PRIVATE rplaycore)

# streamrun: remote play host/guest (Media Foundation H.264/AAC, WGC capture, WASAPI process loopback, SDL guest window)
add_executable(streamrun streamrun.cpp video.cpp capture.cpp audio.cpp upnp.cpp)
target_link_libraries(streamrun PRIVATE rplaycore hubnet SDL2::SDL2 d3d11 dxgi mfplat mfuuid mf mfreadwrite wmcodecdspuuid strmiids ole32 oleaut32 mmdevapi winhttp ws2_32 winmm windowsapp)
target_compile_options(streamrun PRIVATE /EHsc)
