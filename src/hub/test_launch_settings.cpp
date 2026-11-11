// Console test for the launch wiring helpers (launch_settings.h).
#include "launch_settings.h"
#include "launch_guard_logic.h"
#include "capture_hold_logic.h"
#include "ctl_pad_logic.h"
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
    // shell-H2: no launch while ANY session is up (GC NetPlay / 3DS room never set A.proc)
    CHECK(hubui::launch_allowed(false, false));
    CHECK(!hubui::launch_allowed(true, true) && !hubui::launch_allowed(true, false) && !hubui::launch_allowed(false, true));
    // a room's own launch inside its session (3DS Azahar after NP3 is up, Wii U Cemu after RP.proc) must pass; a user launch must not
    CHECK(hubui::launch_allowed(false, true, true) && !hubui::launch_allowed(false, true, false));
    CHECK(!hubui::launch_allowed(true, true, true) && !hubui::launch_allowed(true, false, true));   // a running A.proc still refuses everything
    CHECK(std::string(hubui::launch_busy_reason(true)) == "A game is already running." && std::string(hubui::launch_busy_reason(false)) == "An online game is running.");
    CHECK(hubui::cheats_return_focus(true, true) && !hubui::cheats_return_focus(true, false) && !hubui::cheats_return_focus(false, true));
    // shell-M6: capture dialog press versus hold
    {
        hubui::CaptureHold h;
        CHECK(hubui::capture_hold_step(h, "", false, 0) == hubui::CaptureStep::Wait);
        CHECK(hubui::capture_hold_step(h, "x", true, 0) == hubui::CaptureStep::Commit && h.tok == "x");   // other buttons: at once
        hubui::CaptureHold b;
        CHECK(hubui::capture_hold_step(b, "b", true, 1.0) == hubui::CaptureStep::Wait && b.pending);
        CHECK(hubui::capture_hold_step(b, "", true, 1.5) == hubui::CaptureStep::Wait);
        CHECK(hubui::capture_hold_step(b, "", false, 1.6) == hubui::CaptureStep::Commit && b.tok == "b" && !b.pending);   // tap: assigned
        hubui::CaptureHold k;
        hubui::capture_hold_step(k, "back", true, 2.0);
        CHECK(hubui::capture_hold_step(k, "", true, 2.9) == hubui::CaptureStep::Wait);
        CHECK(hubui::capture_hold_step(k, "", true, 3.0) == hubui::CaptureStep::Cancel && !k.pending);   // hold: cancel
    }
    {   // play.rewind reaches the retrorun command line only when it is off
        Store rw;
        rw.set_game("r", System::SNES, "play.rewind", "off");
        CHECK(hubui::retro_extra_args(System::SNES, hubui::effective_values(rw, System::SNES, "r", false)).find("--rewind off") != std::string::npos);
        CHECK(hubui::retro_extra_args(System::SNES, hubui::effective_values(rw, System::SNES, "other", false)).find("--rewind") == std::string::npos);
    }
    {   // a missing game file is asked about before the cloud-save gate; a stale gate pass is dropped
        using hubui::PlayFirst;
        CHECK(hubui::play_first_step(false, true, false).step == PlayFirst::AskMissing && !hubui::play_first_step(false, true, false).clear_pass);
        CHECK(hubui::play_first_step(false, true, true).step == PlayFirst::AskMissing && hubui::play_first_step(false, true, true).clear_pass);   // the file vanished while the gate ran
        CHECK(hubui::play_first_step(false, false, true).step == PlayFirst::Continue && !hubui::play_first_step(false, false, true).clear_pass);   // cloud_gate_begin consumes the pass itself
        CHECK(hubui::play_first_step(false, false, false).step == PlayFirst::Continue);
        CHECK(hubui::play_first_step(true, true, false).step == PlayFirst::GateBusy && hubui::play_first_step(true, false, true).step == PlayFirst::GateBusy);
    }
    {   // Undo card: Y acts only while the card shows, in pad mode, with the hub in front and no typing
        hubui::UndoPadIn u; u.card_visible = u.pad_mode = u.y_pressed = true;
        CHECK(hubui::undo_card_pad_press(u));
        hubui::UndoPadIn v = u; v.card_visible = false; CHECK(!hubui::undo_card_pad_press(v));   // no card: Y stays with the screen underneath
        v = u; v.pad_mode = false; CHECK(!hubui::undo_card_pad_press(v));
        v = u; v.input_blocked = true; CHECK(!hubui::undo_card_pad_press(v));
        v = u; v.text_input = true; CHECK(!hubui::undo_card_pad_press(v));
        v = u; v.y_pressed = false; CHECK(!hubui::undo_card_pad_press(v));
    }
    {   // Assign players: A edges, never a held A, never an activating press; B / Esc end it
        hubui::AssignAGate g;
        g.prime("p1", true);                                   // A held while the mode opened
        CHECK(!g.a_press("p1", true, false));                  // still held: no claim
        CHECK(!g.a_press("p1", false, false));
        CHECK(g.a_press("p1", true, false));                   // fresh press claims
        CHECK(!g.a_press("p1", true, false));                  // held: once only
        CHECK(!g.a_press("p1", false, false));
        CHECK(!g.a_press("p1", true, true));                   // that press activates a focused button: no claim
        CHECK(!g.a_press("p1", true, false));                  // and it is not a delayed claim once the flag drops
        CHECK(!g.a_press("p2", true, false));                  // a pad never primed: primed, not claimed
        CHECK(!g.a_press("p2", true, false) && !g.a_press("p2", false, false) && g.a_press("p2", true, false));
        g.forget("p2"); CHECK(!g.known("p2")); g.clear(); CHECK(!g.known("p1"));
        CHECK(hubui::assign_end_requested(true, false) && hubui::assign_end_requested(false, true));
        CHECK(!hubui::assign_end_requested(false, false));
    }
    std::printf(g_fail ? "FAILED\n" : "launch settings OK\n");
    return g_fail != 0;
}
