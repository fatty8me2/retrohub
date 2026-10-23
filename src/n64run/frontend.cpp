/* n64run: mupen64plus 2.6.0 frontend with lockstep netplay.
 *
 *   n64run --rom PATH --game ID [--net SERVER --room CODE-N --slot K --players N
 *          --ports P0,P1,P2,P3 --delay auto|D [--share-host-save --host-save-dir DIR]]
 *          [--save-dir DIR] [--emu-dir DIR] [--width W --height H] [--strict-sync] [--no-hash]
 *
 * Exit codes (documented in docs/ARCHITECTURE.md): 0 ok, 2 bad args / ROM, 3 emulator load failure,
 *             4 netplay connection failure / connection lost, 5 desync detected (only with --strict-sync),
 *             6 host-save sync failed.
 * --slot may be any of 0..3 (lobby slots can have gaps); --players is the number of PCs.
 */
#include <windows.h>
#include <shellapi.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "m64p_common.h"
#include "m64p_config.h"
#include "m64p_debugger.h"
#include "m64p_frontend.h"
#include "m64p_plugin.h"
#include "m64p_types.h"

#include "games.h"
#include "game_settings.h"
#include "session.h"

#include "headless.h"
#include "controls_sdl.h"
#include "local_input.h"
#include "test_input.h"
#include "mempak.h"
#include "match.h"
#include "lockstep.h"
#include "runstate.h"
#include "save_sync.h"

namespace fs = std::filesystem;
using namespace n64run;

// Exports of input_plugin.cpp (this exe is the input plugin).
extern "C" m64p_error PluginStartup(m64p_dynlib_handle, void*, void (*)(void*, int, const char*));

static const int kFrontendApiVersion = 0x020102;   // ui-console's value for core 2.x
static const char* kUsage =
    "usage: n64run --rom PATH [--game ID] [--net SERVER --room CODE-N --slot K --players N\n"
    "              --ports P0,P1,P2,P3 --delay auto|D [--share-host-save --host-save-dir DIR]]\n"
    "              [--match SPEC] [--save-dir DIR] [--emu-dir DIR] [--width W --height H] [--strict-sync] [--no-hash]\n"
    "              [--title TEXT] [--fullscreen] [--gfx Key=i|b|s:value]...\n"
    "              [--headless [--realtime] [--frames N] [--screenshot OUT.png] [--dump-ram OFFSET,LEN,FILE]...]\n";

// ---------------------------------------------------------------- logging
static FILE* g_logf = nullptr;

static void logline(const char* tag, const char* msg) {
    char line[1400];
    std::snprintf(line, sizeof line, "[%s] %s\n", tag, msg);
    std::fputs(line, stderr);
    if (g_logf) { std::fputs(line, g_logf); std::fflush(g_logf); }
}
static void logf_(const char* tag, const char* fmt, ...) {
    char buf[1200];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logline(tag, buf);
}
static void m64p_debug_cb(void* ctx, int level, const char* msg) {
    static const char* names[] = {"?", "ERROR", "WARN", "INFO", "STATUS", "VERBOSE"};
    const char* ctxs = ctx ? static_cast<const char*>(ctx) : "m64p";
    char tag[96];
    std::snprintf(tag, sizeof tag, "%s %s", ctxs, level >= 1 && level <= 5 ? names[level] : "?");
    logline(tag, msg);
}

// ---------------------------------------------------------------- args
struct Args {
    std::string rom, game, net, room, save_dir, emu_dir, host_save_dir;
    int slot = 0, players = 1, delay = -1;   // delay -1 = auto (online)
    uint32_t slot_mask = 1;
    int owner[kPorts] = {0, -1, -1, -1};
    bool ports_given = false;
    bool strict_sync = false, no_hash = false, share_host_save = false;
    bool fullscreen = false;
    std::vector<std::string> gfx;   // --gfx Key=t:value (GLideN64 settings from game_settings.h)
    std::string title;   // window caption (UTF-8), from --title; empty = leave the plugin's title
    int width = 960, height = 720;
    bool width_given = false, height_given = false;
    // --headless: hidden window, no audio device, no speed limit; stop after `frames` VIs (0 = until the session ends).
    bool headless = false;
    bool realtime = false;   // --realtime (with --headless): keep the 60 Hz speed limiter, like a real game (QA of stalls)
    long long frames = 0;
    std::string screenshot;
    std::string controls;   // --controls FILE / RETROHUB_CONTROLS: resolved controller profile JSON (controls.h)
    std::string test_input;   // --test-input / N64RUN_TEST_INPUT: scripted local input (QA)
    std::string match;   // --match spec (VS battle jump)
    struct RamDump { uint32_t offset, len; std::string file; };
    std::vector<RamDump> dumps;
};

static bool parse_ports(const std::string& s, int owner[kPorts]) {
    std::stringstream ss(s);
    std::string tok;
    int i = 0;
    while (std::getline(ss, tok, ',')) {
        if (i >= kPorts) return false;
        char* end = nullptr;
        long v = std::strtol(tok.c_str(), &end, 10);
        if (end == tok.c_str() || *end) return false;
        owner[i++] = (int)v;
    }
    for (; i < kPorts; i++) owner[i] = -1;
    return true;
}

static bool parse_args(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        auto val = [&](std::string* out) { if (i + 1 >= argc) return false; *out = argv[++i]; return true; };
        auto ival = [&](int* out) { std::string s; if (!val(&s)) return false; char* e; long v = std::strtol(s.c_str(), &e, 10); if (e == s.c_str() || *e) return false; *out = (int)v; return true; };
        bool ok = true;
        if (k == "--rom") ok = val(&a.rom);
        else if (k == "--game") ok = val(&a.game);
        else if (k == "--net") ok = val(&a.net);
        else if (k == "--room") ok = val(&a.room);
        else if (k == "--slot") ok = ival(&a.slot);
        else if (k == "--players") ok = ival(&a.players);
        else if (k == "--delay") { std::string s; ok = val(&s); if (ok && s == "auto") a.delay = -1; else if (ok) { char* e; long v = std::strtol(s.c_str(), &e, 10); ok = e != s.c_str() && !*e; a.delay = (int)v; if (ok && v < 0) ok = false; } }
        else if (k == "--width") { ok = ival(&a.width); a.width_given = true; }
        else if (k == "--height") { ok = ival(&a.height); a.height_given = true; }
        else if (k == "--headless") a.headless = true;
        else if (k == "--realtime") a.realtime = true;
        else if (k == "--frames") { int f = 0; ok = ival(&f) && f >= 0; a.frames = f; }
        else if (k == "--screenshot") ok = val(&a.screenshot);
        else if (k == "--match") ok = val(&a.match);
        else if (k == "--dump-ram") {
            std::string s;
            ok = val(&s);
            if (ok) {
                size_t c1 = s.find(','), c2 = c1 == std::string::npos ? c1 : s.find(',', c1 + 1);
                ok = c2 != std::string::npos;
                if (ok) {
                    char *e1, *e2;
                    const std::string soff = s.substr(0, c1), slen = s.substr(c1 + 1, c2 - c1 - 1);   // keep alive: e1/e2 point into them
                    unsigned long long off = std::strtoull(soff.c_str(), &e1, 0);
                    unsigned long long len = std::strtoull(slen.c_str(), &e2, 0);
                    std::string file = s.substr(c2 + 1);
                    ok = !*e1 && !*e2 && len > 0 && off + len <= 0x800000ull && !file.empty();
                    if (ok) a.dumps.push_back({(uint32_t)off, (uint32_t)len, file});
                }
            }
        }
        else if (k == "--save-dir") ok = val(&a.save_dir);
        else if (k == "--emu-dir") ok = val(&a.emu_dir);
        else if (k == "--host-save-dir") ok = val(&a.host_save_dir);
        else if (k == "--share-host-save") a.share_host_save = true;
        else if (k == "--ports") { std::string s; ok = val(&s) && parse_ports(s, a.owner); a.ports_given = ok; }
        else if (k == "--strict-sync") a.strict_sync = true;
        else if (k == "--no-hash") a.no_hash = true;
        else if (k == "--fullscreen") a.fullscreen = true;
        else if (k == "--title") ok = val(&a.title);
        else if (k == "--gfx") { std::string g; hub::gs::GfxParam gp; ok = val(&g) && hub::gs::parse_gfx_arg(g, &gp); if (ok) a.gfx.push_back(g); }
        else if (k == "--test-input") ok = val(&a.test_input);
        else if (k == "--controls") ok = val(&a.controls);
        else { std::fprintf(stderr, "unknown argument: %s\n", k.c_str()); return false; }
        if (!ok) { std::fprintf(stderr, "bad value for %s\n", k.c_str()); return false; }
    }
    if (a.controls.empty()) { const char* e = std::getenv("RETROHUB_CONTROLS"); if (e && *e) a.controls = e; }
    if (a.test_input.empty()) { const char* e = std::getenv("N64RUN_TEST_INPUT"); if (e && *e) a.test_input = e; }
    if (a.rom.empty()) { std::fprintf(stderr, "--rom is required\n"); return false; }
    if (!a.headless && (a.frames || !a.screenshot.empty() || !a.dumps.empty())) {
        std::fprintf(stderr, "--frames/--screenshot/--dump-ram need --headless\n"); return false;
    }
    if (a.delay > 30) { std::fprintf(stderr, "--delay must be auto or 0..30\n"); return false; }
    if (a.share_host_save && a.net.empty()) { std::fprintf(stderr, "--share-host-save needs --net\n"); return false; }
    if (!a.net.empty()) {
        if (a.room.empty()) { std::fprintf(stderr, "--net requires --room\n"); return false; }
        if (!a.ports_given) { std::fprintf(stderr, "--net requires --ports\n"); return false; }
        std::string err;
        a.slot_mask = derive_slot_mask(a.owner, a.slot, a.players, &err);
        if (!a.slot_mask) { std::fprintf(stderr, "bad --players/--slot/--ports: %s\n", err.c_str()); return false; }
    }
    bool any = false;
    for (int p = 0; p < kPorts; p++) {
        if (a.owner[p] < -1 || a.owner[p] > 3) { std::fprintf(stderr, "bad --ports value\n"); return false; }
        any |= a.owner[p] >= 0;
    }
    if (a.ports_given && !any) { std::fprintf(stderr, "--ports has no controllers\n"); return false; }
    return true;
}

// ---------------------------------------------------------------- headless frame counter
static std::atomic<bool> g_stop_watchdog{false};
static long long g_frame_limit = 0;      // 0 = unlimited
static long long g_frame_count = 0;
static bool g_limiter_off = false;
static bool g_realtime = false;
static std::unique_ptr<MatchDriver> g_match;
static long long g_vi = 0;
static bool g_headless_mode = false;

// ---------------------------------------------------------------- emulator
struct Core {
    HMODULE dll = nullptr;
    ptr_CoreStartup Startup = nullptr;
    ptr_CoreShutdown Shutdown = nullptr;
    ptr_CoreAttachPlugin Attach = nullptr;
    ptr_CoreDetachPlugin Detach = nullptr;
    ptr_CoreDoCommand DoCommand = nullptr;
    ptr_ConfigOpenSection OpenSection = nullptr;
    ptr_ConfigSetParameter SetParam = nullptr;
    ptr_ConfigSaveFile SaveFile = nullptr;
    ptr_DebugMemGetPointer MemPtr = nullptr;
    ptr_CoreOverrideVidExt OverrideVidExt = nullptr;
};
static Core g_core;

/* --headless: called by the core on every VI (emulator thread). Drops the speed limiter on the first
 * call (it cannot be changed before the emulation thread runs) and stops after --frames VIs. */
static void headless_frame_cb(unsigned int) {
    g_vi++;
    if (g_match && g_core.MemPtr && !g_match->done()) {
        std::string l = g_match->step(static_cast<uint8_t*>(g_core.MemPtr(M64P_DBG_PTR_RDRAM)), g_vi);
        if (!l.empty()) { std::stringstream ls(l); std::string one; while (std::getline(ls, one)) logline("n64run", one.c_str()); }
    }
    if (!g_headless_mode) return;
    if (!g_limiter_off && !g_realtime) {
        int off = 0;
        g_core.DoCommand(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &off);
        g_limiter_off = true;
    }
    g_frame_count++;
    if (g_frame_limit > 0) {
        if (g_frame_count == g_frame_limit - 1) headless_set_shot_mode(ShotMode::Armed);
        if (g_frame_count >= g_frame_limit) g_core.DoCommand(M64CMD_STOP, 0, nullptr);
    }
}

static bool file_exists(const std::string& p) { std::error_code ec; return fs::exists(fs::path(p), ec); }

static std::string find_emu_dir(const std::string& requested) {
    std::vector<std::string> cands;
    if (!requested.empty()) cands.push_back(requested);
    std::string ed = hub::exe_dir();
    cands.push_back(ed + "emu");
    cands.push_back(ed + "..\\..\\deps\\emu");
    cands.push_back(ed + "..\\..\\..\\deps\\emu");
    for (auto& c : cands) {
        fs::path p = fs::absolute(fs::path(c)).lexically_normal();
        if (file_exists((p / "mupen64plus.dll").string())) return p.string();
    }
    return "";
}

template <class T> static bool sym(HMODULE m, const char* name, T* out) {
    *out = reinterpret_cast<T>(GetProcAddress(m, name));
    if (!*out) logf_("n64run", "missing export %s", name);
    return *out != nullptr;
}

static HMODULE load_dll(const std::string& dir, const char* name) {
    std::string p = dir + "\\" + name;
    HMODULE m = LoadLibraryExA(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) logf_("n64run", "cannot load %s (error %lu)", p.c_str(), GetLastError());
    return m;
}

static void cfg_int(m64p_handle h, const char* k, int v) { g_core.SetParam(h, k, M64TYPE_INT, &v); }
static void cfg_bool(m64p_handle h, const char* k, int v) { g_core.SetParam(h, k, M64TYPE_BOOL, &v); }
static void cfg_str(m64p_handle h, const char* k, const std::string& v) { g_core.SetParam(h, k, M64TYPE_STRING, v.c_str()); }

static std::string with_sep(std::string s) {
    if (!s.empty() && s.back() != '\\' && s.back() != '/') s += '\\';
    return s;
}

struct PluginDll {
    HMODULE h = nullptr;
    ptr_PluginStartup startup = nullptr;
};

/* ---- window title -----------------------------------------------------------
 * The GLideN64 plugin creates and captions the game window itself, so we find it
 * (top-level, visible, owned by this process) and set our caption. The plugin may
 * retitle it later, so the watcher re-checks every 2 s until emulation ends. */
static std::atomic<bool> g_title_stop{false};

static BOOL CALLBACK find_game_window(HWND hwnd, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    if (GetWindowTextLengthW(hwnd) == 0) return TRUE;   // needs a caption
    *reinterpret_cast<HWND*>(lp) = hwnd;
    return FALSE;
}

static void title_watch(std::wstring title) {
    using namespace std::chrono;
    const auto start = steady_clock::now();
    while (!g_title_stop.load()) {
        HWND hwnd = nullptr;
        EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&hwnd));
        if (hwnd) {
            wchar_t cur[512] = {0};
            GetWindowTextW(hwnd, cur, 512);
            if (title != cur)   // timeout: the window thread may be busy shutting down
                SendMessageTimeoutW(hwnd, WM_SETTEXT, 0, (LPARAM)title.c_str(), SMTO_ABORTIFHUNG, 200, nullptr);
        }
        bool early = steady_clock::now() - start < seconds(10);
        for (int i = 0; i < (early ? 1 : 8) && !g_title_stop.load(); i++) std::this_thread::sleep_for(milliseconds(250));
    }
}

/* --title arrives as UTF-8 on the command line; take the wide command line so non-ASCII
 * (the em dash) survives the ANSI argv conversion that narrow main() would do. */
static std::wstring wide_title_arg(const std::string& fallback_utf8) {
    int n = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    std::wstring out;
    if (wargv) {
        for (int i = 1; i + 1 < n; i++) if (wcscmp(wargv[i], L"--title") == 0) { out = wargv[i + 1]; break; }
        LocalFree(wargv);
    }
    if (out.empty() && !fallback_utf8.empty()) {
        int m = MultiByteToWideChar(CP_UTF8, 0, fallback_utf8.c_str(), -1, nullptr, 0);
        out.assign(m > 0 ? m - 1 : 0, L'\0');
        if (m > 1) MultiByteToWideChar(CP_UTF8, 0, fallback_utf8.c_str(), -1, out.data(), m);
    }
    return out;
}

static bool start_plugin(PluginDll& p, const std::string& emu_dir, const char* file, const char* ctx) {
    p.h = load_dll(emu_dir, file);
    if (!p.h) return false;
    if (!sym(p.h, "PluginStartup", &p.startup)) return false;
    // First arg is the CORE library handle: plugins resolve the core's Config* API from it.
    m64p_error e = p.startup(g_core.dll, const_cast<char*>(ctx), m64p_debug_cb);
    if (e != M64ERR_SUCCESS) { logf_("n64run", "%s PluginStartup failed (%d)", file, (int)e); return false; }
    return true;
}

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) { std::fputs(kUsage, stderr); return 2; }

    // ---- logging
    {
        std::error_code ec;
        fs::create_directories(hub::data_dir() + "logs", ec);
        g_logf = std::fopen((hub::data_dir() + "logs\\n64run.log").c_str(), "w");
    }
    logf_("n64run", "starting; rom=%s game=%s net=%s", args.rom.c_str(), args.game.c_str(), args.net.empty() ? "(offline)" : args.net.c_str());

    // ---- ROM
    std::vector<uint8_t> rom;
    if (!hub::load_rom(args.rom, &rom)) { logf_("n64run", "not a valid N64 ROM: %s", args.rom.c_str()); return 2; }
    std::string rom_header_name;
    if (args.game.empty()) {
        hub::RomFile rf;
        if (hub::identify_rom(args.rom, &rf) && !rf.game_id.empty()) args.game = rf.game_id;
        else args.game = "unknown";
        rom_header_name = rf.header_name;
    } else {
        hub::RomFile rf;
        if (hub::identify_rom(args.rom, &rf)) rom_header_name = rf.header_name;
    }

    const bool online = !args.net.empty();
    const bool delay_auto = args.delay < 0;   // no explicit --delay: negotiate, then adapt during play

    // ---- ports / local input
    LocalInput input;
    if (!args.headless) hubctl::load_bundled_db();   // SDL_GameControllerDB: DualSense, Switch Pro, 8BitDo ...
    if (!args.controls.empty()) {
        hubctl::Profile prof;
        if (hubctl::read_profile_file(args.controls, &prof) && prof.system == "n64") {
            input.set_profile(prof);
            logf_("n64run", "controls: profile '%s' (%s)", prof.name.c_str(), args.controls.c_str());
        } else logf_("n64run", "controls: cannot use %s; built-in mapping", args.controls.c_str());
    }
    // Headless never reads pads or the keyboard: stray input must not reach a background test run.
    if (!args.headless && !input.init()) logf_("n64run", "SDL game controller init failed; keyboard only");
    if (!online && !args.ports_given) {
        // Offline default: one port per connected pad (at least one, for the keyboard), max 4.
        int n = std::max(1, std::min(4, input.pad_count()));
        for (int p = 0; p < kPorts; p++) args.owner[p] = p < n ? 0 : -1;
    }
    int my_slot = online ? args.slot : 0;
    TestInput test_input;
    if (!args.test_input.empty()) {
        std::string terr;
        if (!test_input.configure(args.test_input, my_slot, &terr)) { logf_("n64run", "%s", terr.c_str()); return 2; }
        logf_("n64run", "test input: %s (slot %d)", args.test_input.c_str(), my_slot);
    }
    if (!online) for (int p = 0; p < kPorts; p++) if (args.owner[p] >= 0) args.owner[p] = 0;
    for (int p = 0; p < kPorts; p++) g_run.owner[p] = args.owner[p];
    g_run.mempak = mempak_enabled(online, args.share_host_save);
    logf_("n64run", "controller paks: %s", g_run.mempak ? "on (saved as .mpk in the save dir)" : "off (online without shared host saves)");
    logf_("n64run", "ports: %d %d %d %d (my slot %d, %d pad(s))", args.owner[0], args.owner[1], args.owner[2], args.owner[3], my_slot, input.pad_count());

    // ---- directories
    std::error_code ec;
    std::string save_dir;
    if (!args.save_dir.empty()) save_dir = args.save_dir;
    else if (online) {
        auto t = std::chrono::system_clock::now().time_since_epoch().count();
        // Throwaway, outside the save vault (the vault is synced; netplay sessions must never reach it).
        save_dir = hub::data_dir() + "netplay\\n64\\" + args.game + "-" + std::to_string((long long)t);
    } else save_dir = hub::data_dir() + "saves\\" + args.game;
    fs::create_directories(save_dir, ec);
    if (online) {
        size_t entries = 0;
        for (auto& e : fs::directory_iterator(save_dir, ec)) { (void)e; entries++; }
        if (entries) logf_("n64run", "WARNING: online save dir %s is not empty; peers may start from different saves", save_dir.c_str());
    }
    std::string cfg_dir = hub::data_dir() + "n64cfg\\" + std::to_string(GetCurrentProcessId());
    fs::remove_all(cfg_dir, ec);
    fs::create_directories(cfg_dir, ec);

    // ---- load emulator
    std::string emu_dir = find_emu_dir(args.emu_dir);
    if (emu_dir.empty()) { logf_("n64run", "cannot find emu\\mupen64plus.dll (looked next to the exe, ..\\..\\deps\\emu; use --emu-dir)"); return 3; }
    logf_("n64run", "emu dir: %s", emu_dir.c_str());
    SetDllDirectoryA(emu_dir.c_str());
    g_core.dll = load_dll(emu_dir, "mupen64plus.dll");
    if (!g_core.dll) return 3;
    bool ok = sym(g_core.dll, "CoreStartup", &g_core.Startup) && sym(g_core.dll, "CoreShutdown", &g_core.Shutdown) &&
              sym(g_core.dll, "CoreAttachPlugin", &g_core.Attach) && sym(g_core.dll, "CoreDetachPlugin", &g_core.Detach) &&
              sym(g_core.dll, "CoreDoCommand", &g_core.DoCommand) && sym(g_core.dll, "ConfigOpenSection", &g_core.OpenSection) &&
              sym(g_core.dll, "ConfigSetParameter", &g_core.SetParam) && sym(g_core.dll, "ConfigSaveFile", &g_core.SaveFile);
    if (!ok) return 3;
    sym(g_core.dll, "DebugMemGetPointer", &g_core.MemPtr);   // optional
    if (args.headless && !sym(g_core.dll, "CoreOverrideVidExt", &g_core.OverrideVidExt)) return 3;

    m64p_error e = g_core.Startup(kFrontendApiVersion, cfg_dir.c_str(), emu_dir.c_str(), const_cast<char*>("Core"), m64p_debug_cb, nullptr, nullptr);
    if (e != M64ERR_SUCCESS) { logf_("n64run", "CoreStartup failed (%d)", (int)e); return 3; }

    // ---- forced deterministic configuration
    m64p_handle core_cfg = nullptr, vid_cfg = nullptr, ev_cfg = nullptr;
    if (g_core.OpenSection("Core", &core_cfg) != M64ERR_SUCCESS) { logf_("n64run", "cannot open Core config"); return 3; }
    cfg_int(core_cfg, "R4300Emulator", 2);
    cfg_bool(core_cfg, "RandomizeInterrupt", 0);
    cfg_int(core_cfg, "CountPerOp", 0);
    cfg_bool(core_cfg, "DisableExtraMem", 0);
    cfg_int(core_cfg, "SiDmaDuration", -1);
    cfg_bool(core_cfg, "EnableDebugger", 0);
    cfg_bool(core_cfg, "OnScreenDisplay", args.headless ? 0 : 1);
    cfg_bool(core_cfg, "AutoStateSlotIncrement", 0);
    cfg_str(core_cfg, "SaveSRAMPath", with_sep(save_dir));
    cfg_str(core_cfg, "SaveStatePath", with_sep(save_dir));
    cfg_str(core_cfg, "SharedDataPath", emu_dir);
    // Disable emulator hotkeys that change emulation state (keep Esc = stop, Alt+Enter = fullscreen).
    if (g_core.OpenSection("CoreEvents", &ev_cfg) == M64ERR_SUCCESS) {
        static const char* off[] = {"Kbd Mapping Save State", "Kbd Mapping Load State", "Kbd Mapping Increment Slot",
                                    "Kbd Mapping Reset", "Kbd Mapping Speed Down", "Kbd Mapping Speed Up",
                                    "Kbd Mapping Pause", "Kbd Mapping Fast Forward", "Kbd Mapping Frame Advance",
                                    "Kbd Mapping Gameshark"};
        for (const char* k : off) cfg_int(ev_cfg, k, 0);
    }
    if (g_core.OpenSection("Video-General", &vid_cfg) == M64ERR_SUCCESS) {
        cfg_bool(vid_cfg, "Fullscreen", args.fullscreen && !args.headless ? 1 : 0);
        // Headless renders into a hidden window; default to a small N64-native size (faster) unless given.
        cfg_int(vid_cfg, "ScreenWidth", args.headless && !args.width_given ? 640 : args.width);
        cfg_int(vid_cfg, "ScreenHeight", args.headless && !args.height_given ? 480 : args.height);
        cfg_bool(vid_cfg, "VerticalSync", args.headless ? 0 : 1);
    }
    {
        /* Test runs (and anyone who sets it) stay silent: N64RUN_MUTE=1 / SSB64_MUTE=1. */
        const char* m1 = std::getenv("N64RUN_MUTE");
        const char* m2 = std::getenv("SSB64_MUTE");
        bool mute = (m1 && m1[0] && m1[0] != '0') || (m2 && m2[0] && m2[0] != '0');
        m64p_handle aud_cfg = nullptr;
        if (mute && g_core.OpenSection("Audio-SDL", &aud_cfg) == M64ERR_SUCCESS) {
            cfg_int(aud_cfg, "VOLUME_DEFAULT", 0);
        }
    }
    g_core.SaveFile();

    // ---- netplay session. This runs before ROM_OPEN: the core reads battery saves when the ROM opens,
    // so the host's saves (--share-host-save) must already be in the save dir by then.
    hubnet::Session session;
    if (online) {
        hubnet::SessionConfig sc;
        sc.server = args.net;
        sc.room = args.room;
        sc.slot = args.slot;
        sc.expected = args.players;
        if (!session.start(sc)) {
            logf_("n64run", "netplay: cannot start session (%s)", session.status().c_str());
            g_core.Shutdown(); fs::remove_all(cfg_dir, ec); return 4;
        }
        uint32_t t0 = hubnet::now_ms();
        std::string last;
        while (!session.ready()) {
            session.update();
            if (session.failed() || hubnet::now_ms() - t0 > 15000) {
                logf_("n64run", "netplay connection failed: %s", session.status().c_str());
                session.stop(); g_core.Shutdown(); fs::remove_all(cfg_dir, ec); return 4;
            }
            std::string st = session.status();
            if (st != last) { logf_("n64run", "netplay: %s", st.c_str()); last = st; }
            Sleep(10);
        }
        logf_("n64run", "netplay: all %d players reachable", args.players);

        {
            DelayNegotiation dn;
            dn.fixed_delay = args.delay;
            dn.my_slot = my_slot;
            dn.slot_mask = args.slot_mask;
            dn.frame_ms = 1000.0 / 60.0;
            int d = 2;
            if (!negotiate_delay(session, dn, [] {}, [](const std::string& m) { logline("n64run", m.c_str()); }, &d)) {
                logf_("n64run", "netplay: input delay negotiation failed (%s)", session.status().c_str());
                session.stop(); g_core.Shutdown(); fs::remove_all(cfg_dir, ec); return 4;
            }
            args.delay = d;
        }
        logf_("n64run", "input delay: %d (%s)", args.delay, "in effect");

        if (args.share_host_save) {
            std::vector<SaveFile> host_files;
            if (my_slot == 0) {
                std::vector<std::string> skipped;
                std::error_code lec;
                if (!args.host_save_dir.empty() && !fs::exists(fs::path(args.host_save_dir), lec)) {
                    // The host has never played this game: a valid state. Guests get an explicit "no files" HDR.
                    logf_("n64run", "save sync: host save dir %s does not exist; sharing NO save files", args.host_save_dir.c_str());
                } else if (!load_save_dir(args.host_save_dir, &host_files, &skipped)) {
                    logf_("n64run", "save sync failed: cannot read host save dir '%s'", args.host_save_dir.c_str());
                    session.stop(); g_core.Shutdown(); fs::remove_all(cfg_dir, ec); return 6;
                }
                for (auto& hf : host_files) {   // host boots from the bytes it sends
                    if (!write_save_file(save_dir, hf.name, hf.data)) {
                        logf_("n64run", "save sync failed: cannot write %s into %s", hf.name.c_str(), save_dir.c_str());
                        session.stop(); g_core.Shutdown(); fs::remove_all(cfg_dir, ec); return 6;
                    }
                }
                if (host_files.empty()) logf_("n64run", "save sync: host has no save files to share (guests will start without saves)");
                else logf_("n64run", "save sync: host shares %zu save file(s) from %s", host_files.size(), args.host_save_dir.c_str());
                for (auto& sk : skipped) logf_("n64run", "save sync: not shared (too large, empty or not a battery save): %s", sk.c_str());
            }
            std::string err;
            if (!run_save_sync(session, my_slot, args.slot_mask, my_slot == 0 ? &host_files : nullptr, save_dir, 20000, &err)) {
                logf_("n64run", "save sync failed: %s", err.c_str());
                session.stop(); g_core.Shutdown(); fs::remove_all(cfg_dir, ec); return 6;
            }
            logf_("n64run", "save sync complete");
        }
    }

    // ---- ROM
    e = g_core.DoCommand(M64CMD_ROM_OPEN, (int)rom.size(), rom.data());
    if (e != M64ERR_SUCCESS) { logf_("n64run", "ROM_OPEN failed (%d)", (int)e); session.stop(); g_core.Shutdown(); return 2; }

    // ---- plugins
    PluginDll gfx, aud, rsp;
    if (args.headless) {
        // Hidden SDL window + GL context for GLideN64; no audio plugin at all (the core uses its dummy audio).
        if (!headless_install_vidext(g_core.OverrideVidExt)) { logf_("n64run", "headless: CoreOverrideVidExt failed"); g_core.DoCommand(M64CMD_ROM_CLOSE, 0, nullptr); g_core.Shutdown(); return 3; }
        if (!args.screenshot.empty() && args.frames == 0) headless_set_shot_mode(ShotMode::Periodic);
        g_frame_limit = args.frames;
    }
    std::string hdr_up; for (char c : rom_header_name) hdr_up += (char)std::toupper((unsigned char)c);
    const bool det_gfx = online && (hdr_up.find("MARIOPARTY") != std::string::npos || args.game == "mp1" || args.game == "mp2" || args.game == "mp3");
    if (!args.gfx.empty() || det_gfx) {
        /* Per-game GLideN64 settings, set before the plugin starts (it keeps existing values). Online, the options that
         * change emulation results (framebuffer emulation) are ignored whatever the caller passed. */
        m64p_handle gh = nullptr;
        if (g_core.OpenSection("Video-GLideN64", &gh) == M64ERR_SUCCESS) {
            if (det_gfx) {
                /* These games keep CPU-visible framebuffers; GLideN64's framebuffer emulation (GPU readbacks into RDRAM) is
                 * timing dependent, so peers' RDRAM (and sometimes game state) diverged. Plain HLE rendering is deterministic. */
                cfg_bool(gh, "EnableFBEmulation", 0);
                cfg_int(gh, "EnableCopyColorToRDRAM", 0);
                cfg_int(gh, "EnableCopyDepthToRDRAM", 0);
                logf_("n64run", "netplay: framebuffer emulation off for %s (deterministic RDRAM)", args.game.c_str());
            }
            for (const std::string& a : args.gfx) {
                hub::gs::GfxParam gp;
                if (!hub::gs::parse_gfx_arg(a, &gp)) continue;
                if (online && hub::gs::n64_key_locked_online(gp.key)) { logf_("n64run", "gfx %s ignored online", gp.key.c_str()); continue; }
                if (gp.type == 'i') cfg_int(gh, gp.key.c_str(), gp.ival);
                else if (gp.type == 'b') cfg_bool(gh, gp.key.c_str(), gp.ival);
                else cfg_str(gh, gp.key.c_str(), gp.sval);
                logf_("n64run", "gfx %s", a.c_str());
            }
        }
    }
    if (!start_plugin(gfx, emu_dir, "mupen64plus-video-GLideN64.dll", "Video") ||
        (!args.headless && !start_plugin(aud, emu_dir, "mupen64plus-audio-sdl.dll", "Audio")) ||
        !start_plugin(rsp, emu_dir, "mupen64plus-rsp-hle.dll", "RSP")) {
        g_core.DoCommand(M64CMD_ROM_CLOSE, 0, nullptr);
        g_core.Shutdown();
        return 3;
    }
    if (PluginStartup(g_core.dll, const_cast<char*>("Input"), m64p_debug_cb) != M64ERR_SUCCESS) return 3;
    HMODULE self = GetModuleHandle(nullptr);
    if (g_core.Attach(M64PLUGIN_GFX, gfx.h) != M64ERR_SUCCESS || (!args.headless && g_core.Attach(M64PLUGIN_AUDIO, aud.h) != M64ERR_SUCCESS) ||
        g_core.Attach(M64PLUGIN_INPUT, self) != M64ERR_SUCCESS || g_core.Attach(M64PLUGIN_RSP, rsp.h) != M64ERR_SUCCESS) {
        logf_("n64run", "CoreAttachPlugin failed");
        g_core.DoCommand(M64CMD_ROM_CLOSE, 0, nullptr);
        g_core.Shutdown();
        return 3;
    }

    if (!args.match.empty()) {
        MatchSpec ms; std::string merr;
        if (!parse_match_spec(args.match, &ms, &merr)) { logf_("n64run", "bad --match: %s", merr.c_str()); g_core.DoCommand(M64CMD_ROM_CLOSE, 0, nullptr); g_core.Shutdown(); return 2; }
        g_match.reset(new MatchDriver(ms, args.owner));
        logf_("n64run", "match: stage %d stocks %d p0..3 present %d%d%d%d", ms.stage, ms.stocks, ms.present[0], ms.present[1], ms.present[2], ms.present[3]);
    }
    {
        LockstepOptions lo;
        for (int p = 0; p < kPorts; p++) lo.owner[p] = args.owner[p];
        lo.my_slot = my_slot;
        lo.num_slots = online ? args.players : 1;
        lo.slot_mask = online ? args.slot_mask : 1u;
        lo.hash_label = "RDRAM";
        if (args.delay < 0) args.delay = 2;   // offline: unused
        lo.delay = args.delay;
        lo.online = online;
        lo.strict_sync = args.strict_sync;
        lo.adaptive_delay = delay_auto && !std::getenv("N64RUN_FIXED_DELAY");
        LockstepHooks hk;
        if (test_input.active()) {
            hk.update_local = [&] { test_input.update(); };
            hk.read_local = [&](int idx) { return test_input.read(idx); };
        } else {
            hk.update_local = [&] { input.update(); };
            hk.read_local = [&](int idx) { return input.read(idx); };
        }
        hk.idle = [] { SDL_PumpEvents(); };
        hk.request_stop = [] { g_core.DoCommand(M64CMD_STOP, 0, nullptr); };
        hk.log = [](const std::string& s) { logline("lockstep", s.c_str()); };
        if (!args.no_hash && g_core.MemPtr) {
            hk.hash_rdram = [](uint64_t* out) {
                void* p = g_core.MemPtr(M64P_DBG_PTR_RDRAM);
                if (!p) return false;
                *out = hash_block(p, 8u * 1024u * 1024u);
                return true;
            };
        } else logf_("n64run", "RDRAM desync hashing disabled");
        g_run.lockstep.reset(new Lockstep(lo, online ? &session : nullptr, hk));

        if (online && !g_run.lockstep->start_barrier(15000)) {
            logf_("n64run", "netplay: start barrier failed (%s)", session.status().c_str());
            g_run.lockstep.reset();
            goto fail_net;
        }

        logf_("n64run", "executing");
        std::thread title_thread;
        std::wstring wtitle = args.headless ? std::wstring() : wide_title_arg(args.title);
        if (!wtitle.empty()) {
            g_title_stop = false;
            title_thread = std::thread(title_watch, wtitle);
        }
        g_headless_mode = args.headless;
        g_realtime = args.realtime;
        if (args.headless || g_match) g_core.DoCommand(M64CMD_SET_FRAME_CALLBACK, 0, reinterpret_cast<void*>(&headless_frame_cb));
        /* Boot watchdog: a game that never reads a controller within 90 s is stuck in its own boot code (seen with a
         * bad Pokemon Stadium dump: it spins in a `bal .` loop at 0x80000198). Say so instead of hanging silently. */
        std::thread([] {
            for (int i = 0; i < 90 * 10; i++) { Sleep(100); if (g_run.lockstep && g_run.lockstep->samples_done() > 0) return; if (g_stop_watchdog) return; }
            logf_("n64run", "ERROR: the game made no controller poll in 90 s - it is stuck in its boot code (bad or modified ROM dump?). Exiting.");
            std::fflush(nullptr);
            ExitProcess(4);
        }).detach();
        e = g_core.DoCommand(M64CMD_EXECUTE, 0, nullptr);   // blocks until the emulation stops
        g_stop_watchdog = true;
        g_title_stop = true;
        if (title_thread.joinable()) title_thread.join();
        logf_("n64run", "emulation ended (%d)", (int)e);
        if (args.headless) {
            logf_("n64run", "headless: %lld VIs", g_frame_count);
            if (g_core.MemPtr) {
                uint8_t* ram = static_cast<uint8_t*>(g_core.MemPtr(M64P_DBG_PTR_RDRAM));
                if (ram) {
                    logf_("n64run", "headless: final RDRAM hash %016llx", (unsigned long long)hash_block(ram, 8u * 1024u * 1024u));
                    for (auto& d : args.dumps) {
                        // RDRAM is stored as host-endian 32-bit words; ^3 on the byte index yields N64 (big-endian) byte order.
                        std::vector<uint8_t> out(d.len);
                        for (uint32_t i = 0; i < d.len; i++) out[i] = ram[(d.offset + i) ^ 3u];
                        FILE* f = std::fopen(d.file.c_str(), "wb");
                        if (f && std::fwrite(out.data(), 1, out.size(), f) == out.size()) logf_("n64run", "headless: dumped %u bytes at 0x%X to %s", d.len, d.offset, d.file.c_str());
                        else logf_("n64run", "headless: cannot write %s", d.file.c_str());
                        if (f) std::fclose(f);
                    }
                } else if (!args.dumps.empty()) logf_("n64run", "headless: RDRAM pointer unavailable; --dump-ram skipped");
            }
            if (!args.screenshot.empty()) {
                if (headless_write_screenshot(args.screenshot)) logf_("n64run", "headless: screenshot %s", args.screenshot.c_str());
                else logf_("n64run", "headless: no frame captured for --screenshot");
            }
        }
        if (online && g_run.lockstep) { g_run.lockstep->log_stall_summary(); g_run.lockstep->finish(); }
    }

    {
        int code = 0;
        if (g_run.lockstep && g_run.lockstep->connection_lost()) { logf_("n64run", "Connection lost"); code = 4; }
        else if (g_run.lockstep && g_run.lockstep->peer_left() && !g_run.lockstep->desync_detected()) logf_("n64run", "a partner ended the session");
        else if (g_run.lockstep && g_run.lockstep->desync_detected()) {
            logf_("n64run", "desync was detected during the session");
            if (args.strict_sync) code = 5;
        } else if (e != M64ERR_SUCCESS) code = 3;

        g_core.DoCommand(M64CMD_ROM_CLOSE, 0, nullptr);
        g_core.Detach(M64PLUGIN_GFX);
        g_core.Detach(M64PLUGIN_AUDIO);
        g_core.Detach(M64PLUGIN_INPUT);
        g_core.Detach(M64PLUGIN_RSP);
        g_core.Shutdown();
        g_run.lockstep.reset();
        session.stop();
        fs::remove_all(cfg_dir, ec);
        return code;
    }

fail_net:
    g_core.DoCommand(M64CMD_ROM_CLOSE, 0, nullptr);
    g_core.Detach(M64PLUGIN_GFX);
    g_core.Detach(M64PLUGIN_AUDIO);
    g_core.Detach(M64PLUGIN_INPUT);
    g_core.Detach(M64PLUGIN_RSP);
    g_core.Shutdown();
    session.stop();
    fs::remove_all(cfg_dir, ec);
    return 4;
}
