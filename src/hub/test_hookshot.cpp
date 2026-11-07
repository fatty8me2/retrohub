// Console test for Hookshot (src/hub/hookshot.cpp): supports(), ready(), the build stamp, use_for() with the switch.runner setting, and the
// save session (vault <-> Hookshot's user\ folder). Temp dirs only. The save session runs against hookshot::user_dir() under the test exe's own
// userdata folder (never the real RetroHub data), which this test wipes before and after.
#include <windows.h>
#include "emulators.h"
#include "games.h"
#include "game_settings.h"
#include "hookshot.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

static void write_text(const fs::path& p, const std::string& t) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(t.data(), (std::streamsize)t.size());
}
static std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static std::string U8(const fs::path& p) { return p.u8string(); }

static hub::GameEntry game(const std::string& key) {
    hub::GameEntry e;
    e.system = hub::System::Switch;
    e.key = key;
    return e;
}

static void make_build(const fs::path& d, bool exe, bool vk, bool sdl, bool romfs, const char* commit) {
    fs::create_directories(d);
    if (exe) write_text(d / "hookshot.exe", "x");
    if (vk) write_text(d / "hookshot_vk.dll", "x");
    if (sdl) write_text(d / "SDL2.dll", "x");
    if (romfs) fs::create_directories(d / "romfs");
    if (commit) {
        hookshot::Stamp s;
        s.commit = commit;
        s.game_build = hookshot::kGameBuild;
        s.built = 1;
        hookshot::write_stamp(U8(d), s);
    }
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / ("hs_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    // ---- supports()
    CHECK(hookshot::supports(game("switch:0400486024dec000")));
    CHECK(hookshot::supports(game("switch:0400486024DEC000")));
    CHECK(hookshot::supports(game("switch:0400486024dec000-v2")));
    CHECK(!hookshot::supports(game("switch:0100abcd12340000")));
    CHECK(!hookshot::supports(game("switch:0400486024dec0001")));
    CHECK(!hookshot::supports(game("switch:0400486024dec00")));
    CHECK(!hookshot::supports(game("")));
    { hub::GameEntry g = game("switch:0400486024dec000"); g.system = hub::System::N64; CHECK(!hookshot::supports(g)); }

    // ---- stamp round trip
    {
        fs::path d = tmp / "stamp";
        fs::create_directories(d);
        hookshot::Stamp s, r;
        s.commit = "abc"; s.game_build = "e0f3ccfa"; s.source = "C:\\games\\oot \"x\".nsp"; s.built = 1700000000; s.size = 123456789012ull;
        CHECK(!hookshot::read_stamp(U8(d), &r));
        CHECK(hookshot::write_stamp(U8(d), s));
        CHECK(hookshot::read_stamp(U8(d), &r));
        CHECK(r.commit == s.commit && r.game_build == s.game_build && r.source == s.source && r.built == s.built && r.size == s.size);
    }

    // ---- ready()
    {
        std::string why;
        CHECK(!hookshot::ready(&why, U8(tmp / "nothing")) && !why.empty());
        make_build(tmp / "r1", false, true, true, true, hookshot::kSourceCommit);
        CHECK(!hookshot::ready(&why, U8(tmp / "r1")) && why.find("not built") != std::string::npos);
        make_build(tmp / "r2", true, false, true, true, hookshot::kSourceCommit);
        CHECK(!hookshot::ready(&why, U8(tmp / "r2")) && why.find("hookshot_vk.dll") != std::string::npos);
        make_build(tmp / "r3", true, true, false, true, hookshot::kSourceCommit);
        CHECK(!hookshot::ready(&why, U8(tmp / "r3")) && why.find("SDL2.dll") != std::string::npos);
        make_build(tmp / "r4", true, true, true, false, hookshot::kSourceCommit);
        CHECK(!hookshot::ready(&why, U8(tmp / "r4")) && why.find("romfs") != std::string::npos);
        make_build(tmp / "r5", true, true, true, true, nullptr);
        CHECK(!hookshot::ready(&why, U8(tmp / "r5")) && why.find("stamp") != std::string::npos);
        make_build(tmp / "r6", true, true, true, true, "0000000000000000000000000000000000000000");
        CHECK(!hookshot::ready(&why, U8(tmp / "r6")) && why.find("rebuild") != std::string::npos);
        make_build(tmp / "ok", true, true, true, true, hookshot::kSourceCommit);
        CHECK(hookshot::ready(&why, U8(tmp / "ok")) && why.empty());
    }

    // ---- use_for(): ready() looks at game_dir(), so put a good build there for this part
    const fs::path gdir = fs::u8path(hookshot::game_dir());
    fs::remove_all(gdir, ec);
    {
        hub::GameEntry oot = game("switch:0400486024dec000"), other = game("switch:0100abcd12340000");
        hub::gs::Values v;
        CHECK(!hookshot::use_for(oot, v));   // not built
        make_build(gdir, true, true, true, true, hookshot::kSourceCommit);
        CHECK(hookshot::use_for(oot, v));    // no value: auto
        v["switch.runner"] = "auto";
        CHECK(hookshot::use_for(oot, v));
        v["switch.runner"] = "lichii";
        CHECK(!hookshot::use_for(oot, v));
        v["switch.runner"] = "auto";
        CHECK(!hookshot::use_for(other, v));
        const hub::gs::Option* o = hub::gs::find_option("switch.runner");
        CHECK(o && o->def == "auto" && o->scope == hub::gs::Scope::Visual && o->choices.size() == 2);
        hub::gs::Values dv = hub::gs::defaults_for(hub::System::Switch);
        CHECK(dv["switch.runner"] == "auto");
        CHECK(hookshot::use_for(oot, dv));
        fs::remove_all(gdir, ec);
    }

    // ---- save session
    {
        const fs::path vault = tmp / "vault";
        const fs::path bat = vault / "switch" / "switch_0400486024dec000" / "battery";
        const fs::path vsave = bat / "save" / "0400486024DEC000";
        const fs::path hsave = fs::u8path(hookshot::user_dir()) / hookshot::kSaveRel;
        hub::GameEntry oot = game("switch:0400486024dec000");
        fs::remove_all(fs::u8path(hookshot::user_dir()), ec);

        // Lichii's manifest from an earlier session must stay as it was
        const fs::path lman = vault / ".sync" / "emu-switch-switch_0400486024dec000.json";
        write_text(lman, "{\"lichii\":\"untouched\"}");

        write_text(vsave / "slot_00" / "progress.sav", "VAULT-1");
        write_text(vsave / "option.sav", "OPT");
        hubemu::SaveSession s = hookshot::begin_save_session(oot, U8(bat));
        CHECK(s.active && s.error.empty());
        CHECK(s.rels.size() == 1 && s.rels[0] == "save/5a3c9e1f0b7d24680000000000000001");
        CHECK(s.vault_rels.size() == 1 && s.vault_rels[0] == "save/0400486024DEC000");
        CHECK(fs::u8path(s.manifest_path).filename() == "emu-switch-switch_0400486024dec000-hookshot.json");
        CHECK(read_text(hsave / "slot_00" / "progress.sav") == "VAULT-1");   // vault -> Hookshot on begin
        CHECK(read_text(hsave / "option.sav") == "OPT");

        // the game plays: newer file, plus a new one
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        write_text(hsave / "slot_00" / "progress.sav", "HOOKSHOT-2");
        write_text(hsave / "account.sav", "ACC");
        hubemu::end_session(s);
        CHECK(s.error.empty());
        CHECK(read_text(vsave / "slot_00" / "progress.sav") == "HOOKSHOT-2");   // Hookshot -> vault on end
        CHECK(read_text(vsave / "account.sav") == "ACC");
        CHECK(!fs::exists(bat / "save" / "5a3c9e1f0b7d24680000000000000001"));   // the vault only ever holds the title id folder
        {
            bool backed_up = false;   // the older vault copy was backed up
            for (auto& de : fs::recursive_directory_iterator(fs::u8path(s.backup_dir), ec))
                if (de.is_regular_file() && read_text(de.path()) == "VAULT-1") backed_up = true;
            CHECK(backed_up);
        }
        CHECK(read_text(lman) == "{\"lichii\":\"untouched\"}");

        // newest wins: a newer vault file (another PC) replaces Hookshot's, which is backed up
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        write_text(vsave / "option.sav", "OPT-FROM-PC-B");
        hubemu::SaveSession s2 = hookshot::begin_save_session(oot, U8(bat) + "\\");
        CHECK(s2.active && s2.error.empty());
        CHECK(read_text(hsave / "option.sav") == "OPT-FROM-PC-B");
        CHECK(read_text(hsave / "slot_00" / "progress.sav") == "HOOKSHOT-2");   // the newer Hookshot copy is kept
        bool backed = false;
        for (auto& de : fs::recursive_directory_iterator(fs::u8path(s2.backup_dir), ec))
            if (de.is_regular_file() && read_text(de.path()) == "OPT") backed = true;
        CHECK(backed);   // Hookshot's overwritten copy kept
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        write_text(hsave / "option.sav", "OPT-NEWER-HERE");
        hubemu::end_session(s2);
        CHECK(read_text(vsave / "option.sav") == "OPT-NEWER-HERE");
        CHECK(read_text(lman) == "{\"lichii\":\"untouched\"}");

        // not this game: nothing happens
        hub::GameEntry other = game("switch:0100abcd12340000");
        CHECK(!hookshot::begin_save_session(other, U8(vault / "switch" / "x" / "battery")).active);
        fs::remove_all(fs::u8path(hookshot::user_dir()), ec);
    }

    // ---- AV1 probe: only that it answers (the answer depends on the PC) and is stable until a recheck
    {
        const bool a = hookshot::av1_decoder_present();
        CHECK(hookshot::av1_decoder_present() == a);
        CHECK(hookshot::av1_decoder_present(true) == a);
        std::printf("av1 decoder present: %s\n", a ? "yes" : "no");
    }

    fs::remove_all(tmp, ec);
    fs::remove_all(fs::u8path(hookshot::game_dir()), ec);
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hookshot tests passed\n");
    return 0;
}
