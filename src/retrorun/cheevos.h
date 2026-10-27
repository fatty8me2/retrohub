#pragma once
/* RetroAchievements for retrorun, on rcheevos' rc_client (third_party/rcheevos). Offline play only: main.cpp never starts it online.
 * Kept free of Core/SDL so the console test (src/cheevos/test_cheevos.cpp) drives it with fake memory and canned server replies.
 *
 * main.cpp hooks (all tiny): start() after the game loaded, frame() after every retro_run, allow(Feature) before a state load /
 * cheat / fast-forward / slow-down, stop() at exit. Token comes from userdata\retroachievements.json (cheevos_data.h); the
 * password is never seen here. Unlocks: OSD toast + per-game cache and recent list for the hub. */
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cheevos_data.h"
#include "cheevos_login.h"
#include "third_party/libretro.h"

namespace cheevos {

struct Host {
    /* libretro memory block lookup (retro_get_memory_data/size). */
    std::function<void(unsigned id, uint8_t** data, size_t* size)> core_memory;
    std::vector<retro_memory_descriptor> mmap;   // SET_MEMORY_MAPS copy (empty = use the plain memory blocks)
    HttpPost http;                               // real_http() in the runner
    bool inline_http = false;                    // tests: run requests on the calling thread
    std::function<void(const std::string&)> log;
};

struct Params {
    Config cfg;
    std::string cache_dir, game_key, game_title_hint;
    std::string system;      // nes snes gb gbc gba nds ps1
    std::string rom_path;    // file the hash is made from
    long long (*now)() = nullptr;   // test clock (unix seconds); null = time()
};

class Session {
  public:
    Session();
    ~Session();
    /* False (with a log line) when not applicable: not signed in, unsupported system, no memory map, unhashable file. */
    bool start(const Params& p, const Host& h);
    void frame();     // after each retro_run: processes achievements and finished HTTP replies
    void idle();      // while paused / between frames: HTTP replies only
    void reset();     // emulated console reset
    void stop();
    bool active() const;
    bool hardcore() const;
    /* Hardcore gate: true = allowed. When blocked, shows the OSD message once per second. */
    bool allow(Feature f);
    bool allow_speed(double speed);
    /* Test/inspection. */
    bool game_ready() const;
    unsigned unlocked() const;
    const GameCache& cache() const;

    struct Impl;
  private:
    Impl* d_;
};

unsigned console_for_system(const std::string& system);   // RC_CONSOLE_* (0 = unsupported)

}  // namespace cheevos
