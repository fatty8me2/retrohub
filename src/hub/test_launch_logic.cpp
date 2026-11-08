// Console test for the pure launch decisions and command-line pieces (launch_logic.h, launch_guard_logic.h, launch_settings.h).
// The core each retrorun system uses, quoting of titles/paths, the resume rules, and the
// guard that stops a second game from starting. No processes, no files.
#include <cstdio>
#include <string>

#include "launch_guard_logic.h"
#include "launch_logic.h"
#include "launch_settings.h"

using namespace hubui;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

int main() {
    using hub::System;
    // ---- libretro core per system ----
    CHECK(retro_core_name(System::GBA, "sameboy") == "mgba");
    CHECK(retro_core_name(System::GB, "sameboy") == "sameboy");
    CHECK(retro_core_name(System::GBC, "gambatte") == "gambatte");   // the player's Game Boy core setting
    CHECK(retro_core_name(System::NES, "x") == "nestopia");
    CHECK(retro_core_name(System::SNES, "x") == "snes9x");
    CHECK(retro_core_name(System::NDS, "x") == "melondsds");
    CHECK(retro_core_name(System::N64, "x").empty());   // not a retrorun system
    CHECK(retro_core_name(System::GC, "x").empty());
    // every retrorun system has a core
    for (System s : {System::GBA, System::GB, System::GBC, System::NES, System::SNES, System::NDS}) CHECK(!retro_core_name(s, "sameboy").empty());

    // ---- title and save stem ----
    {
        std::string t = title_arg("Zelda \"Link\"");
        CHECK(t.rfind(" --title ", 0) == 0);
        CHECK(has(t, "\xE2\x80\x94 RetroHub"));
        CHECK(!has(t, "\\\""));          // the quotes in the title were dropped, none escaped
        CHECK(has(t, "Zelda Link"));
        CHECK(save_stem_arg("").empty());
        CHECK(save_stem_arg("My Game (USA)") == " --save-stem \"My Game (USA)\"");
        CHECK(save_stem_arg("Game") == " --save-stem \"Game\"");   // always quoted
    }

    // ---- n64run base line ----
    {
        std::string a = n64_base_args("C:\\roms\\My Game.z64", "", "C:\\v\\n64\\k", "My Game");
        CHECK(a.rfind("--rom \"C:\\roms\\My Game.z64\" --game unknown --save-dir ", 0) == 0);   // path with a space stays one argument
        CHECK(has(a, " --title "));
        CHECK(has(n64_base_args("r.z64", "smash64", "d", "t"), " --game smash64 "));
        CHECK(n64_wants_result_file("remix") && n64_wants_result_file("smash64"));
        CHECK(!n64_wants_result_file("") && !n64_wants_result_file("oot") && !n64_wants_result_file("Remix"));
    }

    // ---- retrorun base line ----
    {
        std::string a = retro_base_args("C:\\c\\mgba_libretro.dll", "C:\\r\\a b.gba", "gba", "C:\\v\\b", "C:\\v\\s", " --resume", "A B", "inner");
        CHECK(a.rfind("--core \"C:\\c\\mgba_libretro.dll\" --rom \"C:\\r\\a b.gba\" --system gba --save-dir ", 0) == 0);
        size_t st = a.find(" --state-dir "), rs = a.find(" --resume"), ti = a.find(" --title "), ss = a.find(" --save-stem ");
        CHECK(st != std::string::npos && rs != std::string::npos && ti != std::string::npos && ss != std::string::npos);
        CHECK(st < rs && rs < ti && ti < ss);   // the order the runner and the resume stripping rely on
        std::string nds = retro_base_args("c.dll", "r.nds", "nds", "b", "s", "", "T", "");
        CHECK(!has(nds, "--resume") && !has(nds, "--save-stem"));   // DS never resumes
        CHECK(!has(retro_base_args("c", "r", "nes", "b", "", "", "T", ""), "--state-dir"));
    }

    // ---- resume handling ----
    {
        std::string a = retro_base_args("c.dll", "r.gb", "gb", "b", "s", " --resume", "T", "");
        std::string b = erase_resume_arg(a);
        CHECK(has(a, " --resume") && !has(b, "--resume"));
        CHECK(b == retro_base_args("c.dll", "r.gb", "gb", "b", "s", "", "T", ""));   // nothing else touched
        CHECK(erase_resume_arg("--core x") == "--core x");   // no resume: unchanged
        // n64: resume needs a state dir, added once, never twice
        CHECK(ensure_state_dir_for_resume("--rom r --resume", "C:\\s t") == "--rom r --resume --state-dir \"C:\\s t\"");
        CHECK(ensure_state_dir_for_resume("--rom r", "C:\\s") == "--rom r");   // no resume: no state dir
        CHECK(ensure_state_dir_for_resume("--rom r --resume --state-dir x", "C:\\s") == "--rom r --resume --state-dir x");
    }

    // ---- may a game start now (shell-H2 guard) ----
    CHECK(launch_allowed(false, false));
    CHECK(!launch_allowed(true, false));
    CHECK(!launch_allowed(false, true));                 // NetPlay / a 3DS room never set A.proc
    CHECK(launch_allowed(false, true, true));            // a room's own game inside its session
    CHECK(!launch_allowed(true, true, true));            // but never on top of a running game
    CHECK(std::string(launch_busy_reason(true)) == "A game is already running.");
    CHECK(std::string(launch_busy_reason(false)) == "An online game is running.");
    CHECK(cheats_return_focus(true, true) && !cheats_return_focus(true, false) && !cheats_return_focus(false, true));

    // ---- command-line quoting used by every runner ----
    CHECK(cmd_quote("") == "\"\"");
    CHECK(join_cmd({"--a", "b c", ""}) == " --a \"b c\" \"\"");

    std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
