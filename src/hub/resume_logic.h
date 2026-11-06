#pragma once
/* Auto-resume guard (Plan 4, saves-F1; generalised in Wave 3 R1). Pure: the caller passes file times (unix seconds, 0 = file missing).
 *
 * The runner's --resume loads <states>/auto.state at start. When the game's battery save is NEWER than that state (another PC's
 * save arrived through the cloud or a sync folder, or the game was played without writing a state), resuming the old state
 * rolls the progress back and the next in-game save writes over the newer one. So the newer save wins: no --resume, and the
 * old resume point stays available from the game page (state strip, "Resume").
 *
 * At exit the runner writes the battery file BEFORE auto.state (save_srm, then the state), so a same-session battery file is never
 * newer than the state; kResumeSlackSec only absorbs whole-second mtimes (the cloud stores unix seconds) and file-system rounding.
 * Clock skew between PCs can hide an older-looking newer save from the mtime rule alone; the state sidecar (auto.resume.json,
 * written by the hub when the game closes: which PC, when, which game, and the newest battery time at that moment) closes that:
 * a battery file newer than the sidecar's own battery time did not come from the run that wrote the state.
 *
 * The same rules apply to every system that resumes (N64, GB, GBC, GBA, NES, SNES): never online, never when a real console startup
 * replaces the resume, never when the save is newer. The player may then choose to continue from this PC's older resume point. */
#include <cstdint>
#include <cstdlib>
#include <string>

namespace hubui {

constexpr int64_t kResumeSlackSec = 15;

/* state_time: auto.state mtime. newest_battery_time: newest file in the game's battery folder. */
inline bool resume_allowed(int64_t state_time, int64_t newest_battery_time) {
    if (state_time <= 0 || newest_battery_time <= 0) return true;   // nothing to protect: unchanged behaviour
    return newest_battery_time <= state_time + kResumeSlackSec;
}

/* ---- the state sidecar: <states>\auto.resume.json ---- */
struct ResumeSidecar {
    std::string pc;            // machine id of the PC that wrote the state (data_dir\pc_id.txt)
    std::string pc_name;       // its Windows name, for the wording
    std::string game_key;
    int64_t time = 0;          // unix seconds when the sidecar was written (the state's exit)
    int64_t battery_time = 0;  // newest battery file time at that moment (0 = no battery file)
    bool valid = false;
};

inline std::string resume_json_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) o += ' ';
        else o += (char)c;
    }
    return o;
}

inline std::string make_resume_sidecar(const ResumeSidecar& s) {
    return "{\"v\":1,\"pc\":\"" + resume_json_escape(s.pc) + "\",\"pc_name\":\"" + resume_json_escape(s.pc_name) + "\",\"key\":\"" +
           resume_json_escape(s.game_key) + "\",\"time\":" + std::to_string((long long)s.time) + ",\"battery_time\":" +
           std::to_string((long long)s.battery_time) + "}";
}

/* Reads what make_resume_sidecar wrote (flat object, our own format; anything else is "not valid"). */
inline bool resume_json_field(const std::string& t, const std::string& k, std::string* out, bool str) {
    const std::string pat = "\"" + k + "\":";
    size_t p = t.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    if (str) {
        if (p >= t.size() || t[p] != '"') return false;
        std::string v;
        for (++p; p < t.size() && t[p] != '"'; ++p) {
            if (t[p] == '\\' && p + 1 < t.size()) ++p;
            v += t[p];
        }
        if (p >= t.size()) return false;
        *out = v;
        return true;
    }
    size_t e = p;
    while (e < t.size() && (t[e] == '-' || (t[e] >= '0' && t[e] <= '9'))) ++e;
    if (e == p) return false;
    *out = t.substr(p, e - p);
    return true;
}

inline ResumeSidecar parse_resume_sidecar(const std::string& t) {
    ResumeSidecar s;
    std::string a, b;
    if (!resume_json_field(t, "pc", &s.pc, true) || s.pc.empty()) return s;
    resume_json_field(t, "pc_name", &s.pc_name, true);
    if (!resume_json_field(t, "key", &s.game_key, true)) return s;
    if (!resume_json_field(t, "time", &a, false) || !resume_json_field(t, "battery_time", &b, false)) return s;
    s.time = std::atoll(a.c_str());
    s.battery_time = std::atoll(b.c_str());
    s.valid = true;
    return s;
}

/* ---- the decision ---- */
enum class ResumeChoice { Ask, Newest, ThisPc };   // the player's answer to a newer-save conflict (Ask = not answered yet)
enum class ResumeResult {
    Fresh,        // no resume: nothing to resume, online, or a real console startup replaces the resume
    Resume,       // pass the resume flag
    NewerSave,    // a newer save exists AND a resume point exists: the player may choose; the default is the newest save
};

struct ResumeInput {
    int64_t state_time = 0;            // auto.state mtime (0 = none)
    int64_t newest_battery_time = 0;   // newest live battery file (0 = none)
    ResumeSidecar sidecar;             // valid = false when missing / unreadable
    std::string this_pc;               // this PC's machine id ("" = unknown)
    std::string game_key;
    bool online = false;               // netplay / rollback run: never resume
    bool real_boot = false;            // a real console startup is used and drops the resume
};

/* Is the resume point older than a newer save? The mtime rule always applies; the sidecar adds the clock-skew-proof rule, but only
 * when it describes THIS game on THIS PC (a sidecar copied from another PC says nothing about this PC's battery times). */
inline bool newer_save_exists(const ResumeInput& in) {
    if (in.state_time <= 0 || in.newest_battery_time <= 0) return false;
    if (!resume_allowed(in.state_time, in.newest_battery_time)) return true;
    const ResumeSidecar& s = in.sidecar;
    if (s.valid && !in.this_pc.empty() && s.pc == in.this_pc && s.game_key == in.game_key && s.battery_time > 0 &&
        in.newest_battery_time > s.battery_time + kResumeSlackSec)
        return true;
    return false;
}

inline ResumeResult decide_resume(const ResumeInput& in, ResumeChoice choice) {
    if (in.online || in.real_boot) return ResumeResult::Fresh;
    if (in.state_time <= 0) return ResumeResult::Fresh;
    if (!newer_save_exists(in)) return ResumeResult::Resume;
    return choice == ResumeChoice::ThisPc ? ResumeResult::Resume : ResumeResult::NewerSave;
}

/* Does the launch wait for the player's choice? Only a conflict that has not been answered. */
inline bool resume_needs_choice(const ResumeInput& in, ResumeChoice choice) {
    return decide_resume(in, choice) == ResumeResult::NewerSave && choice == ResumeChoice::Ask;
}

}  // namespace hubui
