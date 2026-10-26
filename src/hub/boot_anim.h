#pragma once
/* Game start animation: shelf -> game media flies to the console -> inserted -> LED + startup sound -> fade -> launch.
 * Shared contract between three lanes:
 *   boot_anim.cpp  (engine + flow + settings)  agent/boot-anim
 *   boot_art.cpp   (vector art)                agent/boot-art
 *   boot_sound.cpp (procedural sounds)         agent/boot-sound
 * Until boot_art/boot_sound exist, boot_stub_art.cpp / boot_stub_sound.cpp supply placeholders (CMake drops a stub as soon as the
 * real file exists). Pure logic (media mapping, options, timeline, state machine) lives in boot_logic.h. */

#include <functional>
#include "imgui.h"
#include "library.h"
#include "boot_logic.h"

namespace bootanim {

/* ---- boot_art.cpp (agent/boot-art) implements. All drawing is ImDrawList vector art, no textures except `label`. ---- */
void draw_shelf(ImDrawList*, ImVec2 min, ImVec2 max, float t);                // background shelf with a row of game boxes/carts
/* lid_open 0..1 (lid/tray/slot cover open amount), power 0..1 (LED brightness). scale 1.0 = ~300 px wide console. */
void draw_console(hub::System, ImDrawList*, ImVec2 center, float scale, float lid_open, float power);
/* rot in radians (disc spin / tilt); label = cover art texture (null = plain), uv0/uv1 select the region. scale 1.0 = ~200 px. */
void draw_media(hub::System, ImDrawList*, ImVec2 center, float scale, float rot, ImTextureID label, ImVec2 label_uv0, ImVec2 label_uv1);

/* ---- boot_sound.cpp (agent/boot-sound) implements. sound_play is non-blocking; sound_stop cuts it.
 *      Master volume: scale the output by bootanim::options().volume (0..1) at play time. ---- */
void sound_play(hub::System, bool short_cut = false);   // short_cut: ~2 s cut with a quick fade (short animation)
void sound_stop();   // 150 ms fade-out; only for a user skip (the sound otherwise plays to its end after launch)
bool sound_available(hub::System);

/* ---- boot_anim.cpp (engine) ---- */
Options& options();               // live options; the hub mirrors its Settings into this (boot_sync_options in launch.cpp)

/* `online`: a room launch (never longer than the short version). `quick`: resume/quick-launch path (short version).
 * on_done fires exactly once: when the animation ends, is skipped, or immediately when it is disabled. */
void start(const hub::GameEntry&, std::function<void()> on_done, bool online = false, bool quick = false);
bool active();
void draw();     // call every frame from the overlay pass (modal full-screen); fires on_done once when finished or skipped
void skip();     // any button/click/Esc

/* Screenshot aid (HUB_SHOT_BOOTANIM=<system>:<t>): freeze the animation at t seconds for the given system. */
void shot_freeze(hub::System, float t);
bool shot_frozen();

} // namespace bootanim
