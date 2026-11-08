/* RetroHub.exe - the launcher front door.
 *
 * Layout: full-window custom UI (no floating windows): left sidebar nav + content pages
 * (Library, Play Online, Saves, Controllers, Settings, About). Drawn with Dear ImGui on SDL_Renderer.
 *
 * Launch contracts:
 *  - N64 (emulated):  n64run.exe --rom <path> --game <id|unknown> --save-dir <battery dir>
 *                     [--net <server> --room <CODE>-<launch> --slot <lobby slot> --players <n>
 *                      --ports <p0,p1,p2,p3> --delay <d>]
 *  - GBA/GB/GBC/DS:   retrorun.exe --core <x_libretro.dll> --rom <path> --system <gba|gb|gbc|nds>
 *                     --save-dir <battery dir> --state-dir <states dir> --resume [--fullscreen]
 *                     [--net ... same flags as n64run; online uses a fresh temp save dir, no --resume]
 *  - GameCube:        Dolphin (RetroHub download, portable) -b -e <game>; see hubemu::dolphin_args
 *  - 3DS:             <data>\emulators\azahar\azahar.exe -f <game>   (src/hub/emulators.cpp; downloaded on demand)
 *  - Wii U:           Cemu.exe -g <game> -f  (an existing install, else <data>\emulators\cemu with a portable folder)
 *  - Smash (native):  BattleShip.exe --from-hub --no-update-check
 *                     (ALWAYS passed: skips handing back to the hub, and RetroHub's updater is the only one)
 *                     Online, environment variables:
 *                       SSB64_LOBBY_HOST=<server>  (host)   or  SSB64_LOBBY_JOIN=<server> (joiners)
 *                       SSB64_LOBBY_CODE=<CODE>             (BattleShip joins/hosts this exact code;
 *                                                           players pick fighter/stage in its Online window)
 *                       (no autostart: the host presses Start match in Smash)
 */
#include "winutil.h"
#include "pcsx2.h"
#include "ps2.h"
#include "ppsspp.h"
#include "lichii.h"
#include "nx_runner.h"
#include "wiiu_meta.h"
#include "invite.h"
#include "dolphin_speed.h"
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"
#include "app.h"
#include "launch_guard_logic.h"
#include "exit_combo.h"
#include "remote_play.h"
#include "hint_popup.h"
#include "controls_sdl.h"
#include "qm_logic.h"
#include "resume_logic.h"
#include "online_caps.h"
#include "wave3_hooks.h"
#include "resume_more.h"
#include "hookshot.h"
#include "hookshot_build.h"
#include <thread>
#include <xinput.h>

hubemu::Installer EMU;   // download-on-demand job + its dialog state
bool EMU_UPDATE_SKIP[8] = {};
UnzipJob UNZ;
hubnp3ds::Runner NP3;                 // 3DS local-wireless forwarder (np_3ds)
std::string NP3_mp;                    // "-m ..." for Azahar once NP3 is ready
hub::GameEntry NP3_game;
bool NP3_pending = false;

/* ---------- process launching ---------- */

static starthint::Popup g_hint;

bool spawn(const std::string& exe, const std::string& args_in, const std::string& cwd,
                  const std::vector<std::pair<std::string, std::string>>& env, const RunInfo& ri) {
    if (launch_blocked("spawn")) return false;
    std::string args = args_in;
    if (!A.fresh_key.empty()) {   // Home hero "Start fresh": one launch without the auto resume
        if (A.fresh_key == ri.key) { size_t k = args.find(" --resume"); if (k != std::string::npos) args.erase(k, 9); }
        A.fresh_key.clear();
    }
    if (!A.resume_key.empty()) {   // "Resume": one-shot --load-state for exactly this game on a retrorun launch (Lane 2 runners; docs/RUNNER_CTL)
        if (A.resume_key == ri.key && (file_name(exe) == "retrorun.exe" || file_name(exe) == "n64run.exe") && !A.resume_state.empty()) args += " --load-state " + quote(A.resume_state);
        A.resume_key.clear();
        A.resume_state.clear();
    }
    if (file_name(exe) == "retrorun.exe" && args.find(" --net") == std::string::npos && args.find("--net ") != 0)
        args += achievements_launch_args();   // RetroAchievements: offline runs only (never online/rollback/DS wireless)
    if (g_headless) {
        std::string ev;
        for (auto& kv : env) ev += " " + kv.first + "=" + (kv.first == "SSB64_CONTROLS" ? std::string("<profile>") : kv.second);
        shot_trace("spawn", file_name(exe) + " | " + args + (ev.empty() ? "" : " |env" + ev));
        set_notice("Headless test run: launch skipped.");
        return false;
    }
    if (!hubui::launch_allowed(A.proc != nullptr, session_active(), /*system_launch=*/true)) { set_notice(hubui::launch_busy_reason(true)); return false; }   // rooms start their own game inside their own session: only A.proc refuses
    if (!file_exists(exe)) { set_notice("Cannot find " + file_name(exe) + " (" + exe + ")"); return false; }
    if (!A.st.start_hint) SetEnvironmentVariableW(L"RETROHUB_START_HINT", L"0");   // retrorun / n64run read it (inherited by the child)
    std::wstring cmd = L"\"" + W(exe) + L"\" " + W(args);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    for (auto& e : env) SetEnvironmentVariableW(W(e.first).c_str(), W(e.second).c_str());
    const std::string exe_name = file_name(exe);
    const bool own_runner = exe_name == "retrorun.exe" || exe_name == "n64run.exe" || exe_name == "streamrun.exe" || exe == smash_exe_path();   // our own processes that open /hub: they prove the account (strict rooms); emulators never get it
    if (own_runner) fr_ident_env(true);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    /* CREATE_NO_WINDOW: n64run / retrorun are console-subsystem programs (they log to files). Without the flag Windows gives them a
     * console, which on PCs where Windows Terminal is the default console host opens a visible terminal window over the game.
     * GUI emulators (Dolphin, PCSX2, Azahar, Cemu) ignore the flag. */
    BOOL ok = CreateProcessW(W(exe).c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, W(cwd).c_str(), &si, &pi);
    DWORD err = GetLastError();
    for (auto& e : env) SetEnvironmentVariableW(W(e.first).c_str(), nullptr);
    if (own_runner) fr_ident_env(false);
    SetEnvironmentVariableW(L"RETROHUB_START_HINT", nullptr);
    if (!ok) { set_notice("Could not start " + file_name(exe) + " (error " + std::to_string(err) + ")"); return false; }
    CloseHandle(pi.hThread);
    if (ri.after_exit) {   // external emulator: follow every process it starts, so a relaunch / hand-off keeps the session open
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job && !AssignProcessToJobObject(job, pi.hProcess)) { CloseHandle(job); job = nullptr; }
        A.job_h = job;
    }
    A.proc = pi.hProcess;
    A.proc_title = ri.title;
    A.run = ri;
    A.run.controls_path.clear();
    A.run.controls_sys.clear();
    if (args.find("--controls ") != std::string::npos && !g_launch_controls.first.empty()) { A.run.controls_path = g_launch_controls.first; A.run.controls_sys = g_launch_controls.second; }
    g_launch_controls = {};
    A.run.cloud_started = false;
    A.run.pad_exit = ri.after_exit != nullptr || !ri.o2r_prep.empty();   // external emulators and the native Smash port; n64run / retrorun handle the combo themselves
    A.run.start_unix = unix_now();
    A.run.start_tick = GetTickCount64();
    g_hint = starthint::Popup();   // fresh 5 s controller-tips card for this game (external emulators; the runners draw their own)
    cloud_run_started();
    return true;
}

void sync_smash_prep();

/* ---------- universal leave-game combo for games RetroHub does not own (Dolphin / Azahar / Cemu / mGBA / BattleShip) ---------- */

static bool any_pad_combo() {
    if (autotest_active()) {   // QA only: RETROHUB_AUTOTEST_COMBO_AFTER=<seconds> holds Back+Start virtually once the game has run that long
        static const char* after = std::getenv("RETROHUB_AUTOTEST_COMBO_AFTER");
        if (after && A.proc && GetTickCount64() - A.run.start_tick >= (ULONGLONG)(std::atof(after) * 1000.0)) {
            static bool logged = false;
            if (!logged) { logged = true; autotest_log("virtual exit combo (Back+Start) held"); }
            return true;
        }
    }
    for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE xs{};
        if (XInputGetState(i, &xs) == ERROR_SUCCESS && hubctl::xinput_combo_down(xs.Gamepad.wButtons)) return true;
    }
    SDL_GameControllerUpdate();
    for (auto* c : A.pads) {
        hubctl::PadState st;
        hubctl::read_state(c, &st);
        if (hubctl::exit_combo_down(st)) return true;
    }
    return false;
}

static std::vector<DWORD> game_pids() {
    std::vector<DWORD> v;
    if (A.job_h) {
        char buf[sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 64 * sizeof(ULONG_PTR)];
        auto* l = (JOBOBJECT_BASIC_PROCESS_ID_LIST*)buf;
        if (QueryInformationJobObject(A.job_h, JobObjectBasicProcessIdList, l, sizeof buf, nullptr))
            for (DWORD i = 0; i < l->NumberOfAssignedProcesses; ++i) v.push_back((DWORD)l->ProcessIdList[i]);
    } else if (A.proc) v.push_back(GetProcessId(A.proc));
    return v;
}
/* Largest visible, captioned top-level window of the running game's processes (the one the tips card is anchored to). */
static HWND game_main_window() {
    struct Pick { std::vector<DWORD> pids; HWND best = nullptr; long long area = 0; } pk;
    pk.pids = game_pids();
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* p = (Pick*)lp;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (std::find(p->pids.begin(), p->pids.end(), pid) == p->pids.end() || !IsWindowVisible(h) || (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
        RECT r{};
        if (!GetClientRect(h, &r)) return TRUE;
        long long a = (long long)(r.right - r.left) * (r.bottom - r.top);
        if (a > p->area) { p->area = a; p->best = h; }
        return TRUE;
    }, (LPARAM)&pk);
    return pk.best;
}

bool game_window_pid(unsigned long* pid) {
    HWND h = game_main_window();
    if (!h) return false;
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    *pid = p;
    return p != 0;
}

static void start_hint_tick() {
    if (g_headless || !A.proc || !A.run.pad_exit || !A.st.start_hint) { g_hint.close(); return; }
    g_hint.tick(game_main_window(), false, false);
}

struct CloseCtx { std::vector<DWORD> pids; int n = 0; };
static BOOL CALLBACK close_cb(HWND h, LPARAM lp) {
    auto* c = (CloseCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    for (DWORD p : c->pids) if (p == pid && IsWindowVisible(h)) { PostMessageW(h, WM_CLOSE, 0, 0); c->n++; break; }
    return TRUE;
}

/* Reliable foreground for a USER-requested raise (View+Y open / close). The foreground lock is bypassed by attaching to the current
 * foreground thread's input queue plus the Alt-key trick; verified with GetForegroundWindow and retried briefly (<= ~200 ms). */
static bool force_foreground(HWND target) {
    const HWND before = GetForegroundWindow();
    struct Log { HWND t, b; ~Log() { qm_log(std::string("force_foreground target=") + (t == A.hwnd ? "hub" : "game") + " before=" + qm_describe_window(b) + " after=" + qm_describe_window(GetForegroundWindow())); } } log_guard{target, before};
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (GetForegroundWindow() == target) return true;
        HWND fg = GetForegroundWindow();
        DWORD fgt = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
        const bool attached = fgt && fgt != me && AttachThreadInput(me, fgt, TRUE);
        keybd_event(VK_MENU, 0, 0, 0);
        BringWindowToTop(target);   // popup-guard: ok (user hotkey)
        SetForegroundWindow(target);   // popup-guard: ok (user hotkey)
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        if (attached) AttachThreadInput(me, fgt, FALSE);
        if (GetForegroundWindow() == target) return true;
        Sleep(40);
    }
    return GetForegroundWindow() == target;
}

std::string qm_describe_window(void* hw) {
    HWND h = (HWND)hw;
    if (!h) return "none";
    char cls[64] = "?", title[80] = "";
    GetClassNameA(h, cls, sizeof cls);
    GetWindowTextA(h, title, sizeof title);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    char buf[256];
    std::snprintf(buf, sizeof buf, "%p[pid %lu %s \"%s\"%s]", (void*)h, (unsigned long)pid, cls, title, h == A.hwnd ? " HUB" : "");
    return buf;
}
bool hub_is_foreground() { return A.hwnd && GetForegroundWindow() == A.hwnd; }

void focus_hub() {
    if (!A.hwnd || g_headless) return;
    /* Only for the user's own View+Y / cheats pad combo during a game (qm_open_live, cheats_open_live). Never automatic. */
    if (IsIconic(A.hwnd)) ShowWindow(A.hwnd, SW_RESTORE);   // popup-guard: ok (user hotkey)
    if (qm_is_open()) hub_menu_topmost(true);   // the menu stays visible even if the game takes the foreground back (Dolphin did)
    if (!force_foreground(A.hwnd))
        std::fprintf(stderr, "focus_hub: could not take the foreground (foreground=%p hub=%p); the quick menu still reads the pad directly\n", (void*)GetForegroundWindow(), (void*)A.hwnd);
}

/* Only while the user's quick menu is open (set by focus_hub from qm_open_live, always cleared when the menu closes). */
void hub_menu_topmost(bool on) {
    static bool is_on = false;
    if (!A.hwnd || g_headless || on == is_on) return;
    is_on = on;
    SetWindowPos(A.hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // popup-guard: ok (user's View+Y menu only; cleared on close)
}

void pad_exit_watch() {
    static hubctl::ExitCombo combo;
    static bool closing = false;
    static ULONGLONG closed_at = 0;
    if (!A.proc || !A.run.pad_exit || g_headless) { combo.reset(); closing = false; return; }
    ULONGLONG now = GetTickCount64();
    if (closing) {   // emulator ignored the close for 20 s: leave it alone (never kill the user's emulator)
        if (now - closed_at > 20000) closing = false;
        return;
    }
    if (!combo.update(any_pad_combo(), (long long)now)) return;
    closing = true;
    closed_at = now;
    set_notice(A.proc_title + ": closing (Back + Start held)...", 8.0);
    request_game_close();
}

/* Lichii builds 88-95 (build 88 release notes, known issue; still unfixed in build 95): a fence-worker shutdown race can leave lichii.exe waiting after its window closed. The session
 * (save copy-back, vault sync) waits for the whole job, so once the game had a window and every window of the job has been gone for 15 s,
 * the job is ended. Only RetroHub's own Lichii runs in that job (Kind::Lichii is never a user's install); by handle, never by image name. */
static bool g_hookshot_run = false;   // the running game is Hookshot (set by launch_hookshot)
bool hookshot_running() { return g_hookshot_run && A.proc; }
static bool g_fritchii_run = false;   // the running Switch game is Fritchii (set at spawn)
bool fritchii_running() { return g_fritchii_run && A.proc; }

void lichii_hang_watch() {
    static bool had_window = false;
    static ULONGLONG gone_since = 0;
    static ULONGLONG cpu_at_gone = 0, idle_since = 0;
    if (!A.proc) { g_hookshot_run = false; g_fritchii_run = false; }
    if (!A.proc || A.run.system_short != "switch" || g_headless || g_hookshot_run) { had_window = false; gone_since = 0; return; }   // Hookshot has its own exit; the hang is a Lichii issue (builds 88-95)
    const ULONGLONG now = GetTickCount64();
    if (game_main_window()) { had_window = true; gone_since = 0; return; }
    if (!had_window) return;
    auto cpu = []() -> ULONGLONG {   // total CPU time (100 ns units) of the game process
        FILETIME c, x, k, u;
        if (!GetProcessTimes(A.proc, &c, &x, &k, &u)) return 0;
        return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
    };
    if (!gone_since) { gone_since = now; cpu_at_gone = cpu(); idle_since = now; return; }
    {   /* still busy (saving, shutting threads down): wait; only a process that stopped using the CPU counts as hung */
        const ULONGLONG c = cpu();
        if (c > cpu_at_gone + 2000000) { cpu_at_gone = c; idle_since = now; }   // more than 0.2 s of CPU since the last check
    }
    if (now - gone_since < 15000 || now - idle_since < 10000) return;
    autotest_log("lichii: window closed 15 s ago but the process is still running (Lichii exit hang): ending its job");
    if (A.job_h) TerminateJobObject(A.job_h, 0);
    else TerminateProcess(A.proc, 0);
    had_window = false;
    gone_since = 0;
}

void* running_game_hwnd() { return g_headless ? nullptr : (void*)game_main_window(); }
std::vector<unsigned long> running_game_pids() { std::vector<unsigned long> v; for (DWORD p : game_pids()) v.push_back(p); return v; }

void focus_game() {
    if (g_headless) return;
    hub_menu_topmost(false);   // first: clearing it after the game is raised would put the hub back above the game
    CloseCtx c;
    c.pids = game_pids();
    struct Pick { std::vector<DWORD> pids; HWND hwnd = nullptr; } pk;
    pk.pids = c.pids;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* p = (Pick*)lp;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        for (DWORD q : p->pids) if (q == pid && IsWindowVisible(h) && !p->hwnd) { p->hwnd = h; return FALSE; }
        return TRUE;
    }, (LPARAM)&pk);
    if (!pk.hwnd) return;
    if (IsIconic(pk.hwnd)) ShowWindow(pk.hwnd, SW_RESTORE);   // popup-guard: ok (raises the GAME, not the hub)
    if (!force_foreground(pk.hwnd))   // popup-guard: ok (raises the GAME, not the hub)
        std::fprintf(stderr, "focus_game: game did not become the foreground window\n");
}

/* Wii U: Cemu's default global hotkey "toggle pad screen" is Ctrl+Tab (shown in its first-run assistant). Cemu reads it from the
 * keyboard state of its window, so the game window is brought forward and the keys are injected. */
/* Raises the running game and presses one key chord, but only while a game window is the foreground window (re-checked before every key-down;
 * releases always go out). Returns how many keys were pressed. Shared by the Wii U pad-screen toggle and the PSP state keys. */
static int send_game_chord(const std::vector<int>& keys) {
    focus_game();
    std::vector<DWORD> pids = game_pids();
    auto game_fg = [&]() {   // re-checked before every key-down; releases always go out
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        for (DWORD p : pids) if (p == pid) return !qm_is_open();
        return false;
    };
    return hubqm::run_key_chord(keys, game_fg, [](int k, bool up) {
        INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = (WORD)k; i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        SendInput(1, &i, sizeof(INPUT));
    });
}

void wiiu_toggle_gamepad_screen() {
    if (g_headless || !A.proc) return;
    if (autotest_active()) { autotest_log("wiiu: toggle pad screen hotkey (not injected: QA runs on a hidden desktop, injected keys would reach the user's)"); return; }
    std::vector<int> keys = hubemu::cemu_toggle_gamepad_keys();
    int n = send_game_chord(keys);
    autotest_log(n == (int)keys.size() ? "wiiu: toggle pad screen hotkey sent" : "wiiu: toggle pad screen hotkey aborted (game not foreground)");
}

/* PSP: PPSSPP keeps F2 (quick save state) and F4 (quick load state) as built-in accelerators while RetroHub's controls.ini leaves them unmapped
 * (ppsspp.h). Same pattern as the Wii U pad-screen chord: the game window is raised by the user's own request, keys go out only while the game is foreground. */
void ppsspp_send_state(bool save) {
    if (g_headless || !A.proc || A.run.system_short != "psp") return;
    if (autotest_active()) { autotest_log(std::string("psp: ") + (save ? "save" : "load") + " state key (not injected: QA runs on a hidden desktop)"); return; }
    std::vector<int> keys = hubemu::ppsspp_state_keys(save);
    int n = send_game_chord(keys);
    autotest_log(n == (int)keys.size() ? "psp: state key sent" : "psp: state key aborted (game not foreground)");
}

/* Dolphin Speed row: Dolphin's emulation-speed hotkeys are bound to Ctrl+Shift+F13/F14 (emulators.cpp, dolphin_hotkeys_ini).
 * After the menu closes and focus_game() ran, wait (up to 3 s) until a game window is the foreground window, then press each key.
 * Keys are only ever sent while the game is foreground, so they cannot reach another app. Runs off the UI thread (70 presses at
 * 8x take a few seconds). Nothing is raised or shown here. */
void dolphin_send_speed_keys(const std::vector<int>& vks, std::function<void(int)> done_cb) {
    if (g_headless || !A.proc || vks.empty()) { if (done_cb) done_cb(-1); return; }
    if (autotest_active()) { autotest_log("dolphin: speed hotkeys (" + std::to_string(vks.size()) + ") not injected: QA runs on a hidden desktop"); if (done_cb) done_cb(-1); return; }
    std::vector<DWORD> pids = game_pids();
    std::thread([pids, vks, done_cb]() {
        auto game_fg = [&]() {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            for (DWORD p : pids) if (p == pid) return true;
            return false;
        };
        for (int i = 0; i < 60 && !game_fg(); ++i) Sleep(50);
        if (!game_fg()) { qm_log("speed burst: game never became foreground, not sent"); if (done_cb) done_cb(-1); return; }
        Sleep(150);
        auto key = [](int vk, bool up) {
            INPUT i{};
            i.type = INPUT_KEYBOARD;
            i.ki.wScan = (WORD)vk;   // a scancode: DirectInput (Dolphin's keyboard backend) ignores virtual-key-only events
            i.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
            SendInput(1, &i, sizeof(INPUT));
        };
        /* re-checked before every key event: game still foreground and the hub menu not open; modifiers always released */
        const int done = hubqm::run_key_burst(vks, hubqm::kDolphinScanCtrl, hubqm::kDolphinScanShift, [&] { return game_fg() && !qm_is_open(); },
                             [&](int sc, bool up) { key(sc, up); Sleep(up ? 35 : 20); });   // edge-triggered in Dolphin: hold, then release
        qm_log("speed burst: " + std::to_string(done) + "/" + std::to_string(vks.size()) + " presses");
        if (done_cb) done_cb(done);
    }).detach();
}

/* View (Back) + right bumper while a Wii U game runs. Edge-triggered: one toggle per press. */
void wiiu_pad_watch() {
    static bool prev = false;
    bool down = false;
    if (A.proc && !g_headless && A.run.system_short == "wiiu") {
        for (DWORD i = 0; i < 4 && !down; ++i) {
            XINPUT_STATE xs{};
            if (XInputGetState(i, &xs) == ERROR_SUCCESS && hubemu::cemu_toggle_combo(xs.Gamepad.wButtons)) down = true;
        }
        for (auto* c : A.pads)
            if (!down && SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK) && SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) down = true;
        if (autotest_active()) if (const char* after = std::getenv("RETROHUB_AUTOTEST_PADSWAP_AFTER"))   // QA only: press the combo once after N seconds
            if (!prev && GetTickCount64() - A.run.start_tick >= (ULONGLONG)(std::atof(after) * 1000.0)) { static bool fired = false; if (!fired) { fired = true; down = true; } }
    }
    if (down && !prev) wiiu_toggle_gamepad_screen();
    prev = down;
}

void request_game_close() {
    if (A.run.pad_exit_fn) { A.run.pad_exit_fn(); return; }
    CloseCtx c;
    c.pids = game_pids();
    EnumWindows(close_cb, (LPARAM)&c);
}

void close_pid_windows(unsigned long pid) {
    CloseCtx c;
    c.pids.push_back((DWORD)pid);
    EnumWindows(close_cb, (LPARAM)&c);
}

void poll_process() {
    sync_smash_prep();
    pad_exit_watch();
    start_hint_tick();
    cheats_pad_watch();
    wiiu_pad_watch();
    lichii_hang_watch();
    if (!A.proc) return;
    if (WaitForSingleObject(A.proc, 0) != WAIT_OBJECT_0) return;
    if (A.job_h) {   // the emulator may have handed the game to a child: wait until the whole job is gone
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION ji{};
        if (QueryInformationJobObject(A.job_h, JobObjectBasicAccountingInformation, &ji, sizeof ji, nullptr) && ji.ActiveProcesses > 0) return;
        CloseHandle(A.job_h);
        A.job_h = nullptr;
    }
    DWORD code = 0;
    GetExitCodeProcess(A.proc, &code);
    CloseHandle(A.proc);
    A.proc = nullptr;
    hub::rom_cache_pin(nullptr);   // its unpacked copy may be trimmed again
    /* no focus_hub() here: the game just closed, the hub must not jump in front of whatever the user does next */
    sync_smash_prep();
    transferpak_run_finished();   // Wave 3 X1: the Game Boy game's save changed too
    palpark_process_ended();   // Wave 3 X3: a GBA game in the DS Slot-2 may have a new save
    if (A.run.after_exit) { auto fn = std::move(A.run.after_exit); A.run.after_exit = nullptr; fn(); }
    uint64_t secs = (GetTickCount64() - A.run.start_tick) / 1000;
    if (!A.run.key.empty()) {
        if (secs >= 1) { hub::record_play(A.run.key, A.run.start_unix, secs); if (!A.run.system_short.empty()) hub::saves::playlog_append(A.run.system_short, A.run.key, (int64_t)A.run.start_unix, (int64_t)secs); }
        resume_note_exit(A.run.system_short, A.run.key, A.run.online, A.run.start_unix);   // Wave 3 R1: which PC wrote the resume point
        if (hubui::exit_should_touch_game(!A.run.key.empty(), A.run.track, A.run.online, A.run.system_short))
            { hub::saves::touch_game(A.run.system_short, A.run.key, A.run.title); hub::saves::history_snapshot_game(A.run.system_short, A.run.key); }
        if (A.run.cloud_started) {
            A.run.cloud_started = false;
            /* The lease release is an HTTP call (up to 5 s): never on the frame thread. Tracked: relaunch of this game waits for it, shutdown joins it. */
            hub::saves::GameId cid{A.run.system_short, A.run.key};
            if (!cloud_bg_run(cid.gkey(), [cid]() { hub::saves::cloud_game_exited(cid); }))
                hub::saves::cloud_game_exited(cid);   // shutting down (window hidden): do it here so the lease release is never dropped
        }   // after touch_game: pushes this game in the background
        hub::flush_library_state();
        if (secs >= 1) fr_playtime_session_ended();
        refresh_state(A.run.key);
    }
    A.vault_dirty = true;
    A.info_at = -100;
    if (!A.run.result_file.empty()) { rivals_ingest(A.run.result_file); A.run.result_file.clear(); }   // before the room state is reset
    if (A.run.online) group_session_ended(secs);
    room_session_ended();
    if (A.run.remote_play) set_notice(hubrp::exit_text(code, false), 20.0);   // streamrun guest: its own exit codes
    else if (!A.run.left_name.empty() && (code == 0 || code == 4 || code == 8)) set_notice(hubui::exit_text_for(code, A.run.left_name), 20.0);
    else if (A.run.dropin && !hubui::dropin_exit_text(code).empty()) set_notice(hubui::dropin_exit_text(code), 20.0);
    else set_notice(A.proc_title + ": " + (code == 0 ? std::string("closed.") : hubui::exit_code_text(code)), 20.0);
    auto_sync();
}

/* Core DLL: <exe>\cores\ (packaged) or <Hub>\deps\cores\ (dev). */
std::string find_core(const std::string& name) {
    std::string f = name + "_libretro.dll";
    std::string p = hub::exe_dir() + "cores\\" + f;
    if (file_exists(p)) return p;
    return find_upward("deps\\cores\\" + f);
}
std::string quote(const std::string& s) { return winutil::quote_arg(s); }

/* Game window caption: "<title> — RetroHub" (UTF-8 em dash), quoted for the command line. */
std::string title_arg(const std::string& game_title) {
    std::string t = game_title;
    t.erase(std::remove(t.begin(), t.end(), '"'), t.end());   // a quote would break the argument
    return " --title " + quote(t + " \xE2\x80\x94 RetroHub");
}

/* retrorun names the DS save <stem>.srm after the ROM file. A zipped ROM is extracted as rom.<ext>, so pass the inner name's stem. */
std::string save_stem_arg(const hub::GameEntry& e) {
    return e.save_stem.empty() ? std::string() : " --save-stem " + quote(e.save_stem);
}

/* Env for every Smash Classic launch: ROM hand-off + vault save path. Migrates the legacy save once (copy, never delete). */
/* The Smash ROM in the scanned library ("" when none). */
std::string smash_rom_from_library() {
    std::vector<hubui::RomRef> refs;
    for (auto& g : A.lib) refs.push_back({g.catalog_id, g.serial, g.path});
    return hubui::find_smash_rom(refs);
}

std::vector<std::pair<std::string, std::string>> smash_env(const std::string& exe) {
    std::string rom = smash_rom_from_library();
    std::string save = hubui::smash_save_file(hub::saves::battery_dir(hubui::kSmashSaveSystem, hubui::kSmashSaveKey));
    std::string legacy = dir_of(exe) + "ssb64_save.bin";
    bool vault_has = file_exists(save), legacy_has = file_exists(legacy);
    if (hubui::smash_migrate_legacy_save(legacy, save)) {
        // copied once; the vault owns the save from now on
    } else if (!A.st.smash_save_notice_shown &&
               hubui::smash_save_conflict(vault_has, legacy_has, read_text_file(save), read_text_file(legacy))) {
        // both exist and differ: the vault wins; tell the player once where the old local save is
        A.st.smash_save_notice_shown = true;
        mark_dirty();
        push_toast(hubui::smash_save_conflict_text(legacy), ui::WARN, 12.0);
    }
    auto env = hubui::smash_launch_env(rom, save);
    {   // BattleShip applies RetroHub's n64 profile (with --from-hub): SSB64_CONTROLS=<resolved profile json>
        hubctl::Profile p = controls_profile("n64", hubui::kSmashSaveKey);
        /* Not customized (same mapping as the built-in default, even if an "n64-custom" copy exists from just opening the page):
         * send the built-in name so BattleShip keeps its natural Smash layout (hub_controls_apply.cpp checks "default-n64"). */
        p = hubctl::canonical_name(p);
        std::string path = hub::data_dir() + "controls_run_smash.json";
        if (hubctl::write_profile_file(path, p)) env.push_back({"SSB64_CONTROLS", path});
    }
    return env;
}

/* "Preparing" toast: kept up while the native Smash process runs and BattleShip.o2r is still missing; removed when either ends. */
void sync_smash_prep() {
    const std::string txt = hubui::smash_prep_text();
    bool want = A.proc && !A.run.o2r_prep.empty() && hubui::smash_prep_visible(true, file_exists(A.run.o2r_prep));
    if (want) {
        double now = ImGui::GetTime();
        for (auto& t : A.toasts)
            if (t.text == txt) { t.until = now + 14.0; return; }
        push_toast(txt, ui::WARN, 14.0);
        return;
    }
    A.toasts.erase(std::remove_if(A.toasts.begin(), A.toasts.end(), [&](const Toast& t) { return t.text == txt; }), A.toasts.end());
}

RunInfo run_info_for(const hub::GameEntry& e, bool online) {
    RunInfo ri;
    ri.key = e.key;
    ri.system_short = is_native(e) ? std::string(hubui::kSmashSaveSystem) : sys_short(e);
    ri.title = e.title;
    ri.track = true;   // the native port is tracked too: its save lives in the vault (n64 / native:smash64)
    ri.online = online;
    qm_prepare(ri, e);
    return ri;
}


/* ---------- 3DS (Azahar) and Wii U (Cemu): external emulators, see docs/EMULATORS.md ---------- */



/* Size of the image inside a zipped 3DS game (read once from the zip's directory). */
uint64_t unpacked_size(const hub::GameEntry& e) {
    if (e.system == hub::System::GC || e.system == hub::System::PS2 || e.system == hub::System::PSP || (e.system == hub::System::WiiU && e.issue == "extract")) return e.unpacked ? e.unpacked : e.size;   // from the scan cache; never runs tar on the UI thread
    static std::map<std::string, uint64_t> cache;
    auto it = cache.find(e.path);
    if (it == cache.end()) it = cache.emplace(e.path, hub::zip_3ds_unpacked_size(e)).first;
    return it->second ? it->second : e.size;
}

std::string launch_path_for(const hub::GameEntry& e) {
    if (e.issue == "extract") return hub::extracted_3ds_path(e);
    return e.path;
}

/* Hookshot (hookshot.h): the native Ocarina of Time build plays instead of Lichii. Offline only; its save sits in the same vault folder Lichii uses. */
static void launch_hookshot(const hub::GameEntry& e, const RunInfo& ri, const std::string& battery) {
    const std::string exe = hookshot::exe_path();
    if (g_headless) {
        shot_trace("external", "hookshot ready | " + exe);
        set_notice("Headless test run: launch skipped.");
        return;
    }
    if (hookshot::swap_in_progress()) { set_notice("Hookshot is being updated: try again in a moment.", 8.0); return; }
    if (hubemu::is_running(exe)) { set_notice("Hookshot is already running. Close it first, then start the game from RetroHub.", 10.0); return; }
    if (!hookshot::av1_decoder_present(true)) { hookshot_av1_ask(e); return; }   // Hookshot would open a top-most box over the game: ask here instead
    auto sess = std::make_shared<hubemu::SaveSession>(hookshot::begin_save_session(e, battery));
    autotest_log("external: hookshot save session " + std::string(sess->active ? "active" : "inactive") + " " + sess->summary);
    if (!sess->error.empty()) {   // the vault -> Hookshot copy failed: starting would play an older or missing save (and end_session must not run)
        set_notice("Couldn't copy your save into Hookshot (" + sess->error + "). The game was not started, so nothing changes.", 15.0);
        return;
    }
    std::vector<std::pair<std::string, std::string>> env;
    if (hub::gs::is_fullscreen(gs_values_for(e))) env.push_back({"HOS_FULLSCREEN", "1"});
    RunInfo r = ri;
    r.after_exit = [sess]() {   // Hookshot's save folder -> vault
        hubemu::end_session(*sess);
        autotest_log("end_session: " + sess->summary + (sess->error.empty() ? "" : " ERROR " + sess->error));
        if (!sess->error.empty()) push_toast("Could not copy the save into the vault: " + sess->error + " (Hookshot's copy is untouched)", ui::WARN, 15.0);
    };
    autotest_log("external: starting " + exe);
    if (spawn(exe, "", hookshot::game_dir(), env, r)) g_hookshot_run = true;
    else autotest_log("external: spawn failed");
}

void launch_external(const hub::GameEntry& e, const RunInfo& ri, const std::string& battery, const std::string& mp_arg) {
    if (launch_blocked("launch_external")) return;
    hubemu::Kind kind = hubemu::kind_for(e.system);
    if (kind == hubemu::Kind::Lichii && !ri.online && !ri.room && hookshot_lichii_once_consume(e.key)) { /* "Play with Lichii this time": skip Hookshot once */ }
    else if (kind == hubemu::Kind::Lichii && !ri.online && !ri.room && hookshot::use_for(e, gs_values_for(e))) { launch_hookshot(e, ri, battery); return; }   // before any Lichii-only step: works without Lichii, keys or firmware
    if (kind == hubemu::Kind::Lichii) kind = hubemu::nx_emulator_for(e, gs_values_for(e), ri.online || ri.room);   // opt-in per game (switch.runner), offline only like Hookshot: the same code path as Lichii, with Fritchii's own install folder
    hubemu::Bios ps2_bios;
    if (kind == hubemu::Kind::Pcsx2) {   // no BIOS: say so before anything is unpacked, downloaded or started
        ps2_bios = hubemu::pcsx2_find_bios(hubemu::pcsx2_bios_search_dirs(A.st.ps2_bios_dir, A.st.rom_folders));
        if (ps2_bios.file.empty()) { ps2_bios_ask(e.key); return; }
    }
    if ((e.system == hub::System::N3DS || e.system == hub::System::GC || e.system == hub::System::PS2 || e.system == hub::System::PSP || e.system == hub::System::WiiU) && e.issue == "extract" && hub::extracted_3ds_path(e).empty()) {   // archived image: ask once, unpack, then start
        UNZ.key = e.key;
        if (gamepack_owns(e)) start_unzip(e);   // installed from a shared pack: the player already chose this game, no question
        else UNZ.state = UnzipJob::Asking;
        return;
    }
    hubemu::Install inst = hubemu::find(kind, A.st.cemu_exe, e.path);
    {   // a download / update job already runs (or its question is up): never ask over it, and never start the build it is replacing
        const bool want_install = inst.exe.empty();
        const bool want_update = !want_install && inst.outdated && !g_headless && !EMU_UPDATE_SKIP[(int)kind] && !hubemu::is_running(inst.exe);
        if (EMU.state == hubemu::Installer::Working && (EMU.kind == kind || want_install || want_update)) {
            set_toast(std::string(hubemu::name(EMU.kind)) + (EMU.updating ? " is updating" : " is downloading") + ". Press Play again when it is done.", false, 6.0);
            return;
        }
        if (EMU.state == hubemu::Installer::Asking && (EMU.kind == kind || want_install || want_update)) return;   // its question is already on screen
    }
    if (inst.exe.empty()) {   // not installed: ask, download, then start this game
        shot_trace("external", std::string(hubemu::name(kind)) + " not-installed -> install dialog");
        EMU.kind = kind;
        EMU.game_key = e.key;
        EMU.updating = false;
        EMU.state = hubemu::Installer::Asking;
        return;
    }
    if (inst.outdated && !g_headless && !EMU_UPDATE_SKIP[(int)kind] && !hubemu::is_running(inst.exe)) {   // an older pinned build: offer the in-place update first (never while it runs)
        EMU.kind = kind;
        EMU.game_key = e.key;
        EMU.updating = true;
        EMU.state = hubemu::Installer::Asking;
        return;
    }
    if (g_headless) {
        hub::gs::Values gv = gs_values_for(e);
        bool fs0 = hub::gs::is_fullscreen(gv);
        std::string g0 = launch_path_for(e);
        std::string a0 = kind == hubemu::Kind::Azahar ? hubemu::azahar_args(g0, fs0, mp_arg)
                         : kind == hubemu::Kind::Dolphin ? hubemu::dolphin_args(g0, fs0, "<card>")
                         : kind == hubemu::Kind::Pcsx2 ? hubemu::pcsx2_args(g0, fs0)
                         : kind == hubemu::Kind::Ppsspp ? hubemu::ppsspp_args(g0, fs0)
                         : hubemu::is_nx(kind) ? hubemu::lichii_args(g0, fs0) : hubemu::cemu_args(g0, fs0);
        if (hubemu::is_nx(kind)) { hubemu::NxSetup ns = hubemu::lichii_setup_status(inst.dir); a0 += std::string(" | keys ") + (ns.prod_keys ? "yes" : "no") + " firmware " + (ns.firmware ? "yes" : "no"); }
        if (hubemu::is_nx(kind)) {
            bool pre = false, upd = false;
            for (const auto& kv : hubemu::nx_env(kind, gv, hubemu::physical_ram_bytes())) { if (kv.first == "EDEN_NX2_PRELOAD_ALL_PIPELINES") pre = true; if (kv.first == "EDEN_UPDATE_URL") upd = true; }
            a0 += std::string(" | preload all shaders ") + (pre ? "yes" : "no");
            std::string line = std::string(kind == hubemu::Kind::Fritchii ? "fritchii" : "lichii") + ": EDEN_NX2_PRELOAD_ALL_PIPELINES " + (pre ? "set" : "not set");
            if (kind == hubemu::Kind::Fritchii) line += std::string(", EDEN_UPDATE_URL ") + (upd ? "pinned" : "missing") + ", leftover update files: " + (hubemu::update_leftovers(kind, inst.dir).empty() ? "none" : hubemu::update_leftovers(kind, inst.dir));
            autotest_log(line);
        }
        shot_trace("external", std::string(hubemu::name(kind)) + " installed | " + a0);
        set_notice("Headless test run: launch skipped.");
        return;
    }
    if (kind == hubemu::Kind::Fritchii) {
        if (!hubemu::update_leftovers(kind, inst.dir).empty()) {   // its own updater was cut short: starting it would open an "Interrupted Update" box over the game. Offer the normal reinstall instead.
            EMU.kind = kind;
            EMU.game_key = e.key;
            EMU.updating = true;
            EMU.state = hubemu::Installer::Asking;
            return;
        }
        if (hubemu::is_running(inst.exe)) { set_notice("Lichii or Fritchii is already running. Close it first, then start the game from RetroHub.", 10.0); return; }   // (same program name): no copying while one runs
        if (nxprep_gate(e, inst.dir)) return;   // one-time copy of keys / firmware / shader cache from Lichii (own thread, dialog); the launch resumes by itself
    }
    if (hubemu::is_nx(kind) && !switch_ready_or_ask(e, inst.dir, kind)) return;   // keys / firmware first (the player's own; switch_ui.cpp)
    if (hubemu::is_running(inst.exe)) {   // a second launch would hand over to the running instance and end the session early
        set_notice(std::string(hubemu::name(kind)) + " is already running. Close it first, then start the game from RetroHub.", 10.0);
        return;
    }
    if (kind == hubemu::Kind::Azahar) {
        if (inst.managed) { hubctl::Profile p3 = controls_profile("3ds", e.key); hubemu::prepare_azahar(inst.dir, p3, azahar_pad_for(p3)); }
    } else if (kind == hubemu::Kind::Dolphin) {
        hubemu::dolphin_migrate_old(hub::data_dir(), hub::exe_dir(), inst.dir);
        dolphin_speed_start();   // first: virtual DSU pad on 127.0.0.1:26761.. (first free port; DSUClient.ini gets that port); false = all busy -> quickmenu uses the keyboard path
        { hubctl::Profile pg = controls_profile("gc", e.key); hubemu::prepare_dolphin(inst.dir, dolphin_pads_for(pg), pg); }
        real_boot_dolphin_apply(inst.dir, real_boot_plan(e));   // GameCube IPL startup (this launch only)
        if (!ri.online && !ri.room) gba_link_apply_dolphin(e, inst.dir);   // Wave 3 X2: integrated GBA per port (offline only; GC netplay never comes through here, the guard is belt and braces)
        cheats_apply_dolphin(e, inst.dir +"\\User", false, true, nullptr);   // enabled cheats -> GameSettings\<GAMEID>.ini (stale RetroHub entries removed)
        if (autotest_active() && std::getenv("RETROHUB_AUTOTEST_GCPAD_INI")) {   // QA only: scripted pad (constant/timer expressions) instead of the user's mapping
            std::error_code ec;
            std::filesystem::copy_file(std::getenv("RETROHUB_AUTOTEST_GCPAD_INI"), std::filesystem::path(inst.dir) / "User" / "Config" / "GCPadNew.ini", std::filesystem::copy_options::overwrite_existing, ec);
            autotest_log(std::string("gcpad override: ") + (ec ? ec.message() : "ok"));
        }
    } else if (kind == hubemu::Kind::Pcsx2) {
        hubctl::Profile pp = controls_profile("ps2", e.key);
        const bool buzz = hubemu::pcsx2_is_buzz_game(e.catalog_id) || e.title.compare(0, 5, "Buzz!") == 0;   // a Buzz! quiz outside the catalog needs the buzzers too
        const int ps2_players = std::max(2, local_players_for(e));   // two pads always work; the multitap only for a game with 3-4 players
        std::string perr = hubemu::prepare_pcsx2(inst.dir, ps2_pads_for(pp, ps2_players), pp, ps2_bios, e.key, gs_values_for(e), buzz);
        if (!perr.empty()) { set_notice("Could not set up PCSX2: " + perr, 12.0); return; }
        real_boot_pcsx2_apply(inst.dir, real_boot_plan(e));   // fast boot off = the PS2 BIOS intro plays
    } else if (kind == hubemu::Kind::Ppsspp) {   // portable memstick under the managed dir; controls from the "psp" profile on the first XInput pad
        hubctl::Profile pq = controls_profile("psp", e.key);
        hubemu::CemuPads cp = cemu_pads_for(pq);
        std::string perr = hubemu::prepare_ppsspp(inst.dir, pq, cp.xinput.empty() ? 0 : cp.xinput[0], gs_values_for(e));
        if (!perr.empty()) { set_notice("Could not set up PPSSPP: " + perr, 12.0); return; }
    } else if (hubemu::is_nx(kind)) {   // (Lichii and Fritchii) portable user\ under the managed dir; player 1 from the "switch" profile on one SDL pad
        hubctl::Profile px = controls_profile("switch", e.key);
        std::string guid; int port = 0; hubemu::SdlBinds binds;
        const std::string nm = hubemu::name(kind);
        if (!lichii_pad_for(px, &guid, &port, &binds)) set_notice("No controller found: connect one and " + nm + " picks it up (or set it in " + nm + "'s Controls).", 10.0);
        std::string perr = hubemu::prepare_lichii(inst.dir, px, guid, port, gs_values_for(e), hub::gs::is_fullscreen(gs_values_for(e)), &binds,
                                                  kind == hubemu::Kind::Fritchii ? &hubemu::nx_skip_keys(kind) : nullptr);
        if (perr.empty() && kind == hubemu::Kind::Fritchii) perr = hubemu::prepare_fritchii(inst.dir);   // pins Fritchii's settings migration (after the config write above)
        if (!perr.empty()) { set_notice("Could not set up " + nm + ": " + perr, 12.0); return; }
    } else if (inst.managed) {
        { hubctl::Profile pw = controls_profile("wiiu", e.key); hubemu::prepare_cemu_managed(inst.dir, pw, ri.dsu_players > 0 ? hubemu::cemu_pads_with_dsu(cemu_pads_for_own(pw), ri.dsu_players) : cemu_pads_for_own(pw)); }
    } else if (A.st.cemu_manage_controls) {   // the user's own Cemu: backed up once, then RetroHub's controls (setting is ON by default)
        const char* ad = std::getenv("APPDATA");
        hubctl::Profile pw = controls_profile("wiiu", e.key);
        hubemu::prepare_cemu_own(hubemu::cemu_user_data_dir(inst.dir, ad ? ad : ""), pw, ri.dsu_players > 0 ? hubemu::cemu_pads_with_dsu(cemu_pads_for_own(pw), ri.dsu_players) : cemu_pads_for_own(pw), true);
    } else if (ri.dsu_players > 0) {
        set_notice("Remote play needs RetroHub to manage Cemu's controls (Settings > Wii U) - the guests' controllers won't reach the game otherwise.", 15.0);
    }
    hub::gs::Values gsv = gs_values_for(e);
    bool fs = hub::gs::is_fullscreen(gsv);
    std::string game = launch_path_for(e);
    if (kind == hubemu::Kind::Pcsx2) {   // PCSX2 cannot open a .cue: hand it the bin (or an iso built once in the rom-cache)
        std::string img, ierr;
        if (!hub::ps2::launch_image(game, hub::extract_3ds_dir(e), &img, &ierr)) { set_notice("Could not start " + e.title + ": " + ierr + ".", 12.0); return; }
        game = img;
    }
    uint64_t wiiu_tid = 0;
    std::string cemu_user;   // Cemu's user data folder (portable\ / beside settings.xml / %APPDATA%\Cemu)
    if (kind == hubemu::Kind::Cemu) {
        const char* ad = std::getenv("APPDATA");
        cemu_user = inst.managed ? inst.dir + "\\portable" : hubemu::cemu_user_data_dir(inst.dir, ad ? ad : "");
        wiiu_tid = hubemu::title_id_from_key(e.key);
        if (!wiiu_tid) wiiu_tid = hubemu::wiiu_title_id_of_file(game);   // archives / disc-less keys: read from the unpacked .wua
        if (inst.managed) hubemu::apply_game_settings(kind, inst.dir, gsv);
        else if (!hubemu::prepare_cemu_own_settings(cemu_user, gsv)) autotest_log("cemu: settings.xml of the own install left untouched (missing or no backup possible)");
        autotest_log("cemu: user data " + cemu_user + " title " + hub::hex16(wiiu_tid));
        hubemu::apply_cemu_game_profile(cemu_user, wiiu_tid, gsv);
        autotest_log("cemu: profile applied");
    } else {
        hubemu::apply_game_settings(kind, inst.dir, gsv);
    }
    if (hubemu::is_nx(kind) && e.system == hub::System::Switch) {   // the newest save across Lichii, Fritchii and the vault goes into THIS emulator's folder first (a no-op while only one of them is installed)
        hubemu::NxSaveSync ss;
        ss.launching = kind;
        ss.title_id = hubemu::title_id_from_key(e.key);
        const hubemu::Install li = hubemu::find(hubemu::Kind::Lichii, "", ""), fr = hubemu::find(hubemu::Kind::Fritchii, "", "");
        ss.lichii_dir = li.exe.empty() ? std::string() : li.dir;
        ss.fritchii_dir = fr.exe.empty() ? std::string() : fr.dir;
        ss.battery = battery;
        ss.game_label = e.key;
        const hubemu::NxSaveSyncReport rep = hubemu::nx_sync_saves_before_launch(ss);
        if (rep.ran) autotest_log("nx saves: " + (rep.ok ? rep.summary : "ERROR " + rep.error));
        if (!rep.ok) { set_notice("Couldn't compare your saves between Lichii and Fritchii (" + rep.error + "). The game was not started. Nothing was lost: every save file that could have been replaced was backed up first (save-backups, and the vault's _conflicts folder).", 15.0); return; }
    }
    auto sess = std::make_shared<hubemu::SaveSession>(hubemu::begin_session(e, inst, battery, "", wiiu_tid, hubemu::nx_manifest_suffix(kind)));
    autotest_log("external: save session " + std::string(sess->active ? "active" : "inactive") + " " + sess->summary);
    std::string args = kind == hubemu::Kind::Azahar ? hubemu::azahar_args(game, fs, mp_arg)
                       : kind == hubemu::Kind::Dolphin ? hubemu::dolphin_args(game, fs, hubemu::dolphin_card_root(inst.dir, e.key) + "\\GCI", hub::gs::volume_of(gsv))
                       : kind == hubemu::Kind::Pcsx2 ? hubemu::pcsx2_args(game, fs)
                       : kind == hubemu::Kind::Ppsspp ? hubemu::ppsspp_args(game, fs)
                       : hubemu::is_nx(kind) ? hubemu::lichii_args(game, fs)
                                                       : hubemu::cemu_args(game, fs);
    if (autotest_active() && std::getenv("RETROHUB_AUTOTEST_DOLPHIN_ARGS") && kind == hubemu::Kind::Dolphin)   // QA only: extra Dolphin flags (e.g. -m movie.dtm)
        args = std::string(std::getenv("RETROHUB_AUTOTEST_DOLPHIN_ARGS")) + " " + args;
    if (e.issue == "encrypted") set_notice("This 3DS image is encrypted: Azahar needs your own AES keys (aes_keys.txt in its user\\sysdata folder).", 12.0);
    if (e.issue == "keys") {
        std::string why = hub::wiiu_keys_message(hub::wiiu_keys_check(cemu_user, inst.dir, hubemu::wiiu_probe()), "wud");
        set_notice(why.empty() ? std::string("Disc images (.wud/.wux) need your disc key in Cemu's keys.txt. A .wua needs no keys.") : why, 20.0);
    }
    RunInfo r = ri;
    const bool cemu_managed = inst.managed;
    r.after_exit = [sess, kind, cemu_user, wiiu_tid, cemu_managed]() {   // emulator save folder -> vault, before the sync runs
        if (kind == hubemu::Kind::Cemu) {   // put back everything RetroHub changed in the user's Cemu for this session
            hubemu::revert_cemu_game_profile(cemu_user, wiiu_tid);
            if (!cemu_managed) hubemu::revert_cemu_own_settings(cemu_user);
        }
        hubemu::end_session(*sess);
        if (kind == hubemu::Kind::Dolphin) gba_link_after_exit();   // Wave 3 X2: GBA saves -> vault (no-op without a GBA link)
        autotest_log("end_session: " + sess->summary + (sess->error.empty() ? "" : " ERROR " + sess->error));
        NP3.stop();
        if (!sess->error.empty()) push_toast("Could not copy the save into the vault: " + sess->error + " (your emulator's copy is untouched)", ui::WARN, 15.0);
    };
    autotest_log("external: starting " + inst.exe + " " + args);
    std::vector<std::pair<std::string, std::string>> xenv;   // added to RetroHub's environment for the child (spawn)
    if (hubemu::is_nx(kind)) { xenv = hubemu::nx_env(kind, gsv, hubemu::physical_ram_bytes()); autotest_log(std::string(hubemu::name(kind)) + ": " + std::to_string(xenv.size()) + " extra environment variable(s)"); }
    if (!spawn(inst.exe, args, inst.dir, xenv, r)) { autotest_log("external: spawn failed"); NP3.stop(); }
    else g_fritchii_run = kind == hubemu::Kind::Fritchii;
}

void start_unzip(const hub::GameEntry& e) {
    if (UNZ.th.joinable()) UNZ.th.join();
    UNZ.done = false;
    UNZ.ok = false;
    UNZ.cancel = false;
    UNZ.got = 0;
    UNZ.total = unpacked_size(e);
    UNZ.error.clear();
    UNZ.state = UnzipJob::Working;
    hub::GameEntry copy = e;
    UNZ.th = std::thread([copy]() {
        std::string out, err;
        bool ok = hub::extract_3ds_zip(copy, [](uint64_t done, uint64_t total) { UNZ.got = done; UNZ.total = total; return !UNZ.cancel.load(); }, &out, &err);
        if (!ok) { std::lock_guard<std::mutex> lk(UNZ.mu); UNZ.error = err; }
        UNZ.ok = ok;
        UNZ.done = true;
    });
}
void flash_taskbar() {
    if (!A.hwnd || g_headless || game_active()) return;   // a flash is still a popup: nothing while a game runs
    FLASHWINFO fi{};
    fi.cbSize = sizeof(fi);
    fi.hwnd = A.hwnd;
    fi.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;   // taskbar button only, until the user looks at RetroHub; no activation
    fi.uCount = 6;
    FlashWindowEx(&fi);   // popup-guard: ok (guarded by game_active above)
}

void add_game_shortcut(const hub::GameEntry& e) {
    std::wstring dir = shell_shortcut_dir();
    if (dir.empty()) { set_notice("Desktop shortcuts are only created by an installed RetroHub."); return; }
    std::wstring key = W(e.key);
    if (!invite_is_valid_game_key(key)) { set_notice("This game cannot be added to the desktop."); return; }
    std::wstring icon;
    if (hubart::enabled()) {   // cover art as the icon when it is already cached
        std::string name, png;
        int w = 0, h = 0;
        std::string err;
        if (hubart::detail::resolve_name(e, &name, nullptr) &&
            hubart::detail::fetch_decode(sys_short(e), name, hubart::Kind::Boxart, &w, &h, &err, &png)) {
            int iw = 0, ih = 0, comp = 0;
            std::ifstream pf(std::filesystem::path(W(png)), std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(pf)), std::istreambuf_iterator<char>());
            unsigned char* px = bytes.empty() ? nullptr : stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &iw, &ih, &comp, 4);
            if (px) {
                std::wstring icodir = W(hub::data_dir() + "icons\\");
                CreateDirectoryW(icodir.c_str(), nullptr);
                std::wstring ico = icodir + key + L".ico";
                if (shell_write_ico(ico, px, iw, ih)) icon = ico;
                stbi_image_free(px);
            }
        }
    }
    std::wstring lnk = dir + L"\\" + shell_game_shortcut_name(W(e.title));
    if (shell_write_game_shortcut(lnk, winutil::exe_path_w(), key, W(e.title), icon)) set_toast("Added " + e.title + " to your desktop.");
    else set_notice("Could not create the shortcut.");
}

/* saves-F1 / Wave 3 R1 (resume_logic.h, resume_more.cpp): " --resume" only when the resume point is not older than the newest save,
 * or the player chose it; the older-save choice is asked once at Play (resume_choice_gate). */
static std::string resume_arg(const hub::GameEntry& e, const std::string& battery, const std::string& states, bool blocked) {
    return resume_launch_arg(e, battery, states, blocked);
}

void play_entry(const hub::GameEntry& e_in) {
    if (launch_blocked("play_entry")) return;
    if (!hubui::launch_allowed(A.proc != nullptr, session_active())) {   // shell-H2: GameCube NetPlay / a 3DS room never set A.proc, so ask every session
        A.resume_key.clear();
        A.resume_state.clear();
        set_notice(hubui::launch_busy_reason(A.proc != nullptr));
        return;
    }
    hub::GameEntry hacked;   // ROM hack / translation (Plan 4 H3): launch the patched copy prepared in rom-cache\hacks
    if (is_hack(e_in)) {
        std::string patched;
        if (!hack_ensure_rom(e_in, &patched)) return;   // still preparing (shown, Play resumes) or failed (notice shown)
        hacked = e_in;
        hacked.path = patched;
    }
    const hub::GameEntry& e = is_hack(e_in) ? hacked : e_in;
    if (A.cloud_gate.phase != CloudGate::Idle) { set_notice("Checking cloud saves...", 4.0); return; }
    if (cloud_gate_begin(e, [key = e.key]() {
            if (const hub::GameEntry* g = find_entry(key)) { A.sel_game = key; hub::GameEntry copy = *g; A.cloud_gate.pass_key = key; play_entry(copy); }
        })) return;
    if (A.job.running) {
        std::string key = e.key;
        A.after_job = [key]() { if (const hub::GameEntry* g = find_entry(key)) { A.sel_game = key; hub::GameEntry copy = *g; play_entry(copy); } };
        set_notice("Syncing your saves first, then starting " + e.title + "...", 10.0);
        return;
    }
    A.st.last_game = room_id(e);
    mark_dirty();
    hub::rom_cache_touch(e);   // LRU stamp for the unpacked copy (no-op for plain files)
    hub::rom_cache_pin(&e);    // and never trimmed while it runs (library-14)
    if (rom_missing(e)) { missing_rom_ask(e.key); return; }   // the file is gone: Locate / Rescan / Remove popup
    RunInfo ri = run_info_for(e, false);
    std::string sys = sys_short(e);
    if (is_kanto(e)) { kt_play(e); return; }
    if (is_native(e)) {
        std::string exe = smash_exe_path();
        std::string o2r = dir_of(exe) + "BattleShip.o2r";
        std::string rom = smash_rom_from_library();
        bool o2r_ok = file_exists(o2r);
        std::string md5 = (!o2r_ok && !rom.empty()) ? hubemu::md5_file(rom) : std::string();   // only the first run needs the ROM
        hubui::SmashPlay d = hubui::smash_play_decision(o2r_ok, !rom.empty(), md5);
        if (d == hubui::SmashPlay::NeedRom) { push_toast(hubui::smash_need_rom_text(), ui::WARN, 9.0); return; }
        if (d == hubui::SmashPlay::WarnMd5) push_toast(hubui::smash_md5_warn_text(md5), ui::WARN, 12.0);
        ri.o2r_prep = o2r;
        auto env = smash_env(exe);
        ri.result_file = hub::data_dir() + "smash_result.json";   // each finished VS match (Plan 4: local matches too, for party mode and brackets)
        { std::error_code ec; std::filesystem::remove(std::filesystem::u8path(ri.result_file), ec); }
        env.push_back({"SSB64_RESULT_FILE", ri.result_file});
        spawn(exe, "--from-hub --no-update-check", dir_of(exe), env, ri);   // RetroHub's updater is the only one
        return;
    }
    std::string battery = no_slash(hub::saves::battery_dir(sys, e.key));
    std::string states = no_slash(hub::saves::states_dir(sys, e.key));
    {   // Wave 3 R1: another PC's save is newer than this PC's resume point: ask once, here in the hub, before anything starts
        bool blocked = false;
        if (e.system == hub::System::GB || e.system == hub::System::GBC || e.system == hub::System::GBA) {
            const RealBootPlan p = real_boot_plan(e);
            blocked = p.use && hub::realboot::drops_resume(e.system);
        }
        if (resume_choice_gate(e, blocked)) return;
    }
    switch (e.system) {
        case hub::System::N64: {
            std::string args = "--rom " + quote(e.path) + " --game " + (e.catalog_id.empty() ? "unknown" : e.catalog_id) +
                               " --save-dir " + quote(battery) + title_arg(e.title) + controls_arg("n64", e.key);
            args += hubui::n64_extra_args(gs_values_for(e));
            if (e.catalog_id == "remix" || e.catalog_id == "smash64") {   // Smash Remix / Smash 64 ROM: VS results for rivals, party mode and brackets
                ri.result_file = hub::data_dir() + "n64_result.json";
                { std::error_code ec; std::filesystem::remove(std::filesystem::u8path(ri.result_file), ec); }
                args += " --result-file " + quote(ri.result_file);
            }
            args += cheats_launch_args(e, false, true, nullptr);
            const size_t pre_tp = args.size();
            args += transferpak_launch_args(e);   // Wave 3 X1: Game Boy cartridge in the Transfer Pak (offline)
            const bool tp_on = args.size() > pre_tp;   // a state would restore the cartridge's save too: no resume then
            args += qm_args(ri, e, true);
            args += resume_arg(e, battery, states, tp_on);   // Wave 3 R1: n64run --resume loads <state-dir>/auto.state (offline only)
            if (args.find(" --resume") != std::string::npos && args.find("--state-dir ") == std::string::npos) args += " --state-dir " + quote(states);
            if (!file_exists(hub::exe_dir() + "emu\\mupen64plus.dll")) {
                std::string emu = find_upward("deps\\emu\\mupen64plus.dll");
                if (!emu.empty()) args += " --emu-dir " + quote(no_slash(dir_of(emu)));
            }
            spawn(n64run_path(), args, hub::exe_dir(), {}, ri);
            break;
        }
        case hub::System::GBA:
        case hub::System::GB:
        case hub::System::GBC: {
            std::string core = find_core(e.system == hub::System::GBA ? "mgba" : A.st.gb_core);
            if (core.empty()) { set_notice("Emulator core for " + std::string(hub::system_name(e.system)) + " not found (cores folder)."); return; }
            const RealBootPlan rb_plan = real_boot_plan(e);
            const bool rb_drops = rb_plan.use && hub::realboot::drops_resume(e.system);   // a real startup replaces the resume
            std::string args = "--core " + quote(core) + " --rom " + quote(e.path) + " --system " + sys + " --save-dir " + quote(battery) +
                               " --state-dir " + quote(states) + resume_arg(e, battery, states, rb_drops) + title_arg(e.title) + save_stem_arg(e) + controls_arg("retro", e.key);
            args += hubui::retro_extra_args(e.system, gs_values_for(e));
            {   // the console's own startup (boot ROM / BIOS of the player); a resumed state would skip it
                bool dr = false;
                std::string rb = real_boot_retro_args(e, real_boot_plan(e), &dr);
                if (dr) { size_t k = args.find(" --resume"); if (k != std::string::npos) args.erase(k, 9); }
                args += rb;
            }
            args += cheats_launch_args(e, false, true, nullptr);
            args += qm_args(ri, e, false);
            spawn(hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
            break;
        }
        case hub::System::NES:
        case hub::System::SNES: {   // Nestopia UE (NES) / Snes9x (SNES) through retrorun; battery .srm in the vault, auto-resume state like GBA
            const bool nes = e.system == hub::System::NES;
            std::string core = find_core(nes ? "nestopia" : "snes9x");
            if (core.empty()) { set_notice("Emulator core for " + std::string(hub::system_name(e.system)) + " not found (cores folder)."); return; }
            std::string args = "--core " + quote(core) + " --rom " + quote(e.path) + " --system " + sys + " --save-dir " + quote(battery) +
                               " --state-dir " + quote(states) + resume_arg(e, battery, states, false) + title_arg(e.title) + save_stem_arg(e) + controls_arg(sys, e.key);
            args += hubui::retro_extra_args(e.system, gs_values_for(e));
            args += " --max-local " + std::to_string(hubcaps::runner_local_seats(e.system, hub::find_game(e.catalog_id)));   // seats of this game: retrorun never boots an adapter for a 1-2 player game
            args += cheats_launch_args(e, false, true, nullptr);
            args += qm_args(ri, e, false);
            spawn(hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
            break;
        }
        case hub::System::NDS: {   // melonDS DS core; no --resume (a stale auto state could overwrite a save synced from another PC)
            std::string core = find_core("melondsds");
            if (core.empty()) { set_notice("Emulator core for Nintendo DS not found (cores folder)."); return; }
            std::string args = "--core " + quote(core) + " --rom " + quote(e.path) + " --system nds --save-dir " + quote(battery) +
                               " --state-dir " + quote(states) + title_arg(e.title) + save_stem_arg(e) + controls_arg("retro", e.key);
            args += hubui::retro_extra_args(e.system, gs_values_for(e));
            args += real_boot_retro_args(e, real_boot_plan(e), nullptr);   // DS menu through the player's BIOS + firmware
            args += cheats_launch_args(e, false, true, nullptr);
            args += palpark_launch_args(e);   // Wave 3 X3: GBA game in Slot-2 (offline)
            args += qm_args(ri, e, false);
            spawn(hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
            break;
        }
        case hub::System::PS1:   // Beetle PSX through retrorun; no --resume (the memory card is the save)
            ps1_play(e, ri, battery, states);
            break;
        case hub::System::GC:
        case hub::System::PS2:
        case hub::System::PSP:
        case hub::System::Switch:
        case hub::System::N3DS:
        case hub::System::WiiU:
            launch_external(e, ri, battery);
            break;
        default: set_notice("Unsupported system."); break;
    }
}

