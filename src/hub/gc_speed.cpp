/* GameCube online speed harness (QA tool, not a ctest). Runs one or two Dolphin instances on a private hidden desktop, muted, with a
 * scripted virtual pad (DSU server on 127.0.0.1) so the game gets past its menus, and records the REAL emulation speed per second.
 *
 * Speed source: a VI-rate counter in the game's own RAM (MEM1 read with ReadProcessMemory). `--calibrate` (offline, 100 % speed) finds
 * words that advance by 59-61 every second for many seconds in a row (the SDK's VI retrace count / the game's 60 Hz frame counter);
 * they are written to <game>.vi.txt and the measuring runs read them: speed % = delta per wall second / 59.94. The median of all
 * candidates is used, so one frame counter that pauses on a loading screen does not skew the trace. Dolphin's own frame timing is
 * collected as well (GFX LogRenderTimeToFile -> User\Logs\render_time.txt).
 *
 * Modes:
 *   offline      one Dolphin, RetroHub's offline config (prepare_dolphin)                                      row A
 *   np-direct    two Dolphins, NetPlay host + guest straight over 127.0.0.1 (no RetroHub forwarder)             rows B / C / I
 *   np-fw        two Dolphins through hubnp3ds::Runner forwarders over hubnet::Session (local wrangler)         rows D / E / F
 *   np-trav      two Dolphins, Dolphin's own traversal server (host code), no RetroHub forwarder                row H
 * Options:
 *   --stock      NetPlay with Dolphin's defaults (only the address / nickname / game folder keys are written)  row C
 *   --ini S.K=V  extra Dolphin.ini value for every instance (after RetroHub's writers); --gfx S.K=V for GFX.ini
 *   --buffer N   host pad buffer typed into the NetPlay dialog (default: whatever the ini says)
 *   --pump-ms N  np-fw: call Runner::update() every N ms from a "UI" thread (16 = the hub's frame loop)
 * usage: hub_gc_speed --dolphin <dir> --disc <file> --game GYQE01 --work <dir> --mode offline [--secs 60] [--warm 45] [--calibrate]
 *                     [--server ws://127.0.0.1:8761] [--port 27400] [--dsu 26791] [--video D3D] [--tag name] */
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "controls.h"
#include "dsu_server.h"
#include "emulators.h"
#include "logic.h"
#include "netplay_3ds_run.h"
#include "netplay_dolphin.h"
#include "session.h"

namespace fs = std::filesystem;

static std::string rd(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static void wr(const fs::path& p, const std::string& t) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << t; }
static double now_s() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart; }

struct Opt {
    std::string gci, dolphin, disc, game, work, mode = "offline", server, video = "D3D", tag = "run", pattern = "msb";
    int secs = 60, warm = 45, port = 27400, dsu = 26791, buffer = -1, pump_ms = 16;
    bool calibrate = false, stock = false;
    std::vector<std::string> ini, gfx;
};

/* ---------- scripted pad ---------- */
static std::atomic<bool> g_quit{false};
static int g_shot_every = 20;
static std::atomic<double> g_game_t0{0};   // set once every instance runs the game: the script clock starts there
/* Script: "msb" = Start on the title, then A every 0.6 s (menus default to Exhibition / CPU picks), B never;
 * "seq:<t>=<btn>,..." = custom one-shot presses (seconds since start; btn a|b|x|y|z|s|u|d|l|r). R3 = screenshot every g_shot_every s. */
static uint16_t btn_of(char c) {
    switch (c) { case 'a': return dsu::kA; case 'b': return dsu::kB; case 'x': return dsu::kX; case 'y': return dsu::kY; case 'z': return dsu::kRB;
                 case 's': return dsu::kStart; case 'u': return dsu::kUp; case 'd': return dsu::kDown; case 'l': return dsu::kLeft; case 'r': return dsu::kRight; }
    return 0;
}
static void pad_thread(dsu::DsuServer* srv, std::string pattern) {
    std::vector<std::pair<double, uint16_t>> seq;
    if (pattern.rfind("seq:", 0) == 0) {
        std::stringstream ss(pattern.substr(4)); std::string it;
        while (std::getline(ss, it, ',')) { size_t eq = it.find('='); if (eq != std::string::npos && eq + 1 < it.size()) seq.push_back({std::atof(it.substr(0, eq).c_str()), btn_of(it[eq + 1])}); }
    }
    while (!g_quit && g_game_t0.load() == 0) Sleep(10);
    const double t0 = g_game_t0.load();
    while (!g_quit) {
        const double t = now_s() - t0;
        const int ms = (int)(t * 1000);
        dsu::Pad p;
        if (!seq.empty()) {
            for (auto& e : seq) if (t >= e.first && t < e.first + 0.12) p.buttons |= e.second;
        } else if (t > 3) {
            if (pattern == "msb") {
                if (t < 40 && ms % 4000 < 100) p.buttons |= dsu::kStart;   // health screen / title
                if (ms % 600 < 90) p.buttons |= dsu::kA;
            } else {
                if (ms % 500 < 80) p.buttons |= dsu::kA;
                if (ms % 8000 < 100) p.buttons |= dsu::kStart;
            }
        }
        if (ms % (g_shot_every * 1000) < 100) p.buttons |= dsu::kR3;
        srv->set_pad(0, p);
        Sleep(10);
    }
}

/* ---------- per-instance user dir ---------- */
static const char* kDev = "DSUClient/0/RHGC";
static void setup_user(const Opt& o, const fs::path& inst, bool netplay_stock) {
    std::error_code ec;
    fs::remove_all(inst, ec);
    fs::create_directories(inst / "User" / "Config");
    hubemu::DolphinPads pads;
    pads.dev[0] = kDev;
    hubemu::prepare_dolphin(inst.string(), pads, hubctl::default_profile("gc"));   // RetroHub's offline writers (Dolphin.ini, GFX.ini, pads, hotkeys)
    const fs::path cfg = inst / "User" / "Config";
    std::string t = rd(cfg / "Dolphin.ini");
    t = hubui::ini_set(t, "Core", "GFXBackend", o.video);
    t = hubui::ini_set(t, "Core", "SIDevice0", "6");
    t = hubui::ini_set(t, "Core", "SlotA", "8");   // GCI folder memory card (as RetroHub installs use): no "progress will not be saved" prompt
    t = hubui::ini_set(t, "DSP", "Backend", "No Audio Output");
    t = hubui::ini_set(t, "DSP", "Volume", "0");
    t = hubui::ini_set(t, "Interface", "ConfirmStop", "False");
    t = hubui::ini_set(t, "Interface", "UsePanicHandlers", "False");
    t = hubui::ini_set(t, "Display", "RenderToMain", "False");
    t = hubui::ini_set(t, "General", "ISOPath0", fs::path(o.disc).parent_path().string());
    t = hubui::ini_set(t, "General", "ISOPaths", "1");
    t = hubui::ini_set(t, "Input", "BackgroundInput", "True");   // the window on a hidden desktop never has focus
    t = hubui::ini_set(t, "General", "HotkeysRequireFocus", "False");
    t = hubui::ini_set(t, "Analytics", "Enabled", "False");
    t = hubui::ini_set(t, "Analytics", "PermissionAsked", "True");
    t = hubui::ini_set(t, "AutoUpdate", "UpdateTrack", "");
    (void)netplay_stock;
    wr(cfg / "Dolphin.ini", t);
    std::string g = rd(cfg / "GFX.ini");
    g = hubui::ini_set(g, "Settings", "LogRenderTimeToFile", "True");
    wr(cfg / "GFX.ini", g);
    wr(cfg / "DSUClient.ini", "[Server]\nEnabled = True\nEntries = RHGC:127.0.0.1:" + std::to_string(o.dsu) + ";\n");
    wr(cfg / "GCPadNew.ini",
       std::string("[GCPad1]\nDevice = ") + kDev +
           "\nButtons/A = Cross\nButtons/B = Circle\nButtons/X = Square\nButtons/Y = Triangle\nButtons/Z = R1\nButtons/Start = Options\n"
           "Main Stick/Up = `Left Y+`\nMain Stick/Down = `Left Y-`\nMain Stick/Left = `Left X-`\nMain Stick/Right = `Left X+`\nMain Stick/Calibration = 100.00 141.42 100.00 141.42 100.00 141.42 100.00 141.42\n"
           "D-Pad/Up = `Pad N`\nD-Pad/Down = `Pad S`\nD-Pad/Left = `Pad W`\nD-Pad/Right = `Pad E`\nTriggers/L = L2\nTriggers/R = R2\n"
           "[GCPad2]\nDevice = DInput/0/Keyboard Mouse\n[GCPad3]\nDevice = DInput/0/Keyboard Mouse\n[GCPad4]\nDevice = DInput/0/Keyboard Mouse\n");
    wr(cfg / "Hotkeys.ini", std::string("[Hotkeys]\nDevice = ") + kDev + "\nGeneral/Take Screenshot = R3\n");
}
/* Memory card like the hub: SlotA = GCI folder, the folder passed with -C (a copy of the given saves, never the original). */
static std::wstring card_args(const Opt& o, const fs::path& inst) {
    const fs::path card = inst / "GCI";
    std::error_code ec;
    fs::create_directories(card, ec);
    if (!o.gci.empty())
        for (auto& e : fs::directory_iterator(o.gci, ec))
            if (e.path().extension() == ".gci") fs::copy_file(e.path(), card / e.path().filename(), fs::copy_options::overwrite_existing, ec);
    return L" -C Dolphin.Core.SlotA=8 -C \"Dolphin.Core.GCIFolderAPathOverride=" + card.wstring() + L"\"";
}
static void apply_overrides(const Opt& o, const fs::path& inst) {
    const fs::path cfg = inst / "User" / "Config";
    auto app = [](const fs::path& f, const std::vector<std::string>& kv) {
        std::string t = rd(f);
        for (const std::string& e : kv) {
            size_t dot = e.find('.'), eq = e.find('=');
            if (dot == std::string::npos || eq == std::string::npos || eq < dot) continue;
            t = hubui::ini_set(t, e.substr(0, dot), e.substr(dot + 1, eq - dot - 1), e.substr(eq + 1));
        }
        wr(f, t);
    };
    app(cfg / "Dolphin.ini", o.ini);
    app(cfg / "GFX.ini", o.gfx);
}

/* ---------- guest RAM ---------- */
static uintptr_t find_mem1(HANDLE proc, const std::string& game) {
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0; VirtualQueryEx(proc, (LPCVOID)a, &mbi, sizeof mbi) == sizeof mbi; a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_MAPPED || mbi.RegionSize < 0x1800000) continue;
        char id[6] = {};
        SIZE_T n = 0;
        if (ReadProcessMemory(proc, mbi.BaseAddress, id, 6, &n) && n == 6 && !memcmp(id, game.data(), 6)) return (uintptr_t)mbi.BaseAddress;
    }
    return 0;
}
static bool read_u32(HANDLE proc, uintptr_t base, uint32_t off, uint32_t* v) {
    uint8_t b[4]; SIZE_T n = 0;
    if (!ReadProcessMemory(proc, (LPCVOID)(base + off), b, 4, &n) || n != 4) return false;
    *v = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    return true;
}
static std::vector<uint32_t> snap(HANDLE proc, uintptr_t base) {
    std::vector<uint32_t> v(0x1800000 / 4);
    SIZE_T n = 0;
    ReadProcessMemory(proc, (LPCVOID)base, v.data(), v.size() * 4, &n);
    for (auto& x : v) x = _byteswap_ulong(x);
    return v;
}

struct Inst { std::string name; HANDLE proc = nullptr; DWORD pid = 0; uintptr_t ram = 0; fs::path dir; };

static HANDLE spawn_on_desktop(const std::wstring& cmd, const std::wstring& cwd, const std::wstring& desk, DWORD* pid) {
    STARTUPINFOW si{}; si.cb = sizeof si; si.lpDesktop = (LPWSTR)desk.c_str();
    PROCESS_INFORMATION pi{};
    std::wstring c = cmd;
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si, &pi)) return nullptr;
    CloseHandle(pi.hThread);
    *pid = pi.dwProcessId;
    return pi.hProcess;
}
static void close_pid(DWORD pid) {
    if (!pid) return;
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!h) return;
    TerminateProcess(h, 0);   // only our own processes (pids we started)
    WaitForSingleObject(h, 5000);
    CloseHandle(h);
}

static std::vector<uint32_t> load_vi(const Opt& o) {
    std::vector<uint32_t> v;
    std::ifstream f(fs::path(o.work) / (o.game + ".vi.txt"));
    std::string l;
    while (std::getline(f, l)) if (!l.empty()) v.push_back((uint32_t)std::stoul(l, nullptr, 16));
    return v;
}

static double median(std::vector<double> v) { if (v.empty()) return -1; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }

int main(int argc, char** argv) {
    Opt o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nx = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--dolphin") o.dolphin = nx(); else if (a == "--disc") o.disc = nx(); else if (a == "--game") o.game = nx();
        else if (a == "--work") o.work = nx(); else if (a == "--mode") o.mode = nx(); else if (a == "--server") o.server = nx();
        else if (a == "--video") o.video = nx(); else if (a == "--tag") o.tag = nx(); else if (a == "--pattern") o.pattern = nx();
        else if (a == "--secs") o.secs = std::atoi(nx().c_str()); else if (a == "--warm") o.warm = std::atoi(nx().c_str());
        else if (a == "--port") o.port = std::atoi(nx().c_str()); else if (a == "--dsu") o.dsu = std::atoi(nx().c_str());
        else if (a == "--buffer") o.buffer = std::atoi(nx().c_str()); else if (a == "--pump-ms") o.pump_ms = std::atoi(nx().c_str());
        else if (a == "--calibrate") o.calibrate = true; else if (a == "--stock") o.stock = true;
        else if (a == "--gci") o.gci = nx();
        else if (a == "--shot-every") g_shot_every = std::max(2, std::atoi(nx().c_str()));
        else if (a == "--ini") o.ini.push_back(nx()); else if (a == "--gfx") o.gfx.push_back(nx());
    }
    const fs::path exe = fs::path(o.dolphin) / "Dolphin.exe";
    if (!fs::exists(exe) || !fs::exists(o.disc) || o.game.size() != 6 || o.work.size() < 6) { std::printf("usage error (see the header comment)\n"); return 2; }
    const bool np = o.mode != "offline" && o.mode != "offline2";   // offline2: two independent single-player Dolphins at once (same CPU load as a 2-instance NetPlay test)
    if (o.mode == "np-fw") { if (o.server.empty()) { std::printf("--server needed\n"); return 2; } }
    timeBeginPeriod(1);
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);

    dsu::DsuServer srv;
    if (!srv.start((uint16_t)o.dsu, true)) { std::printf("FAIL: DSU port %d busy\n", o.dsu); return 1; }
    srv.set_pad(0, dsu::Pad());
    const fs::path run = fs::path(o.work) / o.tag;
    std::error_code ec;
    fs::remove_all(run, ec);
    fs::create_directories(run);
    const std::wstring desk = L"rhgcspeed_" + std::to_wstring(GetCurrentProcessId());
    HDESK hd = CreateDesktopW(desk.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!hd) { std::printf("FAIL CreateDesktop\n"); return 1; }
    const std::string desk8(desk.begin(), desk.end());

    std::vector<Inst> insts;
    std::vector<DWORD> own_pids;
    gcnp::Run ra, rb;
    hubnp3ds::Runner fh, fg;
    std::atomic<bool> pump_quit{false};
    std::thread pumper;
    int fails = 0;
    std::string note;

    if (!np) for (int k = 0; k < (o.mode == "offline2" ? 2 : 1); ++k) {
        Inst a; a.name = k ? "solo2" : "solo"; a.dir = run / a.name;
        setup_user(o, a.dir, false);
        apply_overrides(o, a.dir);
        std::wstring cmd = L"\"" + exe.wstring() + L"\" -u \"" + (a.dir / "User").wstring() + L"\" -b" + card_args(o, a.dir) + L" -e \"" + fs::path(o.disc).wstring() + L"\"";
        a.proc = spawn_on_desktop(cmd, exe.parent_path().wstring(), desk, &a.pid);
        if (!a.proc) { std::printf("FAIL: start Dolphin\n"); return 1; }
        own_pids.push_back(a.pid);
        insts.push_back(a);
    } else {
        gcnp::Spec s;
        s.game_id = o.game; s.players = 2; s.port = o.port; s.title = "";
        s.mode = o.mode == "np-trav" ? "traversal" : "direct";
        s.addr = "127.0.0.1";
        Inst h; h.name = "host"; h.dir = run / "host";
        Inst g; g.name = "guest"; g.dir = run / "guest";
        setup_user(o, h.dir, o.stock);
        setup_user(o, g.dir, o.stock);
        hubnp3ds::Plan plan; plan.port = o.port; plan.expected = 2;
        gcnp::Spec gs = s;
        if (o.mode == "np-fw") {
            std::string err;
            const std::string code = hubnet::new_room_code();   // the room alphabet has no 0/1
            if (!fh.start_host_forward(o.server, code, 1, plan, "gc", &err) || !fg.start_guest(o.server, code, 1, plan, 1, &err, "gc")) { std::printf("FAIL fw: %s\n", err.c_str()); return 1; }
            pumper = std::thread([&] { while (!pump_quit) { fh.update(); fg.update(); Sleep((DWORD)std::max(1, o.pump_ms)); } });   // the hub's UI loop
            for (int i = 0; i < 400 && !(fh.ready() && fg.ready()); ++i) { Sleep(100); if (fh.failed() || fg.failed()) break; }
            if (!fh.ready() || !fg.ready()) { std::printf("FAIL fw connect: %s / %s\n", fh.status().c_str(), fg.status().c_str()); pump_quit = true; pumper.join(); return 1; }
            Sleep(1500);
            note = std::string("forwarder relayed=") + (fg.any_relayed() ? "1" : "0") + " ping=" + std::to_string(fg.worst_ping_ms()) + "ms";
            gs.port = fg.connect_port();
        }
        for (Inst* x : {&h, &g}) {
            const bool host = x == &h;
            const fs::path ini = x->dir / "User" / "Config" / "Dolphin.ini";
            const gcnp::Spec& sp = host ? s : gs;
            if (o.stock) {   // Dolphin's own NetPlay defaults: only what is needed to find each other
                std::string t = rd(ini);
                t = hubui::ini_set(t, "NetPlay", "Nickname", host ? "HostA" : "GuestB");
                t = hubui::ini_set(t, "NetPlay", "TraversalChoice", sp.mode == "direct" ? "direct" : "traversal");
                t = hubui::ini_set(t, "NetPlay", "Address", sp.addr);
                t = hubui::ini_set(t, "NetPlay", "ConnectPort", std::to_string(sp.port));
                t = hubui::ini_set(t, "NetPlay", "HostPort", std::to_string(sp.port));
                wr(ini, t);
            } else {
                gcnp::IniParams ip; ip.host = host; ip.nickname = host ? "HostA" : "GuestB"; ip.iso_dir = fs::path(o.disc).parent_path().string(); ip.mute = true;
                if (o.mode == "np-fw") { ip.ping_ms = fh.worst_ping_ms(); ip.relayed = fh.any_relayed(); }
                wr(ini, gcnp::ini_patch(rd(ini), sp, ip));
            }
            apply_overrides(o, x->dir);
        }
        if (!gcnp::start(true, exe.string(), (h.dir / "User").string(), s, 0, (run / "workA").string(), desk8, fs::path(card_args(o, h.dir)).string(), &ra)) { std::printf("FAIL host start: %s\n", ra.error.c_str()); return 1; }
        own_pids.push_back(ra.dolphin_pid); own_pids.push_back(ra.driver_pid);
        if (o.buffer > 0) gcnp::set_host_buffer(ra, o.buffer);
        else if (o.mode == "np-fw" && !o.stock) gcnp::set_host_buffer(ra, gcnp::pad_buffer_for_ping(fh.worst_ping_ms(), fh.any_relayed()));   // what the hub does once the room is connected
        std::string st;
        for (int i = 0; i < 600 && (st = gcnp::read_status(ra)).find("stage:netplay-open") == std::string::npos; ++i) { Sleep(250); if (st.find("error:") != std::string::npos) break; }
        if (o.mode == "np-trav") {
            std::string code;
            for (int i = 0; i < 240 && (code = gcnp::read_code(ra)).empty(); ++i) Sleep(250);
            if (code.empty()) { std::printf("FAIL: no traversal host code\n%s\n", gcnp::read_status(ra).c_str()); ++fails; }
            note = "traversal code " + code;
            gs.host_code = code;
            const fs::path gi = g.dir / "User" / "Config" / "Dolphin.ini";
            std::string t = rd(gi);
            t = hubui::ini_set(t, "NetPlay", "TraversalChoice", "traversal");
            t = hubui::ini_set(t, "NetPlay", "HostCode", code);
            wr(gi, t);
        }
        if (!fails && !gcnp::start(false, exe.string(), (g.dir / "User").string(), gs, 1, (run / "workB").string(), desk8, fs::path(card_args(o, g.dir)).string(), &rb)) { std::printf("FAIL guest start\n"); ++fails; }
        own_pids.push_back(rb.dolphin_pid); own_pids.push_back(rb.driver_pid);
        h.pid = ra.dolphin_pid; g.pid = rb.dolphin_pid;
        h.proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, h.pid);
        g.proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, g.pid);
        insts.push_back(h); insts.push_back(g);
    }

    std::thread padth(pad_thread, &srv, o.pattern);
    /* wait for the game (MEM1 holding the disc id), then warm up into gameplay */
    const double t_launch = now_s();
    for (auto& x : insts) {
        for (int i = 0; i < 1200 && !x.ram && !fails; ++i) {
            Sleep(250);
            if (!x.proc || WaitForSingleObject(x.proc, 0) == WAIT_OBJECT_0) { std::printf("FAIL: %s Dolphin exited\n", x.name.c_str()); ++fails; break; }
            x.ram = find_mem1(x.proc, o.game);
        }
        if (!x.ram && !fails) { std::printf("FAIL: %s game RAM not found\n%s\n%s\n", x.name.c_str(), gcnp::read_status(ra).c_str(), gcnp::read_status(rb).c_str()); ++fails; }
    }
    const double t_boot = now_s() - t_launch;
    g_game_t0 = now_s();
    std::printf("[%s] mode=%s game=%s boot %.1fs dsu_clients=%d %s\n", o.tag.c_str(), o.mode.c_str(), o.game.c_str(), t_boot, srv.clients(), note.c_str());

    if (!fails) {
        Sleep((DWORD)o.warm * 1000);
        if (o.calibrate) {
            Inst& x = insts[0];
            std::vector<uint32_t> prev = snap(x.proc, x.ram);
            std::vector<int> ok(prev.size(), 0);
            std::vector<double> sum(prev.size(), 0.0);
            double tp = now_s();
            for (int k = 0; k < o.secs; ++k) {
                Sleep(1000);
                std::vector<uint32_t> cur = snap(x.proc, x.ram);
                double tn = now_s(), dt = tn - tp; tp = tn;
                for (size_t i = 0; i < cur.size(); ++i) {
                    double r = (double)(uint32_t)(cur[i] - prev[i]) / dt;
                    if (r >= 40 && r <= 80) { ++ok[i]; sum[i] += r; } else ok[i] = -1000000;   // a loading hitch moves one second, not the average
                }
                prev.swap(cur);
            }
            std::string out;
            int n = 0;
            for (size_t i = 0; i < ok.size() && n < 32; ++i) if (ok[i] >= o.secs && sum[i] / ok[i] >= 59.0 && sum[i] / ok[i] <= 61.0) { char b[16]; snprintf(b, sizeof b, "%08X\n", (unsigned)(i * 4)); out += b; ++n; }
            wr(fs::path(o.work) / (o.game + ".vi.txt"), out);
            std::printf("calibrate: %d VI-rate counters (every second 40-80/s, mean 59-61/s) over %d s: %s\n", n, o.secs, out.empty() ? "(none)" : "");
            std::printf("%s", out.c_str());
            if (!n) ++fails;
        } else {
            const std::vector<uint32_t> vi = load_vi(o);
            if (vi.empty()) { std::printf("FAIL: no %s.vi.txt (run --calibrate offline first)\n", o.game.c_str()); ++fails; }
            std::ofstream csv(run / "trace.csv");
            csv << "sec";
            for (auto& x : insts) csv << "," << x.name << "_pct";
            csv << "\n";
            std::vector<std::vector<uint32_t>> last(insts.size());
            for (size_t j = 0; j < insts.size(); ++j) for (uint32_t a : vi) { uint32_t v = 0; read_u32(insts[j].proc, insts[j].ram, a, &v); last[j].push_back(v); }
            double tp = now_s();
            std::vector<std::vector<double>> trace(insts.size());
            std::string dgline = "FWRATE host datagrams out/s:";   // np-fw: Dolphin NetPlay packets the host forwarder carried each second (pad packets ~ SI polls)
            unsigned long long dg_last = fh.active() ? fh.datagrams_out() : 0;
            for (int k = 0; k < o.secs && !fails; ++k) {
                Sleep(1000);
                double tn = now_s(), dt = tn - tp; tp = tn;
                csv << k + 1;
                for (size_t j = 0; j < insts.size(); ++j) {
                    std::vector<double> r;
                    for (size_t c = 0; c < vi.size(); ++c) { uint32_t v = 0; if (!read_u32(insts[j].proc, insts[j].ram, vi[c], &v)) continue; r.push_back((double)(uint32_t)(v - last[j][c]) / dt / 59.94 * 100.0); last[j][c] = v; }
                    double m = median(r);
                    trace[j].push_back(m);
                    char b[32]; snprintf(b, sizeof b, ",%.1f", m); csv << b;
                    if (WaitForSingleObject(insts[j].proc, 0) == WAIT_OBJECT_0) { std::printf("FAIL: %s died\n", insts[j].name.c_str()); ++fails; }
                }
                csv << "\n";
                if (fh.active()) { unsigned long long d = fh.datagrams_out(); char b[16]; snprintf(b, sizeof b, " %llu", d - dg_last); dgline += b; dg_last = d; }
            }
            if (fh.active()) std::printf("%s\n", dgline.c_str());
            for (size_t j = 0; j < insts.size(); ++j) {
                std::vector<double> t = trace[j];
                if (t.empty()) continue;
                double sum = 0; int below90 = 0, below50 = 0; for (double v : t) { sum += v; if (v < 90) ++below90; if (v < 50) ++below50; }
                std::sort(t.begin(), t.end());
                std::printf("RESULT %s %s: mean %.1f%% median %.1f%% p10 %.1f%% min %.1f%% | secs<90%%: %d secs<50%%: %d of %zu\n", o.tag.c_str(), insts[j].name.c_str(), sum / t.size(), t[t.size() / 2], t[t.size() / 10], t[0], below90, below50, t.size());
                std::string line = "TRACE " + insts[j].name + ":";
                for (double v : trace[j]) { char b[16]; snprintf(b, sizeof b, " %.0f", v); line += b; }
                std::printf("%s\n", line.c_str());
            }
        }
    }
    /* Dolphin's own timing: vblank_times.txt = wall ms between emulated VIs (60/s at full speed), render_times.txt = per presented frame */
    for (auto& x : insts) {
        for (const char* fn : {"vblank_times.txt", "render_times.txt"}) {
            std::ifstream f(x.dir / "User" / "Logs" / fn);
            std::vector<double> ft; std::string l;
            while (std::getline(f, l)) { try { if (!l.empty()) ft.push_back(std::stod(l)); } catch (...) {} }
            if (ft.size() < 200) { std::printf("DOLPHIN %s %s: %zu values\n", x.name.c_str(), fn, ft.size()); continue; }
            /* the measured window: the last o.secs seconds of wall time */
            double acc = 0; size_t i0 = ft.size();
            while (i0 > 0 && acc < o.secs * 1000.0) acc += ft[--i0];
            std::vector<double> tail(ft.begin() + i0, ft.end());
            double sum = 0; int big = 0; for (double v : tail) { sum += v; if (v > 50) ++big; }
            const double rate = tail.size() / (sum / 1000.0);
            std::printf("DOLPHIN %s %s: %zu in %.1f s = %.1f/s%s, intervals >50 ms: %d, max %.0f ms\n", x.name.c_str(), fn, tail.size(), sum / 1000.0, rate,
                        std::string(fn) == "vblank_times.txt" ? (" (speed " + std::to_string((int)(rate / 59.94 * 100 + 0.5)) + "%)").c_str() : "", big, *std::max_element(tail.begin(), tail.end()));
            if (std::string(fn) == "vblank_times.txt") {   // per wall second: emulated VIs that landed in it
                std::string line = "VBTRACE " + x.name + ":"; double t = 0; int n = 0;
                for (double v : tail) { t += v; ++n; if (t >= 1000) { char b2[16]; snprintf(b2, sizeof b2, " %.0f", n / 59.94 * 100 * 1000 / t); line += b2; t = 0; n = 0; } }
                std::printf("%s\n", line.c_str());
            }
        }
    }
    if (np) {
        std::string sa = gcnp::read_status(ra), sb = gcnp::read_status(rb);
        wr(run / "host_status.txt", sa); wr(run / "guest_status.txt", sb);
        for (const std::string* s : {&sa, &sb}) { std::istringstream is(*s); std::string l; while (std::getline(is, l)) if (l.rfind("error:", 0) == 0 || l.rfind("buffer:", 0) == 0 || l.rfind("padbuffer:", 0) == 0 || l.rfind("desync", 0) == 0 || l.find("esync") != std::string::npos) std::printf("driver: %s\n", l.c_str()); }
    }

    g_quit = true;
    padth.join();
    for (auto& x : insts) if (x.proc) CloseHandle(x.proc);
    for (DWORD p : own_pids) close_pid(p);
    if (pumper.joinable()) { pump_quit = true; pumper.join(); }
    fh.stop(); fg.stop();
    srv.stop();
    CloseDesktop(hd);
    timeEndPeriod(1);
    std::printf(fails ? "RUN FAILED (%d)\n" : "RUN OK\n", fails);
    return fails ? 1 : 0;
}
