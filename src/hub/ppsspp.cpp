#include "ppsspp.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <fstream>

#include "logic.h"
#include "winutil.h"

namespace hubemu {

namespace fs = std::filesystem;

static fs::path P(const std::string& s) { return fs::u8path(s); }
static std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    return (a.back() == '\\' || a.back() == '/') ? a + b : a + "\\" + b;
}
static std::string read_text(const std::string& p) {
    std::ifstream f(P(p), std::ios::binary);
    return f ? std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()) : std::string();
}
static bool write_text(const std::string& p, const std::string& t) {
    std::error_code ec;
    fs::create_directories(P(p).parent_path(), ec);
    std::ofstream f(P(p), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(t.data(), (std::streamsize)t.size());
    return (bool)f;
}

std::string ppsspp_system_dir(const std::string& d) { return join(d, "memstick\\PSP\\SYSTEM"); }

std::string ppsspp_ini_defaults(const std::string& in) {
    std::string t = in;
    t = hubui::ini_set(t, "General", "FirstRun", "False");
    t = hubui::ini_set(t, "General", "CheckForNewVersion", "False");
    t = hubui::ini_set(t, "General", "AskForExitConfirmationAfterSeconds", "0");
    t = hubui::ini_set(t, "General", "PauseOnLostFocus", "False");
    t = hubui::ini_set(t, "General", "PauseWhenMinimized", "False");
    return t;
}

std::string ppsspp_ini_launch(const std::string& in, const hub::gs::Values& v) {
    std::string t = in;
    t = hubui::ini_set(t, "Graphics", "FullScreen", hub::gs::is_fullscreen(v) ? "True" : "False");
    auto i = v.find("psp.res");
    t = hubui::ini_set(t, "Graphics", "InternalResolution", i == v.end() ? "2" : i->second);
    return t;
}

/* ------------------------------------------------------------------ controls */

/* SDL button index -> Android key code (what PPSSPP's XInput device reports), 0 = none */
static const int kSdlButtonCode[21] = {96, 97, 99, 100, 109, 0, 108, 106, 107, 102, 103, 19, 20, 21, 22, 0, 0, 0, 0, 0, 0};
/* leftx lefty rightx righty lefttrigger righttrigger -> JOYSTICK_AXIS_X Y Z RZ LTRIGGER RTRIGGER */
static const int kAxisId[6] = {0, 1, 11, 14, 17, 18};

static std::string axis_binding(int slot, int axis, int sign) {
    if (axis < 0 || axis > 5) return "";
    return std::to_string(20 + slot) + "-" + std::to_string(4000 + kAxisId[axis] * 2 + (sign < 0 ? 1 : 0));
}

std::string ppsspp_binding(const hubctl::Term& t, int slot) {
    if (slot < 0 || slot > 3) return "";
    if (!t.axis) {
        if (t.idx < 0 || t.idx >= 21 || !kSdlButtonCode[t.idx]) return "";
        return std::to_string(20 + slot) + "-" + std::to_string(kSdlButtonCode[t.idx]);
    }
    return axis_binding(slot, t.idx, t.idx >= 4 ? 1 : t.sign);
}

std::vector<int> ppsspp_state_keys(bool save) { return {save ? (int)VK_F2 : (int)VK_F4}; }

static std::string first_binding(const hubctl::Profile& prof, const std::string& control, int slot) {
    auto it = prof.buttons.find(control);
    std::vector<hubctl::Term> terms;
    if (it == prof.buttons.end() || !hubctl::parse_expr(it->second, prof.axis_threshold, &terms)) return "";
    for (const auto& t : terms) { std::string b = ppsspp_binding(t, slot); if (!b.empty()) return b; }
    return "";
}

std::string ppsspp_controls_ini(const hubctl::Profile& prof, int slot) {
    /* PPSSPP's own keyboard keys (Android key codes: Z 54, X 52, A 29, S 47, Q 45, W 51, Space 62, V 50, arrows 19..22, I/K/J/L 37/39/38/40)
     * stay beside the pad, so a keyboard still plays. */
    struct M { const char* control; const char* name; const char* kb; };
    static const M map[] = {
        {"up", "Up", "1-19"}, {"down", "Down", "1-20"}, {"left", "Left", "1-21"}, {"right", "Right", "1-22"},
        {"circle", "Circle", "1-52"}, {"cross", "Cross", "1-54"}, {"square", "Square", "1-29"}, {"triangle", "Triangle", "1-47"},
        {"start", "Start", "1-62"}, {"select", "Select", "1-50"}, {"l", "L", "1-45"}, {"r", "R", "1-51"},
    };
    std::string o = "# Written by RetroHub before every launch from its PSP controller profile. Change the mapping in RetroHub.\n[ControlMapping]\n";
    for (const auto& m : map) {
        std::string pad = first_binding(prof, m.control, slot);
        o += std::string(m.name) + " = " + (pad.empty() ? std::string() : pad + ",") + m.kb + "\n";
    }
    int ax = 0, ay = 1;
    auto it = prof.analogs.find("stick");
    if (it != prof.analogs.end()) {
        if (hubctl::axis_index(it->second.x) >= 0) ax = hubctl::axis_index(it->second.x);
        if (hubctl::axis_index(it->second.y) >= 0) ay = hubctl::axis_index(it->second.y);
    }
    /* XInput reports stick-up as a positive Y value, and PPSSPP's An.Up on that pad is "axis Y, +1" (KeyMapDefaults.cpp, defaultXInputKeyMap) */
    o += "An.Up = " + axis_binding(slot, ay, 1) + ",1-37\n";
    o += "An.Down = " + axis_binding(slot, ay, -1) + ",1-39\n";
    o += "An.Left = " + axis_binding(slot, ax, -1) + ",1-38\n";
    o += "An.Right = " + axis_binding(slot, ax, 1) + ",1-40\n";
    return o;
}

std::string prepare_ppsspp(const std::string& install_dir, const hubctl::Profile& prof, int slot, const hub::gs::Values& v) {
    std::error_code ec;
    fs::remove(P(join(install_dir, "installed.txt")), ec);   // would send PPSSPP to the user's Documents folder; RetroHub's copy stays portable
    const std::string dir = ppsspp_system_dir(install_dir);
    const std::string ini = join(dir, "ppsspp.ini"), marker = join(dir, "retrohub-defaults.txt");
    std::string t = read_text(ini);
    const bool first = read_text(marker).empty();
    if (first) t = ppsspp_ini_defaults(t);
    t = ppsspp_ini_launch(t, v);
    if (!write_text(ini, t)) return "could not write ppsspp.ini";
    if (first) write_text(marker, "RetroHub wrote PPSSPP's defaults once; controls.ini is rewritten before every launch.\n");
    if (!write_text(join(dir, "controls.ini"), ppsspp_controls_ini(prof, slot))) return "could not write controls.ini";
    return "";
}

std::string ppsspp_args(const std::string& game, bool fullscreen) {
    return std::string(fullscreen ? "--fullscreen " : "--windowed ") + winutil::quote_arg(game);
}

}  // namespace hubemu
