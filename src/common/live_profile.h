#pragma once
/* Live controller reload (runner side), shared by retrorun and n64run (docs/RUNNER_CTL.md "Live controls").
 *
 * The hub rewrites the profile file a runner was started with (--controls FILE) and sends runctl::Cmd::ReloadControls. The control
 * thread calls load_for_reload() and posts the result into a LiveProfile mailbox; the thread that reads the pads takes it at the start
 * of its next poll and rebuilds its mapper (and reopens the pads when the port order changed). Local only: what travels to other
 * players is the mapped RetroPad / N64 button word, so a reload never touches netplay state or which port this PC owns. */
#include <atomic>
#include <mutex>
#include <string>

#include "controls.h"

namespace hubctl {

/* Reads `path` and checks it is a profile for `system` ("n64", "retro", "ps1", "nes", "snes"). False with a human reason otherwise. */
inline bool load_for_reload(const std::string& path, const std::string& system, Profile* out, std::string* err = nullptr) {
    if (path.empty()) { if (err) *err = "This game was started without a controls file"; return false; }
    Profile p;
    if (!read_profile_file(path, &p)) { if (err) *err = "The controls file could not be read"; return false; }
    if (p.system != system) { if (err) *err = "The controls file is for another system"; return false; }
    if (out) *out = p;
    return true;
}

/* Single-slot mailbox: post() from any thread, take() from the input thread. A newer post replaces an untaken one. */
class LiveProfile {
  public:
    void post(const Profile& p) {
        std::lock_guard<std::mutex> g(m_);
        p_ = p;
        has_.store(true);
    }
    bool pending() const { return has_.load(); }
    bool take(Profile* out) {
        if (!has_.load()) return false;
        std::lock_guard<std::mutex> g(m_);
        if (!has_.load()) return false;
        if (out) *out = p_;
        has_.store(false);
        return true;
    }
  private:
    std::mutex m_;
    Profile p_;
    std::atomic<bool> has_{false};
};

/* True when applying `next` over `cur` must reopen the pads (the port order changed). */
inline bool ports_changed(const Profile& cur, const Profile& next) { return cur.port_order != next.port_order; }

}  // namespace hubctl
