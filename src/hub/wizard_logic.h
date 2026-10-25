#pragma once
/* Pure step logic for the first-run setup wizard (no UI, no SDL). See onboarding.cpp for the screens. */
#include "username.h"

#include <string>

namespace hubui {
namespace wiz {

enum class Step { Welcome = 0, Controller, Games, Name, Cloud, Done };
constexpr int kStepCount = 6;

inline Step next(Step s) { return s == Step::Done ? s : static_cast<Step>(static_cast<int>(s) + 1); }
inline Step back(Step s) { return s == Step::Welcome ? s : static_cast<Step>(static_cast<int>(s) - 1); }
inline int index(Step s) { return static_cast<int>(s); }
inline Step from_index(int i) { return i < 0 ? Step::Welcome : i >= kStepCount ? Step::Done : static_cast<Step>(i); }

/* Existing installs that already went through "Find my games" or have games never see the wizard on their own. */
inline bool should_autorun(bool setup_done, bool find_games_done, bool have_rom_folders) {
    return !setup_done && !find_games_done && !have_rom_folders;
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

/* The name step takes a valid username (3-16 letters, digits, _; see net/username.h). */
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
