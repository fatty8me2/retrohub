#pragma once
/* Pure logic for the View+Y quick menu's Speed and Volume rows (quickmenu.cpp). Header-only; covered by hub_qm_test. */
#include <algorithm>
#include <string>
#include <vector>

#include "runner_ctl.h"

namespace hubqm {

constexpr int kSpeedSteps = 9;      // 0.25x 0.5x 1x 1.5x 2x 3x 5x 8x Max
constexpr int kSpeedNormal = 2;     // index of 1x: every session starts here
constexpr int kVolumeStep = 5;

/* Synthesized hotkey burst (Dolphin speed): a modifier pair is held around the presses. Before EVERY key event the target must
 * still be the foreground window and no hub menu may be open; otherwise the burst aborts. Modifiers that were pressed are ALWAYS
 * released (the key-ups are harmless to whatever has focus), so Ctrl/Shift can never stay stuck. Returns presses completed. */
template <class FgOk, class Send>
int run_key_burst(const std::vector<int>& vks, int mod_a, int mod_b, FgOk fg_ok, Send send /* (scan, up) */) {
    if (!fg_ok()) return 0;
    send(mod_a, false); send(mod_b, false);
    int done = 0;
    for (int vk : vks) {
        if (!fg_ok()) break;
        send(vk, false);
        if (!fg_ok()) { send(vk, true); break; }   // never leave the key itself down
        send(vk, true);
        ++done;
    }
    send(mod_b, true); send(mod_a, true);
    return done;
}

/* A chord (e.g. Ctrl+Tab): foreground re-checked before every key-down; whatever was pressed is always released in reverse. */
template <class FgOk, class Send>
int run_key_chord(const std::vector<int>& keys, FgOk fg_ok, Send send /* (key, up) */) {
    size_t pressed = 0;
    for (int k : keys) {
        if (!fg_ok()) break;
        send(k, false);
        ++pressed;
    }
    for (size_t i = pressed; i > 0; --i) send(keys[i - 1], true);
    return (int)pressed;
}

/* index -> runner_ctl speed value (0 = max) */
inline double speed_value(int idx) {
    idx = std::max(0, std::min(kSpeedSteps - 1, idx));
    return idx == kSpeedSteps - 1 ? 0.0 : runctl::kSpeeds[idx];
}
inline std::string speed_text(int idx) {   // "2x", "0.5x", "Max"
    std::string l = runctl::speed_label(speed_value(idx));   // "Speed 2x" / "Speed max"
    if (l.size() > 6) l = l.substr(6);
    if (l == "max") return "Max";
    return l;
}
inline int speed_step(int idx, int dir) { return std::max(0, std::min(kSpeedSteps - 1, idx + (dir < 0 ? -1 : dir > 0 ? 1 : 0))); }
inline int volume_step(int v, int dir) { return std::max(0, std::min(100, v + (dir < 0 ? -kVolumeStep : dir > 0 ? kVolumeStep : 0))); }

/* External emulators have no control channel: the hub scales the emulator's Windows audio session. `base` is the volume the
 * emulator already applies itself (Dolphin: -C Dolphin.DSP.Volume at launch; the others: 100), `cur` the slider.
 * Returns the session volume 0..1 (never boosts above what the emulator plays). */
inline float ext_session_scalar(int cur, int base) {
    cur = std::max(0, std::min(100, cur)); base = std::max(0, std::min(100, base));
    if (base <= 0) return cur > 0 ? 1.f : 0.f;
    return std::min(1.f, (float)cur / (float)base);
}

/* Speed: built-in runners only (control channel), and never online. */
inline bool speed_available(bool has_ctl, bool online) { return has_ctl && !online; }

/* ---- Dolphin (no control channel): speed through its own emulation-speed hotkeys, bound by RetroHub to Ctrl+Shift+F13 / F14 ----
 * Verified against dolphin-emu/dolphin tag 2509 (the bundled build): DolphinQt/HotkeyScheduler.cpp ~519-535: Decrease Emulation
 * Speed = EmulationSpeed - 0.1 (ignored when the result would be <= 0, so the floor is 0.1), Increase = + 0.1 (no cap); a result
 * within 0.95..1.05 snaps to exactly 1.0. Presses are edge-triggered (Core/HotkeyManager.cpp IsPressed), so each needs a release.
 * "Disable Emulation Speed Limit" (HK_TOGGLE_THROTTLE, default TAB) is HOLD-to-disable, not a toggle, so there is no Max step for
 * Dolphin: the row stops at 8x. The session starts at 1.0 (written to Dolphin.ini every launch). Our steps map to the nearest
 * reachable value, counted in 0.1 presses from 1x. */
constexpr int kDolphinSteps = kSpeedSteps - 1;                        // 0.25x .. 8x (no Max)
inline int dolphin_tenths(int idx) {                                  // target speed in tenths of 1x
    static const int t[kDolphinSteps] = {3, 5, 10, 15, 20, 30, 50, 80};   // 0.25x -> 0.3 (nearest reachable, ties up)
    idx = std::max(0, std::min(kDolphinSteps - 1, idx));
    return t[idx];
}
inline std::string dolphin_speed_text(int idx) {                      // what Dolphin really runs at ("0.3x" for our 0.25x)
    int t = dolphin_tenths(idx);
    return t % 10 == 0 ? std::to_string(t / 10) + "x" : std::to_string(t / 10) + "." + std::to_string(t % 10) + "x";
}
inline int dolphin_speed_step(int idx, int dir) { return std::max(0, std::min(kDolphinSteps - 1, idx + (dir < 0 ? -1 : dir > 0 ? 1 : 0))); }
enum class DolphinKey { Increase, Decrease };
inline int dolphin_vk(DolphinKey k) { return k == DolphinKey::Increase ? 0x7C : 0x7D; }              // VK_F13 / VK_F14
inline int dolphin_scan(int vk) { return vk == 0x7C ? 0x64 : vk == 0x7D ? 0x65 : 0; }               // DIK_F13 / DIK_F14 (DirectInput reads scancodes)
constexpr int kDolphinScanCtrl = 0x1D, kDolphinScanShift = 0x2A;
// Hotkeys.ini keys are "<group>/<control>" (ControlGroup::LoadConfig: base + name + "/" + c->name; group "Emulation Speed").
inline const char* dolphin_hotkey_name(DolphinKey k) { return k == DolphinKey::Increase ? "Emulation Speed/Increase Emulation Speed" : "Emulation Speed/Decrease Emulation Speed"; }
inline const char* dolphin_hotkey_expr(DolphinKey k) { return k == DolphinKey::Increase ? "@(Ctrl+Shift+F13)" : "@(Ctrl+Shift+F14)"; }
/* Dolphin's current speed. `tenths` is the real speed in tenths of 1x as far as we know; `known` goes false when a key burst was cut
 * short (a press may or may not have registered), after which a plan starts from Dolphin's 0.1 floor instead of guessing.
 * `cur` is the step index shown in the menu. */
struct DolphinSpeed { int cur = kSpeedNormal; int tenths = 10; bool known = true; };
inline int dolphin_idx(const DolphinSpeed& s) { return s.cur; }
constexpr int kDolphinFloorPresses = 90;   // decreases that surely reach Dolphin's 0.1x floor from anything we can set (<= 8x)
/* Non-mutating plan to step `to`. Known state: the difference in tenths. Unknown: floor first, then up. */
inline std::vector<DolphinKey> dolphin_plan_for(const DolphinSpeed& s, int to) {
    std::vector<DolphinKey> v;
    to = std::max(0, std::min(kDolphinSteps - 1, to));
    int from = s.tenths;
    if (!s.known) { v.assign(kDolphinFloorPresses, DolphinKey::Decrease); from = 1; }
    int d = dolphin_tenths(to) - from;
    for (int i = 0; i < (d < 0 ? -d : d); ++i) v.push_back(d < 0 ? DolphinKey::Decrease : DolphinKey::Increase);
    return v;
}
/* Does the menu's chosen step differ from what Dolphin runs (or do we not know)? */
inline bool dolphin_needs_apply(const DolphinSpeed& s, int to) { return !s.known || s.tenths != dolphin_tenths(to); }
/* After a burst that completed `done` of `plan`'s presses. A full run lands exactly on `to`; a partial one leaves the state unknown. */
inline void dolphin_commit(DolphinSpeed& s, int to, const std::vector<DolphinKey>& plan, int done) {
    to = std::max(0, std::min(kDolphinSteps - 1, to));
    if (done >= (int)plan.size()) { s.cur = to; s.tenths = dolphin_tenths(to); s.known = true; }
    else { s.known = false; }
}
/* Legacy single-step helper (tests): plans and assumes success. */
inline std::vector<DolphinKey> dolphin_plan(DolphinSpeed& s, int to) {
    auto v = dolphin_plan_for(s, to);
    dolphin_commit(s, to, v, (int)v.size());
    return v;
}

/* ---- quick menu open / close gating ---- */
/* View+Y opens the menu once: one open per press (rising edge), never while it is open, never within `cooldown` ms of the menu
 * opening or closing (a held / bouncing combo or the closing B cannot reopen or re-close it). The combo must be released before it re-arms. */
struct OpenGate {
    bool prev = false, was_open = false;
    long long changed_ms = -100000;
    static constexpr long long kCooldownMs = 700;
    bool update(bool combo_down, bool menu_open, long long now_ms) {
        if (menu_open != was_open) { was_open = menu_open; changed_ms = now_ms; }
        const bool edge = combo_down && !prev;
        prev = combo_down;
        return edge && !menu_open && now_ms - changed_ms >= kCooldownMs;
    }
};
/* B closes the menu, but not in the first moments after it opened (a B held in the game as the menu appears is not a "close"). */
inline bool back_allowed(long long opened_ms, long long now_ms) { return now_ms - opened_ms >= 400; }
/* A button combo that must be held `hold_ms` (View+LB: reset Dolphin to 1x). Fires once per hold. */
struct HoldGate {
    long long since = -1;
    bool fired = false;
    bool update(bool down, long long now_ms, long long hold_ms = 1000) {
        if (!down) { since = -1; fired = false; return false; }
        if (since < 0) since = now_ms;
        if (!fired && now_ms - since >= hold_ms) { fired = true; return true; }
        return false;
    }
};
/* Hub focus while the menu is open: ask for the foreground again (bounded, spaced) if something else took it. */
struct FocusRetry {
    int tries = 0;
    long long last_ms = -100000;
    void reset() { tries = 0; last_ms = -100000; }
    bool should_retry(bool hub_fg, long long now_ms, int max_tries = 6, long long gap_ms = 500) {
        if (hub_fg || tries >= max_tries || now_ms - last_ms < gap_ms) return false;
        ++tries; last_ms = now_ms;
        return true;
    }
};
/* Retry schedule for a Dolphin speed change that did not go through. */
inline long long dolphin_retry_delay_ms(int attempt) { return attempt <= 0 ? 1500 : attempt == 1 ? 3000 : 6000; }
constexpr int kDolphinMaxRetries = 4;

/* The Speed row in the Game menu. Built-in runners and Dolphin have it (offline only); other emulators show it disabled. */
enum class SpeedRow { Hidden, Enabled, Online, Unsupported };
inline SpeedRow speed_row(bool has_ctl, bool is_dolphin, bool online, bool external) {
    if (has_ctl || is_dolphin) return online ? SpeedRow::Online : SpeedRow::Enabled;
    return external ? SpeedRow::Unsupported : SpeedRow::Hidden;
}

/* ---- Plan 4: pause while the menu is open (runners-H3), pass the controller (P4), emote wheel (P5) ---- */
/* Pause on open: offline built-in runners only (a paused peer would stall an online room), when the setting is on and the menu is up. */
inline bool pause_available(bool has_ctl, bool online) { return has_ctl && !online; }
inline bool auto_pause_wanted(bool setting_on, bool menu_open, bool has_ctl, bool online) { return setting_on && menu_open && pause_available(has_ctl, online); }

/* Pass controller: --ports value ("0,0,1,-1": index = controller port, token = lobby slot) -> the port a lobby slot owns (first one), -1 = none. */
inline int port_of_slot(const std::string& ports, int slot) {
    int idx = 0;
    std::string tok;
    for (size_t i = 0; i <= ports.size(); ++i) {
        if (i == ports.size() || ports[i] == ',') {
            if (!tok.empty() && tok == std::to_string(slot)) return idx;
            tok.clear();
            ++idx;
        } else if (ports[i] != ' ') tok += ports[i];
    }
    return -1;
}
/* "Passing to Mickey in 3", "... in 3..2", "... in 3..2..1" (n = how many numbers are shown, 1..3). */
inline std::string countdown_text(const std::string& name, int n) {
    n = std::max(1, std::min(3, n));
    std::string t = "Passing to " + name + " in 3";
    if (n >= 2) t += "..2";
    if (n >= 3) t += "..1";
    return t;
}
constexpr const char* kHandoffUnsupportedText = "Passing the controller works in NES and SNES online games for now";
/* The toast for a failed hand-off ack: "unsupported" gets the plain explanation, anything else is the runner's own reason. */
inline std::string handoff_fail_text(const std::string& msg) {
    if (msg.empty() || msg == "unsupported") return kHandoffUnsupportedText;
    return msg;
}

/* Emote wheel: wire code -> label. The code is what travels in {kind:"emote", e:<code>}. */
struct Emote { const char* code; const char* label; };
inline const std::vector<Emote>& emotes() {
    static const std::vector<Emote> v = {{"gg", "GG"}, {"nice", "NICE!"}, {"lol", "LOL"}, {"oof", "OOF"}, {"more", "ONE MORE?"}};
    return v;
}
inline const Emote* find_emote(const std::string& code) {
    for (const auto& e : emotes()) if (code == e.code) return &e;
    return nullptr;
}
inline std::string emote_json(const std::string& code) { return "{\"kind\":\"emote\",\"e\":\"" + code + "\"}"; }   // codes are plain [a-z]
/* One emote per kMinGapMs from this PC. */
struct EmoteLimiter {
    static constexpr long long kMinGapMs = 2000;
    long long last_ms = -1000000;
    bool allow(long long now_ms) {
        if (now_ms - last_ms < kMinGapMs) return false;
        last_ms = now_ms;
        return true;
    }
};
/* Incoming emotes are rate limited per sender slot too (a room member cannot flood the OSD): one per kMinGapMs / 2 each. */
struct EmoteInLimiter {
    long long last_ms[8] = {-1000000, -1000000, -1000000, -1000000, -1000000, -1000000, -1000000, -1000000};
    bool allow(int slot, long long now_ms) {
        if (slot < 0 || slot >= 8) return false;
        if (now_ms - last_ms[slot] < EmoteLimiter::kMinGapMs / 2) return false;
        last_ms[slot] = now_ms;
        return true;
    }
};

/* Commands to the runner go out one at a time: control.json holds a single command, and a second write before the runner has read the
 * first replaces it. next() says when the head of the queue may go: the previous command is acked, or kMaxWaitMs passed. */
struct CmdPacer {
    static constexpr long long kMaxWaitMs = 700;
    long long sent_seq = 0, sent_ms = -1000000, acked_seq = 0;
    bool may_send(long long now_ms) const { return sent_seq == 0 || acked_seq >= sent_seq || now_ms - sent_ms >= kMaxWaitMs; }
    void sent(long long seq, long long now_ms) { sent_seq = seq; sent_ms = now_ms; }
    void ack(long long seq) { if (seq > acked_seq) acked_seq = seq; }
};

}  // namespace hubqm
