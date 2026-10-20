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

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
#include "session.h"

#include "local_input.h"
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
    "              [--save-dir DIR] [--emu-dir DIR] [--width W --height H] [--strict-sync] [--no-hash]\n";

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
    int width = 960, height = 720;
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
        else if (k == "--width") ok = ival(&a.width);
        else if (k == "--height") ok = ival(&a.height);
        else if (k == "--save-dir") ok = val(&a.save_dir);
        else if (k == "--emu-dir") ok = val(&a.emu_dir);
        else if (k == "--host-save-dir") ok = val(&a.host_save_dir);
        else if (k == "--share-host-save") a.share_host_save = true;
        else if (k == "--ports") { std::string s; ok = val(&s) && parse_ports(s, a.owner); a.ports_given = ok; }
        else if (k == "--strict-sync") a.strict_sync = true;
        else if (k == "--no-hash") a.no_hash = true;
        else { std::fprintf(stderr, "unknown argument: %s\n", k.c_str()); return false; }
        if (!ok) { std::fprintf(stderr, "bad value for %s\n", k.c_str()); return false; }
    }
    if (a.rom.empty()) { std::fprintf(stderr, "--rom is required\n"); return false; }
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
};
static Core g_core;

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
    if (args.game.empty()) {
        hub::RomFile rf;
        if (hub::identify_rom(args.rom, &rf) && !rf.game_id.empty()) args.game = rf.game_id;
        else args.game = "unknown";
    }

    const bool online = !args.net.empty();

    // ---- ports / local input
    LocalInput input;
    if (!input.init()) logf_("n64run", "SDL game controller init failed; keyboard only");
    if (!online && !args.ports_given) {
        // Offline default: one port per connected pad (at least one, for the keyboard), max 4.
        int n = std::max(1, std::min(4, input.pad_count()));
        for (int p = 0; p < kPorts; p++) args.owner[p] = p < n ? 0 : -1;
    }
    int my_slot = online ? args.slot : 0;
    if (!online) for (int p = 0; p < kPorts; p++) if (args.owner[p] >= 0) args.owner[p] = 0;
    for (int p = 0; p < kPorts; p++) g_run.owner[p] = args.owner[p];
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
    cfg_bool(core_cfg, "OnScreenDisplay", 1);
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
        cfg_bool(vid_cfg, "Fullscreen", 0);
        cfg_int(vid_cfg, "ScreenWidth", args.width);
        cfg_int(vid_cfg, "ScreenHeight", args.height);
        cfg_bool(vid_cfg, "VerticalSync", 1);
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

        if (args.delay < 0) {
            DelayNegotiation dn;
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
    if (!start_plugin(gfx, emu_dir, "mupen64plus-video-GLideN64.dll", "Video") ||
        !start_plugin(aud, emu_dir, "mupen64plus-audio-sdl.dll", "Audio") ||
        !start_plugin(rsp, emu_dir, "mupen64plus-rsp-hle.dll", "RSP")) {
        g_core.DoCommand(M64CMD_ROM_CLOSE, 0, nullptr);
        g_core.Shutdown();
        return 3;
    }
    if (PluginStartup(g_core.dll, const_cast<char*>("Input"), m64p_debug_cb) != M64ERR_SUCCESS) return 3;
    HMODULE self = GetModuleHandle(nullptr);
    if (g_core.Attach(M64PLUGIN_GFX, gfx.h) != M64ERR_SUCCESS || g_core.Attach(M64PLUGIN_AUDIO, aud.h) != M64ERR_SUCCESS ||
        g_core.Attach(M64PLUGIN_INPUT, self) != M64ERR_SUCCESS || g_core.Attach(M64PLUGIN_RSP, rsp.h) != M64ERR_SUCCESS) {
        logf_("n64run", "CoreAttachPlugin failed");
        g_core.DoCommand(M64CMD_ROM_CLOSE, 0, nullptr);
        g_core.Shutdown();
        return 3;
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
        LockstepHooks hk;
        hk.update_local = [&] { input.update(); };
        hk.read_local = [&](int idx) { return input.read(idx); };
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
        e = g_core.DoCommand(M64CMD_EXECUTE, 0, nullptr);   // blocks until the emulation stops
        logf_("n64run", "emulation ended (%d)", (int)e);
        if (online && g_run.lockstep) g_run.lockstep->log_stall_summary();
    }

    {
        int code = 0;
        if (g_run.lockstep && g_run.lockstep->connection_lost()) { logf_("n64run", "Connection lost"); code = 4; }
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
