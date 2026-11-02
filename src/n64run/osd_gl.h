#pragma once
/* The runner's on-screen display (osd.h) drawn into the game window itself, so toasts ("Saved state", volume, a friend's chat) and the hub's
 * corner text (party standings, challenge timers) are visible in fullscreen, where the window caption cannot show them (runners-M2).
 *
 * WHERE it draws matters. The frame callback (M64CMD_SET_FRAME_CALLBACK, the "VI callback" of frontend.cpp) runs right after the video
 * plugin processed a display list, while GLideN64 still renders into its own framebuffer objects: anything drawn there lands in the
 * emulated N64 framebuffer (it can be copied back into RDRAM, which online play hashes, and it persists into later frames). So the overlay
 * is drawn from the input plugin's RenderCallback instead. The core calls that from the video plugin's rendering callback, i.e. at the
 * plugin's swap, after the plugin composed the finished frame onto the window's default framebuffer and immediately before it swaps the
 * buffers (the spot the core's own OSD uses). The overlay therefore only ever touches the presented frame, never the emulation.
 *
 * The drawing is core-profile safe (GLSL 3.30, a vertex-less fullscreen triangle, an own VAO/texture/program) and puts back every piece of
 * GL state it touches, because the plugin caches state and would otherwise misrender the next frame. The layer is rendered at 1/s of the
 * window size (s = window height / 240, so osd.h always picks its smallest font) and stretched with nearest filtering: the 5x7 font stays
 * crisp at any size, and a fade costs a few hundred KB of upload instead of a whole 4K frame.
 *
 * Default: on for a real window; N64RUN_OSD_GL=0 turns it off, N64RUN_OSD_GL=1 turns it on in --headless runs (QA reads the screenshot). */
#include <string>

namespace n64run {

void osd_gl_configure(bool windowed);          // main(), before the emulation starts; reads N64RUN_OSD_GL
void osd_gl_set_logger(void (*log)(const char* line));
bool osd_gl_enabled();

/* The input plugin's RenderCallback. Video-plugin thread, GL context current, default framebuffer bound, just before the swap. */
void osd_gl_render();

struct OsdGlInfo {
    bool enabled = false;
    bool failed = false;      // could not initialise (no GL 3.3 entry points, shader build failed): the caption mirror carries the text
    unsigned draws = 0;       // frames the overlay was drawn into
    unsigned uploads = 0;     // texture uploads
    int win_w = 0, win_h = 0, layer_w = 0, layer_h = 0;
    std::string gl;           // GL_VERSION / GL_RENDERER of the context it drew into
};
OsdGlInfo osd_gl_info();

}  // namespace n64run
