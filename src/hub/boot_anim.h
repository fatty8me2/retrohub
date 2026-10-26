#pragma once
/* Game start animation: shelf -> game media flies to the console -> inserted -> LED + startup sound -> fade -> launch.
 * Shared contract between three lanes:
 *   boot_anim.cpp  (engine + flow + settings)  agent/boot-anim
 *   boot_art.cpp   (vector art)                agent/boot-art
 *   boot_sound.cpp (procedural sounds)         agent/boot-sound
 * Until boot_art/boot_sound exist, boot_stub.cpp supplies placeholders (excluded from the build once the real files exist). */

#include <functional>
#include "imgui.h"
#include "common/library.h"

namespace bootanim {

enum class Media { Cartridge, SmallCart /*GB/GBC/GBA*/, Card /*DS/3DS*/, MiniDisc /*GC*/, Disc /*PS1/PS2/WiiU*/, None };
Media media_for(hub::System s);

/* ---- boot_art.cpp (agent/boot-art) implements. All drawing is ImDrawList vector art, no textures except `label`. ---- */
void draw_shelf(ImDrawList*, ImVec2 min, ImVec2 max, float t);                // background shelf with a row of game boxes/carts
/* lid_open 0..1 (lid/tray/slot cover open amount), power 0..1 (LED brightness). scale 1.0 = ~300 px wide console. */
void draw_console(hub::System, ImDrawList*, ImVec2 center, float scale, float lid_open, float power);
/* rot in radians (disc spin / tilt); label = cover art texture (null = plain), uv0/uv1 select the region. scale 1.0 = ~200 px. */
void draw_media(hub::System, ImDrawList*, ImVec2 center, float scale, float rot, ImTextureID label, ImVec2 label_uv0, ImVec2 label_uv1);

/* ---- boot_sound.cpp (agent/boot-sound) implements. sound_play is non-blocking; sound_stop cuts it. ---- */
void sound_play(hub::System);
void sound_stop();
bool sound_available(hub::System);
void sound_set_volume(float v01);   // 0..1, applied to subsequent and current playback

/* ---- boot_anim.cpp (engine) ---- */
struct Options {
    bool enabled = true;          // master switch
    bool sounds = true;
    float volume = 0.7f;
    bool short_version = false;   // ~1 s
    bool skip_online = true;      // skip (or 1 s) for online launches
};
Options& options();
void load_options();              // from hub settings store
void save_options();

/* Pure timeline (testable). Phase boundaries in seconds; total() is the whole animation. */
enum class Phase { Idle, Slide, Fly, Insert, Power, Fade, Done };
struct Timeline { float slide, fly, insert, power, fade; float total() const { return slide + fly + insert + power + fade; } };
Timeline timeline_for(bool short_version);                 // full ~3.0 s, short ~1.0 s
Phase phase_at(const Timeline&, float t);
float phase_progress(const Timeline&, float t);            // 0..1 within the phase at t
/* What to do for a launch: 0 = skip entirely, 1 = short, 2 = full. */
enum class Mode { Skip, Short, Full };
Mode choose_mode(const Options&, bool online_launch, bool quick_path);
float ease_in_out(float x);
float ease_out_back(float x);

/* `online`: a room launch (never delayed beyond the short version). `quick`: resume/quick-launch path (short). */
void start(const hub::GameEntry&, std::function<void()> on_done, bool online = false, bool quick = false);
bool active();
void draw();     // call every frame near the end of the UI draw (fullscreen overlay); fires on_done once when finished or skipped
void skip();     // any button/click/Esc

} // namespace bootanim
