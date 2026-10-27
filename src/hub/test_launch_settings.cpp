// Console test for the launch wiring helpers (launch_settings.h).
#include "launch_settings.h"
#include <cstdio>
using namespace hub; using namespace hub::gs;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
int main(int argc, char** argv) {
    if (argc > 1) {   // --emit-n64 | --emit-gb : print the extra args for a game with a few overrides (used by the headless run)
        Store e; bool n = std::string(argv[1]) == "--emit-n64";
        System sy = n ? System::N64 : System::GB;
        if (n) { e.set_game("k", sy, "n64.res", "2"); e.set_game("k", sy, "n64.aa", "4"); }
        else { e.set_game("k", sy, "scale.filter", "nearest"); e.set_game("k", sy, "scale.integer", "false"); }
        Values v = hubui::effective_values(e, sy, "k", false);
        std::printf("%s\n", (n ? hubui::n64_extra_args(v) : hubui::retro_extra_args(sy, v)).c_str());
        return 0;
    }
    CHECK(hubui::cmd_quote("a=b") == "a=b");
    CHECK(hubui::cmd_quote("a b") == "\"a b\"");
    CHECK(hubui::cmd_quote("C:\\x y\\") == "\"C:\\x y\\\\\"");
    Store st;
    // global fullscreen is the default; system/game override it
    CHECK(is_fullscreen(hubui::effective_values(st, System::N64, "g", true)));
    CHECK(!is_fullscreen(hubui::effective_values(st, System::N64, "g", false)));
    st.set_system(System::GB, "fullscreen", "false");
    CHECK(!is_fullscreen(hubui::effective_values(st, System::GB, "g", true)));
    CHECK(is_fullscreen(hubui::effective_values(st, System::GBA, "g", true)));
    st.set_game("g", System::GB, "fullscreen", "true");
    CHECK(is_fullscreen(hubui::effective_values(st, System::GB, "g", false)));
    // arg builders
    st.set_game("n", System::N64, "n64.res", "2");
    Values v = hubui::effective_values(st, System::N64, "n", true);
    std::string a = hubui::n64_extra_args(v);
    CHECK(a.find(" --gfx UseNativeResolutionFactor=i:2") != std::string::npos);
    CHECK(a.find("AspectRatio=i:1") != std::string::npos);
    CHECK(a.size() >= 13 && a.substr(a.size() - 13) == " --fullscreen");
    st.set_game("z", System::GB, "scale.integer", "false");
    Values gb = hubui::effective_values(st, System::GB, "z", false);
    std::string r = hubui::retro_extra_args(System::GB, gb);
    CHECK(r.find("--fullscreen") == std::string::npos);
    CHECK(r.find("--integer-scale 0") != std::string::npos);
    // online: host sends sync values, guest takes them from the launch body, locked -> default
    st.set_game("p", System::GBA, "gba.skip_bios", "true");
    Values host = hubui::effective_values(st, System::GBA, "p", false);
    std::string frag = hubui::host_launch_fragment(host);
    CHECK(frag.find("gba.skip_bios") != std::string::npos);
    std::string body = "{\"kind\":\"launch\"" + frag + "}";
    Values guest_local = hubui::effective_values(Store(), System::GBA, "p", false);
    CHECK(hubui::online_values(System::GBA, guest_local, false, body)["gba.skip_bios"] == "true");
    CHECK(hubui::online_values(System::GBA, guest_local, false, "{\"kind\":\"launch\"}")["gba.skip_bios"] == "false");   // older host
    CHECK(hubui::online_values(System::GBA, host, true, "")["gba.skip_bios"] == "true");
    CHECK(hubui::host_launch_fragment(hubui::effective_values(Store(), System::N64, "x", false)).empty());
    // locked option cannot be forced in online
    Store lk; lk.set_game("l", System::N64, "n64.fb_emulation", "off");
    Values lv = hubui::effective_values(lk, System::N64, "l", false);
    Values lo = hubui::online_values(System::N64, lv, true, "");
    for (auto& o : options()) if (o.scope == Scope::Locked && o.key.rfind("n64.", 0) == 0) CHECK(lo[o.key] == o.def);
    // volume: global default (100 = no argument), system / game override, clamped
    {
        Store v;
        CHECK(hub::gs::volume_of(hubui::effective_values(v, System::GB, "g", false)) == 100);
        CHECK(hubui::retro_extra_args(System::GB, hubui::effective_values(v, System::GB, "g", false)).find("--volume") == std::string::npos);
        Values g40 = hubui::effective_values(v, System::GB, "g", false, 40);
        CHECK(hub::gs::volume_of(g40) == 40 && hubui::retro_extra_args(System::GB, g40).find("--volume 40") != std::string::npos);
        CHECK(hubui::n64_extra_args(hubui::effective_values(v, System::N64, "g", false, 0)).find("--volume 0") != std::string::npos);
        v.set_game("g", System::GB, "volume", "70");
        CHECK(hub::gs::volume_of(hubui::effective_values(v, System::GB, "g", false, 40)) == 70);   // per-game beats the global default
        CHECK(!v.set_game("g", System::GB, "volume", "101") && !v.set_game("g", System::GB, "volume", "loud"));
        CHECK(hub::gs::volume_of(Values{{"volume", "999"}}) == 100 && hub::gs::volume_of(Values{{"volume", "-5"}}) == 0 && hub::gs::volume_of(Values()) == 100);
    }
    std::printf(g_fail ? "FAILED\n" : "launch settings OK\n");
    return g_fail != 0;
}
