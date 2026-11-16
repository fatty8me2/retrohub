#pragma once
/* Pure helpers for the game option rows: a game's Settings tab and Settings > Video & Games > Game defaults (settings_games_ui.cpp), plus
 * the Switch "play with" choice there and in the Switch block of Settings > Video & Games (switch_ui.cpp): the headings and their order, and
 * the "Plays with" value shown. No ImGui. Sections, controls and pill words: option_sections.h. Unit tested by hub_option_rows_test (and
 * hub_nx_runner_test for the "Plays with" value against the real runner decision). */
#include <algorithm>
#include <string>
#include <vector>

namespace hubui {

/* ---- groups ---- */

/* The heading a stored option group is shown under. Display only: the stored group names and the setting keys never change. */
inline std::string option_group_label(const std::string& g) {
    if (g == "Video") return "Graphics";
    if (g == "Color") return "Display";
    if (g == "Emulation") return "Gameplay";
    return g;
}

/* The order the headings are drawn in: the known ones first, then any other heading in the order it first appears in `present`, so an option
 * whose group is not in the known list is still drawn (a missing heading once hid Multitap and Memory card 2). Each heading once. */
inline std::vector<std::string> option_group_order(const std::vector<std::string>& present) {
    static const char* const known[] = {"Graphics", "Display", "Audio", "Gameplay", "Controls", "Saves", "Advanced"};
    std::vector<std::string> out;
    auto has = [&](const std::string& s) { return std::find(out.begin(), out.end(), s) != out.end(); };
    auto there = [&](const std::string& s) { return std::find(present.begin(), present.end(), s) != present.end(); };
    for (const char* k : known) if (there(k)) out.push_back(k);
    for (const std::string& p : present) if (!has(p)) out.push_back(p);
    return out;
}

/* (Row layout: the UI kit's option groups size the label and control columns themselves, ui_controls.h.) */

/* ---- Switch: which program plays ("switch.runner") ---- */

/* The "Plays with" value to SHOW for one game: what Play starts. `stored` = the resolved setting, `fritchii_planned` = nx_planned_emulator(e,
 * false) == Fritchii (the one runner decision). A chosen native build (hookshot / longshot) shows as chosen; otherwise the emulator that runs,
 * so Ocarina of Time with nothing chosen shows Fritchii, as Play does. */
inline std::string runner_shown(const std::string& stored, bool fritchii_planned) {
    if (stored == "hookshot" || stored == "longshot") return stored;
    return fritchii_planned ? "fritchii" : "lichii";
}

/* The Switch-wide combo (every Switch and Switch 2 game). Nothing chosen and Fritchii obtainable: every game plays with Lichii except Ocarina
 * of Time, which gets Fritchii (nx_emulator_for), so the preview says both. Otherwise the chosen value's label. */
inline std::string switch_wide_runner_preview(const std::string& stored_label, bool chosen, bool fritchii_obtainable) {
    if (chosen || !fritchii_obtainable) return stored_label;
    return "Lichii, Fritchii for Ocarina of Time";
}

}  // namespace hubui
