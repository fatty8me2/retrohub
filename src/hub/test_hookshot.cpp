// Console test for Hookshot and Longshot (src/hub/hookshot.cpp): supports(), flavors, ready(), the build stamp, decide() / use_for() with the
// switch.runner setting, and the save session (vault <-> the flavor's user\ folder). Temp dirs only. The save session runs against hookshot::user_dir() under the test exe's own
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
        // a build from the previous audited source (137c423) still plays; the card offers the newer one as a rebuild (never forced)
        make_build(tmp / "old", true, true, true, true, hookshot::kPreviousAuditedCommit);
        CHECK(hookshot::ready(&why, U8(tmp / "old")) && why.empty());
        {
            hookshot::Stamp so, sn;
            CHECK(hookshot::read_stamp(U8(tmp / "old"), &so) && hookshot::is_older_audited(so));
            CHECK(hookshot::read_stamp(U8(tmp / "ok"), &sn) && !hookshot::is_older_audited(sn));
            CHECK(std::string(hookshot::kSourceCommit) != hookshot::kPreviousAuditedCommit);
        }
    }

    // ---- flavors
    {
        using hookshot::Flavor;
        Flavor f = Flavor::Hookshot;
        CHECK(hookshot::flavor_from_id("hookshot", &f) && f == Flavor::Hookshot);
        CHECK(hookshot::flavor_from_id("longshot", &f) && f == Flavor::Longshot);
        f = Flavor::Hookshot;
        CHECK(!hookshot::flavor_from_id("lichii", &f) && f == Flavor::Hookshot);
        CHECK(!hookshot::flavor_from_id("fritchii", &f) && !hookshot::flavor_from_id("auto", &f) && !hookshot::flavor_from_id("", &f));
        CHECK(!hookshot::flavor_from_id("Longshot", &f) && !hookshot::flavor_from_id("HOOKSHOT", &f) && !hookshot::flavor_from_id("longshot ", &f));
        CHECK(hookshot::flavor_from_id("longshot", nullptr));
        CHECK(hookshot::kFlavorCount == 2 && hookshot::kFlavors[0] == Flavor::Hookshot && hookshot::kFlavors[1] == Flavor::Longshot);

        const hookshot::FlavorInfo& h = hookshot::info(Flavor::Hookshot);
        const hookshot::FlavorInfo& l = hookshot::info(Flavor::Longshot);
        CHECK(h.flavor == Flavor::Hookshot && l.flavor == Flavor::Longshot);
        CHECK(std::string(h.id) == "hookshot" && std::string(l.id) == "longshot");
        CHECK(std::string(h.name) == "Hookshot" && std::string(l.name) == "Longshot");
        CHECK(h.journal_kind == 100 && l.journal_kind == 101);
        CHECK(std::string(h.manifest_suffix) == "-hookshot" && std::string(l.manifest_suffix) == "-longshot");
        CHECK(std::string(h.source_commit) == hookshot::kSourceCommit && std::string(h.source_version) == hookshot::kSourceVersion);
        CHECK(std::string(l.source_commit) == "5c1f00dd444e735bf27b73b2f91c0617f930ddac" && std::string(l.source_version) == "v1.0.0");
        CHECK(std::string(h.source_commit) != l.source_commit);
        CHECK(std::string(h.previous_commit) == hookshot::kPreviousAuditedCommit && std::string(l.previous_commit).empty());
        CHECK(std::string(h.source_zip) != l.source_zip && std::string(h.source_zip_sha256) != l.source_zip_sha256);
        CHECK(std::string(l.source_zip_sha256).size() == 64 && std::string(h.source_zip_sha256).size() == 64);
        CHECK(std::string(l.source_zip) == "longshot-src-5c1f00d.zip");
        CHECK(std::string(h.exe_rel) == "hookshot.exe" && std::string(l.exe_rel) == "hookshot.exe");   // Longshot v1.0.0 still builds hookshot.exe
        for (Flavor k : hookshot::kFlavors) { Flavor back = Flavor::Hookshot; CHECK(hookshot::flavor_from_id(hookshot::info(k).id, &back) && back == k); }

        // folders: distinct, under <data>\emulators\<id>; the default argument is Hookshot
        const std::string gh = hookshot::game_dir(Flavor::Hookshot), gl = hookshot::game_dir(Flavor::Longshot);
        CHECK(gh != gl);
        CHECK(hookshot::game_dir() == gh && hookshot::exe_path() == hookshot::exe_path(Flavor::Hookshot) && hookshot::user_dir() == hookshot::user_dir(Flavor::Hookshot));
        CHECK(fs::u8path(gh).filename() == "hookshot" && fs::u8path(gl).filename() == "longshot");
        CHECK(fs::u8path(gh).parent_path() == fs::u8path(gl).parent_path() && fs::u8path(gl).parent_path().filename() == "emulators");
        CHECK(fs::u8path(hookshot::exe_path(Flavor::Longshot)) == fs::u8path(gl) / "hookshot.exe");
        CHECK(fs::u8path(hookshot::user_dir(Flavor::Longshot)) == fs::u8path(gl) / "user");
        CHECK(hookshot::exe_path(Flavor::Hookshot) != hookshot::exe_path(Flavor::Longshot));
        CHECK(hookshot::user_dir(Flavor::Hookshot) != hookshot::user_dir(Flavor::Longshot));

        // ready(): a build stamped for one flavor is not ready as the other
        std::string why;
        make_build(tmp / "fh", true, true, true, true, hookshot::kSourceCommit);
        make_build(tmp / "fl", true, true, true, true, l.source_commit);
        CHECK(hookshot::ready(&why, U8(tmp / "fh"), Flavor::Hookshot) && why.empty());
        CHECK(hookshot::ready(&why, U8(tmp / "fl"), Flavor::Longshot) && why.empty());
        CHECK(!hookshot::ready(&why, U8(tmp / "fh"), Flavor::Longshot) && why.find("rebuild") != std::string::npos && why.find("Longshot") != std::string::npos);
        CHECK(!hookshot::ready(&why, U8(tmp / "fl"), Flavor::Hookshot) && why.find("rebuild") != std::string::npos && why.find("Hookshot") != std::string::npos);
        CHECK(!hookshot::ready(&why, U8(tmp / "fl")));   // default flavor = Hookshot
        // previous_commit only for Hookshot
        make_build(tmp / "fprev", true, true, true, true, hookshot::kPreviousAuditedCommit);
        CHECK(hookshot::ready(&why, U8(tmp / "fprev"), Flavor::Hookshot));
        CHECK(!hookshot::ready(&why, U8(tmp / "fprev"), Flavor::Longshot));
        hookshot::Stamp sp, sl;
        CHECK(hookshot::read_stamp(U8(tmp / "fprev"), &sp) && hookshot::is_older_audited(sp, Flavor::Hookshot) && !hookshot::is_older_audited(sp, Flavor::Longshot));
        CHECK(hookshot::read_stamp(U8(tmp / "fl"), &sl) && !hookshot::is_older_audited(sl, Flavor::Longshot) && !hookshot::is_older_audited(sl, Flavor::Hookshot));
        make_build(tmp / "fl2", true, false, true, true, l.source_commit);
        CHECK(!hookshot::ready(&why, U8(tmp / "fl2"), Flavor::Longshot) && why.find("hookshot_vk.dll") != std::string::npos);
        CHECK(!hookshot::ready(&why, U8(tmp / "nothing-l"), Flavor::Longshot) && why.find("not built") != std::string::npos);
        // with no dir given, ready() looks at that flavor's own game folder
        fs::remove_all(fs::u8path(gh), ec);
        fs::remove_all(fs::u8path(gl), ec);
        CHECK(!hookshot::ready(&why, std::string(), Flavor::Longshot) && !hookshot::ready(&why, std::string(), Flavor::Hookshot));
        make_build(fs::u8path(gh), true, true, true, true, hookshot::kSourceCommit);
        CHECK(hookshot::ready(&why, std::string(), Flavor::Hookshot) && !hookshot::ready(&why, std::string(), Flavor::Longshot));
        fs::remove_all(fs::u8path(gh), ec);
        make_build(fs::u8path(gl), true, true, true, true, l.source_commit);
        CHECK(hookshot::ready(&why, std::string(), Flavor::Longshot) && !hookshot::ready(&why, std::string(), Flavor::Hookshot));
        fs::remove_all(fs::u8path(gl), ec);
    }

    // ---- use_for(): ready() looks at game_dir(), so put a good build there for this part
    const fs::path gdir = fs::u8path(hookshot::game_dir());
    fs::remove_all(gdir, ec);
    {
        hub::GameEntry oot = game("switch:0400486024dec000"), other = game("switch:0100abcd12340000");
        hub::gs::Values v;
        CHECK(!hookshot::use_for(oot, v));   // not built
        v["switch.runner"] = "hookshot";
        CHECK(!hookshot::use_for(oot, v));   // chosen but not built: use_for is false (decide() says NativeNotBuilt: Play asks, it never starts Lichii by itself)
        v.clear();
        make_build(gdir, true, true, true, true, hookshot::kSourceCommit);
        CHECK(!hookshot::use_for(oot, v));   // built, no value: Lichii (Hookshot is opt-in)
        v["switch.runner"] = "auto";
        CHECK(!hookshot::use_for(oot, v));   // the old v1.37.0 value means Lichii
        v["switch.runner"] = "lichii";
        CHECK(!hookshot::use_for(oot, v));
        v["switch.runner"] = "hookshot";
        CHECK(hookshot::use_for(oot, v));    // built and chosen
        CHECK(!hookshot::use_for(other, v));
        const hub::gs::Option* o = hub::gs::find_option("switch.runner");
        CHECK(o && o->def == "lichii" && o->scope == hub::gs::Scope::Visual && o->choices.size() == 4);
        CHECK(o && o->choices[0].value == "lichii" && o->choices[1].value == "fritchii" && o->choices[2].value == "hookshot" && o->choices[3].value == "longshot");
        v["switch.runner"] = "fritchii";
        CHECK(!hookshot::use_for(oot, v));   // Fritchii is not Hookshot: use_for stays false (the launch picks the emulator)
        hub::gs::Values dv = hub::gs::defaults_for(hub::System::Switch);
        CHECK(dv["switch.runner"] == "lichii");
        CHECK(!hookshot::use_for(oot, dv));
        // a stored "auto" is dropped on load / merge / resolve and the default (Lichii) applies
        {
            hub::gs::Store st;
            CHECK(st.from_json("{\"version\":\"1\",\"systems\":{},\"games\":{\"switch:0400486024dec000\":{\"switch.runner\":\"auto\"}}}"));
            CHECK(st.resolve(hub::System::Switch, "switch:0400486024dec000")["switch.runner"] == "lichii");
            CHECK(st.source(hub::System::Switch, "switch:0400486024dec000", "switch.runner") == 0);
            CHECK(!st.set_game("switch:0400486024dec000", hub::System::Switch, "switch.runner", "auto"));
            CHECK(st.set_game("switch:0400486024dec000", hub::System::Switch, "switch.runner", "hookshot"));
            CHECK(st.resolve(hub::System::Switch, "switch:0400486024dec000")["switch.runner"] == "hookshot");
        }
        fs::remove_all(gdir, ec);
    }

    // ---- decide(): the whole table. ready() looks at game_dir(f), so builds are put there.
    {
        using hookshot::Choice;
        using hookshot::Flavor;
        const fs::path gh = fs::u8path(hookshot::game_dir(Flavor::Hookshot)), gl = fs::u8path(hookshot::game_dir(Flavor::Longshot));
        fs::remove_all(gh, ec);
        fs::remove_all(gl, ec);
        hub::GameEntry oot = game("switch:0400486024dec000"), other = game("switch:0100abcd12340000");
        hub::gs::Values v;
        auto set = [&](const char* r) { v.clear(); if (r) v["switch.runner"] = r; };
        hookshot::Decision d;

        // nothing built: only a chosen native build says NativeNotBuilt
        set(nullptr);
        CHECK(hookshot::decide(oot, v).choice == Choice::Emulator);
        for (const char* r : {"lichii", "fritchii", "auto", "", "Longshot", "bogus"}) { set(r); CHECK(hookshot::decide(oot, v).choice == Choice::Emulator); }
        set("hookshot");
        d = hookshot::decide(oot, v);
        CHECK(d.choice == Choice::NativeNotBuilt && d.flavor == Flavor::Hookshot && !d.why.empty());
        set("longshot");
        d = hookshot::decide(oot, v);
        CHECK(d.choice == Choice::NativeNotBuilt && d.flavor == Flavor::Longshot && !d.why.empty());
        // a game that is not Ocarina of Time never gets a native decision
        for (const char* r : {"hookshot", "longshot", "lichii", "fritchii"}) { set(r); CHECK(hookshot::decide(other, v).choice == Choice::Emulator); }
        { hub::GameEntry n64 = game("switch:0400486024dec000"); n64.system = hub::System::N64; set("longshot"); CHECK(hookshot::decide(n64, v).choice == Choice::Emulator); }

        // only Hookshot built
        make_build(gh, true, true, true, true, hookshot::kSourceCommit);
        set(nullptr);    CHECK(hookshot::decide(oot, v).choice == Choice::Emulator);
        set("lichii");   CHECK(hookshot::decide(oot, v).choice == Choice::Emulator);
        set("fritchii"); CHECK(hookshot::decide(oot, v).choice == Choice::Emulator);
        set("auto");     CHECK(hookshot::decide(oot, v).choice == Choice::Emulator);
        set("hookshot");
        d = hookshot::decide(oot, v);
        CHECK(d.choice == Choice::Native && d.flavor == Flavor::Hookshot && d.why.empty());
        CHECK(hookshot::decide(other, v).choice == Choice::Emulator);
        // longshot chosen while only Hookshot is built: NativeNotBuilt as Longshot
        set("longshot");
        d = hookshot::decide(oot, v);
        CHECK(d.choice == Choice::NativeNotBuilt && d.flavor == Flavor::Longshot);
        CHECK(!hookshot::use_for(oot, v));

        // only Longshot built: hookshot chosen must be NativeNotBuilt with flavor Hookshot (not Longshot, not Emulator)
        fs::remove_all(gh, ec);
        make_build(gl, true, true, true, true, hookshot::info(Flavor::Longshot).source_commit);
        set("hookshot");
        d = hookshot::decide(oot, v);
        CHECK(d.choice == Choice::NativeNotBuilt && d.flavor == Flavor::Hookshot && !d.why.empty());
        CHECK(!hookshot::use_for(oot, v));
        set("longshot");
        d = hookshot::decide(oot, v);
        CHECK(d.choice == Choice::Native && d.flavor == Flavor::Longshot && d.why.empty());
        CHECK(!hookshot::use_for(oot, v));   // use_for stays the Hookshot-Native wrapper
        for (const char* r : {"lichii", "fritchii", "auto"}) { set(r); CHECK(hookshot::decide(oot, v).choice == Choice::Emulator); }
        set(nullptr);    CHECK(hookshot::decide(oot, v).choice == Choice::Emulator);

        // both built
        make_build(gh, true, true, true, true, hookshot::kSourceCommit);
        set("hookshot"); d = hookshot::decide(oot, v); CHECK(d.choice == Choice::Native && d.flavor == Flavor::Hookshot); CHECK(hookshot::use_for(oot, v));
        set("longshot"); d = hookshot::decide(oot, v); CHECK(d.choice == Choice::Native && d.flavor == Flavor::Longshot); CHECK(!hookshot::use_for(oot, v));

        // Hookshot's stamp inside Longshot's folder is not ready: rebuild needed
        fs::remove_all(gl, ec);
        make_build(gl, true, true, true, true, hookshot::kSourceCommit);
        set("longshot");
        d = hookshot::decide(oot, v);
        CHECK(d.choice == Choice::NativeNotBuilt && d.flavor == Flavor::Longshot && d.why.find("rebuild") != std::string::npos);
        fs::remove_all(gh, ec);
        fs::remove_all(gl, ec);
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

    // ---- save session per flavor: Longshot's own folder and manifest, the same vault folder, and the save follows the player between builds
    {
        using hookshot::Flavor;
        const fs::path vault = tmp / "vault-fl";
        const fs::path bat = vault / "switch" / "switch_0400486024dec000" / "battery";
        const fs::path vsave = bat / "save" / "0400486024DEC000";
        const fs::path hsave = fs::u8path(hookshot::user_dir(Flavor::Hookshot)) / hookshot::kSaveRel;
        const fs::path lsave = fs::u8path(hookshot::user_dir(Flavor::Longshot)) / hookshot::kSaveRel;
        hub::GameEntry oot = game("switch:0400486024dec000");
        fs::remove_all(fs::u8path(hookshot::user_dir(Flavor::Hookshot)), ec);
        fs::remove_all(fs::u8path(hookshot::user_dir(Flavor::Longshot)), ec);
        auto nap = [] { std::this_thread::sleep_for(std::chrono::milliseconds(60)); };

        write_text(vsave / "slot_00" / "progress.sav", "VAULT-1");
        hubemu::SaveSession a = hookshot::begin_save_session(oot, U8(bat), Flavor::Longshot);
        CHECK(a.active && a.error.empty());
        CHECK(fs::u8path(a.emu_root) == fs::u8path(hookshot::user_dir(Flavor::Longshot)));
        CHECK(fs::u8path(a.emu_root) != fs::u8path(hookshot::user_dir(Flavor::Hookshot)));
        CHECK(fs::u8path(a.manifest_path).filename() == "emu-switch-switch_0400486024dec000-longshot.json");
        CHECK(a.rels.size() == 1 && a.rels[0] == hookshot::kSaveRel && a.vault_rels.size() == 1 && a.vault_rels[0] == "save/0400486024DEC000");
        CHECK(read_text(lsave / "slot_00" / "progress.sav") == "VAULT-1");
        CHECK(!fs::exists(hsave / "slot_00" / "progress.sav"));   // Hookshot's folder is untouched by a Longshot session
        {   // same vault rel and backup folder as a Hookshot session
            hubemu::SaveSession hs0 = hookshot::begin_save_session(oot, U8(bat), Flavor::Hookshot);
            CHECK(hs0.vault_rels == a.vault_rels && hs0.rels == a.rels && hs0.backup_dir == a.backup_dir && hs0.battery == a.battery);
            CHECK(fs::u8path(hs0.manifest_path).filename() == "emu-switch-switch_0400486024dec000-hookshot.json");
            CHECK(fs::u8path(hs0.manifest_path).parent_path() == fs::u8path(a.manifest_path).parent_path());
            hubemu::end_session(hs0);
            fs::remove_all(fs::u8path(hookshot::user_dir(Flavor::Hookshot)), ec);
        }
        CHECK(fs::u8path(hookshot::begin_save_session(oot, U8(bat)).emu_root) == fs::u8path(hookshot::user_dir(Flavor::Hookshot)));   // default = Hookshot
        fs::remove_all(fs::u8path(hookshot::user_dir(Flavor::Hookshot)), ec);

        // Longshot plays and ends: the vault has it; then a Hookshot session receives exactly that save
        nap();
        write_text(lsave / "slot_00" / "progress.sav", "LONGSHOT-2");
        write_text(lsave / "account.sav", "ACC-L");
        hubemu::end_session(a);
        CHECK(a.error.empty());
        CHECK(read_text(vsave / "slot_00" / "progress.sav") == "LONGSHOT-2" && read_text(vsave / "account.sav") == "ACC-L");
        hubemu::SaveSession b = hookshot::begin_save_session(oot, U8(bat), Flavor::Hookshot);
        CHECK(b.active && b.error.empty());
        CHECK(read_text(hsave / "slot_00" / "progress.sav") == "LONGSHOT-2" && read_text(hsave / "account.sav") == "ACC-L");
        // Hookshot plays on and ends: a later Longshot session receives that
        nap();
        write_text(hsave / "slot_00" / "progress.sav", "HOOKSHOT-3");
        hubemu::end_session(b);
        CHECK(b.error.empty() && read_text(vsave / "slot_00" / "progress.sav") == "HOOKSHOT-3");
        hubemu::SaveSession c = hookshot::begin_save_session(oot, U8(bat), Flavor::Longshot);
        CHECK(c.active && read_text(lsave / "slot_00" / "progress.sav") == "HOOKSHOT-3");   // the newer vault file replaces Longshot's older one
        nap();
        write_text(lsave / "slot_00" / "progress.sav", "LONGSHOT-4");
        hubemu::end_session(c);
        CHECK(c.error.empty() && read_text(vsave / "slot_00" / "progress.sav") == "LONGSHOT-4");
        // each flavor has its own manifest; neither is Lichii's
        CHECK(fs::exists(vault / ".sync" / "emu-switch-switch_0400486024dec000-longshot.json"));
        CHECK(fs::exists(vault / ".sync" / "emu-switch-switch_0400486024dec000-hookshot.json"));
        CHECK(!fs::exists(vault / ".sync" / "emu-switch-switch_0400486024dec000.json"));
        CHECK(!fs::exists(bat / "save" / "5a3c9e1f0b7d24680000000000000001"));
        CHECK(!hookshot::begin_save_session(game("switch:0100abcd12340000"), U8(vault / "switch" / "x" / "battery"), Flavor::Longshot).active);
        CHECK(!hookshot::begin_save_session(oot, "", Flavor::Longshot).active);
        fs::remove_all(fs::u8path(hookshot::user_dir(Flavor::Hookshot)), ec);
        fs::remove_all(fs::u8path(hookshot::user_dir(Flavor::Longshot)), ec);
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
    fs::remove_all(fs::u8path(hookshot::game_dir(hookshot::Flavor::Longshot)), ec);
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hookshot tests passed\n");
    return 0;
}
