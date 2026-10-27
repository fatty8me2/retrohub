#pragma once
/* Offline in-game features of retrorun: rewind (rewind.h) + run-ahead (runahead.h) behind one per-frame call.
 * Everything talks to the core through Hooks, so the same code runs against a fake core in the unit test.
 * Both features are refused unless Settings::offline (never online / link / DS wireless / rollback). */
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "rewind.h"
#include "runahead.h"

namespace retrofeat {

enum class Note { Info, Good, Warn, Error };

struct Hooks {
    std::function<void()> run;                                  // one retro_run()
    std::function<void()> real_frame;                           // right after the REAL frame ran (before any run-ahead / never on rewind steps): RetroAchievements
    std::function<size_t()> state_size;
    std::function<bool(void*, size_t)> save;
    std::function<bool(const void*, size_t)> load;
    std::function<uint64_t()> video_hash;                       // selftests only
    std::function<uint64_t()> now_us;                           // default: steady_clock
    std::function<void(bool)> speculative;                      // true: mute audio (run-ahead / rewind frames)
    std::function<void(const std::string&, Note)> notify;       // OSD toast
    std::function<void(const std::string&)> corner;             // OSD persistent corner text ("" removes it)
};

struct Settings {
    std::string system;        // gba gb gbc nds ps1 nes snes ...
    int rewind = 0;            // 0 auto, 1 on, 2 off
    int runahead = 0;          // runahead::kAuto, 0, 1, 2
    double fps = 60.0;
    bool offline = true;
    bool measure = true;       // auto-disable when too slow (off for headless runs: timing is meaningless there)
};

class Features {
  public:
    Features(Hooks h, Settings s);

    /* Replaces core.run() in the offline main loop: runs one frame (or one rewind step), captures, runs ahead. */
    void frame();

    /* Rewind control: hold (pad combo) and latch (runner_ctl). False + OSD message when rewind is unavailable. */
    bool set_held(bool down);
    bool set_latched(bool on);
    bool rewinding() const { return rewinding_; }
    bool rewind_active() const { return rewind_on_; }
    int runahead_frames() const { return ra_frames_; }
    const rwd::Ring* ring() const { return ring_.get(); }

    /* Scripted checks (headless, constant input). Return true on success; `report` gets a one-line result. */
    bool selftest_rewind(int frames, int back, std::string* report);
    bool selftest_runahead(int frames, std::string* report);

  private:
    uint64_t now() const;
    void note(const std::string& s, Note n = Note::Info) const { if (h_.notify) h_.notify(s, n); }
    bool ensure_buffers();
    void rewind_step();
    void disable_rewind(const std::string& why);

    Hooks h_;
    Settings s_;
    bool rewind_on_ = false;
    int ra_frames_ = 0;
    bool rewinding_ = false, held_ = false, latched_ = false, announced_ = false;
    std::unique_ptr<rwd::Ring> ring_;
    std::vector<uint8_t> scratch_;
    size_t state_bytes_ = 0;
    size_t steps_back_ = 0;
    // timing: average over `win` frames
    int win_n_ = 0;
    uint64_t cap_us_ = 0, total_us_ = 0;
    bool timing_done_ = false;
};

}  // namespace retrofeat
