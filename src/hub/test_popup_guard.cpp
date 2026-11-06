// Console test: the "never pop up over the user's game" policy (popup_guard.h) plus a source lint over src/hub AND src/common.
// The lint fails when a raise / restore / show / focus / flash / MessageBox call appears outside window_guard.cpp without a
// "popup-guard: ok (reason)" marker, and when the set of marked sites differs from the known list below (so adding one is a
// conscious edit of this test). A second lint pins every place that injects synthetic input or attaches to another thread's input
// (the Alt-tap trick that takes the foreground): only the registered sites below may.
//
// THE ONE ALLOWED EXCEPTION: View+Y (the player pressed it) brings the game menu forward. launch.cpp force_foreground() attaches to the
// foreground thread's input, taps Alt with keybd_event so Windows lets it take the foreground, then BringWindowToTop/SetForegroundWindow
// (the marked "user hotkey" sites). It is registered in kSynthetic below (launch.cpp: keybd_event x2 + AttachThreadInput x2) and described
// in popup_guard.h and docs/ARCHITECTURE.md ("Shell: never over a game"). The two SendInput sites in the same file only send a hotkey
// to the game window while it is already the foreground window (Cemu pad-screen toggle, Dolphin speed keys); they raise nothing.
#include "popup_guard.h"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#ifndef HUB_SRC_DIR
#error HUB_SRC_DIR must point at src/hub
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static void logic() {
    using namespace hubguard;
    CHECK(game_active(true, false) && game_active(false, true) && game_active(true, true) && !game_active(false, false));
    CHECK(may_surface(false) && !may_surface(true));
    // prompts (update, what's new, join, rejoin): need no game AND hub focus; headless always passes
    CHECK(may_open_prompt(false, true, false));
    CHECK(!may_open_prompt(true, true, false));    // game running
    CHECK(!may_open_prompt(false, false, false));  // hub not focused
    CHECK(!may_open_prompt(true, false, false));
    CHECK(may_open_prompt(true, false, true));     // screenshot / QA harness
    CHECK(may_restore_after_exit(true) && !may_restore_after_exit(false));
    CHECK(start_quiet(true, false) && start_quiet(false, true) && !start_quiet(false, false));
    {   // every session kind alone makes a game active, including the ones that never set A.proc
        Sessions none;
        CHECK(!any_session(none) && !game_active(none));
        auto only = [](bool Sessions::*m) { Sessions s; s.*m = true; return s; };
        CHECK(game_active(only(&Sessions::proc)));          // offline, PS2 online, Smash Classic, Kanto mGBA, remote-play guest viewer, 3DS/Wii U emulator
        CHECK(game_active(only(&Sessions::job)));           // emulator job object still holding processes
        CHECK(game_active(only(&Sessions::gc_online)));     // GameCube NetPlay (GCO.active; no A.proc)
        CHECK(game_active(only(&Sessions::remote_play)));   // Wii U remote-play host streamrun
        CHECK(game_active(only(&Sessions::kanto)));
        CHECK(game_active(only(&Sessions::n3ds_room)));     // 3DS room link / Azahar start pending
        CHECK(game_active(only(&Sessions::os_busy)) && !any_session(only(&Sessions::os_busy)));   // OS full-screen guess: not a hub session
        // a prompt needs NO session of any kind, no full-screen app, and the hub focused
        for (bool Sessions::*m : {&Sessions::proc, &Sessions::job, &Sessions::gc_online, &Sessions::remote_play, &Sessions::kanto, &Sessions::n3ds_room, &Sessions::os_busy})
            CHECK(!may_open_prompt(game_active(only(m)), true, false) && !may_surface(game_active(only(m))));
        CHECK(may_open_prompt(game_active(none), true, false) && may_surface(game_active(none)));
        // update relaunch: quiet only for an active game (flag from the hub, or the OS full-screen state); a browser in front changes nothing
        CHECK(start_quiet(false, game_active(true, false)) && start_quiet(false, game_active(false, true)) && !start_quiet(false, game_active(false, false)));
        // an update installs only with NO session of any kind (A.proc, job, GameCube NetPlay, 3DS room, remote play, Kanto) and nothing left over
        for (bool Sessions::*m : {&Sessions::proc, &Sessions::job, &Sessions::gc_online, &Sessions::remote_play, &Sessions::kanto, &Sessions::n3ds_room})
            CHECK(!may_install_update(any_session(only(m)), false));
        CHECK(may_install_update(any_session(none), false));
        CHECK(!may_install_update(false, true));    // a game from an earlier RetroHub run is still running from the install folder
        CHECK(!may_install_update(true, true));
        // an OS full-screen/busy app alone does not hold an install back (the hub restarts minimised, nothing is touched)
        CHECK(may_install_update(any_session(only(&Sessions::os_busy)), false));
    }
    // Windows toasts: never while a game runs, whatever the opt-in says
    CHECK(!may_windows_toast(true) && may_windows_toast(false));
    // TV mode: F11 in the focused hub always works; every other route only with no game running
    CHECK(may_enter_tv_mode(true, true) && may_enter_tv_mode(true, false));
    CHECK(!may_enter_tv_mode(false, true) && may_enter_tv_mode(false, false));
    CHECK(is_shell_class(L"Shell_TrayWnd") && is_shell_class(L"Progman") && is_shell_class(L"WorkerW"));
    CHECK(!is_shell_class(L"SDL_app") && !is_shell_class(L"") && !is_shell_class(nullptr) && !is_shell_class(L"Shell_TrayWndX"));
}

namespace fs = std::filesystem;

static const char* kApis[] = {"SDL_RaiseWindow", "SDL_ShowWindow", "SDL_RestoreWindow", "SDL_SetWindowFullscreen", "SetForegroundWindow",
                              "BringWindowToTop", "ShowWindow(", "FlashWindow", "MessageBox", "AllowSetForegroundWindow", "SetActiveWindow",
                              "HWND_TOPMOST", "SDL_SetWindowAlwaysOnTop", "SDL_FlashWindow"};

/* Synthetic input / input-queue attachment: how a process takes the foreground without being allowed to. Pinned site by site. */
static const char* kSynthetic[] = {"keybd_event", "SendInput", "mouse_event", "AttachThreadInput", "SwitchToThisWindow", "LockSetForegroundWindow"};

static bool ident(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

static bool has_token(const std::string& code, const std::string& s) {
    for (size_t p = code.find(s); p != std::string::npos; p = code.find(s, p + 1))
        if (p == 0 || !ident(code[p - 1])) return true;   // not a suffix of another name (e.g. wShowWindow)
    return false;
}

static bool has_api(const std::string& code) {
    for (const char* a : kApis) if (has_token(code, a)) return true;
    return false;
}

struct ScanResult {
    int scanned = 0;
    std::vector<std::string> violations;                 // unmarked raise/show calls, MessageBox exemptions
    std::map<std::string, int> marked;                   // file -> marked raise/show lines
    std::map<std::pair<std::string, std::string>, int> synthetic;   // (file, token) -> lines
};

/* Walks one directory (non-recursive, like the original lint) and reports; the caller decides what is allowed. */
static ScanResult scan_dir(const fs::path& dir, const std::string& label) {
    ScanResult r;
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (!de.is_regular_file()) continue;
        const std::string name = de.path().filename().string();
        const std::string ext = de.path().extension().string();
        if ((ext != ".cpp" && ext != ".h") || name.rfind("test_", 0) == 0 || name == "popup_guard.h" || name == "window_guard.cpp") continue;
        std::ifstream in(de.path());
        std::string line;
        int ln = 0;
        bool block = false;
        ++r.scanned;
        while (std::getline(in, line)) {
            ++ln;
            std::string t = line;
            t.erase(0, t.find_first_not_of(" \t") == std::string::npos ? t.size() : t.find_first_not_of(" \t"));
            if (block) { if (t.find("*/") != std::string::npos) block = false; continue; }
            if (t.rfind("/*", 0) == 0) { if (t.find("*/") == std::string::npos) block = true; continue; }
            if (t.rfind("//", 0) == 0 || t.rfind("*", 0) == 0) continue;
            std::string code = line.substr(0, line.find("//"));
            for (const char* s : kSynthetic) if (has_token(code, s)) ++r.synthetic[{name, s}];
            if (!has_api(code)) continue;
            if (line.find("popup-guard: ok") == std::string::npos) {
                r.violations.push_back("unguarded raise/show call: " + label + "/" + name + ":" + std::to_string(ln) + ": " + t);
            } else {
                if (code.find("MessageBox") != std::string::npos) r.violations.push_back("MessageBox may never be exempted: " + label + "/" + name + ":" + std::to_string(ln));
                ++r.marked[name];
            }
        }
    }
    return r;
}

static void report(const char* what, const ScanResult& r) {
    for (const auto& v : r.violations) { std::printf("FAIL %s\n", v.c_str()); ++fails; }
    (void)what;
}

static void expect_marked(const char* label, const ScanResult& r, const std::map<std::string, int>& known) {
    for (const auto& k : known) {
        auto it = r.marked.find(k.first);
        int n = it == r.marked.end() ? 0 : it->second;
        if (n != k.second) { std::printf("FAIL %s/%s: %d marked sites, expected %d\n", label, k.first.c_str(), n, k.second); ++fails; }
    }
    for (const auto& s : r.marked)
        if (!known.count(s.first)) { std::printf("FAIL %s/%s has marked sites but is not in the known list\n", label, s.first.c_str()); ++fails; }
}

static void lint() {
    // Every marked site, by file -> number of lines. A new marked site means editing this table on purpose.
    const std::map<std::string, int> known_hub = {
        {"tv_mode.cpp", 5},    // F11 / Settings checkbox with no game (may_enter_tv_mode) / leave / startup fullscreen (guarded by game_active) / post-exit re-fullscreen (hub already focused)
        {"launch.cpp", 6},     // hub_menu_topmost (View+Y menu open only, cleared on close), force_foreground (2: user's View+Y open / close only), focus_hub restore, focus_game restore, flash_taskbar (guarded by game_active), focus_game (raises the GAME), flash_taskbar (guarded by game_active)
        {"emu_live.cpp", 3},   // manual dev tool hub_emu_live, not part of RetroHub.exe
    };
    // src/common is shared by the hub and the runners: nothing there may raise, show or topmost a window. (The runners' own game
    // windows live in src/n64run and src/retrorun and are not scanned.) The start-tips card is an owned, non-topmost popup with no
    // ShowWindow call, so it needs no marker.
    const std::map<std::string, int> known_common = {};
    // Synthetic input / input-queue attachment, by (file, token) -> lines. THE ONE ALLOWED EXCEPTION is View+Y bringing the game menu
    // forward (launch.cpp force_foreground: Alt tap + attached input so the foreground can be taken); everything else is hotkeys sent to a
    // game window that is already the foreground window.
    const std::map<std::pair<std::string, std::string>, int> known_synthetic = {
        {{"launch.cpp", "keybd_event"}, 2},         // View+Y: the Alt tap that lets the menu / the game take the foreground (player pressed it)
        {{"launch.cpp", "AttachThreadInput"}, 2},  // View+Y: attach to the foreground thread's input for that call, detach after
        {{"launch.cpp", "SendInput"}, 2},          // Cemu pad-screen toggle + Dolphin speed hotkeys, only while the game window is already the foreground window
    };

    const fs::path hub_dir = fs::path(HUB_SRC_DIR);
    const fs::path common_dir = hub_dir.parent_path() / "common";
    ScanResult hub = scan_dir(hub_dir, "src/hub");
    ScanResult common = scan_dir(common_dir, "src/common");
    report("hub", hub);
    report("common", common);
    CHECK(hub.scanned > 40);       // the scan really walked src/hub
    CHECK(common.scanned > 20);    // ... and src/common
    expect_marked("src/hub", hub, known_hub);
    expect_marked("src/common", common, known_common);

    std::map<std::pair<std::string, std::string>, int> seen_syn;
    for (const auto& s : hub.synthetic) seen_syn[s.first] = s.second;
    for (const auto& s : common.synthetic)
        if (seen_syn.count(s.first)) { std::printf("FAIL %s appears in both src/hub and src/common\n", s.first.first.c_str()); ++fails; }
        else seen_syn[{"common/" + s.first.first, s.first.second}] = s.second;
    for (const auto& s : seen_syn) {
        auto it = known_synthetic.find(s.first);
        if (it == known_synthetic.end()) { std::printf("FAIL %s: %d line(s) use %s (synthetic input / input attachment) and the site is not registered\n", s.first.first.c_str(), s.second, s.first.second.c_str()); ++fails; }
        else if (it->second != s.second) { std::printf("FAIL %s %s: %d line(s), expected %d\n", s.first.first.c_str(), s.first.second.c_str(), s.second, it->second); ++fails; }
    }
    for (const auto& k : known_synthetic)
        if (!seen_syn.count(k.first)) { std::printf("FAIL %s %s: registered but not found (update the table)\n", k.first.first.c_str(), k.first.second.c_str()); ++fails; }
}

/* The lint must actually catch things: a scratch directory with a bad file is reported, a marked line is counted, comments are ignored. */
static void lint_self_test() {
    const fs::path dir = fs::temp_directory_path() / ("hub_popup_guard_selftest_" + std::to_string((unsigned long long)std::time(nullptr)));
    std::error_code ec;
    fs::create_directories(dir, ec);
    {
        std::ofstream o(dir / "bad.cpp");
        o << "void f(HWND h) {\n"
             "    SetForegroundWindow(h);\n"                                   // unguarded
             "    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, 0);\n"              // unguarded topmost
             "    ShowWindow(h, SW_SHOWNORMAL);   // popup-guard: ok (a reason)\n"   // marked
             "    // SetForegroundWindow(h) in a comment\n"
             "    MessageBoxW(nullptr, L\"x\", L\"y\", 0);   // popup-guard: ok (not allowed for MessageBox)\n"
             "    keybd_event(VK_MENU, 0, 0, 0);\n"
             "    int wShowWindow = 1;\n"                                       // not the API
             "}\n";
    }
    ScanResult r = scan_dir(dir, "scratch");
    CHECK(r.scanned == 1);
    CHECK(r.violations.size() == 3);   // SetForegroundWindow, HWND_TOPMOST, and the exempted MessageBox
    CHECK(r.marked["bad.cpp"] == 2);   // the marked ShowWindow and the (rejected) marked MessageBox are both counted
    CHECK((r.synthetic[{"bad.cpp", "keybd_event"}]) == 1);
    fs::remove_all(dir, ec);
}

/* The UI Automation helper scripts (data/helpers/*.ps1) used to live inside the C++ sources and were covered by the lint above.
 * They are files now, so pin the same rule: no foreground-taking or synthetic-input calls. */
static void lint_helper_scripts() {
    const fs::path dir = fs::path(HUB_SRC_DIR).parent_path().parent_path() / "data" / "helpers";
    int n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".ps1") continue;
        ++n;
        std::ifstream f(e.path(), std::ios::binary);
        std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        for (const char* bad : {"SetForegroundWindow", "BringWindowToTop", "AppActivate", "SendKeys", "keybd_event", "AttachThreadInput", "SwitchToThisWindow", "MessageBox"})
            if (t.find(bad) != std::string::npos) { std::printf("FAIL %s uses %s\n", e.path().filename().string().c_str(), bad); ++fails; }
        std::string t2 = t;   // "SendInput" is allowed only as a word inside a comment line
        size_t pos = 0;
        while ((pos = t2.find("SendInput", pos)) != std::string::npos) {
            size_t ls = t2.rfind('\n', pos); ls = ls == std::string::npos ? 0 : ls + 1;
            if (t2.substr(ls, pos - ls).find('#') == std::string::npos) { std::printf("FAIL %s calls SendInput\n", e.path().filename().string().c_str()); ++fails; }
            pos += 9;
        }
    }
    CHECK(n >= 2);
}

int main() {
    lint_helper_scripts();
    logic();
    lint_self_test();
    lint();
    std::printf(fails ? "hub_popup_guard_test FAILED\n" : "hub_popup_guard_test OK\n");
    return fails ? 1 : 0;
}
