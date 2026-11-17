#pragma once
/* Pure rules for the game option pages drawn with the UI kit: a game's Settings tab and Settings > Video & Games > Game defaults
 * (settings_games_ui.cpp), plus the Switch-wide "play with" row (switch_ui.cpp). No ImGui. Unit tested by hub_option_sections_test.
 *   - which sections start open, and the short summary a closed section shows;
 *   - the open / closed memory kept in hub.json ("option_sections", Settings::option_sections), keyed by heading, so "Advanced" stays the
 *     way the player left it on every game and every console;
 *   - which kit control a stored option gets (dropdown, toggle, stepper, text field); the stored values never change;
 *   - the status pill words, the same on a game's page and in Game defaults. */
#include "game_settings.h"

#include <algorithm>
#include <climits>
#include <map>
#include <string>
#include <vector>

namespace hubui {

/* ---- which sections start open ---- */

constexpr int kOptBigPage = 12;   // a page with more rows than this starts with only its first two sections open

/* Before the player opened or closed it, does the section headed `group` start open? `index` = its place on the page (0 = first),
 * `holds_runner` = it holds the "Plays with" row, `rows` = rows on the whole page. Graphics and the "Plays with" section always start
 * open, Advanced starts closed, and the rest start open unless the page is long, where only the first two do. */
inline bool option_section_default_open(const std::string& group, int index, bool holds_runner, int rows, int big = kOptBigPage) {
    if (group == "Graphics" || holds_runner) return true;
    if (group == "Advanced") return false;
    if (rows <= big) return true;
    return index < 2;
}

/* What a closed section says about its rows: "3 settings", "3 settings, 1 changed". */
inline std::string option_section_summary(int rows, int changed) {
    std::string s = std::to_string(rows < 0 ? 0 : rows) + (rows == 1 ? " setting" : " settings");
    if (changed > 0) s += ", " + std::to_string(changed) + " changed";
    return s;
}

/* ---- two columns on a wide body (a game's Settings tab) ---- */

/* Every length in spacing units u (ui::sp(1)): a column is at least kOptColMinU wide (about 610 px at 100 %, where a label and a control
 * still read well side by side) and at most kOptColMaxU (the kit's row width, about 900 px). */
constexpr float kOptColMinU = 76.f, kOptColMaxU = 112.f, kOptColGapU = 4.f;

struct OptionFlow {
    int cols = 1;          // 1 or 2
    float col_w = 0.f;     // each column's width
    float gap = 0.f;       // between the columns
};

/* Two columns when the body (avail px wide) holds two columns of the minimum width with the gap between them, else one. 1920x1080 at
 * 100 % gets two (the full-window page and the desk pane), 1280x720 and 1920x1080 at 150 % one. */
inline OptionFlow option_flow(float avail, float u) {
    OptionFlow f;
    f.gap = kOptColGapU * u;
    if (avail >= 2.f * kOptColMinU * u + f.gap) {
        f.cols = 2;
        f.col_w = std::min(kOptColMaxU * u, (avail - f.gap) * 0.5f);
    } else {
        f.col_w = std::max(0.f, std::min(avail, kOptColMaxU * u));
    }
    return f;
}

/* Whole sections, in order, split between two columns: the first k go left, the rest right. `rows` = each section's height in rows as
 * drawn (its header counts as one; a closed section is only its header). The taller column is as short as it can be; when two splits
 * tie, the left column is the taller one (it is read first). Fewer than two sections: k = n (one column). */
inline int option_flow_split(const std::vector<int>& rows) {
    const int n = (int)rows.size();
    if (n < 2) return n;
    long total = 0;
    for (int r : rows) total += std::max(0, r);
    int best = 1;
    long best_max = LONG_MAX;
    bool best_left = false;
    long left = 0;
    for (int k = 1; k < n; ++k) {
        left += std::max(0, rows[(size_t)k - 1]);
        const long right = total - left;
        const long mx = std::max(left, right);
        const bool left_taller = left >= right;
        if (mx < best_max || (mx == best_max && left_taller && !best_left)) {
            best = k;
            best_max = mx;
            best_left = left_taller;
        }
    }
    return best;
}

/* ---- open / closed memory (hub.json "option_sections": "Advanced=0,Audio=1") ---- */

constexpr size_t kSectionStateMax = 64;   // entries kept; a key past this is not remembered (there are about ten headings)

inline std::map<std::string, bool> section_states_parse(const std::string& s) {
    std::map<std::string, bool> m;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        const std::string tok = s.substr(i, j - i);
        const size_t eq = tok.find('=');
        if (eq != std::string::npos && eq > 0 && eq + 2 == tok.size() && (tok[eq + 1] == '0' || tok[eq + 1] == '1') && m.size() < kSectionStateMax)
            m[tok.substr(0, eq)] = tok[eq + 1] == '1';
        i = j + 1;
    }
    return m;
}

inline std::string section_states_format(const std::map<std::string, bool>& m) {
    std::string o;
    for (const auto& kv : m) {
        if (kv.first.empty() || kv.first.find_first_of(",=") != std::string::npos) continue;
        if (!o.empty()) o += ',';
        o += kv.first + (kv.second ? "=1" : "=0");
    }
    return o;
}

/* -1 = the player never opened or closed it, 0 = closed, 1 = open. */
inline int section_state_get(const std::string& stored, const std::string& key) {
    const auto m = section_states_parse(stored);
    const auto it = m.find(key);
    return it == m.end() ? -1 : (it->second ? 1 : 0);
}

/* `stored` with `key` set; unchanged for a key that cannot be stored (empty, or holding ',' or '='), or a new key past kSectionStateMax. */
inline std::string section_state_put(const std::string& stored, const std::string& key, bool open) {
    if (key.empty() || key.find_first_of(",=") != std::string::npos) return stored;
    auto m = section_states_parse(stored);
    if (!m.count(key) && m.size() >= kSectionStateMax) return stored;
    m[key] = open;
    return section_states_format(m);
}

/* ---- which control a row gets ---- */

enum class OptCtl { Dropdown, Toggle, Stepper, Text };

/* A Bool, or a choice of exactly "on" / "off", is a toggle (it still stores "true" / "false" or "on" / "off"). A short ordered range reads
 * better as a stepper: volume, the resolution scales and the anti-aliasing steps. Any other choice is a dropdown, text a text field. */
inline OptCtl option_control(const hub::gs::Option& o) {
    if (o.kind == hub::gs::Kind::Text) return OptCtl::Text;
    if (o.kind == hub::gs::Kind::Bool) return OptCtl::Toggle;
    if (o.choices.size() == 2 && ((o.choices[0].value == "on" && o.choices[1].value == "off") || (o.choices[0].value == "off" && o.choices[1].value == "on")))
        return OptCtl::Toggle;
    static const char* const stepped[] = {"volume", "n64.aa", "ps1.res", "gc.res", "3ds.res", "ps2.res", "psp.res", "switch.res"};
    for (const char* k : stepped)
        if (o.key == k) return OptCtl::Stepper;
    return OptCtl::Dropdown;
}

/* The toggle's position for a stored value, and the value a toggle position stores. */
inline bool option_toggle_on(const hub::gs::Option& o, const std::string& v) { return o.kind == hub::gs::Kind::Bool ? v == "true" : v == "on"; }
inline std::string option_toggle_value(const hub::gs::Option& o, bool on) {
    if (o.kind == hub::gs::Kind::Bool) return on ? "true" : "false";
    return on ? "on" : "off";
}

/* Index of the stored value among the choices, -1 when it is not one of them. */
inline int option_choice_index(const hub::gs::Option& o, const std::string& v) {
    for (size_t i = 0; i < o.choices.size(); ++i)
        if (o.choices[i].value == v) return (int)i;
    return -1;
}

/* Whole number in `s` (digits only), -1 otherwise. */
inline long option_whole(const std::string& s) {
    if (s.empty() || s.size() > 9) return -1;
    long n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return -1;
        n = n * 10 + (c - '0');
    }
    return n;
}

/* Where a stepper starts from: the stored value's choice, else the nearest whole-number choice (a Game defaults volume of 75 that comes
 * from Settings > Video & Games sits between 70% and 80%; the lower one on a tie), else -1. */
inline int option_choice_nearest(const hub::gs::Option& o, const std::string& v) {
    const int exact = option_choice_index(o, v);
    if (exact >= 0) return exact;
    const long n = option_whole(v);
    if (n < 0) return -1;
    int best = -1;
    long best_d = 0;
    for (size_t i = 0; i < o.choices.size(); ++i) {
        const long c = option_whole(o.choices[i].value);
        if (c < 0) continue;
        const long d = c > n ? c - n : n - c;
        if (best < 0 || d < best_d) { best = (int)i; best_d = d; }
    }
    return best;
}

/* The text a row shows for a stored value: its choice's label, else the value itself (a volume as "75%"). */
inline std::string option_value_text(const hub::gs::Option& o, const std::string& v) {
    const int i = option_choice_index(o, v);
    if (i >= 0) return o.choices[(size_t)i].label;
    if (o.key == "volume" && option_whole(v) >= 0) return v + "%";
    return v;
}

/* ---- status pills (the same words on a game's Settings tab and in Game defaults) ---- */

/* The row was changed on this page (a game's own value, or a console default), so Reset puts it back. `inherited` = a Nintendo Switch 2
 * default that only comes from the Nintendo Switch defaults: it is not changed here. */
inline const char* option_changed_pill(bool changed_here, bool inherited) {
    if (inherited) return "Uses Switch setting";
    return changed_here ? "Changed" : nullptr;
}

/* What happens to the option in an online game: Sync = the host's value is used, Locked = everyone gets the default. */
/* ---- the two "Reset to defaults" confirms: what is erased, in plain words ---- */

/* A game's Settings tab. The reset drops every choice made for the game (gs::Store::reset_game), also the ones its page does not show:
 * for Ocarina of Time (`plays_with_game`) that is which program plays it and the Fritchii options that only show while Fritchii plays it.
 * The settings live in the save vault, so other PCs get the reset when the saves sync. `console_name` = "Nintendo Switch 2" ... */
inline std::string reset_game_confirm_text(const std::string& console_name, bool plays_with_game) {
    std::string s = "Every setting for this game goes back to the " + console_name + " defaults";
    if (plays_with_game) s += ", including which program it plays with and the Fritchii options that only show while Fritchii plays it";
    return s + ". Your other PCs get the same change when your saves sync.";
}

/* Settings > Video & Games > Game defaults for one console (gs::Store::reset_console). For Nintendo Switch that entry also holds the
 * Switch-wide choice of program (`wide_runner`: one is set), which is its own row above the defaults, not one of the rows reset here. */
inline std::string reset_defaults_confirm_text(const std::string& console_name, bool wide_runner) {
    std::string s = "Every setting here goes back to RetroHub's own default for " + console_name + " games.";
    if (wide_runner) s += " This also clears which program Switch and Switch 2 games play with.";
    return s + " Your other PCs get the same change when your saves sync.";
}

inline const char* option_online_pill(hub::gs::Scope s) {
    if (s == hub::gs::Scope::Sync) return "Host's choice online";
    if (s == hub::gs::Scope::Locked) return "Default when online";
    return nullptr;
}

}  // namespace hubui
