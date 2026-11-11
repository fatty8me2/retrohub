// Console unit test for game_settings (defaults, merge, serialization, online filtering, emitters).
#include "game_settings.h"

#include <cstdio>
#include <fstream>
#include <filesystem>
#include <string>

using namespace hub;
using namespace hub::gs;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static bool has_pair(const std::vector<std::string>& a, const std::string& k, const std::string& v) {
    for (size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == k && a[i + 1] == v) return true;
    return false;
}

int main() {
    // every option has a valid default and applies to some system
    for (auto& o : options()) {
        std::string n;
        CHECK(o.systems != 0);
        CHECK(normalize(o, o.def, &n) && n == o.def);
    }
    CHECK(defaults_for(System::N64).count("n64.res") && !defaults_for(System::N64).count("gc.res"));
    CHECK(defaults_for(System::GB).count("gb.hardware") && defaults_for(System::GBC).count("gb.hardware"));
    CHECK(options_for(System::NDS).size() > 2);

    // layering: option default < system < game
    Store s;
    CHECK(s.resolve(System::N64, "n64:a")["n64.aa"] == "0");
    CHECK(s.set_system(System::N64, "n64.aa", "4"));
    CHECK(s.resolve(System::N64, "n64:a")["n64.aa"] == "4");
    CHECK(s.set_game("n64:a", System::N64, "n64.aa", "2"));
    CHECK(s.resolve(System::N64, "n64:a")["n64.aa"] == "2");
    CHECK(s.resolve(System::N64, "n64:b")["n64.aa"] == "4");
    CHECK(s.source(System::N64, "n64:a", "n64.aa") == 2 && s.source(System::N64, "n64:b", "n64.aa") == 1 &&
          s.source(System::N64, "n64:b", "n64.res") == 0);
    // validation
    CHECK(!s.set_system(System::N64, "n64.aa", "3"));            // not a choice
    CHECK(!s.set_system(System::N64, "gc.res", "2"));            // wrong system
    CHECK(!s.set_game("", System::N64, "n64.aa", "2"));
    CHECK(s.set_game("n64:a", System::N64, "fullscreen", "ON") && s.resolve(System::N64, "n64:a")["fullscreen"] == "true");
    CHECK(!s.set_game("n64:a", System::N64, "n64.texpack", std::string("a\nb")));

    // serialization round trip (incl. escapes and unicode)
    CHECK(s.set_game("n64:a", System::N64, "n64.texpack", "C:\\Packs\\\"x\"\\caf\xC3\xA9"));
    std::string js = s.to_json();
    Store t;
    CHECK(t.from_json(js));
    CHECK(t.resolve(System::N64, "n64:a") == s.resolve(System::N64, "n64:a"));
    CHECK(t.resolve(System::N64, "n64:b") == s.resolve(System::N64, "n64:b"));
    CHECK(t.resolve(System::N64, "n64:a")["n64.texpack"] == "C:\\Packs\\\"x\"\\caf\xC3\xA9");
    // malformed input leaves the store unchanged; bad entries are dropped
    CHECK(!t.from_json("{\"games\": {") && t.resolve(System::N64, "n64:a")["n64.aa"] == "2");
    Store u;
    CHECK(u.from_json("{\"games\":{\"g\":{\"n64.aa\":\"99\",\"nope\":\"1\",\"n64.res\":\"2\"}}}"));
    CHECK(u.resolve(System::N64, "g")["n64.res"] == "2" && u.resolve(System::N64, "g")["n64.aa"] == "0");

    // resets
    s.reset_game("n64:a");
    CHECK(!s.has_game_overrides("n64:a") && s.resolve(System::N64, "n64:a")["n64.aa"] == "4");
    s.clear_system_key(System::N64, "n64.aa");
    CHECK(s.resolve(System::N64, "n64:a")["n64.aa"] == "0");
    s.set_system(System::GC, "gc.res", "4"); s.set_game("gc:x", System::GC, "gc.res", "3");
    s.reset_system(System::GC);
    CHECK(s.resolve(System::GC, "gc:x")["gc.res"] == "3" && s.resolve(System::GC, "gc:y")["gc.res"] == "2");
    s.reset_all();
    CHECK(s.resolve(System::GC, "gc:x")["gc.res"] == "2");

    // merge
    Store a, b;
    a.set_system(System::GBA, "scale.filter", "linear"); b.set_system(System::GBA, "scale.filter", "nearest");
    b.set_game("gba:z", System::GBA, "gba.skip_bios", "true");
    a.merge_from(b);
    CHECK(a.resolve(System::GBA, "gba:z")["scale.filter"] == "nearest" && a.resolve(System::GBA, "gba:z")["gba.skip_bios"] == "true");

    // vault file
    fs::path d = fs::temp_directory_path() / "hub_gs_test_vault";
    fs::remove_all(d);
    Store f;
    CHECK(f.load(d.u8string()));   // missing = empty
    f.set_game("gb:k", System::GB, "gb.hardware", "dmg");
    CHECK(f.save(d.u8string()) && fs::exists(Store::file_path(d.u8string())));
    Store g;
    CHECK(g.load(d.u8string()) && g.resolve(System::GB, "gb:k")["gb.hardware"] == "dmg");
    fs::remove_all(d);

    // online: Locked forced to default, Sync from host, Visual kept
    Store o;
    o.set_game("n64:a", System::N64, "n64.fb_emulation", "off");
    o.set_game("n64:a", System::N64, "n64.res", "4");
    Values r = o.resolve(System::N64, "n64:a");
    CHECK(r["n64.fb_emulation"] == "off");
    Values eff = online_effective(System::N64, r, Values());
    CHECK(eff["n64.fb_emulation"] == "default" && eff["n64.res"] == "4");
    CHECK(sync_fragment(r).empty());   // N64 has no Sync options
    Store gb;
    gb.set_game("gb:k", System::GB, "gb.hardware", "agb");
    gb.set_game("gb:k", System::GB, "scale.filter", "linear");
    Values host = gb.resolve(System::GB, "gb:k");
    std::string frag = sync_fragment(host);
    CHECK(frag == ",\"gs\":{\"gb.hardware\":\"agb\"}");
    Values wire = extract_sync("{\"kind\":\"launch\",\"game\":\"x\"" + frag + "}");
    CHECK(wire.size() == 1 && wire["gb.hardware"] == "agb");
    CHECK(extract_sync("{\"kind\":\"launch\"}").empty());
    CHECK(extract_sync("{\"gs\":{\"gb.hardware\":\"bogus\",\"scale.filter\":\"linear\",\"x\":\"1\"}}").empty());   // invalid / non-sync dropped
    Store guest;
    guest.set_game("gb:k", System::GB, "gb.hardware", "dmg");
    guest.set_game("gb:k", System::GB, "scale.filter", "nearest");
    Values ge = online_effective(System::GB, guest.resolve(System::GB, "gb:k"), wire);
    CHECK(ge["gb.hardware"] == "agb" && ge["scale.filter"] == "nearest");   // host's emulation value, own visual value
    Values old = online_effective(System::GB, guest.resolve(System::GB, "gb:k"), Values());   // older host: default
    CHECK(old["gb.hardware"] == "auto");
    CHECK(online_effective(System::GB, host, sync_subset(host)) == host);   // host keeps its own

    // emitters
    Store e;
    e.set_game("n64:a", System::N64, "n64.res", "2");
    e.set_game("n64:a", System::N64, "n64.aspect", "widescreen");
    e.set_game("n64:a", System::N64, "n64.aa", "4");
    e.set_game("n64:a", System::N64, "n64.texpack", "D:\\tex");
    e.set_game("n64:a", System::N64, "n64.fb_emulation", "off");
    std::vector<std::string> na = n64_args(e.resolve(System::N64, "n64:a"));
    CHECK(has_pair(na, "--gfx", "UseNativeResolutionFactor=i:2") && has_pair(na, "--gfx", "AspectRatio=i:3") &&
          has_pair(na, "--gfx", "MultiSampling=i:4") && has_pair(na, "--gfx", "txPath=s:D:\\tex") &&
          has_pair(na, "--gfx", "txHiresEnable=b:1") && has_pair(na, "--gfx", "EnableFBEmulation=b:0"));
    std::vector<std::string> nd = n64_args(online_effective(System::N64, e.resolve(System::N64, "n64:a"), Values()));
    CHECK(!has_pair(nd, "--gfx", "EnableFBEmulation=b:0"));
    CHECK(n64_key_locked_online("EnableFBEmulation") && !n64_key_locked_online("MultiSampling"));
    GfxParam gp;
    CHECK(parse_gfx_arg("txPath=s:D:\\tex", &gp) && gp.type == 's' && gp.sval == "D:\\tex");
    CHECK(parse_gfx_arg("MultiSampling=i:4", &gp) && gp.ival == 4);
    CHECK(!parse_gfx_arg("X=b:2", &gp) && !parse_gfx_arg("X=i:", &gp) && !parse_gfx_arg("novalue", &gp) && !parse_gfx_arg("X=q:1", &gp));
    for (auto& p : n64_gliden_params(defaults_for(System::N64))) CHECK(p.first == "AspectRatio");   // defaults: only the aspect

    Store rr;
    rr.set_game("gbc:k", System::GBC, "scale.filter", "linear");
    rr.set_game("gbc:k", System::GBC, "scale.integer", "false");
    rr.set_game("gbc:k", System::GBC, "gb.color", "on");
    rr.set_game("gbc:k", System::GBC, "gb.hardware", "cgb");
    std::vector<std::string> ra = retrorun_args(System::GBC, rr.resolve(System::GBC, "gbc:k"));
    CHECK(has_pair(ra, "--filter", "linear") && has_pair(ra, "--integer-scale", "0") &&
          has_pair(ra, "--opt", "gambatte_gbc_color_correction=always") && has_pair(ra, "--opt", "gambatte_gb_hwmode=GBC"));
    CHECK(retrorun_args(System::GBA, defaults_for(System::GBA)).empty());
    rr.set_game("nds:k", System::NDS, "ds.layout", "left-right");
    CHECK(has_pair(retrorun_args(System::NDS, rr.resolve(System::NDS, "nds:k")), "--opt", "melonds_screen_layout1=left-right"));
    CHECK(is_fullscreen(Values{{"fullscreen", "true"}}) && !is_fullscreen(Values()));
    // screen filter + bezel: per console default and per-game override reach retrorun as --fx / --bezel
    rr.set_system(System::GBA, "fx.filter", "lcd");
    rr.set_game("gba:k", System::GBA, "fx.bezel", "true");
    CHECK(has_pair(retrorun_args(System::GBA, rr.resolve(System::GBA, "gba:k")), "--fx", "lcd") && has_pair(retrorun_args(System::GBA, rr.resolve(System::GBA, "gba:k")), "--bezel", "1"));
    {   // play.rewind: auto (default) adds nothing; off reaches retrorun as --rewind off; bad values refused
        CHECK(!has_pair(retrorun_args(System::GBA, defaults_for(System::GBA)), "--rewind", "off") && defaults_for(System::GBA).at("play.rewind") == "auto");
        Store rw;
        CHECK(rw.set_game("gba:r", System::GBA, "play.rewind", "off") && !rw.set_game("gba:r", System::GBA, "play.rewind", "on"));
        CHECK(has_pair(retrorun_args(System::GBA, rw.resolve(System::GBA, "gba:r")), "--rewind", "off"));
        CHECK(!has_pair(retrorun_args(System::GBA, rw.resolve(System::GBA, "gba:other")), "--rewind", "off"));
        CHECK(!rw.set_game("n64:r", System::N64, "play.rewind", "off"));   // N64 has no rewind
    }
    CHECK(!rr.set_game("gba:k", System::GBA, "fx.filter", "bogus"));
    CHECK(!has_pair(retrorun_args(System::GBA, rr.resolve(System::GBA, "other:k")), "--bezel", "1"));

    {   // saves-F11: per-game stamps, merge game by game (newer stamp wins the whole entry), a reset is a stamped tombstone, merge-on-write, garbage never wins
        fs::path v = fs::temp_directory_path() / "hub_gs_merge_test";
        std::error_code ec;
        fs::remove_all(v, ec);
        const std::string vd = v.u8string();
        set_stamp_clock(1000);
        Store pc1;
        pc1.set_game("n64:a", System::N64, "fullscreen", "true");
        pc1.set_game("n64:c", System::N64, "fullscreen", "true");
        CHECK(!pc1.empty() && Store().empty());
        CHECK(pc1.save(vd));
        set_stamp_clock(2000);
        Store pc2;   // another PC: its own game, a newer value for n64:a
        pc2.set_game("n64:a", System::N64, "fullscreen", "false");
        pc2.set_game("n64:b", System::N64, "fullscreen", "true");
        pc2.set_game("n64:c", System::N64, "fullscreen", "false");
        pc2.reset_game("n64:c");   // a reset writes a stamped, empty entry: a tombstone
        // the cloud engine merges pc2's file into the file on disk
        CHECK(merge_cloud_into_vault(vd, pc2.to_json()));
        Store m;
        CHECK(m.load(vd));
        CHECK(m.resolve(System::N64, "n64:a")["fullscreen"] == "false");   // the newer entry won
        CHECK(m.resolve(System::N64, "n64:b")["fullscreen"] == "true");    // only pc2 had it: kept
        CHECK(!m.has_game_overrides("n64:c"));                             // the newer reset is not undone by the union
        // a stale in-memory store (pc1 loaded long ago, still believes n64:a is true) saves: the newer disk data survives
        set_stamp_clock(1500);
        pc1.set_game("n64:d", System::N64, "fullscreen", "true");
        CHECK(pc1.save(vd));
        CHECK(m.load(vd) && m.resolve(System::N64, "n64:a")["fullscreen"] == "false" && m.has_game_overrides("n64:b") && m.has_game_overrides("n64:d") && !m.has_game_overrides("n64:c"));
        // a reset here is stamped and wins over the older disk entry
        set_stamp_clock(3000);
        m.reset_game("n64:b");
        CHECK(m.save(vd));
        Store back;
        CHECK(back.load(vd) && !back.has_game_overrides("n64:b") && back.has_game_overrides("n64:d"));
        // garbage from the cloud never replaces the file; a malformed local file is treated as empty
        CHECK(!merge_cloud_into_vault(vd, "not json") && back.load(vd) && back.has_game_overrides("n64:d"));
        { std::ofstream f(Store::file_path(vd), std::ios::binary | std::ios::trunc); f << "{broken"; }
        CHECK(merge_cloud_into_vault(vd, pc2.to_json()) && back.load(vd) && back.has_game_overrides("n64:a"));
        // an older RetroHub's file has no stamps: still loads, and its keys are kept by a union
        // an older, unstamped file next to a stamped entry: every key of both is kept, the stamped side wins a clash (this PC's pre-stamp choices never vanish)
        {
            fs::path v2 = fs::temp_directory_path() / "hub_gs_legacy_test";
            fs::remove_all(v2, ec);
            const std::string vd2 = v2.u8string();
            Store old;
            CHECK(old.from_json("{\"games\":{\"n64:z\":{\"fullscreen\":\"true\",\"n64.res\":\"2\"}}}") && old.save(vd2));
            set_stamp_clock(500);
            Store fresh;
            fresh.set_game("n64:z", System::N64, "fullscreen", "false");
            CHECK(merge_cloud_into_vault(vd2, fresh.to_json()));
            Store got;
            CHECK(got.load(vd2) && got.resolve(System::N64, "n64:z")["fullscreen"] == "false" && got.resolve(System::N64, "n64:z")["n64.res"] == "2");
            // a malformed file on disk is copied to _conflicts before a save replaces it
            { std::ofstream f(Store::file_path(vd2), std::ios::binary | std::ios::trunc); f << "{broken"; }
            CHECK(got.save(vd2));
            int kept = 0;
            for (auto& de : fs::directory_iterator(v2 / "_conflicts" / "_settings", ec)) if (de.path().filename().string().rfind("game_settings.json.unreadable", 0) == 0) ++kept;
            CHECK(kept == 1);
            Store again;
            CHECK(again.load(vd2) && again.resolve(System::N64, "n64:z")["n64.res"] == "2");
            fs::remove_all(v2, ec);
        }
        Store legacy;
        CHECK(legacy.from_json("{\"games\":{\"n64:z\":{\"fullscreen\":\"true\"}}}") && legacy.has_game_overrides("n64:z") && !legacy.empty());
        set_stamp_clock(0);
        fs::remove_all(v, ec);
    }

    // console layer: Nintendo Switch 2 on top of Nintendo Switch
    {
        set_stamp_clock(100);
        Store s;
        const std::string fs_k = "fullscreen";
        // other consoles: the console calls equal the System calls
        CHECK(s.set_console("n64", "n64.res", "2") && s.resolve(System::N64, "g")["n64.res"] == "2" && s.resolve_console("n64", "g")["n64.res"] == "2");
        CHECK(s.source_console("n64", "g", "n64.res") == 1 && s.source(System::N64, "g", "n64.res") == 1);
        CHECK(!s.set_console("nope", "fullscreen", "true") && s.resolve_console("nope", "g").empty() && s.source_console("nope", "g", fs_k) == 0);
        CHECK(!s.set_console("n64", "gb.hardware", "x"));   // validation is the system's
        // Switch 2 inherits Switch until it sets its own
        CHECK(s.set_console("switch", "fullscreen", "false"));
        CHECK(s.resolve_console("switch2", "")["fullscreen"] == "false");
        CHECK(s.source_console("switch2", "", "fullscreen") == 1 && !s.has_console_value("switch2", "fullscreen") && s.has_console_value("switch", "fullscreen"));
        CHECK(s.resolve_console("switch", "")["fullscreen"] == "false");
        CHECK(s.set_console("switch2", "fullscreen", "true"));
        CHECK(s.resolve_console("switch2", "")["fullscreen"] == "true" && s.resolve_console("switch", "")["fullscreen"] == "false");
        CHECK(s.has_console_value("switch2", "fullscreen") && s.source_console("switch2", "", "fullscreen") == 1);
        CHECK(s.resolve(System::Switch, "")["fullscreen"] == "false");   // the System call is the Switch layer only
        // game overrides win over both layers and are shared: keyed on the game, not the console
        CHECK(s.set_game("switch:abc", System::Switch, "fullscreen", "false"));
        CHECK(s.resolve_console("switch2", "switch:abc")["fullscreen"] == "false" && s.source_console("switch2", "switch:abc", "fullscreen") == 2);
        GameEntry g2; g2.system = System::Switch; g2.nx2 = true; g2.key = "switch:abc";
        GameEntry g1; g1.system = System::Switch; g1.key = "switch:def";
        CHECK(s.resolve_game(g2)["fullscreen"] == "false");
        s.clear_game_key("switch:abc", "fullscreen");
        CHECK(s.resolve_game(g2)["fullscreen"] == "true" && s.resolve_game(g1)["fullscreen"] == "false");
        // JSON round trip keeps the switch2 entry and its stamp
        const std::string js = s.to_json();
        CHECK(js.find("\"switch2\"") != std::string::npos);
        Store back;
        CHECK(back.from_json(js) && back.resolve_console("switch2", "")["fullscreen"] == "true" && back.resolve_console("switch", "")["fullscreen"] == "false");
        CHECK(back.to_json() == js);
        // clearing the only key drops Switch 2's own entry: it falls back to Switch's value
        set_stamp_clock(200);
        back.clear_console_key("switch2", "fullscreen");
        CHECK(back.resolve_console("switch2", "")["fullscreen"] == "false" && back.resolve_console("switch", "")["fullscreen"] == "false");
        set_stamp_clock(300);
        Store t;
        CHECK(t.from_json(js));
        t.reset_console("switch2");
        CHECK(t.resolve_console("switch2", "")["fullscreen"] == "false" && t.resolve_console("switch", "")["fullscreen"] == "false");
        CHECK(t.to_json().find("\"s:switch2\"") != std::string::npos || t.to_json().find("switch2") != std::string::npos);   // the stamped tombstone is kept
        // cloud merge into a vault file keeps "switch2" (the newer stamp wins the whole entry) and leaves "switch" to its own stamp
        std::error_code ec;
        fs::path v = fs::temp_directory_path() / "hub_gs_console_test";
        fs::remove_all(v, ec);
        const std::string vd = v.u8string();
        set_stamp_clock(100);
        Store disk;
        CHECK(disk.from_json(js) && disk.save(vd));
        set_stamp_clock(400);
        Store cloud;
        CHECK(cloud.set_console("switch2", "volume", "40") && cloud.set_console("switch", "fullscreen", "true"));
        CHECK(merge_cloud_into_vault(vd, cloud.to_json()));
        Store merged;
        CHECK(merged.load(vd));
        CHECK(merged.resolve_console("switch2", "")["volume"] == "40" && merged.has_console_value("switch2", "volume"));
        CHECK(merged.resolve_console("switch", "")["fullscreen"] == "true");
        // a tombstone from another PC removes the Switch 2 entry here, and the file still loads
        set_stamp_clock(500);
        Store reset_pc;
        CHECK(reset_pc.from_json(merged.to_json()));
        reset_pc.reset_console("switch2");
        CHECK(merge_cloud_into_vault(vd, reset_pc.to_json()));
        Store after;
        CHECK(after.load(vd) && !after.has_console_value("switch2", "volume") && after.resolve_console("switch", "")["fullscreen"] == "true");
        Store again;
        CHECK(again.from_json(after.to_json()) && again.to_json() == after.to_json());
        // an old file with no switch2 entry loads and Switch 2 inherits
        Store legacy_sw;
        CHECK(legacy_sw.from_json("{\"systems\":{\"switch\":{\"fullscreen\":\"false\"}}}") && legacy_sw.resolve_console("switch2", "")["fullscreen"] == "false");
        set_stamp_clock(0);
        fs::remove_all(v, ec);
    }

    std::printf(g_fail ? "FAILED (%d)\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
