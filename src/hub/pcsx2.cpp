#include "pcsx2.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "logic.h"
#include "winutil.h"

namespace hubemu {

namespace fs = std::filesystem;

static fs::path P(const std::string& s) { return fs::u8path(s); }
static std::string S(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
static std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32); return s; }
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

/* ------------------------------------------------------------------ BIOS */

bool pcsx2_bios_name_ok(const std::string& name, uint64_t size) {
    std::string n = lower(name);
    if (n.size() < 5 || n.compare(n.size() - 4, 4, ".bin") != 0) return false;
    if (size < (1u << 20) || size > (8u << 20)) return false;
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (n.compare(0, 4, "scph") == 0) {
        size_t i = 4;
        if (i < n.size() && n[i] == '-') ++i;
        return i < n.size() && digit(n[i]);
    }
    return n.compare(0, 4, "ps2-") == 0 || n.compare(0, 3, "ps2") == 0;
}

/* A PS2 BIOS dump that is not named like a .bin: "SCPH-90001_BIOS_V18_USA_230.ROM0" (the 4 MB main ROM of a split dump). */
static bool bios_rom0_ok(const std::string& name, uint64_t size) {
    std::string n = lower(name);
    return n.size() > 9 && n.compare(n.size() - 5, 5, ".rom0") == 0 && n.compare(0, 4, "scph") == 0 && size == (4u << 20);
}

static bool bios_has_reset_magic(const fs::path& p) {   // every PS2 BIOS image starts with the ROMDIR entry "RESET"
    std::ifstream f(p, std::ios::binary);
    char b[5] = {};
    f.read(b, 5);
    return f.gcount() == 5 && std::memcmp(b, "RESET", 5) == 0;
}

/* newest USA dump first: (USA/NTSC in the name, model number, plain .bin over .rom0) */
static long long bios_rank(const std::string& name) {
    std::string n = lower(name);
    long long usa = (n.find("usa") != std::string::npos || n.find("ntsc") != std::string::npos) ? 1 : 0;
    long long model = 0;
    size_t i = n.find("scph");
    if (i != std::string::npos) {
        i += 4;
        if (i < n.size() && n[i] == '-') ++i;
        for (int k = 0; k < 6 && i < n.size() && n[i] >= '0' && n[i] <= '9'; ++k, ++i) model = model * 10 + (n[i] - '0');
    }
    long long bin = n.size() > 4 && n.compare(n.size() - 4, 4, ".bin") == 0 ? 1 : 0;
    return usa * 100000000000LL + model * 10 + bin;
}

/* Files in `dir` that look like a PS2 BIOS; `depth` > 0 also walks subfolders (vendor folders), skipping macOS junk and checking the ROM magic. */
static void bios_collect(const std::string& dir, int depth, std::vector<std::pair<std::string, std::string>>* out) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(P(dir), ec)) return;
    for (fs::directory_iterator it(P(dir), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        std::string n = S(it->path().filename());
        if (n.compare(0, 2, "._") == 0 || lower(n) == "__macosx" || lower(n) == ".ds_store") continue;
        if (it->is_directory(e2)) {
            if (depth > 0 && !it->is_symlink(e2)) bios_collect(S(it->path()), depth - 1, out);
            continue;
        }
        if (!it->is_regular_file(e2)) continue;
        uint64_t sz = (uint64_t)it->file_size(e2);
        if (pcsx2_bios_name_ok(n, sz)) out->push_back({dir, n});
        else if (bios_rom0_ok(n, sz) && bios_has_reset_magic(it->path())) out->push_back({dir, n});
    }
}

static bool bios_in(const std::string& dir, Bios* out, int depth = 0) {
    std::vector<std::pair<std::string, std::string>> c;
    bios_collect(dir, depth, &c);
    if (c.empty()) return false;
    size_t best = 0;
    for (size_t i = 1; i < c.size(); ++i) {
        long long ra = bios_rank(c[i].second), rb = bios_rank(c[best].second);
        if (ra > rb || (ra == rb && lower(c[i].second) < lower(c[best].second))) best = i;
    }
    out->dir = c[best].first;
    out->file = c[best].second;
    return true;
}

Bios pcsx2_find_bios(const std::vector<std::string>& dirs) {
    Bios b;
    for (const std::string& d : dirs) {
        const std::string leaf = lower(S(P(d).filename()));
        const bool deep = leaf.find("bios") != std::string::npos;   // "GamesBios", "BIOS": vendor subfolders up to 4 levels down
        if (bios_in(d, &b, deep ? 4 : 0)) return b;
        for (const char* sub : {"BIOS", "bios", "Bios", "GamesBios"})
            if (bios_in(join(d, sub), &b, 4)) return b;
        if (leaf.compare(0, 5, "games") == 0) {
            std::string par = S(P(d).parent_path());
            for (const char* sub : {"BIOS", "Bios", "GamesBios"})
                if (bios_in(join(par, sub), &b, 4)) return b;
        }
    }
    return Bios();
}

std::vector<std::string> pcsx2_bios_search_dirs(const std::string& chosen, const std::vector<std::string>& rom_folders) {
    std::vector<std::string> v;
    if (!chosen.empty()) v.push_back(chosen);
    const char* up = std::getenv("USERPROFILE");
    if (up) { v.push_back(join(up, "Desktop\\Everything\\BIOS")); v.push_back(join(up, "Desktop\\Everything\\GamesBios")); }
    for (const auto& f : rom_folders) {
        v.push_back(f);
        std::string par = S(P(f).parent_path());
        if (!par.empty() && par != f) v.push_back(par);
    }
    return v;
}

const char* pcsx2_bios_missing_text() {
    return "PlayStation 2 games need a PS2 BIOS (a file like scph39001.bin from your own console). Put it in a BIOS folder next to your games, "
           "or choose the folder in Settings > PlayStation 2.";
}

/* ------------------------------------------------------------------ pads */

Ps2Pads pcsx2_pads_with(const std::vector<PadDesc>& by_port, const std::vector<PadDesc>& by_sdl) {
    Ps2Pads p;
    for (size_t i = 0; i < by_port.size() && p.count < 4; ++i) {
        int idx = -1;
        for (size_t k = 0; k < by_sdl.size(); ++k) if (by_sdl[k].key == by_port[i].key) { idx = (int)k; break; }
        if (idx < 0) idx = (int)i;
        p.sdl[p.count++] = idx;
    }
    p.multitap = p.count > 2;
    return p;
}

static const char* kSdlButton[] = {"A", "B", "X", "Y", "Back", "Guide", "Start", "LeftStick", "RightStick", "LeftShoulder", "RightShoulder",
                                   "DPadUp", "DPadDown", "DPadLeft", "DPadRight", "Misc1", "Paddle1", "Paddle2", "Paddle3", "Paddle4", "Touchpad"};
static const char* kSdlAxis[] = {"LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger"};

std::string pcsx2_binding(const hubctl::Term& t, int sdl) {
    std::string dev = "SDL-" + std::to_string(sdl) + "/";
    if (!t.axis) return t.idx >= 0 && t.idx < 21 ? dev + kSdlButton[t.idx] : "";
    if (t.idx < 0 || t.idx > 5) return "";
    bool trigger = t.idx >= 4;
    return dev + ((t.sign < 0 && !trigger) ? "-" : "+") + kSdlAxis[t.idx];
}

std::string pcsx2_ini_defaults(const std::string& in) {
    std::string t = in;
    t = hubui::ini_set(t, "UI", "SetupWizardIncomplete", "false");
    t = hubui::ini_set(t, "UI", "ConfirmShutdown", "false");
    t = hubui::ini_set(t, "UI", "StartFullscreen", "true");
    t = hubui::ini_set(t, "UI", "InhibitScreensaver", "true");
    t = hubui::ini_set(t, "UI", "DiscordPresence", "false");
    t = hubui::ini_set(t, "UI", "PauseOnFocusLoss", "false");
    t = hubui::ini_set(t, "AutoUpdater", "CheckAtStartup", "false");
    t = hubui::ini_set(t, "EmuCore", "EnableFastBoot", "true");
    t = hubui::ini_set(t, "InputSources", "Keyboard", "true");
    t = hubui::ini_set(t, "InputSources", "Mouse", "true");
    t = hubui::ini_set(t, "InputSources", "SDL", "true");
    t = hubui::ini_set(t, "InputSources", "SDLControllerEnhancedMode", "false");
    t = hubui::ini_set(t, "InputSources", "XInput", "false");
    return t;
}

static std::string num(double v) { char b[32]; std::snprintf(b, sizeof b, "%g", v); return b; }

static std::string first_binding(const hubctl::Profile& prof, const std::string& control, int sdl) {
    auto it = prof.buttons.find(control);
    std::vector<hubctl::Term> terms;
    if (it == prof.buttons.end() || !hubctl::parse_expr(it->second, prof.axis_threshold, &terms)) return "";
    for (const auto& t : terms) { std::string b = pcsx2_binding(t, sdl); if (!b.empty()) return b; }
    return "";
}

std::string pcsx2_ini_pads(const std::string& in, const Ps2Pads& pads, const hubctl::Profile& prof) {
    std::string t = in;
    t = hubui::ini_set(t, "Pad", "MultitapPort1", pads.multitap ? "true" : "false");
    t = hubui::ini_set(t, "Pad", "MultitapPort2", "false");
    /* player n -> [PadN]: without a multitap Pad1, Pad2; with it Pad1, Pad3, Pad4, Pad5 (port 1 multitap), Pad2 is port 2. */
    static const int kPlain[4] = {1, 2, 0, 0}, kTap[4] = {1, 3, 4, 5};
    bool used[9] = {};
    struct M { const char* control; const char* key; const char* kb; };
    static const M map[] = {
        {"up", "Up", "Keyboard/Up"}, {"right", "Right", "Keyboard/Right"}, {"down", "Down", "Keyboard/Down"}, {"left", "Left", "Keyboard/Left"},
        {"triangle", "Triangle", "Keyboard/I"}, {"circle", "Circle", "Keyboard/L"}, {"cross", "Cross", "Keyboard/K"}, {"square", "Square", "Keyboard/J"},
        {"select", "Select", "Keyboard/Backspace"}, {"start", "Start", "Keyboard/Return"},
        {"l1", "L1", "Keyboard/Q"}, {"l2", "L2", "Keyboard/1"}, {"r1", "R1", "Keyboard/E"}, {"r2", "R2", "Keyboard/3"},
        {"l3", "L3", "Keyboard/2"}, {"r3", "R3", "Keyboard/4"},
    };
    struct Stick { const char* key; const char* kb; const char* axis; int sign; bool right; };
    static const Stick sticks[] = {
        {"LUp", "Keyboard/W", "y", -1, false}, {"LRight", "Keyboard/D", "x", 1, false}, {"LDown", "Keyboard/S", "y", 1, false}, {"LLeft", "Keyboard/A", "x", -1, false},
        {"RUp", "Keyboard/T", "y", -1, true}, {"RRight", "Keyboard/H", "x", 1, true}, {"RDown", "Keyboard/G", "y", 1, true}, {"RLeft", "Keyboard/F", "x", -1, true},
    };
    auto write_pad = [&](int n, int sdl) {   // sdl < 0: keyboard
        std::string sec = "Pad" + std::to_string(n);
        used[n] = true;
        t = hubui::ini_set(t, sec, "Type", "DualShock2");
        t = hubui::ini_set(t, sec, "PressureModifier", "0.5");
        for (const auto& m : map) t = hubui::ini_set(t, sec, m.key, sdl < 0 ? m.kb : first_binding(prof, m.control, sdl));
        for (const auto& s : sticks) {
            std::string v;
            if (sdl < 0) v = s.kb;
            else {
                auto it = prof.analogs.find(s.right ? "right_stick" : "left_stick");
                std::string ax = it == prof.analogs.end() ? "" : (s.axis[0] == 'x' ? it->second.x : it->second.y);
                int ai = hubctl::axis_index(ax);
                if (ai >= 0) { hubctl::Term tm; tm.axis = true; tm.idx = ai; tm.sign = s.sign; v = pcsx2_binding(tm, sdl); }
            }
            t = hubui::ini_set(t, sec, s.key, v);
        }
        if (sdl >= 0) {
            t = hubui::ini_set(t, sec, "LargeMotor", "SDL-" + std::to_string(sdl) + "/LargeMotor");
            t = hubui::ini_set(t, sec, "SmallMotor", "SDL-" + std::to_string(sdl) + "/SmallMotor");
            auto it = prof.analogs.find("left_stick");
            double dz = it == prof.analogs.end() ? 0.0 : it->second.deadzone;
            if (dz > 0) t = hubui::ini_set(t, sec, "Deadzone", num(dz));
        } else {
            t = hubui::ini_set(t, sec, "LargeMotor", "");
            t = hubui::ini_set(t, sec, "SmallMotor", "");
        }
    };
    if (pads.count == 0) write_pad(1, -1);
    for (int i = 0; i < pads.count && i < 4; ++i) {
        int n = pads.multitap ? kTap[i] : kPlain[i];
        write_pad(n, pads.sdl[i]);
    }
    for (int n = 1; n <= 8; ++n)
        if (!used[n]) t = hubui::ini_set(t, "Pad" + std::to_string(n), "Type", "None");
    return t;
}

/* ------------------------------------------------------------------ per launch */

static std::string gv(const hub::gs::Values& v, const char* k, const char* d) { auto i = v.find(k); return i == v.end() ? std::string(d) : i->second; }

std::string pcsx2_ini_launch(const std::string& in, const Bios& bios, const std::string& card_rel, const hub::gs::Values& v) {
    std::string t = in;
    if (!bios.file.empty()) {
        t = hubui::ini_set(t, "Folders", "Bios", bios.dir);
        t = hubui::ini_set(t, "Filenames", "BIOS", bios.file);
    }
    t = hubui::ini_set(t, "MemoryCards", "Slot1_Enable", "true");
    t = hubui::ini_set(t, "MemoryCards", "Slot1_Filename", card_rel);
    t = hubui::ini_set(t, "MemoryCards", "Slot2_Enable", "false");
    std::string r = gv(v, "ps2.renderer", "auto");
    t = hubui::ini_set(t, "EmuCore/GS", "Renderer", r == "vulkan" ? "14" : r == "dx12" ? "15" : r == "dx11" ? "3" : "-1");
    t = hubui::ini_set(t, "EmuCore/GS", "upscale_multiplier", gv(v, "ps2.res", "2"));
    t = hubui::ini_set(t, "EmuCore", "EnableWideScreenPatches", gv(v, "ps2.widescreen", "false") == "true" ? "true" : "false");
    return t;
}

std::string pcsx2_card_rel(const std::string& key) { return "RetroHub/" + pcsx2_safe_key(key) + "/Mcd001.ps2"; }

std::string prepare_pcsx2(const std::string& install_dir, const Ps2Pads& pads, const hubctl::Profile& prof, const Bios& bios,
                          const std::string& key, const hub::gs::Values& v) {
    if (!fs::exists(P(join(install_dir, "portable.ini")))) write_text(join(install_dir, "portable.ini"), "");   // never touches the user's own PCSX2
    std::string ini = join(install_dir, "inis\\PCSX2.ini");
    std::string marker = join(install_dir, "inis\\retrohub-defaults.txt");
    std::string want = hubctl::hash_hex(hubctl::mapping_hash(prof));
    for (int i = 0; i < pads.count; ++i) want += ";" + std::to_string(pads.sdl[i]);
    want += pads.multitap ? ";tap" : "";
    std::string have = read_text(marker);
    std::string t = read_text(ini);
    bool first = have.empty();
    if (first) t = pcsx2_ini_defaults(t);
    if (first || have.find("pads: " + want + "\n") == std::string::npos) t = pcsx2_ini_pads(t, pads, prof);
    t = pcsx2_ini_launch(t, bios, pcsx2_card_rel(key), v);
    if (!write_text(ini, t)) return "could not write PCSX2.ini";
    if (first || have.find("pads: " + want + "\n") == std::string::npos)
        write_text(marker, "RetroHub wrote PCSX2's defaults and pad bindings. Pads are rewritten when the RetroHub profile or pads change.\npads: " + want + "\n");
    std::error_code ec;
    fs::create_directories(P(join(install_dir, hubemu::pcsx2_card_dir_rel(key))), ec);
    return "";
}

std::string pcsx2_args(const std::string& game, bool fullscreen) {
    return std::string("-batch -nogui ") + (fullscreen ? "-fullscreen" : "-nofullscreen") + " -- " + winutil::quote_arg(game);
}

}  // namespace hubemu
