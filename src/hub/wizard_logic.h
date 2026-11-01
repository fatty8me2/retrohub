#pragma once
/* Pure step logic for the first-run setup wizard (no UI, no SDL). See onboarding.cpp for the screens. */
#include "username.h"

#include <cstddef>
#include <string>

namespace hubui {
namespace wiz {

enum class Step { Welcome = 0, Controller, Games, Name, Cloud, Done };
constexpr int kStepCount = 6;

inline Step next(Step s) { return s == Step::Done ? s : static_cast<Step>(static_cast<int>(s) + 1); }
inline Step back(Step s) { return s == Step::Welcome ? s : static_cast<Step>(static_cast<int>(s) - 1); }
inline int index(Step s) { return static_cast<int>(s); }
inline Step from_index(int i) { return i < 0 ? Step::Welcome : i >= kStepCount ? Step::Done : static_cast<Step>(i); }

/* Existing installs that already went through "Find my games" or have games of their own never see the wizard on their own. */
inline bool needs_setup(bool setup_done, bool find_games_done, bool have_user_folders) {
    return !setup_done && !find_games_done && !have_user_folders;
}

/* The installer pre-fills a games folder on a FRESH install (userdata\installer_games_dir.txt; never written by an upgrade), and
 * main() merges it into rom_folders. That must not make a brand-new install look "already set up": a fresh install ALWAYS runs the
 * setup wizard (owner decision, Plan 4 H4), with that folder as a pre-filled answer in its Games step.
 *
 * main() decides BEFORE the merge, from the folders the player chose themselves, and raises this flag for the whole run. It stays
 * raised across restarts through the hand-off file, which main() keeps until the wizard is finished or skipped (setup_done). */
inline bool& installer_prefill() { static bool pending = false; return pending; }
inline void set_installer_prefill(bool on) { installer_prefill() = on; }

/* main()'s first-run decision, taken right after the installer's folder was merged into rom_folders: `folders` = rom_folders.size()
 * AFTER the merge, `installer_folder_listed` = that folder is one of them (just added, or already there). Only the player's OWN
 * folders count as "already set up". */
inline bool fresh_install(bool setup_done, bool find_games_done, size_t folders, bool installer_folder_listed) {
    const size_t own = folders - (installer_folder_listed && folders > 0 ? 1 : 0);
    return needs_setup(setup_done, find_games_done, own > 0);
}

/* Folders the installer pre-filled do not count as "the player already has games": everything that decides about the first-run
 * screens (the wizard, the "What's new" popup that waits for a fresh install) asks this one function. */
inline bool should_autorun(bool setup_done, bool find_games_done, bool have_rom_folders) {
    return needs_setup(setup_done, find_games_done, have_rom_folders && !installer_prefill());
}

/* Controller step headline / hint for n connected pads. */
inline std::string pad_headline(int n) {
    if (n <= 0) return "No controller found";
    if (n == 1) return "Your controller is ready";
    return std::to_string(n) + " controllers are ready";
}
inline std::string pad_hint(int n, bool pad_mode) {
    if (n <= 0) return "No problem: the keyboard and mouse work everywhere. Plug in a controller any time and RetroHub will pick it up.";
    return pad_mode ? "Press A to continue." : "Click Continue (or press A on your controller).";
}
/* Player slot label for the Nth connected pad (0-based). */
inline std::string player_label(int i) { return "Player " + std::to_string(i + 1); }

/* The name step takes a valid username (3-20 characters: letters, digits, spaces and most symbols; see net/username.h). */
inline bool name_ok(const std::string& n) { return hubnet::username_ok(n); }

inline std::string step_title(Step s) {
    switch (s) {
        case Step::Welcome: return "Welcome to RetroHub";
        case Step::Controller: return "Your controller";
        case Step::Games: return "Find your games";
        case Step::Name: return "Pick your username";
        case Step::Done: return "You're all set";
    }
    return "";
}
inline std::string returning_blurb() {
    return "Already have a username on another PC? On that PC open Settings > Account > Add this account to another PC, then type the code here. This PC signs in as you and your cloud saves come with it.";
}

}  // namespace wiz
}  // namespace hubui
