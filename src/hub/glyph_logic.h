#pragma once
/* Controller glyphs: which labels to draw for Xbox / PlayStation / Switch pads. Pure (takes the SDL controller type as an int).
 * Hints in the app are written with Xbox names ("A", "LB", "View", "Menu"); label() maps them to what the player's pad prints.
 * On Switch pads SDL (button-label mode, the default) already reports the button NAMED A as A, so face letters stay. */
#include <cstring>

namespace hubglyph {

enum class Kind { Xbox = 0, PlayStation, Switch };

/* SDL_GameControllerType values: 1 360, 2 One, 3 PS3, 4 PS4, 5 Switch Pro, 7 PS5, 11-13 Joy-Cons; anything else draws Xbox. */
inline Kind kind_from_sdl_type(int t) {
    switch (t) {
        case 3: case 4: case 7: return Kind::PlayStation;
        case 5: case 11: case 12: case 13: return Kind::Switch;
        default: return Kind::Xbox;
    }
}
inline const char* kind_name(Kind k) { return k == Kind::PlayStation ? "PlayStation" : k == Kind::Switch ? "Switch" : "Xbox"; }

/* Label for an Xbox-style hint name. PlayStation face buttons come back as "Cross" "Circle" "Square" "Triangle" (the UI draws shapes). */
inline const char* label(Kind k, const char* x) {
    if (!x) return "";
    auto is = [&](const char* s) { return std::strcmp(x, s) == 0; };
    if (k == Kind::PlayStation) {
        if (is("A")) return "Cross";
        if (is("B")) return "Circle";
        if (is("X")) return "Square";
        if (is("Y")) return "Triangle";
        if (is("LB")) return "L1";
        if (is("RB")) return "R1";
        if (is("LT")) return "L2";
        if (is("RT")) return "R2";
        if (is("View")) return "Share";
        if (is("Menu") || is("Start")) return "Options";
    } else if (k == Kind::Switch) {
        if (is("LB")) return "L";
        if (is("RB")) return "R";
        if (is("LT")) return "ZL";
        if (is("RT")) return "ZR";
        if (is("View")) return "-";
        if (is("Menu") || is("Start")) return "+";
    }
    return x;
}
inline bool is_ps_shape(const char* l) {
    return !std::strcmp(l, "Cross") || !std::strcmp(l, "Circle") || !std::strcmp(l, "Square") || !std::strcmp(l, "Triangle");
}

/* SDL_JoystickPowerLevel: 0 empty 1 low 2 medium 3 full 4 wired 5 max, -1 unknown. */
inline const char* battery_text(int lvl) {
    switch (lvl) {
        case 0: return "Battery empty";
        case 1: return "Battery low";
        case 2: return "Battery medium";
        case 3: case 5: return "Battery full";
        case 4: return "Wired";
        default: return "";
    }
}
inline bool battery_shown(int lvl) { return lvl >= 0 && lvl <= 3 || lvl == 5; }   // wired / unknown: nothing to show
inline bool battery_low(int lvl) { return lvl == 0 || lvl == 1; }
inline int battery_bars(int lvl) { return lvl == 0 ? 0 : lvl == 1 ? 1 : lvl == 2 ? 2 : 3; }

}  // namespace hubglyph
