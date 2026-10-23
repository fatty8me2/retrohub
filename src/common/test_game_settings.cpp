// Console unit test for game_settings (defaults, merge, serialization, online filtering, emitters).
#include "game_settings.h"

#include <cstdio>
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

    std::printf(g_fail ? "FAILED (%d)\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
