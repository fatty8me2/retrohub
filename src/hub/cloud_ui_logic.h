#pragma once
/* Pure UI logic for cloud saves v2 (no ImGui, no I/O): status -> label/tone, quota text, conflict/lease dialog rules,
 * event -> toast mapping, recovery code formatting. Unit tested in test_cloud_ui.cpp. Types come from cloud_engine.h. */
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include "cloud_engine.h"

namespace hubui {
namespace cloudui {

using namespace hub::saves;

enum class Tone { Good, Busy, Warn, Bad, Muted };

struct Chip { std::string text; Tone tone = Tone::Muted; };

/* The status chip for one game. Offline wins over "waiting": when the account cannot be reached the game says Offline. */
inline Chip game_chip(const GameCloudInfo& g, const CloudAccountInfo& a) {
    switch (g.state) {
        case CloudState::Synced: return {"Synced", Tone::Good};
        case CloudState::Syncing:
            return {g.message.find("ownload") != std::string::npos ? "Downloading" : "Uploading", Tone::Busy};
        case CloudState::Waiting:
            if (a.known && !a.online) return {"Offline", Tone::Muted};
            return {"Waiting to sync", Tone::Muted};
        case CloudState::LocalOnly:
            switch (g.local_only) {
                case LocalOnlyReason::OverCap: return {"This PC only (over the limit)", Tone::Warn};
                case LocalOnlyReason::TooLarge: return {"This PC only (too big)", Tone::Warn};
                case LocalOnlyReason::User: return {"This PC only (your choice)", Tone::Muted};
                case LocalOnlyReason::DecideLater: return {"This PC only (undecided)", Tone::Warn};
                default: return {"This PC only", Tone::Muted};
            }
        case CloudState::Conflict: return {"Conflict", Tone::Bad};
        case CloudState::PlayingElsewhere: return {"Open on another PC", Tone::Warn};
        case CloudState::Paused: return {"Paused", Tone::Muted};
        case CloudState::NeedsAttention: return {"Needs attention", Tone::Bad};
        case CloudState::Unknown: break;
    }
    if (a.known && !a.online) return {"Offline", Tone::Muted};
    return {"Checking...", Tone::Muted};
}

/* Short pill for tiles and the details modal. Quiet ("") when everything is fine or nothing is known. */
inline std::string tile_pill(const GameCloudInfo& g, const CloudAccountInfo& a) {
    switch (g.state) {
        case CloudState::Synced: return "CLOUD";
        case CloudState::Unknown: return "";
        case CloudState::Syncing: return game_chip(g, a).text == "Downloading" ? "DOWNLOADING" : "UPLOADING";
        case CloudState::LocalOnly: return "THIS PC ONLY";
        case CloudState::Conflict: return "CONFLICT";
        case CloudState::PlayingElsewhere: return "OTHER PC";
        case CloudState::NeedsAttention: return "CLOUD !";
        default: return a.known && !a.online ? "OFFLINE" : "";
    }
}

inline std::string mb_text(int64_t bytes) {
    char b[48];
    if (bytes > 0 && bytes < 1048576) { std::snprintf(b, sizeof b, "%lld KB", (long long)((bytes + 1023) / 1024)); return b; }
    std::snprintf(b, sizeof b, "%lld MB", (long long)((bytes + 524288) / 1048576));
    return b;
}
/* "18 MB of 50 MB" */
inline std::string quota_text(const CloudAccountInfo& a) {
    if (!a.known || a.cap <= 0) return "";
    return mb_text(a.used) + " of " + mb_text(a.cap);
}
inline float quota_fraction(const CloudAccountInfo& a) {
    if (a.cap <= 0) return 0.f;
    return (float)std::min<double>(1.0, std::max<double>(0.0, (double)a.used / (double)a.cap));
}
inline Tone quota_tone(const CloudAccountInfo& a) {
    const float f = quota_fraction(a);
    if (a.pool == "full" || f >= 0.98f) return Tone::Bad;
    if (a.pool == "tight" || f >= 0.85f) return Tone::Warn;
    return Tone::Good;
}

/* Account headline: one line + tone for the header card. */
inline Chip account_headline(const CloudAccountInfo& a, bool enabled) {
    if (!enabled) return {"Cloud backup is paused", Tone::Muted};
    if (!a.known) return {"Checking your cloud saves...", Tone::Muted};
    if (a.needs_attention) return {"Cloud backup needs attention", Tone::Bad};
    if (a.paused_by_server) return {"Cloud backup is paused for now", Tone::Warn};
    if (!a.online) return {"Offline: saves sync when you are back online", Tone::Muted};
    if (a.queued_games > 0) return {"Backing up your saves...", Tone::Busy};
    return {"Your saves are backed up", Tone::Good};
}

inline std::string attention_help(const std::string& why) {
    if (why == "identity-missing") return "This PC has lost its account file. Use \"Recover with code\" below with your recovery code.";
    return "RetroHub could not sign in to your account. Check your internet connection, or use \"Recover with code\" if you changed PCs.";
}

/* Local-only games explanation. names = titles of games with LocalOnly(OverCap) state. */
inline std::string over_limit_text(const CloudAccountInfo& a, const std::vector<std::string>& names) {
    if (names.empty()) {
        if (a.pool == "full" || (a.cap > 0 && a.used >= a.cap)) return "Cloud space is full. Your small saves still back up; big items such as save states stay on this PC.";
        return "";
    }
    std::string list;
    for (size_t i = 0; i < names.size() && i < 3; ++i) list += (i ? ", " : "") + names[i];
    if (names.size() > 3) list += " and " + std::to_string(names.size() - 3) + " more";
    return "Cloud space is full: older save states of " + list + " stay on this PC only. Your regular saves are still backed up. "
           "Remove a game from the cloud below to make room.";
}

/* "20 Oct 21:14" style, local time. */
inline std::string when_text(int64_t t) {
    if (t <= 0) return "never";
    std::time_t tt = (std::time_t)t;
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    static const char* mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char b[40];
    std::snprintf(b, sizeof b, "%d %s %02d:%02d", lt.tm_mday, mon[lt.tm_mon % 12], lt.tm_hour, lt.tm_min);
    return b;
}
/* "5 minutes ago" for recent times, else the date. */
inline std::string ago_text(int64_t now, int64_t t) {
    if (t <= 0) return "never";
    int64_t d = now - t;
    if (d < 45) return "just now";
    if (d < 3600) return std::to_string(std::max<int64_t>(1, d / 60)) + (d / 60 <= 1 ? " minute ago" : " minutes ago");
    if (d < 86400) return std::to_string(d / 3600) + (d / 3600 == 1 ? " hour ago" : " hours ago");
    return when_text(t);
}

/* Details line under a game row: "Last synced 5 minutes ago - 1 MB in the cloud" / message. */
inline std::string game_detail(const GameCloudInfo& g, const CloudAccountInfo& a, int64_t now) {
    std::string s;
    switch (g.state) {
        case CloudState::PlayingElsewhere:
            return "Open on " + (g.playing_on.empty() ? std::string("another PC") : g.playing_on) + ". Playing here may create a conflict.";
        case CloudState::Conflict: return "This PC and the cloud both changed. Pick which one to keep.";
        case CloudState::LocalOnly:
            if (g.local_only == LocalOnlyReason::OverCap) return "Cloud space is full, so this game's save states stay on this PC.";
            if (g.local_only == LocalOnlyReason::TooLarge) return "A file here is too big for the cloud and stays on this PC.";
            if (g.local_only == LocalOnlyReason::User) return "You chose to keep this game's saves on this PC only.";
            return "Choose which save to keep to start syncing again.";
        case CloudState::Waiting:
            if (a.known && !a.online) return "Offline: will sync when you are back.";
            break;
        default: break;
    }
    if (g.last_synced > 0) s = "Last synced " + ago_text(now, g.last_synced);
    else s = "Not synced yet";
    if (g.cloud_bytes > 0) s += " - " + mb_text(g.cloud_bytes) + " in the cloud";
    if (g.pinned) s += " - kept in cloud";
    return s;
}

/* ---- conflict dialog ---- */
inline bool local_is_newer(const ConflictItem& c) { return c.local.mtime >= c.cloud.mtime; }

inline std::string kb_text(int64_t b) { char s[32]; std::snprintf(s, sizeof s, "%lld KB", (long long)std::max<int64_t>(1, (b + 1023) / 1024)); return s; }

inline std::string side_line(const FileSide& s, bool cloud) {
    if (!s.present) return cloud ? "Cloud: no save yet" : "This PC: no save yet";
    std::string who = s.device_name.empty() ? std::string(cloud ? "another PC" : "this PC") : s.device_name;
    return (cloud ? "Cloud: " + who + ", " : std::string("This PC: played ")) + when_text(s.mtime) + ", " + kb_text(s.size);
}
inline std::string conflict_summary(const std::string& title) { return "Saves for " + title + " differ."; }

/* Preselected choice: the newer file. */
inline ConflictChoice conflict_default() { return ConflictChoice::UseNewer; }
inline const char* conflict_button(ConflictChoice c) {
    switch (c) {
        case ConflictChoice::UseNewer: return "Use the newer one";
        case ConflictChoice::UseLocal: return "Use this PC's";
        case ConflictChoice::UseCloud: return "Use the cloud's";
        default: return "Decide later";
    }
}
inline const char* conflict_hint(ConflictChoice c) {
    switch (c) {
        case ConflictChoice::UseNewer: return "Keeps whichever was saved last. The other is kept as a restorable copy.";
        case ConflictChoice::UseLocal: return "Replaces the cloud copy with this PC's. The cloud one stays in version history.";
        case ConflictChoice::UseCloud: return "Replaces this PC's save with the cloud one. This PC's copy is kept in a backup folder.";
        default: return "Play now with this PC's save. Nothing is uploaded for this game until you decide.";
    }
}
/* Which side wins for a choice (true = local wins), used for the preview line. */
inline bool choice_keeps_local(ConflictChoice c, const ConflictItem& it) {
    if (c == ConflictChoice::UseLocal || c == ConflictChoice::DecideLater) return true;
    if (c == ConflictChoice::UseCloud) return false;
    return local_is_newer(it);
}

inline std::string lease_text(const std::string& title, const std::string& dev, int64_t since) {
    return title + " is open on " + (dev.empty() ? std::string("another PC") : dev) + (since > 0 ? " (since " + when_text(since) + ")" : "") +
           ". Playing here may create a conflict.";
}
inline const char* lease_button(LeaseChoice c) {
    return c == LeaseChoice::Cancel ? "Cancel" : c == LeaseChoice::PlayAnyway ? "Play anyway" : "Take over";
}

/* ---- launch-time notices ---- */
inline std::string checking_text() { return "Getting your save from the cloud..."; }
inline std::string prelaunch_notice(const PrelaunchResult& r) {
    if (r.offline) return "Offline: playing with the saves on this PC. They sync when you are back online.";
    if (r.files_pulled > 0) return "Played on another PC - using that save.";
    return "";
}

/* ---- events -> toasts ---- */
struct ToastSpec { std::string text; bool good = true; double secs = 6.0; bool show = true; };
inline ToastSpec event_toast(const CloudEvent& e) {
    ToastSpec t;
    t.text = e.text;
    t.good = e.good;
    switch (e.kind) {
        case CloudEvent::Pushed: t.show = false; break;   // silent: the Saves page shows it
        case CloudEvent::BackOnline: t.secs = 4.0; break;
        case CloudEvent::NeedsAttention: t.good = false; t.secs = 9.0; break;
        case CloudEvent::ConflictFound: t.good = false; t.secs = 8.0; break;
        case CloudEvent::CapAlmostFull: case CloudEvent::WentLocalOnly: t.good = false; t.secs = 9.0; break;
        default: break;
    }
    if (t.text.empty()) t.show = false;
    return t;
}

/* ---- recovery code ---- */
inline std::string normalize_code(const std::string& in) {
    std::string o;
    for (char c : in) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) o += c;
    }
    return o;
}
inline std::string group_code(const std::string& in) {
    std::string n = normalize_code(in), o;
    for (size_t i = 0; i < n.size(); ++i) { if (i && i % 4 == 0) o += '-'; o += n[i]; }
    return o;
}
/* Cheap pre-check before bothering the engine: the engine does the real validation. */
inline bool code_plausible(const std::string& in) { size_t n = normalize_code(in).size(); return n >= 16 && n <= 160; }

inline std::string code_file_text(const std::string& code, const std::string& device) {
    return "RetroHub recovery code\r\n\r\n" + code + "\r\n\r\nKeep this somewhere safe. With this code you can get your cloud saves back on a new PC "
           "(Settings > Account > Recover with code). Anyone who has it can open your saves, so do not share it.\r\n\r\nWritten on " + device + "\r\n";
}

/* Sort for the games list: problems first, then busy, then the rest by last sync (newest first). */
inline int state_rank(CloudState s) {
    switch (s) {
        case CloudState::Conflict: return 0;
        case CloudState::NeedsAttention: return 1;
        case CloudState::PlayingElsewhere: return 2;
        case CloudState::LocalOnly: return 3;
        case CloudState::Syncing: return 4;
        case CloudState::Waiting: return 5;
        case CloudState::Paused: return 6;
        default: return 7;
    }
}
inline bool game_before(const GameCloudInfo& a, const GameCloudInfo& b) {
    if (state_rank(a.state) != state_rank(b.state)) return state_rank(a.state) < state_rank(b.state);
    return a.last_synced > b.last_synced;
}

}  // namespace cloudui
}  // namespace hubui
