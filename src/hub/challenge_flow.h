#pragma once
/* Friend challenges: the pure state machines behind the UI (no ImGui, no network). Tested by hubchallenge_test.
 *   SendFlow  (the challenger, in the game menu):  Saving -> PickGoal -> [TypeScore] -> PickFriends -> Uploading -> Sending -> Sent | Failed
 *   RunFlow   (the friend who accepted):           Downloading -> Launching -> Running -> Finished (posts a result card)
 * Helpers: friend selection (max 8), score text parsing, the running clock that does not tick while the game menu is open. */
#include "challenge_logic.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace hub::challenge {

constexpr size_t kMaxFriends = 8;   // the Worker accepts 8 recipients per file

enum class SendPhase { Idle, Saving, PickGoal, TypeScore, PickFriends, Uploading, Sending, Sent, Failed };
enum class SendEvent { Begin, StateSaved, GoalFastest, GoalJust, GoalScore, ScoreOk, FriendsOk, UploadsDone, Delivered, Fail, Cancel, Back };

/* One transition; an event that makes no sense in a phase is ignored (the phase is returned unchanged). */
inline SendPhase send_next(SendPhase p, SendEvent e) {
    if (e == SendEvent::Fail) return (p == SendPhase::Idle || p == SendPhase::Sent) ? p : SendPhase::Failed;
    if (e == SendEvent::Cancel) return (p == SendPhase::Uploading || p == SendPhase::Sending) ? p : SendPhase::Idle;   // an upload in flight is finished first
    switch (p) {
    case SendPhase::Idle:        return e == SendEvent::Begin ? SendPhase::Saving : p;
    case SendPhase::Saving:      return e == SendEvent::StateSaved ? SendPhase::PickGoal : p;
    case SendPhase::PickGoal:
        if (e == SendEvent::GoalFastest || e == SendEvent::GoalJust) return SendPhase::PickFriends;
        if (e == SendEvent::GoalScore) return SendPhase::TypeScore;
        return p;
    case SendPhase::TypeScore:   return e == SendEvent::ScoreOk ? SendPhase::PickFriends : e == SendEvent::Back ? SendPhase::PickGoal : p;
    case SendPhase::PickFriends: return e == SendEvent::FriendsOk ? SendPhase::Uploading : e == SendEvent::Back ? SendPhase::PickGoal : p;
    case SendPhase::Uploading:   return e == SendEvent::UploadsDone ? SendPhase::Sending : p;
    case SendPhase::Sending:     return e == SendEvent::Delivered ? SendPhase::Sent : p;
    case SendPhase::Sent: case SendPhase::Failed: return e == SendEvent::Begin ? SendPhase::Saving : p;
    }
    return p;
}
inline bool send_busy(SendPhase p) { return p == SendPhase::Saving || p == SendPhase::Uploading || p == SendPhase::Sending; }

enum class RunPhase { Idle, Downloading, Launching, Running, Finished, Failed };
enum class RunEvent { Accept, Downloaded, Started, Done, GaveUp, Fail, GameEnded };
inline RunPhase run_next(RunPhase p, RunEvent e) {
    if (e == RunEvent::Fail) return (p == RunPhase::Idle || p == RunPhase::Finished) ? p : RunPhase::Failed;
    switch (p) {
    case RunPhase::Idle: case RunPhase::Finished: case RunPhase::Failed: return e == RunEvent::Accept ? RunPhase::Downloading : p;
    case RunPhase::Downloading: return e == RunEvent::Downloaded ? RunPhase::Launching : p;
    case RunPhase::Launching:   return e == RunEvent::Started ? RunPhase::Running : p;
    case RunPhase::Running:     return (e == RunEvent::Done || e == RunEvent::GaveUp || e == RunEvent::GameEnded) ? RunPhase::Finished : p;
    }
    return p;
}

/* Friend selection: toggles uid in `sel`; adding past kMaxFriends is refused (returns false and leaves the list). */
inline bool friend_toggle(std::vector<std::string>* sel, const std::string& uid) {
    auto it = std::find(sel->begin(), sel->end(), uid);
    if (it != sel->end()) { sel->erase(it); return true; }
    if (sel->size() >= kMaxFriends) return false;
    sel->push_back(uid);
    return true;
}

/* "12,000" / "12000" / " 4 500 " -> 12000 (digits only, at most 12 of them). false when there are no digits. */
inline bool parse_score(const std::string& text, int64_t* out) {
    int64_t v = 0; int n = 0;
    for (char c : text) {
        if (std::isdigit((unsigned char)c)) { if (++n > 12) return false; v = v * 10 + (c - '0'); }
        else if (c == ',' || c == ' ' || c == '.' || c == '_') continue;
        else return false;
    }
    if (!n) return false;
    *out = v;
    return true;
}

/* The challenge clock: wall time since the game started, minus the time the game menu was open (the game is paused then). */
struct RunClock {
    int64_t start_ms = -1, paused_total = 0, pause_began = -1;
    void start(int64_t now) { start_ms = now; paused_total = 0; pause_began = -1; }
    bool started() const { return start_ms >= 0; }
    void set_menu_open(bool open, int64_t now) {
        if (!started()) return;
        if (open && pause_began < 0) pause_began = now;
        else if (!open && pause_began >= 0) { paused_total += now - pause_began; pause_began = -1; }
    }
    int64_t elapsed(int64_t now) const {
        if (!started()) return 0;
        int64_t e = now - start_ms - paused_total - (pause_began >= 0 ? now - pause_began : 0);
        return e < 0 ? 0 : e;
    }
};

/* Which rows the game menu shows. */
struct MenuRows { bool challenge = false, challenge_enabled = false, did_it = false, give_up = false; std::string why; };
inline MenuRows menu_rows(const std::string& system, bool has_ctl, bool online, bool running_challenge, bool busy_sending) {
    MenuRows r;
    if (running_challenge) { r.did_it = r.give_up = true; return r; }
    if (!has_ctl || !runner_supports_challenge(system)) return r;   // not offered at all (external emulators, native port)
    r.challenge = true;
    r.challenge_enabled = true;
    if (online) { r.challenge_enabled = false; r.why = offline_only_note(); }
    else if (busy_sending) { r.challenge_enabled = false; r.why = "A challenge is still being sent."; }
    return r;
}

} // namespace hub::challenge
