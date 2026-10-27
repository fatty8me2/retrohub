// Console test: the "never pop up over the user's game" policy (popup_guard.h) plus a source lint over src/hub.
// The lint fails when a raise / restore / show / focus / flash / MessageBox call appears outside window_guard.cpp without a
// "popup-guard: ok (reason)" marker, and when the set of marked sites differs from the known list below (so adding one is a
// conscious edit of this test).
#include "popup_guard.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
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
    }
    CHECK(is_shell_class(L"Shell_TrayWnd") && is_shell_class(L"Progman") && is_shell_class(L"WorkerW"));
    CHECK(!is_shell_class(L"SDL_app") && !is_shell_class(L"") && !is_shell_class(nullptr) && !is_shell_class(L"Shell_TrayWndX"));
}

namespace fs = std::filesystem;

static const char* kApis[] = {"SDL_RaiseWindow", "SDL_ShowWindow", "SDL_RestoreWindow", "SDL_SetWindowFullscreen", "SetForegroundWindow",
                              "BringWindowToTop", "ShowWindow(", "FlashWindow", "MessageBox", "AllowSetForegroundWindow", "SetActiveWindow",
                              "HWND_TOPMOST", "SDL_SetWindowAlwaysOnTop", "SDL_FlashWindow"};

static bool ident(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

static bool has_api(const std::string& code) {
    for (const char* a : kApis) {
        std::string s = a;
        for (size_t p = code.find(s); p != std::string::npos; p = code.find(s, p + 1))
            if (p == 0 || !ident(code[p - 1])) return true;   // not a suffix of another name (e.g. wShowWindow)
    }
    return false;
}

static void lint() {
    // Every marked site, by file -> number of lines. A new marked site means editing this table on purpose.
    const std::map<std::string, int> known = {
        {"tv_mode.cpp", 5},    // F11 toggle / startup fullscreen (guarded by game_active) / post-exit re-fullscreen (hub already focused)
        {"launch.cpp", 5},     // focus_hub (user's View+Y combo only), focus_game (raises the GAME), flash_taskbar (guarded by game_active)
        {"emu_live.cpp", 3},   // manual dev tool hub_emu_live, not part of RetroHub.exe
    };
    std::map<std::string, int> seen;
    int scanned = 0;
    for (const auto& de : fs::directory_iterator(HUB_SRC_DIR)) {
        if (!de.is_regular_file()) continue;
        const std::string name = de.path().filename().string();
        const std::string ext = de.path().extension().string();
        if ((ext != ".cpp" && ext != ".h") || name.rfind("test_", 0) == 0 || name == "popup_guard.h" || name == "window_guard.cpp") continue;
        std::ifstream in(de.path());
        std::string line;
        int ln = 0;
        bool block = false;
        ++scanned;
        while (std::getline(in, line)) {
            ++ln;
            std::string t = line;
            t.erase(0, t.find_first_not_of(" \t") == std::string::npos ? t.size() : t.find_first_not_of(" \t"));
            if (block) { if (t.find("*/") != std::string::npos) block = false; continue; }
            if (t.rfind("/*", 0) == 0) { if (t.find("*/") == std::string::npos) block = true; continue; }
            if (t.rfind("//", 0) == 0 || t.rfind("*", 0) == 0) continue;
            std::string code = line.substr(0, line.find("//"));
            if (!has_api(code)) continue;
            if (line.find("popup-guard: ok") == std::string::npos) {
                std::printf("FAIL unguarded raise/show call: %s:%d: %s\n", name.c_str(), ln, t.c_str());
                ++fails;
            } else {
                if (code.find("MessageBox") != std::string::npos) { std::printf("FAIL MessageBox may never be exempted: %s:%d\n", name.c_str(), ln); ++fails; }
                ++seen[name];
            }
        }
    }
    CHECK(scanned > 40);   // the scan really walked src/hub
    for (const auto& k : known) {
        int n = seen.count(k.first) ? seen[k.first] : 0;
        if (n != k.second) { std::printf("FAIL %s: %d marked sites, expected %d\n", k.first.c_str(), n, k.second); ++fails; }
    }
    for (const auto& s : seen)
        if (!known.count(s.first)) { std::printf("FAIL %s has marked sites but is not in the known list\n", s.first.c_str()); ++fails; }
}

int main() {
    logic();
    lint();
    std::printf(fails ? "hub_popup_guard_test FAILED\n" : "hub_popup_guard_test OK\n");
    return fails ? 1 : 0;
}
