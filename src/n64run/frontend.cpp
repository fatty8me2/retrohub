/* n64run: mupen64plus 2.6.0 frontend with lockstep netplay.
 *
 *   n64run --rom PATH --game ID [--net SERVER --room CODE-N --slot K --players N
 *          --ports P0,P1,P2,P3 --delay auto|D [--share-host-save --host-save-dir DIR]]
 *          [--save-dir DIR] [--emu-dir DIR] [--width W --height H] [--strict-sync] [--no-hash]
 *
 * Exit codes (documented in docs/ARCHITECTURE.md): 0 ok, 2 bad args / ROM, 3 emulator load failure,
 *             4 netplay connection failure / connection lost, 8 a player (host) left so the game ended, 5 desync detected (only with --strict-sync),
 *             6 host-save sync failed. 5 is also returned when the peers' cheat lists differ (--cheats).
 * --slot may be any of 0..3 (lobby slots can have gaps); --players is the number of PCs.
 */
#include "winutil.h"
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
#include <map>
#include <memory>
#include <mutex>
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
#include "log.h"
#include "session.h"
#include "cheats.h"

#include "headless.h"
#include "hint_popup.h"
#include "controls_sdl.h"
#include "local_input.h"
#include "test_input.h"
#include "mempak.h"
#include "match.h"
#include "gs_cheat.h"
#include "lockstep.h"
#include "state_xfer.h"
#include "runstate.h"
#include "runner_ctl.h"
#include "osd.h"
#include "fbmask.h"
#include "clip.h"
#include <GL/gl.h>
#include <set>
#include <ctime>
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
    "              [--cheats FILE] [--title TEXT] [--fullscreen] [--gfx Key=i|b|s:value]...\n"
    "              [--gs-code CODES] [--headless [--realtime] [--frames N] [--screenshot OUT.png] [--dump-ram OFFSET,LEN,FILE]...]\n";

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
    bool full_hash = false;       // --full-hash (with --spectate): the host predates the framebuffer-masked sync hash
    bool legacy_host = false;     // --legacy-host (with --spectate): the host predates N64 live join (keeps RDRAM dithering; no knock)
    bool spectate = false;        // --spectate: watch a running online match as a port-less spectator (server picks the slot; needs --net --room --ports)
    bool dropin = false;          // --dropin: join a running online match into a free port as a friend (hub: Join game; needs --net --room --slot --ports)
    bool dropin_room = false;     // --dropin-room: the room allows drop-in: unowned ports are plugged in from boot (neutral until a friend takes one)
    uint8_t reserved = 0;         // the ports that are (derived from --ports when dropin_room / dropin)
    uint32_t founder_mask = 0;    // dropin: the slots that already play
    std::string name;             // --name (the joiner's name, shown as "<name> joined")
    bool no_spectators = false;   // --no-spectators: as the host, do not record/serve the input history to spectators
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
    std::string load_state;   // --load-state FILE: restore this state right after boot (offline)
    int volume = 100;   // --volume N (0-100): applied once the core runs
    std::string control, state_dir, shot_dir, game_key;   // --control FILE (runner_ctl.h), --state-dir, --shot-dir, --game-key
    std::string cheats;     // --cheats FILE: cheat list JSON (cheats.h); polled for live toggles offline
    bool turbo_online = false;   // --turbo-online: the room host allowed turbo buttons for everyone (offline they are always on)
    std::string controls;   // --controls FILE / RETROHUB_CONTROLS: resolved controller profile JSON (controls.h)
    std::string test_input;   // --test-input / N64RUN_TEST_INPUT: scripted local input (QA)
    std::string match;   // --match spec (VS battle jump)
    std::string gs_code;   // --gs-code "80ADDR 00VV+..." (headless QA: GameShark RAM writes every VI)
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
        else if (k == "--gs-code") ok = val(&a.gs_code);
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
        else if (k == "--spectate") a.spectate = true;
        else if (k == "--dropin") a.dropin = true;
        else if (k == "--dropin-room") a.dropin_room = true;
        else if (k == "--name") ok = val(&a.name);
        else if (k == "--legacy-host") a.legacy_host = true;
        else if (k == "--full-hash") a.full_hash = true;
        else if (k == "--no-spectators") a.no_spectators = true;
        else if (k == "--ports") { std::string s; ok = val(&s) && parse_ports(s, a.owner); a.ports_given = ok; }
        else if (k == "--strict-sync") a.strict_sync = true;
        else if (k == "--no-hash") a.no_hash = true;
        else if (k == "--fullscreen") a.fullscreen = true;
        else if (k == "--title") ok = val(&a.title);
        else if (k == "--gfx") { std::string g; hub::gs::GfxParam gp; ok = val(&g) && hub::gs::parse_gfx_arg(g, &gp); if (ok) a.gfx.push_back(g); }
        else if (k == "--test-input") ok = val(&a.test_input);
        else if (k == "--controls") ok = val(&a.controls);
        else if (k == "--turbo-online") a.turbo_online = true;
        else if (k == "--cheats") ok = val(&a.cheats);
        else if (k == "--volume") { std::string s; ok = val(&s); if (ok) a.volume = std::max(0, std::min(100, atoi(s.c_str()))); }
        else if (k == "--control") ok = val(&a.control);
        else if (k == "--load-state") ok = val(&a.load_state);
        else if (k == "--state-dir") ok = val(&a.state_dir);
        else if (k == "--shot-dir") ok = val(&a.shot_dir);
        else if (k == "--game-key") ok = val(&a.game_key);
        else { std::fprintf(stderr, "unknown argument: %s\n", k.c_str()); return false; }
        if (!ok) { std::fprintf(stderr, "bad value for %s\n", k.c_str()); return false; }
    }
    if (a.controls.empty()) { const char* e = std::getenv("RETROHUB_CONTROLS"); if (e && *e) a.controls = e; }
    if (a.test_input.empty()) {   // the env var is QA-only: needs RETROHUB_QA=1 (the --test-input flag is explicit and always works)
        const char* q = std::getenv("RETROHUB_QA");
        const char* e = std::getenv("N64RUN_TEST_INPUT");
        if (q && *q && std::string(q) != "0" && e && *e) a.test_input = e;
    }
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
        if (a.dropin) {   // a friend dropping in: the plan has our slot in the first free port; we play after the host admits us. Until then we are a spectator that owns nothing.
            if (a.spectate || a.share_host_save || a.slot < 1 || a.slot > 3) { std::fprintf(stderr, "--dropin needs --slot 1..3 and no --spectate/--share-host-save\n"); return false; }
            a.dropin_room = true;
            for (int p = 0; p < kPorts; p++) if (a.owner[p] == a.slot) a.owner[p] = -1;
            a.slot_mask = 1u;
            for (int p = 0; p < kPorts; p++) if (a.owner[p] >= 0 && a.owner[p] < kPorts) a.slot_mask |= 1u << a.owner[p];
            a.founder_mask = a.slot_mask;
            a.players = popcount32(a.slot_mask);
            a.spectate = true;   // internally the joiner runs the spectator path (live state join, replay) until it is admitted
        } else if (a.spectate) {   // players = the owners named in --ports plus the host; the spectator's own slot comes from the server
            if (a.share_host_save) { std::fprintf(stderr, "--spectate cannot be combined with --share-host-save\n"); return false; }
            a.slot_mask = 1u;
            for (int p = 0; p < kPorts; p++) if (a.owner[p] >= 0 && a.owner[p] < kPorts) a.slot_mask |= 1u << a.owner[p];
            a.players = popcount32(a.slot_mask);
            a.slot = 1;   // placeholder; Session adopts the slot the server assigns
        } else {
        a.slot_mask = derive_slot_mask(a.owner, a.slot, a.players, &err);
        if (!a.slot_mask) { std::fprintf(stderr, "bad --players/--slot/--ports: %s\n", err.c_str()); return false; }
        }
    } else if (a.spectate || a.dropin_room) { std::fprintf(stderr, "--spectate/--dropin-room need --net\n"); return false; }
    bool any = false;
    for (int p = 0; p < kPorts; p++) {
        if (a.owner[p] < -1 || a.owner[p] > 3) { std::fprintf(stderr, "bad --ports value\n"); return false; }
        any |= a.owner[p] >= 0;
    }
    if (a.ports_given && !any) { std::fprintf(stderr, "--ports has no controllers\n"); return false; }
    if (a.dropin_room) for (int p = 0; p < kPorts; p++) if (a.owner[p] < 0) a.reserved |= (uint8_t)(1u << p);
    return true;
}

// ---------------------------------------------------------------- headless frame counter
static std::atomic<bool> g_stop_watchdog{false};
static long long g_frame_limit = 0;      // 0 = unlimited
static long long g_frame_count = 0;
static bool g_limiter_off = false;
static bool g_realtime = false;
static std::unique_ptr<MatchDriver> g_match;
static std::vector<n64run::GsLine> g_gs;
static long long g_vi = 0;
static bool g_headless_mode = false;
static clip::Recorder g_clips;            // instant replay (clip.h)
static bool g_clip_prev = false;
static std::atomic<bool> g_clip_want{false};   // set by the pad poll (any thread), served on the frame callback
static unsigned long long g_clip_cool = 0;
static void clip_request_save() {
    std::string m;
    if (g_clips.request_save(std::time(nullptr), &m)) osd::push("Saving clip...");
    else osd::push(m, osd::Kind::Warn);
}
static void clip_poll_done() {
    bool ok = false; std::string m;
    if (!g_clips.poll(&ok, &m)) return;
    logf_("control", "clip: %s %s", ok ? "saved" : "FAILED", m.c_str());
    osd::push(ok ? "Clip saved" : "Clip failed: " + m, ok ? osd::Kind::Good : osd::Kind::Error);
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
    ptr_CoreOverrideVidExt OverrideVidExt = nullptr;
    ptr_CoreAddCheat AddCheat = nullptr;
    ptr_CoreCheatEnabled CheatEnabled = nullptr;
};
static Core g_core;

/* --headless: called by the core on every VI (emulator thread). Drops the speed limiter on the first
 * call (it cannot be changed before the emulation thread runs) and stops after --frames VIs. */
/* ---- cheats (--cheats FILE; format and contract in src/common/cheats.h) ----
 * Offline: the file is polled (~2x/s) from the emulation thread's VI callback and enable/disable/edits apply live.
 * Online: the list is read once at launch, hashed and compared with every peer in the handshake, and applied on the
 * first VI of every PC (deterministic); later file changes are logged and ignored so peers cannot diverge. */
static std::string g_cheats_path;
static bool g_cheats_online = false;
static cheats::CheatSet g_cheats_boot;           // the list read at launch (what the handshake hashed)
static bool g_cheats_first_done = false;
static cheats::FileWatch g_cheats_watch;
static unsigned long long g_cheats_next_poll = 0;
static std::map<std::string, std::string> g_cheats_applied;   // id -> signature of the code list the core holds
static std::map<std::string, bool> g_cheats_state;            // id -> enabled in the core

/* GameShark-button codes (88/89): written once, on a frame a little later, not every VI. */
struct OnceJob { unsigned long long due; std::vector<n64run::GsLine> lines; };
static std::vector<OnceJob> g_once;
static std::map<std::string, std::vector<n64run::GsLine>> g_cheats_once;   // id -> its fire-once lines
static std::map<std::string, int> g_cheats_fire;                           // id -> last "fire" counter acted on
static std::map<std::string, bool> g_cheats_in_core;                       // id -> registered with the core

static void cheats_apply(const cheats::CheatSet& set) {
    if (!g_core.AddCheat || !g_core.CheatEnabled) return;
    const bool first = !g_cheats_first_done;
    std::map<std::string, bool> seen;
    for (const cheats::Cheat& c : set.cheats) {
        seen[c.id] = true;
        std::vector<cheats::N64Code> codes, core_codes, once_codes;
        std::string err;
        if (!cheats::n64_codes(c, &codes, &err)) {
            auto pit = g_cheats_applied.find(c.id);
            if (pit == g_cheats_applied.end() || pit->second != "!" + err)
                logf_("cheats", "skipping '%s' (%s): %s", c.id.c_str(), c.desc.c_str(), err.c_str());
            g_cheats_applied[c.id] = "!" + err;
            continue;
        }
        cheats::n64_split_button(codes, &core_codes, &once_codes);
        std::string sig;
        for (auto& k : codes) sig += std::to_string(k.address) + ":" + std::to_string(k.value) + ";";
        auto it = g_cheats_applied.find(c.id);
        if (it == g_cheats_applied.end() || it->second != sig) {
            if (!core_codes.empty()) {
                std::vector<m64p_cheat_code> list;
                for (auto& k : core_codes) { m64p_cheat_code mc; mc.address = k.address; mc.value = k.value; list.push_back(mc); }
                m64p_error e = g_core.AddCheat(c.id.c_str(), list.data(), (int)list.size());
                if (e != M64ERR_SUCCESS) { logf_("cheats", "CoreAddCheat('%s') failed (%d)", c.id.c_str(), (int)e); continue; }
                g_cheats_in_core[c.id] = true;
            } else if (g_cheats_in_core[c.id]) {
                g_core.CheatEnabled(c.id.c_str(), 0);
                g_cheats_in_core[c.id] = false;
            }
            std::vector<n64run::GsLine> ol;
            for (auto& k : once_codes) ol.push_back({(uint8_t)(k.address >> 24), k.address & 0xFFFFFFu, (uint16_t)k.value});
            g_cheats_once[c.id] = std::move(ol);
            g_cheats_applied[c.id] = sig;
            g_cheats_state.erase(c.id);
            if (codes.empty()) logf_("cheats", "'%s' holds only device-control codes (nothing to do in an emulator)", c.id.c_str());
        }
        auto st = g_cheats_state.find(c.id);
        if (st == g_cheats_state.end() || st->second != c.enabled) {
            bool ok = true;
            if (g_cheats_in_core[c.id]) ok = g_core.CheatEnabled(c.id.c_str(), c.enabled ? 1 : 0) == M64ERR_SUCCESS;
            if (ok) {
                g_cheats_state[c.id] = c.enabled;
                logf_("cheats", "%s '%s' (%s, %zu code%s)", c.enabled ? "enabled" : "disabled", c.id.c_str(), c.desc.c_str(), codes.size(), codes.size() == 1 ? "" : "s");
                auto ol = g_cheats_once.find(c.id);
                if (c.enabled && ol != g_cheats_once.end() && !ol->second.empty()) {   // switched on: fire the button codes once
                    g_once.push_back({(unsigned long long)g_vi + (first ? 180ull : 1ull), ol->second});
                    logf_("cheats", "'%s': GameShark-button codes (%zu) fire once now", c.id.c_str(), ol->second.size());
                }
            }
        }
        /* "Activate": the hub bumps the fire counter; every rise fires the button codes once more (live sessions only). */
        auto fi = g_cheats_fire.find(c.id);
        if (fi == g_cheats_fire.end()) g_cheats_fire[c.id] = c.fire;
        else if (c.fire > fi->second) {
            fi->second = c.fire;
            auto ol = g_cheats_once.find(c.id);
            if (c.enabled && ol != g_cheats_once.end() && !ol->second.empty()) {
                g_once.push_back({(unsigned long long)g_vi + 1ull, ol->second});
                logf_("cheats", "'%s': activated (GameShark-button codes fire once)", c.id.c_str());
            }
        }
    }
    for (auto& kv : g_cheats_state)   // cheats that vanished from the file: switch off
        if (!seen.count(kv.first) && kv.second && g_core.CheatEnabled(kv.first.c_str(), 0) == M64ERR_SUCCESS) { kv.second = false; logf_("cheats", "disabled '%s' (removed from file)", kv.first.c_str()); }
}

static void cheats_tick() {
    if (g_cheats_path.empty()) return;
    if (!g_cheats_first_done) {
        cheats_apply(g_cheats_boot);
        g_cheats_first_done = true;
        return;
    }
    unsigned long long now = GetTickCount64();
    if (now < g_cheats_next_poll) return;
    g_cheats_next_poll = now + 500;
    if (!g_cheats_watch.changed()) return;
    if (g_cheats_online) { logf_("cheats", "cheats file changed during an online session: ignored (all peers must run the same list)"); return; }
    cheats::CheatSet set; std::string err;
    if (!cheats::load_file(g_cheats_path, &set, &err)) { logf_("cheats", "cannot reload cheats: %s", err.c_str()); return; }
    cheats_apply(set);
}

static bool qa_nofix() { return std::getenv("RETROHUB_QA") && std::getenv("N64RUN_QA_NOFIX"); }   // QA baseline: behave like before


/* ---- hub control channel (--control FILE; contract in src/common/runner_ctl.h) ----
 * Polled from the VI callback (emulation thread). Save/load go through the core's own state commands (done on the next VI), so
 * every command is a small state machine that acks once its file exists. One command at a time. */
struct CtlJob {
    enum Kind { None, SaveWait, SaveShot, LoadWait, Shot } kind = None;
    long long start_vi = 0;
    int slot = 0;
    unsigned serial0 = 0;
    std::string dest, thumb;
};
static runctl::Poller g_ctl;
static runctl::InputGate g_in_gate;   // hub quick menu open => neutral local input
static CtlJob g_job;
static std::string g_ctl_state_dir, g_ctl_shot_dir, g_ctl_key, g_ctl_shot_tmp;
static bool g_ctl_online = false;
static std::set<std::string> g_ctl_shot_seen;

static std::set<std::string> list_dir(const std::string& d) {
    std::set<std::string> r;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(fs::path(d), ec)) r.insert(e.path().filename().string());
    return r;
}

/* Starts a frame grab: headless arms the video extension; a real window asks the core for its own screenshot. */
static void ctl_grab_begin() {
    if (g_headless_mode) { g_job.serial0 = n64run::headless_shot_serial(); headless_set_shot_mode(ShotMode::Armed); }
    else if (!g_ctl_shot_tmp.empty()) { g_ctl_shot_seen = list_dir(g_ctl_shot_tmp); g_core.DoCommand(M64CMD_TAKE_NEXT_SCREENSHOT, 0, nullptr); }
}
/* True once the grab was written to dest (max_w 0 = full size). Sets *failed when it cannot happen. */
static bool ctl_grab_end(const std::string& dest, int max_w, bool* failed) {
    if (g_headless_mode) {
        if (n64run::headless_shot_serial() == g_job.serial0) return false;
        if (!headless_write_screenshot(dest, max_w)) *failed = true;
        return true;
    }
    for (auto& n : list_dir(g_ctl_shot_tmp)) {
        if (g_ctl_shot_seen.count(n)) continue;
        std::error_code ec;
        fs::create_directories(fs::path(dest).parent_path(), ec);
        fs::copy_file(fs::path(g_ctl_shot_tmp) / n, fs::path(dest), fs::copy_options::overwrite_existing, ec);
        fs::remove(fs::path(g_ctl_shot_tmp) / n, ec);
        if (ec) *failed = true;
        return true;
    }
    return false;
}

static void ctl_finish(bool ok, const std::string& msg) {
    switch (g_job.kind) {   // the OSD mirrors what the hub is told
        case CtlJob::SaveShot: case CtlJob::SaveWait: osd::push(ok ? "Saved state slot " + std::to_string(g_job.slot) : "Save state failed", ok ? osd::Kind::Good : osd::Kind::Error); break;
        case CtlJob::LoadWait: osd::push(ok ? "Loaded state slot " + std::to_string(g_job.slot) : "Load state failed", ok ? osd::Kind::Good : osd::Kind::Error); break;
        case CtlJob::Shot: osd::push(ok ? "Screenshot saved" : "Screenshot failed", ok ? osd::Kind::Good : osd::Kind::Error); break;
        default: if (!ok) osd::push(msg, osd::Kind::Warn); break;
    }
    g_ctl.ack(ok, msg);
    logf_("control", "%s: %s", ok ? "ok" : "FAILED", msg.c_str());
    g_job = CtlJob();
}

/* Offline resume support: --load-state restores a state on the third VI; auto.state is rewritten every 30 s and when the hub asks
 * for "quit" (the core offers no hook for a plain window close, so that case loses at most 30 s). */
static fbmask::Tracker g_fb;   // framebuffers found from the VI registers (fbmask.h); lead-poll thread only
static std::string g_load_state_path, g_auto_dir, g_key_for_auto;
static unsigned long long g_next_autosave = 0;
static long long g_stop_at_vi = 0;
static void auto_save_now() {
    if (g_auto_dir.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::path(g_auto_dir), ec);
    std::string f = runctl::state_file(g_auto_dir, runctl::kAutoSlot);
    if (g_core.DoCommand(M64CMD_STATE_SAVE, 1, const_cast<char*>(f.c_str())) != M64ERR_SUCCESS) return;
    runctl::Sidecar sc;
    sc.game_key = g_key_for_auto; sc.time = (long long)std::time(nullptr); sc.playtime = (long long)(GetTickCount64() / 1000);
    runctl::write_text_atomic(runctl::sidecar_file(g_auto_dir, runctl::kAutoSlot), runctl::make_sidecar(sc));
}
static void auto_tick() {
    if (!g_load_state_path.empty() && g_vi >= 3) {
        std::string f = g_load_state_path;
        g_load_state_path.clear();
        if (g_core.DoCommand(M64CMD_STATE_LOAD, 1, const_cast<char*>(f.c_str())) != M64ERR_SUCCESS) logf_("n64run", "load-state: the core refused %s", f.c_str());
        else logf_("n64run", "load-state: restoring %s", f.c_str());
        g_next_autosave = GetTickCount64() + 30000;
    }
    if (g_stop_at_vi && g_vi >= g_stop_at_vi) { g_stop_at_vi = 0; g_core.DoCommand(M64CMD_STOP, 0, nullptr); return; }
    if (!g_auto_dir.empty() && !g_stop_at_vi && g_vi > 600) {
        unsigned long long now = GetTickCount64();
        if (now >= g_next_autosave) { g_next_autosave = now + 30000; auto_save_now(); }
    }
}

/* ---------------------------------------------------------------- live spectator join (spectate.h kPktJoin, state_xfer.h)
 * Host (slot 0): a spectator's Join is answered at the next lead-port poll n. The frontend asks the core for a savestate there; mupen takes it
 * at its next interrupt, after poll n's inputs were consumed and before poll n + 1, so it is the state before sample S = n + 1
 * (spectate::lockstep_snapshot_sample). The core writes
 * the gzipped state on its own worker thread (no stall for the players); when it is done the file goes to that spectator with rb::xfer while
 * the confirmed inputs stream from S (Host::start_peer_at).
 * Spectator: before the emulation starts it knocks, downloads, verifies and writes the state to a file; the core loads it at VI 3 and the
 * lockstep counts samples from S once the core reports the load (polls before that are discarded with the boot state they ran in).
 * Proof: the periodic RDRAM sync checks (host vs spectator) must agree from the first one after S. They include the framebuffers GLideN64
 * copies back into RDRAM, so online play turns off its per-frame RDRAM image dithering (RDRAMImageDitheringMode, see below): its noise
 * depends on how many frames the plugin has drawn, which a spectator that loaded a state has not. */
namespace live {
static hubnet::Session* g_sess = nullptr;
static bool g_ok = false;                    // host: snapshots allowed
static std::string g_dir;                    // scratch folder for state files
static std::vector<int> g_joinq;
static std::map<int, uint32_t> g_done;       // slot -> when its snapshot was delivered (knocks still in flight are ignored)
struct Snap {
    int slot = -1;
    uint32_t S = 0, t_join = 0, t_send = 0, job = 0;
    bool sending = false;
    rb::xfer::Sender snd;
};
static std::unique_ptr<Snap> g_snap;        // emulation thread only
static uint8_t g_epoch = 0;
/* Core save jobs. The emulation thread starts at most one at a time (busy) and never starts another until the core has reported the one in
 * flight, even when it gave up on it (timeout, spectator gone): a late SAVECOMPLETE can then only belong to that job. The core's worker
 * thread (the one that wrote the file) reads it, hashes it, deletes it and publishes the result tagged with the job id, so none of that
 * file work runs on the emulation thread. Everything in g_save is under g_save_mx. */
struct SaveJob {
    uint32_t id = 0;            // job in flight / last one started
    bool busy = false;
    std::string path;
    uint32_t done_id = 0;       // the result below belongs to this job
    std::vector<uint8_t> data;  // empty: the save failed
    uint64_t hash = 0;
    double work_ms = 0;         // reading + hashing on the worker thread
};
static std::mutex g_save_mx;
static SaveJob g_save;
static uint32_t g_job_seq = 0;
/* join stall measurement (emulation thread): lead-poll gaps while a snapshot is being prepared vs the usual gap */
static LARGE_INTEGER g_qpf{}, g_last_poll{};
static double g_gap_base_ms = 0, g_gap_join_ms = 0;
static int g_gap_n = 0;
/* spectator */
static uint32_t g_S = 0, g_t_join = 0, g_done_ep = 0x100, g_done_at = 0;
static std::string g_in_path;                // the downloaded state file (removed once the core has loaded it)
static bool g_snapshot = false, g_verified = false, g_live_logged = false;
static spectate::CatchUp g_cu;
static std::string g_rs_in_path;             // desync recovery: the state file the guest asked the core to load
}  // namespace live
static int g_dj_rc = 0;                      // drop-in joiner: why it could not join (hub exit code 9/10/11)
namespace live {

static uint64_t fnv64(const uint8_t* d, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 0x100000001B3ull; }
    return h;
}

static void nolive(int slot, uint8_t reason) {
    uint8_t m[3] = {spectate::kPktNoLive, 0, reason};
    g_sess->send(slot, m, 3);
    if (auto* H = g_run.lockstep->spectate_host()) H->release(slot);
    logf_("spectate", "slot %d joins from sample 0 (no live snapshot, reason %u)", slot, (unsigned)reason);
    g_run.lockstep->dropin_forget(slot, "no live snapshot");   // a drop-in cannot play from sample 0: it retries or leaves
}

/* core state callback (emulation thread for loads, the core's worker thread for finished saves) */
static void state_cb(void*, m64p_core_param p, int v) {
    if (p == M64CORE_STATE_SAVECOMPLETE) {   // the core's worker thread, right after it wrote the file
        std::string path;
        uint32_t id;
        { std::lock_guard<std::mutex> lk(g_save_mx); if (!g_save.busy) return; path = g_save.path; id = g_save.id; }
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter(&t0);
        std::vector<uint8_t> data;
        if (v) {
            if (FILE* fp = std::fopen(path.c_str(), "rb")) {
                std::fseek(fp, 0, SEEK_END);
                long sz = std::ftell(fp);
                std::fseek(fp, 0, SEEK_SET);
                if (sz > 0) { data.resize((size_t)sz); if (std::fread(data.data(), 1, data.size(), fp) != data.size()) data.clear(); }
                std::fclose(fp);
            }
        }
        uint64_t h = data.empty() ? 0 : fnv64(data.data(), data.size());
        std::error_code ec;
        fs::remove(fs::path(path), ec);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        std::lock_guard<std::mutex> lk(g_save_mx);
        g_save.done_id = id; g_save.data = std::move(data); g_save.hash = h;
        g_save.work_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        g_save.busy = false;
    }
    else if (p == M64CORE_STATE_LOADCOMPLETE && g_run.lockstep && g_run.lockstep->resync_waiting()) {   // desync recovery (guest)
        if (!g_rs_in_path.empty()) { std::error_code ec; fs::remove(fs::path(g_rs_in_path), ec); g_rs_in_path.clear(); }
        g_run.lockstep->resync_loaded(v != 0);
    }
    else if (p == M64CORE_STATE_LOADCOMPLETE && g_run.lockstep && g_run.lockstep->spectate_live_waiting()) {
        if (!g_in_path.empty()) { std::error_code ec; fs::remove(fs::path(g_in_path), ec); }
        if (v) {
            g_run.lockstep->spectate_live_loaded();
            logf_("spectate", "live join: the core loaded the state of sample %u %u ms after joining", g_S, hubnet::now_ms() - g_t_join);
        } else {
            logf_("spectate", "live join: the core could not load the live state; leaving");
            g_core.DoCommand(M64CMD_STOP, 0, nullptr);
        }
    }
}

/* host: Join / xfer datagrams from a spectator (emulation thread, from the lockstep's pump) */
static void host_msg(int from, const std::vector<uint8_t>& m) {
    if (m[0] == spectate::kPktJoin) {
        if (std::find(g_joinq.begin(), g_joinq.end(), from) == g_joinq.end()) g_joinq.push_back(from);
    } else if (m[0] == rb::xfer::kPkt && g_snap && g_snap->sending && g_snap->slot == from && m.size() >= 4 && m[2] == from && m[3] == g_snap->snd.epoch()) {
        if (m[1] == rb::xfer::Ack && m.size() >= 4 + 2 + 8) g_snap->snd.on_ack(from, rb::xfer::g16(m.data() + 4), rb::xfer::g64(m.data() + 6));
        else if (m[1] == rb::xfer::Done && m.size() >= 5 && m[4] == 1) g_snap->snd.on_done(from);
    }
}

/* host: lead-port poll n (deterministic point: before sample n's inputs are delivered) */
static void host_poll(uint32_t n) {
    spectate::Host* H = g_run.lockstep->spectate_host();
    if (!H) return;
    {   // poll-to-poll time = the players' pace. While a snapshot is being saved its worst gap is the join stall.
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
        if (g_last_poll.QuadPart) {
            double ms = (double)(t.QuadPart - g_last_poll.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
            if (g_snap && !g_snap->sending) g_gap_join_ms = std::max(g_gap_join_ms, ms);
            else if (!g_snap && ms < 1000) { g_gap_base_ms = (g_gap_base_ms * g_gap_n + ms) / (g_gap_n + 1); if (g_gap_n < 600) g_gap_n++; }
        }
        g_last_poll = t;
    }
    if (g_joinq.empty() || g_run.lockstep->resync_active()) return;   // a desync recovery wants the core's save job: spectators' knocks wait
    std::vector<int> q;
    q.swap(g_joinq);
    const uint32_t now = hubnet::now_ms();
    for (int slot : q) {
        if (!(g_run.lockstep->spec_mask() >> slot & 1u)) continue;
        auto dn = g_done.find(slot);
        if (dn != g_done.end() && now - dn->second < 5000) continue;   // knocks sent before our Begin reached it
        const bool mine = g_snap && g_snap->slot == slot;
        if (!H->held(slot) && dn == g_done.end() && !mine) {   // already streaming to it from 0 (its hold timed out): keep going
            uint8_t m[3] = {spectate::kPktNoLive, 0, spectate::kRefuseEarly};
            g_sess->send(slot, m, 3);
            continue;
        }
        bool busy;
        { std::lock_guard<std::mutex> lk(g_save_mx); busy = g_save.busy; }
        auto d = spectate::decide_join(g_ok, H->total(), g_snap ? 1 : 0, mine, g_snap != nullptr || busy);   // one core save job at a time
        if (d == spectate::JoinDecision::Ignore || d == spectate::JoinDecision::Busy) continue;
        if (d == spectate::JoinDecision::NoLiveEarly) { nolive(slot, spectate::kRefuseEarly); continue; }
        if (d == spectate::JoinDecision::NoLiveUnsupported) { nolive(slot, spectate::kRefuseUnsupported); continue; }
        auto x = std::make_unique<Snap>();
        x->slot = slot; x->t_join = now; x->S = spectate::lockstep_snapshot_sample(n);
        std::string path;
        {
            std::lock_guard<std::mutex> lk(g_save_mx);
            g_save.id = x->job = ++g_job_seq;
            g_save.path = path = g_dir + "\\live_" + std::to_string(g_job_seq) + ".st";
            g_save.busy = true;
        }
        if (g_core.DoCommand(M64CMD_STATE_SAVE, 1, const_cast<char*>(path.c_str())) != M64ERR_SUCCESS) {
            { std::lock_guard<std::mutex> lk(g_save_mx); g_save.busy = false; }
            nolive(slot, spectate::kRefuseUnsupported);
            continue;
        }
        g_gap_join_ms = 0;
        g_snap = std::move(x);
        logf_("spectate", "live join for slot %d: saving the state before sample %u (asked at poll %u)", slot, g_snap->S, n);
    }
}

/* host: after sample n was recorded (emulation thread) */
static void host_tick(uint32_t) {
    spectate::Host* H = g_run.lockstep->spectate_host();
    if (!H) return;
    const uint32_t now = hubnet::now_ms(), sm = g_run.lockstep->spec_mask();
    for (auto it = g_done.begin(); it != g_done.end();) { if (!(sm >> it->first & 1u)) it = g_done.erase(it); else ++it; }
    if (!g_snap) return;
    Snap& x = *g_snap;
    const bool gone = !(sm >> x.slot & 1u);
    if (!x.sending) {
        if (!gone) H->extend_hold(x.slot, now);   // the core is still writing: the hold must not expire into a stream from 0
        bool busy, have;
        std::vector<uint8_t> data;
        uint64_t hash = 0;
        double work_ms = 0;
        {
            std::lock_guard<std::mutex> lk(g_save_mx);
            busy = g_save.busy && g_save.id == x.job;
            have = !busy && g_save.done_id == x.job;
            if (have && !gone && H->total() >= x.S) { data = std::move(g_save.data); hash = g_save.hash; work_ms = g_save.work_ms; g_save.data.clear(); g_save.done_id = 0; }
        }
        if (busy && now - x.t_join > 10000) {   // give up on it; the job stays busy until the core reports it (its file is removed then)
            logf_("spectate", "live join for slot %d: the core did not save the state in 10 s", x.slot);
            if (!gone) nolive(x.slot, spectate::kRefuseUnsupported);
            g_snap.reset();
            return;
        }
        if (gone) { g_snap.reset(); return; }   // a save still in flight stays busy; the worker removes its file when it lands
        if (busy || !have || H->total() < x.S) return;
        const bool rel = g_sess->is_relayed(x.slot);
        const size_t cap = std::min<size_t>(rel ? 1500000 : 3000000, rb::xfer::kMaxChunks * rb::xfer::kChunk);   // relay: keep the room's budget
        if (data.empty() || data.size() > cap) {
            logf_("spectate", "live state for slot %d is %zu bytes (cap %zu): from sample 0 instead", x.slot, data.size(), cap);
            nolive(x.slot, data.empty() ? spectate::kRefuseUnsupported : spectate::kRefuseTooBig);
            g_snap.reset();
            return;
        }
        logf_("spectate", "live join stall: worst player poll gap %.1f ms while the state was saved (typical %.1f ms); file read + hash %.1f ms on the core's worker thread",
              g_gap_join_ms, g_gap_base_ms, work_ms);
        rb::xfer::Meta m;
        m.frame = x.S; m.raw = (uint32_t)data.size(); m.comp = (uint32_t)data.size(); m.flags = 0;   // already gzipped by the core
        m.chunks = (uint16_t)((data.size() + rb::xfer::kChunk - 1) / rb::xfer::kChunk);
        m.hash = hash;
        H->start_peer_at(x.slot, x.S, now);   // inputs from S stream right away; the spectator buffers them while the state downloads
        g_epoch = (uint8_t)((g_epoch + 1) & 0x7F);
        x.snd.start((uint8_t)(0x80 | g_epoch), m, std::move(data), 1u << x.slot, now ? now : 1, rel ? 100.0 : 1200.0);
        x.sending = true; x.t_send = now;
        logf_("spectate", "sending the live state to slot %d: sample %u, %u bytes in %u chunks (%s), ready %u ms after the join", x.slot, m.frame, m.comp,
              (unsigned)m.chunks, rel ? "relay" : "direct", now - x.t_join);
        return;
    }
    if (gone) { g_snap.reset(); return; }
    const int slot = x.slot;
    x.snd.service(now ? now : 1, 0, [&](const uint8_t* d, size_t n) { g_sess->send(slot, d, (int)n); });
    if (x.snd.all_done()) {
        logf_("spectate", "slot %d has the live state %u ms after its join (%zu chunks sent for %u)", slot, now - x.t_join, x.snd.chunks_sent(), (unsigned)x.snd.meta().chunks);
        g_done[slot] = now;
        g_snap.reset();
    } else if (now - x.t_send > 30000) {
        logf_("spectate", "live state transfer to slot %d timed out", slot);
        g_snap.reset();
    }
}

/* spectator: Begin/Chunk repeats after we loaded the state (our Done was lost): say Done again */
static void spec_msg(int, const std::vector<uint8_t>& m) {
    if (m.size() >= 4 && m[0] == rb::xfer::kPkt && (m[1] == rb::xfer::Begin || m[1] == rb::xfer::Chunk) && m[3] == g_done_ep && hubnet::now_ms() - g_done_at > 100) {
        auto v = rb::xfer::header(rb::xfer::Done, g_sess->my_slot(), (uint8_t)g_done_ep);
        v.push_back(1);
        g_sess->send(0, v.data(), (int)v.size());
        g_done_at = hubnet::now_ms();
    }
}

/* spectator: lead-port poll n (only once the live state is in, or from sample 0 without one) */
static void spec_poll(uint32_t n) {
    if (!g_run.lockstep->spectating()) return;   // a drop-in joiner that became a player
    const spectate::Client& c = g_run.lockstep->spectate_client();
    if (g_snapshot && !g_verified && (c.hashes_ok() || c.hashes_bad())) {   // the first RDRAM sync check after the snapshot: the proof
        g_verified = true;
        if (c.hashes_bad()) logf_("spectate", "live join: the first RDRAM sync check after sample %u DIFFERS from the host", g_S);
        else logf_("spectate", "live join verified: the first RDRAM sync check after sample %u matches the host", g_S);
    }
    g_cu.update(c.have_any() ? c.lag() : spectate::kLiveLagSamples + 1, hubnet::now_ms());
    if (g_cu.live_once() && !g_live_logged) {
        g_live_logged = true;
        logf_("spectate", "LIVE at sample %u, %u behind the host, %u ms after joining (%s, %u samples fast-forwarded)", n, c.lag(), g_cu.live_at() - g_t_join,
              g_snapshot ? "live snapshot" : "replay from sample 0", n - (g_snapshot ? g_S : 0));
    }
}

/* ---- desync recovery (lockstep.h resync hooks; shares the save job above) ---- */
/* host, at poll S-1: the state taken at the next interrupt is the one before sample S */
static bool resync_save(uint32_t* job) {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(g_save_mx);
        if (g_save.busy) return false;
        g_save.id = *job = ++g_job_seq;
        g_save.path = path = g_dir + "\\resync_" + std::to_string(g_job_seq) + ".st";
        g_save.busy = true;
    }
    if (g_core.DoCommand(M64CMD_STATE_SAVE, 1, const_cast<char*>(path.c_str())) != M64ERR_SUCCESS) {
        std::lock_guard<std::mutex> lk(g_save_mx);
        g_save.busy = false;
        return false;
    }
    return true;
}
static int resync_fetch(uint32_t job, std::vector<uint8_t>* data, uint64_t* hash) {
    std::lock_guard<std::mutex> lk(g_save_mx);
    if (g_save.busy && g_save.id == job) return 0;
    if (g_save.done_id != job) return -1;
    *data = std::move(g_save.data); *hash = g_save.hash;
    g_save.data.clear(); g_save.done_id = 0;
    return data->empty() ? -1 : 1;
}
static bool resync_load(const std::vector<uint8_t>& st, uint64_t hash) {
    if (fnv64(st.data(), st.size()) != hash) return false;
    std::string path = g_dir + "\\resync_in.st";
    FILE* f = std::fopen(path.c_str(), "wb");
    bool ok = f && std::fwrite(st.data(), 1, st.size(), f) == st.size();
    if (f) std::fclose(f);
    if (!ok) return false;
    g_rs_in_path = path;
    return g_core.DoCommand(M64CMD_STATE_LOAD, 1, const_cast<char*>(path.c_str())) == M64ERR_SUCCESS;
}
/* QA fault injection (RETROHUB_QA=1): N64RUN_QA_POKE=n1,n2,... flips RDRAM words at lead poll n on the slot N64RUN_QA_POKE_SLOT (default 1) */
static std::vector<uint32_t> g_poke;
static void qa_poll(uint32_t n) {
    for (uint32_t& t : g_poke) if (t == n) {
        t = 0xFFFFFFFFu;
        if (uint8_t* ram = static_cast<uint8_t*>(g_core.MemPtr(M64P_DBG_PTR_RDRAM))) { for (int i = 0; i < 64; i++) ram[0x7F0000 + i] ^= 0xA5; logf_("qa", "poked RDRAM at sample %u", n); }
    }
}

/* spectator, before the emulation starts: ask for a live snapshot. false = connection lost. */
static bool spectator_join(n64run::Lockstep& ls, int my_slot, bool legacy_host, const std::string* dropin_name, int* dj_rc) {
    const bool dropin = dropin_name != nullptr;
    spectate::JoinClient jc(!legacy_host && !std::getenv("N64RUN_NO_LIVE_SPECTATE"));
    g_t_join = hubnet::now_ms();
    if (jc.playing()) return true;
    spectate::Client& cli = ls.spectate_client_mut();
    rb::xfer::Receiver rcv;
    uint32_t last_rx = hubnet::now_ms(), dl_progress = 0;
    size_t dl_got = 0;
    std::vector<std::vector<uint8_t>> early;   // inputs from S that overtook our Begin: kept for after it
    const uint32_t t_knock0 = hubnet::now_ms();
    uint32_t no_knock_until = 0, last_busy = 0;
    int busy_n = 0;
    bool heard_host = false;
    for (;;) {
        SDL_PumpEvents();
        g_sess->update();
        int from; std::vector<uint8_t> m;
        const uint32_t now0 = hubnet::now_ms();
        while (g_sess->recv(&from, &m)) {
            if (from != 0 || m.empty()) continue;
            last_rx = now0;
            if (m[0] == spectate::kPktNoLive && dropin && m.size() >= 3) {
                heard_host = true;
                const uint8_t why = m[2];
                logf_("dropin", "the host answered: reason %u", (unsigned)why);
                if (why == n64run::dropin::kRefuseBusy || why == spectate::kRefuseEarly) {   // an admit / recovery is in flight, or the match is too young for a snapshot: ask again in a moment
                    if (now0 - last_busy > 700) { last_busy = now0 ? now0 : 1; busy_n++; }
                    no_knock_until = now0 + 900;
                    if (busy_n > (why == spectate::kRefuseEarly ? 40 : 8)) { *dj_rc = 11; return false; }
                    continue;
                }
                *dj_rc = n64run::dropin::exit_code_for(why == n64run::dropin::kRefuseNoPort || why == n64run::dropin::kRefuseOldPeer ? why : (uint8_t)0);
                return false;
            }
            if (m[0] == spectate::kPktData) {
                heard_host = true;
                if (dropin && jc.mode() == spectate::JoinClient::Mode::Knock && m.size() >= 6 && rb::xfer::g32(m.data() + 2) == 0) { logf_("dropin", "the host streams from sample 0 without a snapshot: cannot drop in"); *dj_rc = 11; return false; }
                if (jc.mode() == spectate::JoinClient::Mode::Knock) {
                    if (m.size() < 6) continue;
                    if (rb::xfer::g32(m.data() + 2) != 0) { if (early.size() < 64) early.push_back(m); continue; }   // inputs from S that overtook our Begin
                    jc.on_legacy_data();
                    logf_("spectate", "the host streams from sample 0 (no live join): replaying the match, fast-forwarding to the live game");
                }
                cli.on_data(m.data(), m.size(), now0);
            } else if (m[0] == spectate::kPktNoLive && m.size() >= 3) {
                if (jc.mode() == spectate::JoinClient::Mode::Knock) {
                    jc.on_nolive(m[2]);
                    logf_("spectate", "the host has no live snapshot for us (reason %u): watching from the start, fast-forwarding to the live game", (unsigned)m[2]);
                }
            } else if (m[0] == rb::xfer::kPkt && m.size() >= 4 && m[2] == 0) {
                if (m[1] == rb::xfer::Begin) {
                    rb::xfer::Meta mt;
                    if (rb::xfer::dec_begin(m.data(), m.size(), &mt) && jc.mode() == spectate::JoinClient::Mode::Knock && rcv.on_begin(m[3], mt)) {
                        heard_host = true;
                        jc.on_begin(mt.frame, now0);
                        cli.start_at(mt.frame);
                        for (auto& e : early) cli.on_data(e.data(), e.size(), now0);
                        early.clear();
                        dl_progress = now0;
                        logf_("spectate", "live join: downloading the state of sample %u (%u bytes, %u chunks) %u ms after joining", mt.frame, mt.comp, (unsigned)mt.chunks, now0 - g_t_join);
                    }
                } else if (m[1] == rb::xfer::Chunk && m.size() >= 6 && jc.mode() == spectate::JoinClient::Mode::Download) {
                    rcv.on_chunk(m[3], rb::xfer::g16(m.data() + 4), m.data() + 6, m.size() - 6);
                }
            }
        }
        const uint32_t now = hubnet::now_ms();
        if (jc.playing()) return true;   // refused / old host: the lockstep plays from sample 0 (and fast-forwards)
        if (dropin && !heard_host && now - t_knock0 > 8000) { logf_("dropin", "the host does not answer: its RetroHub is too old for drop-in"); *dj_rc = 10; return false; }
        if (jc.want_knock(now) && (!dropin || (int32_t)(now - no_knock_until) >= 0)) {
            if (dropin) { auto k = n64run::dropin::encode_knock(spectate::kPktJoin, my_slot, n64run::dropin::kCapDropIn, *dropin_name); g_sess->send(0, k.data(), (int)k.size()); }
            else { uint8_t k[3] = {spectate::kPktJoin, (uint8_t)my_slot, 1}; g_sess->send(0, k, 3); }
        }
        if (jc.mode() == spectate::JoinClient::Mode::Download) {
            if (rcv.received() != dl_got) { dl_got = rcv.received(); dl_progress = now; }
            uint16_t nx; uint64_t bits;
            if (rcv.ack_due(now, &nx, &bits)) { auto v = rb::xfer::enc_ack(my_slot, rcv.epoch(), nx, bits); g_sess->send(0, v.data(), (int)v.size()); }
            cli.service(now, my_slot, [](int slot, const uint8_t* d, int n) { g_sess->send(slot, d, n); });
            if (rcv.complete()) {
                const rb::xfer::Meta& mt = rcv.meta();
                const std::vector<uint8_t>& st = rcv.payload();
                bool ok = st.size() == mt.raw && fnv64(st.data(), st.size()) == mt.hash;
                std::string path = g_dir + "\\live_in.st";
                if (ok) {
                    FILE* f = std::fopen(path.c_str(), "wb");
                    ok = f && std::fwrite(st.data(), 1, st.size(), f) == st.size();
                    if (f) std::fclose(f);
                }
                if (!ok) { logf_("spectate", "the live state did not arrive intact"); return false; }
                g_S = mt.frame; g_snapshot = true;
                ls.spectate_expect_live(mt.frame);
                g_in_path = path;
                g_load_state_path = path;   // auto_tick: the core loads it on VI 3; state_cb starts the samples at S
                jc.on_loaded();
                g_done_ep = rcv.epoch();
                auto v = rb::xfer::header(rb::xfer::Done, my_slot, rcv.epoch());
                v.push_back(1);
                g_sess->send(0, v.data(), (int)v.size());
                g_done_at = now;
                logf_("spectate", "live join: state of sample %u (%u bytes) downloaded %u ms after joining; %u behind the host", mt.frame, mt.comp, now - g_t_join, cli.lag());
                return true;
            }
            if (jc.download_stuck(now, dl_progress)) { logf_("spectate", "the live state stopped arriving"); return false; }
        }
        if (g_sess->failed() || now - last_rx > 15000) { logf_("spectate", "connection lost while joining as a spectator"); return false; }
        Sleep(1);
    }
}
}  // namespace live

/* runner_ctl "speed" / "ff": the core's speed factor (percent) with the limiter on; "max" turns the limiter off. g_fast_speed is what
 * "ff on" selects (last fast speed chosen, 0 = max). Unthrottled headless test runs keep their limiter off. */
static double g_fast_speed = 2.0;
static void set_speed(double sp) {
    const bool free_run = g_headless_mode && !g_realtime;
    int lim = (sp == 0 || free_run) ? 0 : 1;
    int pct = sp == 0 ? 100 : (int)(sp * 100.0 + 0.5);
    g_core.DoCommand(M64CMD_CORE_STATE_SET, M64CORE_SPEED_FACTOR, &pct);
    if (!free_run) g_core.DoCommand(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &lim);
    if (sp == 0 || sp > 1.0) g_fast_speed = sp;
    osd::push(runctl::speed_label(sp));
    logf_("control", "%s", runctl::speed_label(sp).c_str());
}

/* View + RT held: quick boost at the last fast speed (offline only); released: back to 1x, unless a speed was chosen meanwhile. */
static int g_start_volume = 100;
static void n64_apply_start_volume() {   // once, from the first frame callback (the core is running by then)
    static bool done = false;
    if (done) return;
    done = true;
    if (g_start_volume != 100) { int v = g_start_volume; g_core.DoCommand(M64CMD_CORE_STATE_SET, M64CORE_AUDIO_VOLUME, &v); }
}
static bool g_boost_active = false;
static double g_menu_speed = 1.0;   // the speed the hub menu chose (0 = max); the boost only rides on 1x and gives it back on release
static void n64_boost_tick(bool held) {
    if (held && !g_boost_active) { if (g_menu_speed == 1.0) { g_boost_active = true; set_speed(g_fast_speed); } }
    else if (!held && g_boost_active) { g_boost_active = false; set_speed(g_menu_speed); }
}

static void ctl_tick() {
    if (!g_ctl.enabled()) return;
    const long long vi = g_vi;
    if (g_job.kind != CtlJob::None) {
        bool failed = false;
        switch (g_job.kind) {
            case CtlJob::SaveWait: {
                std::error_code ec;
                auto sz = fs::file_size(fs::path(runctl::state_file(g_ctl_state_dir, g_job.slot)), ec);
                if (!ec && sz > 0 && vi > g_job.start_vi + 1) {
                    g_job.kind = CtlJob::SaveShot; g_job.start_vi = vi;
                    ctl_grab_begin();
                } else if (vi > g_job.start_vi + 600) ctl_finish(false, "the core did not write the state");
                break;
            }
            case CtlJob::SaveShot:
                if (ctl_grab_end(g_job.thumb, runctl::kThumbWidth, &failed) || vi > g_job.start_vi + 300) {
                    runctl::Sidecar sc;
                    sc.game_key = g_ctl_key; sc.time = (long long)std::time(nullptr); sc.playtime = (long long)(GetTickCount64() / 1000);
                    runctl::write_text_atomic(runctl::sidecar_file(g_ctl_state_dir, g_job.slot), runctl::make_sidecar(sc));
                    ctl_finish(true, "saved slot " + std::to_string(g_job.slot));
                }
                break;
            case CtlJob::LoadWait:
                if (vi > g_job.start_vi + 2) ctl_finish(true, "loaded");
                break;
            case CtlJob::Shot:
                if (ctl_grab_end(g_job.dest, 0, &failed)) ctl_finish(!failed, failed ? "could not write the screenshot" : g_job.dest);
                else if (vi > g_job.start_vi + 300) ctl_finish(false, "no frame was captured");
                break;
            default: break;
        }
        return;
    }
    {   // the core's own F12 hotkey drops screenshots into the scratch folder: file them into the vault
        static unsigned long long next_sweep = 0;
        unsigned long long now = GetTickCount64();
        if (!g_headless_mode && !g_ctl_shot_tmp.empty() && !g_ctl_shot_dir.empty() && now >= next_sweep) {
            next_sweep = now + 700;
            for (auto& n : list_dir(g_ctl_shot_tmp)) {
                std::error_code ec;
                std::string dest = g_ctl_shot_dir + "/" + runctl::shot_name(std::time(nullptr));
                fs::create_directories(fs::path(g_ctl_shot_dir), ec);
                if (fs::exists(fs::path(dest), ec)) dest = dest.substr(0, dest.size() - 4) + "-" + n;
                fs::rename(fs::path(g_ctl_shot_tmp) / n, fs::path(dest), ec);
                if (!ec) { logf_("control", "screenshot filed: %s", dest.c_str()); osd::push("Screenshot saved", osd::Kind::Good); }
            }
        }
    }
    runctl::Command c;
    if (!g_ctl.poll(GetTickCount64(), &c)) return;
    logf_("control", "seq %lld %s slot %d", c.seq, runctl::cmd_name(c.cmd), c.slot);
    switch (c.cmd) {
        case runctl::Cmd::SaveState:
        case runctl::Cmd::LoadState: {
            if (g_ctl_online) { ctl_finish(false, "Save states are disabled in online sessions"); break; }
            if (g_ctl_state_dir.empty()) { ctl_finish(false, "no state folder"); break; }
            std::error_code ec;
            fs::create_directories(fs::path(g_ctl_state_dir), ec);
            std::string f = runctl::state_file(g_ctl_state_dir, c.slot);
            g_job.slot = c.slot; g_job.start_vi = vi;
            if (c.cmd == runctl::Cmd::SaveState) {
                fs::remove(fs::path(f), ec);
                g_job.thumb = runctl::thumb_file(g_ctl_state_dir, c.slot);
                if (g_core.DoCommand(M64CMD_STATE_SAVE, 1, const_cast<char*>(f.c_str())) != M64ERR_SUCCESS) { ctl_finish(false, "the core refused to save"); break; }
                g_job.kind = CtlJob::SaveWait;
            } else {
                if (!fs::exists(fs::path(f), ec)) { ctl_finish(false, "no state in that slot"); break; }
                if (g_core.DoCommand(M64CMD_STATE_LOAD, 1, const_cast<char*>(f.c_str())) != M64ERR_SUCCESS) { ctl_finish(false, "the core refused to load"); break; }
                g_job.kind = CtlJob::LoadWait;
            }
            break;
        }
        case runctl::Cmd::Screenshot:
            if (g_ctl_shot_dir.empty()) { ctl_finish(false, "no screenshot folder"); break; }
            g_job.kind = CtlJob::Shot; g_job.start_vi = vi;
            g_job.dest = g_ctl_shot_dir + "/" + runctl::shot_name(std::time(nullptr));
            ctl_grab_begin();
            break;
        case runctl::Cmd::Quit:
            g_ctl.ack(true, "quitting");
            if (!g_auto_dir.empty() && !g_ctl_online) { auto_save_now(); g_stop_at_vi = vi + 12; }   // let the core write auto.state first
            else g_core.DoCommand(M64CMD_STOP, 0, nullptr);
            break;
        case runctl::Cmd::Clip: {
            std::string m;
            bool ok = g_clips.request_save(std::time(nullptr), &m);
            if (ok) { osd::push("Saving clip..."); m = "saving"; }
            else osd::push(m, osd::Kind::Warn);
            g_ctl.ack(ok, m);
            break;
        }
        case runctl::Cmd::OsdMsg: osd::push(c.text); g_ctl.ack(true, "shown"); break;
        case runctl::Cmd::Input: g_in_gate.set(c.value != 0, GetTickCount64()); g_ctl.ack(true, c.value ? "input on" : "input off"); break;
        case runctl::Cmd::Volume: case runctl::Cmd::Mute: case runctl::Cmd::Ff: case runctl::Cmd::Speed:
        case runctl::Cmd::Rewind: case runctl::Cmd::Shader: case runctl::Cmd::Overlay:
            if (g_ctl_online && runctl::offline_only(c.cmd)) { g_ctl.ack(false, "Not available in online sessions"); break; }
            if (c.cmd == runctl::Cmd::Volume) {
                int v = c.value;
                g_core.DoCommand(M64CMD_CORE_STATE_SET, M64CORE_AUDIO_VOLUME, &v);
                osd::push("Volume " + std::to_string(v) + "%");
                g_ctl.ack(true, "volume " + std::to_string(v));
            } else if (c.cmd == runctl::Cmd::Mute) {
                int m = c.value ? 1 : 0;
                g_core.DoCommand(M64CMD_CORE_STATE_SET, M64CORE_AUDIO_MUTE, &m);
                osd::push(m ? "Muted" : "Sound on");
                g_ctl.ack(true, m ? "muted" : "unmuted");
            } else if (c.cmd == runctl::Cmd::Ff || c.cmd == runctl::Cmd::Speed) {
                double sp = c.cmd == runctl::Cmd::Speed ? c.speed : (c.value ? g_fast_speed : 1.0);
                g_menu_speed = sp; g_boost_active = false;   // a menu choice made while View+RT is held wins over the boost
                set_speed(sp);
                g_ctl.ack(true, runctl::speed_label(sp));
            } else {
                g_ctl.ack(false, "unsupported");
            }
            break;
        default: ctl_finish(false, "not supported for this system"); break;
    }
}

/* ---- OSD as a GL overlay (EXPERIMENTAL, off unless N64RUN_OSD_GL=1) ----
 * Draws the osd layer as a textured quad with fixed-function GL on the context current in the VI callback, then restores the
 * state it touched. Limits: needs a compatibility context (GLideN64 on wgl normally is one); whether the quad is visible depends
 * on the callback running between the plugin's draw and its buffer swap, which differs per core build, so this is not enabled by
 * default. Without it n64run shows OSD text in the window caption (title_watch) so nothing is lost. */
static void osd_gl_draw() {
    static const bool on = [] { const char* e = std::getenv("N64RUN_OSD_GL"); return e && e[0] == '1'; }();
    if (!on || g_headless_mode || !wglGetCurrentContext()) return;
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    static osd::Layer layer;
    static GLuint tex = 0;
    static int tw = 0, th = 0;
    bool changed = false;
    // The layer covers the whole framebuffer area (vp[0]+vp[2] x vp[1]+vp[3] is not needed: the quad is mapped onto the viewport).
    if (vp[2] <= 0 || vp[3] <= 0 || !osd::render(&layer, vp[2], vp[3], &changed)) return;
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    GLint prog = 0;
    glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &prog);
    typedef void(WINAPI * UseProgramFn)(GLuint);
    static UseProgramFn use_program = reinterpret_cast<UseProgramFn>(wglGetProcAddress("glUseProgram"));
    if (use_program) use_program(0);   // fixed function needs no shader bound
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_LIGHTING);
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (changed || tw != vp[2] || th != vp[3]) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vp[2], vp[3], 0, 0x80E1 /* GL_BGRA */, GL_UNSIGNED_BYTE, layer.px.data());
        tw = vp[2]; th = vp[3];
    }
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, 1, 1, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(1, 0); glVertex2f(1, 0);
    glTexCoord2f(1, 1); glVertex2f(1, 1);
    glTexCoord2f(0, 1); glVertex2f(0, 1);
    glEnd();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();
    if (use_program) use_program((GLuint)prog);
}

static void headless_frame_cb(unsigned int) {
    g_vi++;
    if (!g_once.empty() && g_core.MemPtr) {
        uint8_t* r = static_cast<uint8_t*>(g_core.MemPtr(M64P_DBG_PTR_RDRAM));
        for (size_t i = 0; i < g_once.size();) {
            if (g_vi >= g_once[i].due) { if (r) n64run::gs_apply(r, 8u * 1024u * 1024u, g_once[i].lines); g_once.erase(g_once.begin() + i); }
            else ++i;
        }
    }
    if (!g_gs.empty() && g_core.MemPtr) { uint8_t* r = static_cast<uint8_t*>(g_core.MemPtr(M64P_DBG_PTR_RDRAM)); if (r) n64run::gs_apply(r, 8u * 1024u * 1024u, g_gs); }
    cheats_tick();
    auto_tick();
    ctl_tick();
    clip_poll_done();
    if (g_clip_want.exchange(false)) clip_request_save();
    if (!g_headless_mode && g_clips.enabled() && (g_vi % 3) == 0) {   // windowed: read the game window back at ~20 fps (best effort; see docs/RUNNER_CTL.md)
        GLint vp[4] = {0, 0, 0, 0};
        if (wglGetCurrentContext()) {
            glGetIntegerv(GL_VIEWPORT, vp);
            int w = vp[2], h = vp[3];
            if (w >= 64 && h >= 64 && w <= 4096 && h <= 4096) {
                static std::vector<uint8_t> buf;
                buf.resize((size_t)w * h * 4);
                GLint prev_align = 4;
                glGetIntegerv(GL_PACK_ALIGNMENT, &prev_align);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadBuffer(GL_BACK);
                glReadPixels(vp[0], vp[1], w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
                glPixelStorei(GL_PACK_ALIGNMENT, prev_align);
                g_clips.offer(buf.data(), w, h, w * 4, 20.0, false, true);
            }
        }
    }
    osd_gl_draw();
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

/* Game-start controller tips (src/common/start_hint.h): a click-through popup over the plugin's window, on its own thread.
 * Off with RETROHUB_START_HINT=0 (the hub sets it from "Show controller tips when a game starts"). */
static void hint_watch(bool keyboard) {
    starthint::Popup pop;
    while (!g_title_stop.load() && !pop.done()) {
        HWND hwnd = nullptr;
        EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&hwnd));
        pop.tick(hwnd, keyboard, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
}

static std::wstring widen_ascii(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)std::max(0, n), L' ');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static void title_watch(std::wstring base_title) {
    using namespace std::chrono;
    const auto start = steady_clock::now();
    std::string shown_osd;
    while (!g_title_stop.load()) {
        HWND hwnd = nullptr;
        EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&hwnd));
        shown_osd = osd::latest_text();   // OSD toasts also go to the caption (the GL overlay is experimental)
        std::wstring title = shown_osd.empty() ? base_title : base_title + L"  |  " + widen_ascii(shown_osd);
        if (hwnd) {
            wchar_t cur[512] = {0};
            GetWindowTextW(hwnd, cur, 512);
            if (title != cur)   // timeout: the window thread may be busy shutting down
                SendMessageTimeoutW(hwnd, WM_SETTEXT, 0, (LPARAM)title.c_str(), SMTO_ABORTIFHUNG, 200, nullptr);
        }
        bool early = steady_clock::now() - start < seconds(10);
        for (int i = 0; i < (early ? 1 : 8) && !g_title_stop.load(); i++) {
            std::this_thread::sleep_for(milliseconds(250));
            if (osd::latest_text() != shown_osd) break;   // a toast came or went: update the caption now
        }
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
        out = winutil::to_wide(fallback_utf8);
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
    g_start_volume = args.volume;
    /* QA only (RETROHUB_QA=1 N64RUN_QA_AUDIO=1): headless peers with the real audio plugin on SDL's silent "dummy" driver,
     * so the plugin's buffer / sync behaviour is exercised without any sound. */
    const bool qa_audio = args.headless && args.realtime && std::getenv("RETROHUB_QA") && std::getenv("N64RUN_QA_AUDIO");
    if (qa_audio) _putenv("SDL_AUDIODRIVER=dummy");
    const bool want_audio = !args.headless || qa_audio;
    const bool delay_auto = args.delay < 0;   // no explicit --delay: negotiate, then adapt during play

    // ---- ports / local input
    LocalInput input;
    bool want_rumble = false;
    if (!args.headless) hubctl::load_bundled_db();   // SDL_GameControllerDB: DualSense, Switch Pro, 8BitDo ...
    if (!args.controls.empty()) {
        hubctl::Profile prof;
        if (hubctl::read_profile_file(args.controls, &prof) && prof.system == "n64") {
            input.set_profile(prof);
            want_rumble = prof.rumble;
            logf_("n64run", "controls: profile '%s' (%s)", prof.name.c_str(), args.controls.c_str());
        } else logf_("n64run", "controls: cannot use %s; built-in mapping", args.controls.c_str());
    }
    input.set_turbo(hubctl::turbo_allowed(online, args.turbo_online));
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
    g_run.reserved = args.reserved;
    if (args.reserved) logf_("n64run", "drop-in room: ports %s%s%s%s are plugged in from boot for friends who join later", (args.reserved & 1) ? "1 " : "", (args.reserved & 2) ? "2 " : "", (args.reserved & 4) ? "3 " : "", (args.reserved & 8) ? "4 " : "");
    g_run.mempak = mempak_enabled(online, args.share_host_save);
    g_run.rumble = !args.headless && rumble_enabled(online, want_rumble);
    if (g_run.rumble) {
        static LocalInput* s_input; s_input = &input;
        g_run.rumble_cb = [](int port, bool on) { if (s_input) s_input->rumble(port, on); };   // offline: owned index == port
        logf_("n64run", "Rumble Pak: on (replaces the Controller Pak)");
    }
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
    if (!args.cheats.empty() && !(sym(g_core.dll, "CoreAddCheat", &g_core.AddCheat) && sym(g_core.dll, "CoreCheatEnabled", &g_core.CheatEnabled)))
        logf_("n64run", "this emulator core has no cheat API: --cheats ignored");
    if (args.headless && !sym(g_core.dll, "CoreOverrideVidExt", &g_core.OverrideVidExt)) return 3;

    if (!args.cheats.empty()) {
        std::string cerr;
        if (cheats::load_file(args.cheats, &g_cheats_boot, &cerr)) {
            g_cheats_path = args.cheats;
            g_cheats_online = online;
            g_cheats_watch = cheats::FileWatch(args.cheats);
            logf_("n64run", "cheats: %zu in %s%s", g_cheats_boot.cheats.size(), args.cheats.c_str(), online ? " (online: fixed for the session)" : "");
        } else logf_("n64run", "cheats: cannot read %s (%s); playing without cheats", args.cheats.c_str(), cerr.c_str());
    }
    m64p_error e = g_core.Startup(kFrontendApiVersion, cfg_dir.c_str(), emu_dir.c_str(), const_cast<char*>("Core"), m64p_debug_cb, nullptr, &live::state_cb);
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
    if (!args.control.empty() && !args.headless) {   // windowed screenshots: the core writes into a scratch folder, ctl_tick moves them
        std::string st = save_dir + "\\screenshots_tmp";
        fs::create_directories(st, ec);
        cfg_str(core_cfg, "ScreenshotPath", with_sep(st));
    }
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
        if (g_core.OpenSection("Audio-SDL", &aud_cfg) == M64ERR_SUCCESS) {
            if (mute) cfg_int(aud_cfg, "VOLUME_DEFAULT", 0);
            if (online && !qa_nofix()) {
                /* Netplay: keep the plugin's queue short so audio can never lag far behind the picture
                 * (defaults: 16384-sample ring = ~370 ms worst case). 4096 samples ~ 93 ms at 44.1 kHz. */
                cfg_int(aud_cfg, "PRIMARY_BUFFER_SIZE", 4096);
                cfg_int(aud_cfg, "PRIMARY_BUFFER_TARGET", 1536);
                cfg_int(aud_cfg, "SECONDARY_BUFFER_SIZE", 1024);
            }
        }
    }
    g_core.SaveFile();

    // ---- netplay session. This runs before ROM_OPEN: the core reads battery saves when the ROM opens,
    // so the host's saves (--share-host-save) must already be in the save dir by then.
    hubnet::Session session;
    if (online && std::getenv("RETROHUB_QA") && std::getenv("N64RUN_NET_LOG")) net_set_log_file(std::getenv("N64RUN_NET_LOG"));   // QA: the Session's own log (paths, relay, roster)
    if (online) {
        hubnet::SessionConfig sc;
        sc.server = args.net;
        sc.room = args.room;
        sc.slot = args.slot;
        sc.expected = args.spectate ? 2 : args.players;
        sc.spectate = args.spectate && !args.dropin;
        sc.dropin = args.dropin;
        sc.keep_on_guest_bye = args.dropin_room && !args.dropin;   // players of a drop-in room: a friend leaving must not end the host's session
        std::vector<int> slot_tries{args.slot};
        if (args.dropin) for (int sl = 1; sl <= 3; sl++) if (sl != args.slot && !(args.founder_mask >> sl & 1u)) slot_tries.push_back(sl);   // the plan's slot may be held by another friend who dropped in
        bool connected = false;
        for (size_t ti = 0; ti < slot_tries.size() && !connected; ti++) {
            sc.slot = slot_tries[ti];
            if (!session.start(sc)) {
                logf_("n64run", "netplay: cannot start session (%s)", session.status().c_str());
                g_core.Shutdown(); fs::remove_all(cfg_dir, ec); return args.dropin ? 11 : 4;   // the hub reads 11 as "couldn't join the game in progress"
            }
            uint32_t t0 = hubnet::now_ms();
            std::string last;
            while (!session.ready()) {
                session.update();
                if (session.failed() || hubnet::now_ms() - t0 > 15000) break;
                std::string st = session.status();
                if (st != last) { logf_("n64run", "netplay: %s", st.c_str()); last = st; }
                Sleep(10);
            }
            if (session.ready()) { connected = true; args.slot = sc.slot; if (args.dropin) my_slot = sc.slot; break; }
            const std::string st = session.status();
            logf_("n64run", "netplay connection failed: %s", st.c_str());
            session.stop();
            if (args.dropin && (st.find("taken") != std::string::npos || st.find("full") != std::string::npos) && ti + 1 < slot_tries.size()) continue;
            g_core.Shutdown(); fs::remove_all(cfg_dir, ec);
            if (args.dropin) return (st.find("free controller port") != std::string::npos || st.find("taken") != std::string::npos || st.find("full") != std::string::npos) ? 9 : st.find("busy") != std::string::npos ? 11 : 4;   // every slot taken = the game is full
            return 4;
        }
        logf_("n64run", "netplay: all %d players reachable", args.players);
        if (args.dropin) logf_("n64run", "dropin: joined the game room as slot %d", my_slot);
        else if (args.spectate) { my_slot = session.my_slot(); logf_("n64run", "spectating as slot %d (no port, confirmed inputs only)", my_slot); }

        if (!args.spectate) {
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

        if (!args.spectate) {   // every PC must run the same cheat list (an empty list included)
            uint64_t ch = cheats::set_hash(g_cheats_boot, cheats::Target::N64);
            n64run::CheatSync cs = n64run::exchange_cheat_hash(session, my_slot, args.slot_mask, ch, 8000, [] {}, [](const std::string& m) { logline("n64run", m.c_str()); });
            if (cs == n64run::CheatSync::Mismatch) {
                logf_("n64run", "netplay: cheat lists differ between players; leaving");
                session.stop(); g_core.Shutdown(); fs::remove_all(cfg_dir, ec); return 5;
            }
            if (cs == n64run::CheatSync::Failed) {
                logf_("n64run", "netplay connection lost during the cheat check");
                session.stop(); g_core.Shutdown(); fs::remove_all(cfg_dir, ec); return 4;
            }
        }

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
    if (online && !(args.spectate && args.legacy_host)) {
        /* GLideN64 dithers the image it copies back into RDRAM with a noise pattern that advances with every frame it has drawn. Peers that
         * booted together agree, but a spectator that joined from a savestate has drawn a different number of frames, so its framebuffers in
         * RDRAM (and the RDRAM sync hash) would differ forever. No dither keeps that copy a pure function of the game's own state. Every
         * player of a room runs the same RetroHub version (lobby version gate), so all of them use the same setting. Spectators are not
         * version gated: the hub passes --legacy-host when the host's launch message lacks "n64live" (an older host that still dithers),
         * and that spectator keeps dithering and replays from sample 0 as before. */
        m64p_handle gh = nullptr;
        if (g_core.OpenSection("Video-GLideN64", &gh) == M64ERR_SUCCESS) cfg_int(gh, "RDRAMImageDitheringMode", 0);
    }
    if (!start_plugin(gfx, emu_dir, "mupen64plus-video-GLideN64.dll", "Video") ||
        (want_audio && !start_plugin(aud, emu_dir, "mupen64plus-audio-sdl.dll", "Audio")) ||
        !start_plugin(rsp, emu_dir, "mupen64plus-rsp-hle.dll", "RSP")) {
        g_core.DoCommand(M64CMD_ROM_CLOSE, 0, nullptr);
        g_core.Shutdown();
        return 3;
    }
    if (PluginStartup(g_core.dll, const_cast<char*>("Input"), m64p_debug_cb) != M64ERR_SUCCESS) return 3;
    HMODULE self = GetModuleHandle(nullptr);
    if (g_core.Attach(M64PLUGIN_GFX, gfx.h) != M64ERR_SUCCESS || (want_audio && g_core.Attach(M64PLUGIN_AUDIO, aud.h) != M64ERR_SUCCESS) ||
        g_core.Attach(M64PLUGIN_INPUT, self) != M64ERR_SUCCESS || g_core.Attach(M64PLUGIN_RSP, rsp.h) != M64ERR_SUCCESS) {
        logf_("n64run", "CoreAttachPlugin failed");
        g_core.DoCommand(M64CMD_ROM_CLOSE, 0, nullptr);
        g_core.Shutdown();
        return 3;
    }

    if (!args.gs_code.empty()) {
        if (!n64run::gs_parse(args.gs_code, &g_gs)) { logf_("n64run", "bad --gs-code"); g_core.DoCommand(M64CMD_ROM_CLOSE, 0, nullptr); g_core.Shutdown(); return 2; }
        logf_("n64run", "gs-code: %d lines", (int)g_gs.size());
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
        lo.spectator = args.spectate;
        lo.reserved_ports = args.reserved;
        lo.dropin_join = args.dropin;
        lo.my_name = args.name;
        lo.full_hash = args.full_hash;
        lo.serve_spectators = !args.no_spectators;
        lo.strict_sync = args.strict_sync;
        lo.adaptive_delay = delay_auto && !args.spectate && !std::getenv("N64RUN_FIXED_DELAY");
        LockstepHooks hk;
        if (test_input.active()) {
            hk.update_local = [&] { test_input.update(); };
            hk.read_local = [&](int idx) { return test_input.read(idx); };
        } else {
            hk.update_local = [&] { input.update(); { bool cc = input.clip_held(); unsigned long long tn = GetTickCount64(); if (cc && !g_clip_prev && tn >= g_clip_cool) { g_clip_cool = tn + 3000; g_clip_want = true; } g_clip_prev = cc; } g_in_gate.note_view_y(input.view_y_held(), GetTickCount64()); n64_apply_start_volume(); n64_boost_tick(input.boost_held() && !online); if (input.exit_requested()) { logf_("n64run", "exit combo (Back+Start held): leaving"); g_core.DoCommand(M64CMD_STOP, 0, nullptr); } };
            hk.read_local = [&](int idx) -> uint32_t { return g_in_gate.suppressed(GetTickCount64()) ? 0u : input.read(idx); };   // hub menu open: neutral, lockstep still sends the frame
        }
        hk.idle = [] { SDL_PumpEvents(); };
        if (online && !qa_nofix())
            hk.set_limiter = [](bool on) {
                if (g_headless_mode && !g_realtime) return;   // unthrottled test runs keep the limiter off
                int v = on ? 1 : 0;
                g_core.DoCommand(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &v);
            };
        if (online && g_core.MemPtr) {
            hk.vi_poll = [](uint32_t n) {
                static const uint32_t* vi = static_cast<const uint32_t*>(g_core.MemPtr(M64P_DBG_PTR_VI_REG));
                if (vi) g_fb.sample(n, vi[0], vi[1], vi[2]);
            };
            hk.vi_reset = [] { logf_("fbmask", "restart: %d tracked framebuffer(s) forgotten", g_fb.count()); g_fb.reset(); };
        }
        hk.request_stop = [] { g_core.DoCommand(M64CMD_STOP, 0, nullptr); };
        hk.log = [](const std::string& s) { logline("lockstep", s.c_str()); };
        if (!args.no_hash && g_core.MemPtr) {
            hk.hash_rdram = [](uint64_t* out) {
                void* p = g_core.MemPtr(M64P_DBG_PTR_RDRAM);
                if (!p) return false;
                if (g_run.lockstep && g_run.lockstep->masked_hash()) *out = fbmask::hash(p, 8u * 1024u * 1024u, g_fb.ranges(8u * 1024u * 1024u));
                else *out = hash_block(p, 8u * 1024u * 1024u);
                if (std::getenv("RETROHUB_QA") && std::getenv("N64RUN_QA_BLOCKS")) { std::string l = "QA-BLOCKS"; char b[24]; for (int i = 0; i < 128; i++) { std::snprintf(b, sizeof b, " %04x", (unsigned)(hash_block(static_cast<uint8_t*>(p) + i * 65536, 65536) & 0xFFFF)); l += b; } logf_("qa", "%s", l.c_str()); }
                return true;
            };
        } else logf_("n64run", "RDRAM desync hashing disabled");
        if (online && (!args.spectate || args.dropin)) {   // desync recovery (lockstep.h); a drop-in joiner uses it once it plays
            live::g_sess = &session;
            live::g_dir = cfg_dir;
            hk.resync_save = live::resync_save;
            hk.resync_fetch = live::resync_fetch;
            hk.resync_load = live::resync_load;
            hk.resync_done = [](uint32_t S, uint32_t ms) { logf_("n64run", "resynced at sample %u (%u ms)", S, ms); osd::push("Resynced", osd::Kind::Good); };
            if (const char* pk = std::getenv("RETROHUB_QA") ? std::getenv("N64RUN_QA_POKE") : nullptr) {
                const char* ps = std::getenv("N64RUN_QA_POKE_SLOT");
                if (my_slot == (ps ? std::atoi(ps) : 1)) {
                    for (const char* c = pk; *c;) { live::g_poke.push_back((uint32_t)std::strtoul(c, const_cast<char**>(&c), 10)); if (*c == ',') c++; else break; }
                    hk.qa_poll = live::qa_poll;
                }
            }
        }
        if (online) hk.on_dropin = [](const std::string& nm, int, int, bool joined) { osd::push((nm.empty() ? std::string("A friend") : nm) + (joined ? " joined" : " left"), joined ? osd::Kind::Good : osd::Kind::Info); };
        if (online) {   // live spectator join (namespace live above)
            live::g_sess = &session;
            live::g_dir = cfg_dir;
            if (args.spectate) {
                hk.on_spectate_msg = live::spec_msg;
                hk.on_lead_poll = live::spec_poll;
            } else if (my_slot == 0 && !args.no_spectators) {
                live::g_ok = !std::getenv("N64RUN_NO_LIVE_SPECTATE");
                hk.on_spectate_msg = live::host_msg;
                hk.on_lead_poll = live::host_poll;
                hk.after_sample = live::host_tick;
            }
        }
        g_run.lockstep.reset(new Lockstep(lo, online ? &session : nullptr, hk));
        if (online && g_run.lockstep->spectate_host()) g_run.lockstep->spectate_host()->set_hold_ms(spectate::kHoldMs);   // a new spectator first says whether it wants a live snapshot

        if (online && !g_run.lockstep->start_barrier(15000)) {
            if (g_run.lockstep->setup_mismatch()) g_dj_rc = 2;   // exit 2: the hub says the room's player/port setup was rejected
            logf_("n64run", "netplay: start barrier failed (%s)", session.status().c_str());
            g_run.lockstep.reset();
            goto fail_net;
        }
        if (online && args.spectate && !live::spectator_join(*g_run.lockstep, my_slot, args.legacy_host, args.dropin ? &args.name : nullptr, &g_dj_rc)) {
            logf_("n64run", "netplay: could not join the match as a spectator (%s)", session.status().c_str());
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
        if (!args.shot_dir.empty()) g_clips.set_dir(args.shot_dir);
        if (const char* e = std::getenv("N64RUN_CLIPS"); e && e[0] == '0') g_clips.set_enabled(false);
        else if (args.headless && !args.shot_dir.empty())
            headless_set_frame_sink([](const uint8_t* px, int w, int h) { g_clips.offer(px, w, h, w * 4, 20.0, false, false); }, 3);
        g_ctl_online = online;
        g_key_for_auto = args.game_key;
        if (!online && !args.state_dir.empty() && !g_match) { g_auto_dir = args.state_dir; g_next_autosave = GetTickCount64() + 30000; }
        if (!online && !args.load_state.empty()) g_load_state_path = args.load_state;
        if (!args.control.empty()) {
            g_ctl = runctl::Poller(args.control);
            g_ctl_state_dir = args.state_dir; g_ctl_shot_dir = args.shot_dir; g_ctl_key = args.game_key;
            if (!args.headless) g_ctl_shot_tmp = save_dir + "\\screenshots_tmp";
        }
        g_realtime = args.realtime;
        if (args.headless || g_match || online || !g_gs.empty() || !g_cheats_path.empty() || !args.control.empty() || !g_auto_dir.empty() || !g_load_state_path.empty()) g_core.DoCommand(M64CMD_SET_FRAME_CALLBACK, 0, reinterpret_cast<void*>(&headless_frame_cb));
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
        if (g_run.lockstep && g_run.lockstep->dropin_fail_rc()) {
            code = g_run.lockstep->dropin_fail_rc();   // 9 full, 10 friend must update, 11 other (the hub's drop-in texts)
            logf_("n64run", "dropin: could not join (exit %d)", code);
        }
        else if (g_run.lockstep && g_run.lockstep->connection_lost()) {
            std::string why = session.status();   // 8: the host/partner left (exit_code_text), 4: plain connection loss
            code = (why.find("host left") != std::string::npos || why.find("partner ended") != std::string::npos) ? 8 : 4;
            logf_("n64run", code == 8 ? "A player left, game ended" : "Connection lost");
        }
        else if (g_run.lockstep && g_run.lockstep->peer_left() && !g_run.lockstep->desync_detected()) {
            logf_("n64run", "a partner ended the session");
            if (args.frames == 0) code = 8;   // interactive run: tell the hub (headless --frames runs end together and stay 0)
        }
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
    if (g_dj_rc) { logf_("n64run", "dropin: could not join (exit %d)", g_dj_rc); }
    g_core.DoCommand(M64CMD_ROM_CLOSE, 0, nullptr);
    g_core.Detach(M64PLUGIN_GFX);
    g_core.Detach(M64PLUGIN_AUDIO);
    g_core.Detach(M64PLUGIN_INPUT);
    g_core.Detach(M64PLUGIN_RSP);
    g_core.Shutdown();
    session.stop();
    fs::remove_all(cfg_dir, ec);
    return g_dj_rc ? g_dj_rc : 4;
}
