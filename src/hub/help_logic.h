#pragma once
/* First-hour helpers: the "Finish setup" card's open steps, the scan result line and the scan caption. Pure (no ImGui, no App state). */
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace hubhelp {

/* empty_card_showing: Home is already asking for game folders (the "Add your game folders" card), so the Finish card does not ask again. */
struct SetupInputs { bool setup_done = false, have_games = false, username_claimed = false, net_probe_done = false, have_pad = true, empty_card_showing = false; };

/* Steps still open, in order. Empty = nothing to finish (the Home card hides). A missing pad is not a step: the keyboard works. */
inline std::vector<std::string> open_steps(const SetupInputs& s) {
    std::vector<std::string> v;
    if (!s.have_games && !s.empty_card_showing) v.push_back("Add your games");
    if (!s.username_claimed) v.push_back("Pick a username");
    if (!s.net_probe_done) v.push_back("Allow online play");
    if (!s.setup_done && v.empty()) v.push_back("Finish the setup tour");
    return v;
}

/* Below about 700 px of window height (at 100% UI size) the Finish card is one line, so the bottom bar never cuts it off. */
constexpr float kCompactCardPx = 700.f;
inline bool finish_card_compact(float display_h_px, float ui_scale) {
    return display_h_px < kCompactCardPx * (ui_scale > 0.f ? ui_scale : 1.f);
}
inline std::string join_steps(const std::vector<std::string>& steps) {
    std::string s;
    for (size_t i = 0; i < steps.size(); ++i) { if (i) s += "  -  "; s += steps[i]; }
    return s;
}

/* ---- paths ---- */

inline char fold_c(char c) { return c == '/' ? '\\' : (char)std::tolower((unsigned char)c); }

/* "C:\Users\me\Documents\Games" -> "~\Documents\Games" when `home` (the user's profile folder) is a prefix (case and slash style ignored). */
inline std::string short_path(const std::string& path, const std::string& home) {
    size_t hn = home.size();
    while (hn > 0 && (home[hn - 1] == '\\' || home[hn - 1] == '/')) --hn;
    if (hn == 0 || path.size() < hn) return path;
    for (size_t i = 0; i < hn; ++i) if (fold_c(path[i]) != fold_c(home[i])) return path;
    if (path.size() == hn) return "~";
    if (path[hn] != '\\' && path[hn] != '/') return path;   // "C:\Users\meow" is not under "C:\Users\me"
    return "~" + path.substr(hn);
}

inline size_t utf8_len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}
/* Byte offset of the code point `cp` (0-based) in s. */
inline size_t utf8_off(const std::string& s, size_t cp) {
    size_t n = 0, i = 0;
    for (; i < s.size(); ++i) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) { if (n == cp) return i; ++n; }
    }
    return s.size();
}

/* Keeps the start and the end of a long text and puts "..." in the middle, so the result is at most max_chars characters (never splits a UTF-8 character). */
inline std::string middle_trunc(const std::string& s, size_t max_chars) {
    const size_t n = utf8_len(s);
    if (n <= max_chars) return s;
    if (max_chars <= 3) return std::string(max_chars, '.');
    const size_t keep = max_chars - 3, head = (keep + 1) / 2, tail = keep - head;
    return s.substr(0, utf8_off(s, head)) + "..." + s.substr(utf8_off(s, n - tail));
}

/* ---- scan result ---- */

inline bool path_under(const std::string& path, const std::string& folder) {
    size_t fn = folder.size();
    while (fn > 0 && (folder[fn - 1] == '\\' || folder[fn - 1] == '/')) --fn;
    if (fn == 0 || path.size() <= fn) return false;
    for (size_t i = 0; i < fn; ++i) if (fold_c(path[i]) != fold_c(folder[i])) return false;
    return path[fn] == '\\' || path[fn] == '/';
}

inline std::string plural(size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

/* What the scan found. `game_paths` = the file of each game of your own; `folders` = your game folders. folders.empty() = nothing to report. */
struct ScanSummary { std::string text; size_t games = 0, folders_with_games = 0; bool none = false; };

inline ScanSummary scan_summary(const std::vector<std::string>& game_paths, const std::vector<std::string>& folders, const std::string& home = "") {
    ScanSummary r;
    if (folders.empty()) return r;
    r.games = game_paths.size();
    if (game_paths.empty()) {
        r.none = true;
        r.text = folders.size() == 1 ? "Looked in " + short_path(folders[0], home) + ": no game files found."
                                     : "Looked in " + plural(folders.size(), "folder", "folders") + ": no game files found.";
        return r;
    }
    for (const std::string& f : folders) {
        for (const std::string& p : game_paths) if (path_under(p, f)) { ++r.folders_with_games; break; }
    }
    r.text = "Found " + plural(r.games, "game", "games");
    if (r.folders_with_games) r.text += " in " + plural(r.folders_with_games, "folder", "folders");
    r.text += ".";
    return r;
}

/* The empty Home card's line about the last scan, plus the file types a scan reads (`exts` come from the code: addgames::supported_exts()). */
inline std::string file_types_line(const std::vector<std::string>& exts) {
    std::string s;
    for (const std::string& e : exts) { if (!s.empty()) s += ", "; s += e; }
    return s.empty() ? s : "RetroHub reads these file types: " + s + ".";
}

/* ---- scan caption ---- */

constexpr double kBrowseHintAfterS = 10.0;
inline std::string scan_caption_base(int done, int total) {
    return total > 0 ? "Scanning your games...  " + std::to_string(done) + " / " + std::to_string(total) : std::string("Scanning your games...");
}
/* "" until the scan has run for 10 seconds. */
inline std::string scan_browse_hint(double elapsed_s) { return elapsed_s >= kBrowseHintAfterS ? "You can keep browsing while this finishes." : ""; }

}  // namespace hubhelp
