#pragma once
/* n64run --headless support: a mupen64plus video extension (CoreOverrideVidExt) that gives the
 * GLideN64 plugin an OpenGL context on a hidden SDL window, plus a framebuffer grab for --screenshot.
 * No window is ever shown and nothing is presented to the screen. */
#include <functional>
#include <string>

#include "m64p_frontend.h"

namespace n64run {

/* Registers the hidden-window video extension with the core. Call after CoreStartup, before the video
 * plugin starts. Returns false if the core rejects the override. */
bool headless_install_vidext(ptr_CoreOverrideVidExt override_fn);

enum class ShotMode { Off, Armed, Periodic };

/* Off: never read the framebuffer. Armed: grab the next presented frame once, then go Off.
 * Periodic: grab every 30th presented frame (used when the run length is unknown). */
void headless_set_shot_mode(ShotMode m);

/* Writes the most recently grabbed frame as PNG (or BMP by extension). False if nothing was grabbed. */
bool headless_write_screenshot(const std::string& path, int max_w = 0);   // max_w > 0: box-downscaled to at most that width

/* Instant replay in headless runs: every `every`th presented frame is read back and handed to `sink` (RGBA, top-down) before the swap. */
void headless_set_frame_sink(std::function<void(const uint8_t* rgba, int w, int h)> sink, int every);

/* Number of frames grabbed so far (a new value after headless_set_shot_mode(Armed) means the grab happened). */
unsigned headless_shot_serial();

}  // namespace n64run
