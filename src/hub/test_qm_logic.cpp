// Console test for the quick menu's pure speed / volume logic (qm_logic.h).
#include "qm_logic.h"
#include <cstdio>
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
int main() {
    {   // chord: abort between keys still releases what was pressed
        std::vector<int> ev; int calls = 0;
        int n = hubqm::run_key_chord({10, 20}, [&] { return ++calls <= 1; }, [&](int k, bool up) { ev.push_back(up ? -k : k); });
        CHECK(n == 1 && ev.size() == 2 && ev[0] == 10 && ev[1] == -10);
        ev.clear();
        CHECK(hubqm::run_key_chord({10, 20}, [] { return false; }, [&](int k, bool up) { ev.push_back(up ? -k : k); }) == 0 && ev.empty());
        CHECK(hubqm::run_key_chord({10, 20}, [] { return true; }, [&](int k, bool up) { ev.push_back(up ? -k : k); }) == 2 && ev.size() == 4 && ev[2] == -20 && ev[3] == -10);
    }
    {   // input gate
        runctl::InputGate g;
        CHECK(!g.suppressed(0));
        g.set(false, 1000);
        CHECK(g.suppressed(2000));
        g.set(false, 25000);                                  // refresh
        CHECK(g.suppressed(40000));
        CHECK(!g.suppressed(25000 + runctl::kInputOffTimeoutMs + 1) && !g.off);   // hub died: failsafe
        g.set(false, 100000); g.note_view_y(true, 100001);    // the opening press seen after "input off": stays off
        CHECK(g.off);
        g.note_view_y(false, 100004); g.note_view_y(true, 100005);   // View+Y again while the menu is open: still off
        CHECK(g.off);
        g.set(true, 1); CHECK(!g.suppressed(2));
        runctl::Command c; CHECK(runctl::parse_control(runctl::make_control([] { runctl::Command x; x.seq = 3; x.cmd = runctl::Cmd::Input; x.value = 0; return x; }()), &c) && c.cmd == runctl::Cmd::Input && c.value == 0);
        CHECK(!runctl::offline_only(runctl::Cmd::Input));
    }
    {   // key burst: abort on foreground change, modifiers always released
        std::vector<int> ev, keys{1, 2, 3, 4};
        int calls = 0;
        auto send = [&](int s, bool up) { ev.push_back(up ? -s : s); };
        int n = hubqm::run_key_burst(keys, 100, 101, [&] { return ++calls <= 4; }, send);   // fg lost mid-burst
        CHECK(n < 4);
        int dn = 0, upc = 0;
        for (int v : ev) { if (v > 0) ++dn; else ++upc; }
        CHECK(dn == upc && ev.size() >= 2 && ev[ev.size() - 1] == -100 && ev[ev.size() - 2] == -101);
        ev.clear();
        CHECK(hubqm::run_key_burst(keys, 100, 101, [] { return false; }, send) == 0 && ev.empty());   // never sends when not foreground
        ev.clear();
        CHECK(hubqm::run_key_burst(keys, 100, 101, [] { return true; }, send) == 4 && ev.size() == 12);
    }
    using namespace hubqm;
    CHECK(speed_text(0) == "0.25x" && speed_text(1) == "0.5x" && speed_text(2) == "1x" && speed_text(3) == "1.5x");
    CHECK(speed_text(4) == "2x" && speed_text(5) == "3x" && speed_text(6) == "5x" && speed_text(7) == "8x" && speed_text(8) == "Max");
    CHECK(speed_value(kSpeedNormal) == 1.0 && speed_value(8) == 0.0 && speed_value(99) == 0.0 && speed_value(-3) == 0.25);
    for (int i = 0; i < kSpeedSteps; ++i) CHECK(runctl::speed_valid(speed_value(i)));
    CHECK(speed_step(0, -1) == 0 && speed_step(8, 1) == 8 && speed_step(2, 1) == 3 && speed_step(2, -1) == 1);
    CHECK(volume_step(0, -1) == 0 && volume_step(100, 1) == 100 && volume_step(50, 1) == 55 && volume_step(2, -1) == 0);
    CHECK(ext_session_scalar(100, 100) == 1.f && ext_session_scalar(50, 100) == 0.5f && ext_session_scalar(0, 100) == 0.f);
    CHECK(ext_session_scalar(80, 40) == 1.f && ext_session_scalar(20, 40) == 0.5f && ext_session_scalar(10, 0) == 1.f && ext_session_scalar(0, 0) == 0.f);
    CHECK(speed_available(true, false) && !speed_available(true, true) && !speed_available(false, false));
    // volume / mute are local: allowed online; speed is not
    CHECK(!runctl::offline_only(runctl::Cmd::Volume) && !runctl::offline_only(runctl::Cmd::Mute) && runctl::offline_only(runctl::Cmd::Speed));
    // ---- Dolphin speed: 0.1 hotkey steps from 1x, no Max (Dolphin's speed-limit hotkey is hold-only)
    CHECK(dolphin_speed_text(0) == "0.3x" && dolphin_speed_text(1) == "0.5x" && dolphin_speed_text(2) == "1x" && dolphin_speed_text(3) == "1.5x");
    CHECK(dolphin_speed_text(4) == "2x" && dolphin_speed_text(5) == "3x" && dolphin_speed_text(6) == "5x" && dolphin_speed_text(7) == "8x");
    CHECK(dolphin_speed_step(7, 1) == 7 && dolphin_speed_step(0, -1) == 0 && dolphin_speed_step(2, 1) == 3);
    {
        auto count = [](const std::vector<DolphinKey>& v, DolphinKey k) { int n = 0; for (auto x : v) n += x == k; return n; };
        DolphinSpeed s;
        CHECK(dolphin_plan(s, 2).empty() && dolphin_idx(s) == 2);
        auto p = dolphin_plan(s, 3);   // 1x -> 1.5x: 5 increases
        CHECK(p.size() == 5 && count(p, DolphinKey::Increase) == 5 && dolphin_idx(s) == 3);
        p = dolphin_plan(s, 7);        // 1.5x -> 8x: 65 increases
        CHECK(p.size() == 65 && count(p, DolphinKey::Increase) == 65 && dolphin_idx(s) == 7);
        p = dolphin_plan(s, 1);        // 8x -> 0.5x: 75 decreases
        CHECK(p.size() == 75 && count(p, DolphinKey::Decrease) == 75 && dolphin_idx(s) == 1);
        p = dolphin_plan(s, 0);        // 0.5x -> 0.3x
        CHECK(p.size() == 2 && count(p, DolphinKey::Decrease) == 2);
        p = dolphin_plan(s, 2);        // 0.3x -> 1x: 7 increases (never below Dolphin's 0.1 floor on the way)
        CHECK(p.size() == 7 && count(p, DolphinKey::Increase) == 7);
        CHECK(dolphin_plan(s, 99).size() == 70 && dolphin_idx(s) == 7);   // out-of-range clamps to 8x
    }
    CHECK(dolphin_vk(DolphinKey::Increase) == 0x7C && dolphin_vk(DolphinKey::Decrease) == 0x7D);
    CHECK(dolphin_scan(0x7C) == 0x64 && dolphin_scan(0x7D) == 0x65);   // DIK_F13 / DIK_F14
    CHECK(std::string(dolphin_hotkey_name(DolphinKey::Increase)) == "Emulation Speed/Increase Emulation Speed" && std::string(dolphin_hotkey_name(DolphinKey::Decrease)) == "Emulation Speed/Decrease Emulation Speed");
    CHECK(std::string(dolphin_hotkey_expr(DolphinKey::Increase)) == "@(Ctrl+Shift+F13)" && std::string(dolphin_hotkey_expr(DolphinKey::Decrease)) == "@(Ctrl+Shift+F14)");
    CHECK(speed_row(true, false, false, false) == SpeedRow::Enabled && speed_row(true, false, true, false) == SpeedRow::Online);
    CHECK(speed_row(false, true, false, true) == SpeedRow::Enabled && speed_row(false, true, true, true) == SpeedRow::Online);
    CHECK(speed_row(false, false, false, true) == SpeedRow::Unsupported && speed_row(false, false, true, true) == SpeedRow::Unsupported);
    CHECK(speed_row(false, false, false, false) == SpeedRow::Hidden);
    {   // OpenGate: one open per press, never while open, cooldown after open/close
        OpenGate g;
        CHECK(!g.update(false, false, 1000));
        CHECK(g.update(true, false, 2000));                 // rising edge opens
        CHECK(!g.update(true, false, 2016));                // held: no second open
        CHECK(!g.update(true, true, 2050));                 // menu is now open
        CHECK(!g.update(false, true, 2100) && !g.update(true, true, 2200));   // pressing again while open does not toggle
        CHECK(!g.update(false, true, 3000));
        CHECK(!g.update(false, false, 3100));               // menu closed at 3100
        CHECK(!g.update(true, false, 3300));                // bounce inside the 700 ms cooldown: ignored
        CHECK(!g.update(false, false, 3400));
        CHECK(g.update(true, false, 3900));                 // released, cooled down: opens again
        // a combo still held through a close must be released first
        OpenGate h;
        CHECK(h.update(true, false, 5000));
        CHECK(!h.update(true, true, 5010));
        CHECK(!h.update(true, false, 9000));                // closed with the combo still down: no re-open without a new edge
    }
    CHECK(!back_allowed(1000, 1200) && back_allowed(1000, 1400));
    {   // HoldGate: View+LB for 1 s fires once per hold
        HoldGate h;
        CHECK(!h.update(true, 0) && !h.update(true, 999) && h.update(true, 1000));
        CHECK(!h.update(true, 2500));                       // still held: not again
        CHECK(!h.update(false, 2600) && !h.update(true, 2700) && h.update(true, 3700));
    }
    {   // FocusRetry: bounded and spaced
        FocusRetry f;
        CHECK(!f.should_retry(true, 0));
        CHECK(f.should_retry(false, 1000) && !f.should_retry(false, 1200) && f.should_retry(false, 1600));
        int n = 2;
        for (long long t = 3000; t < 20000; t += 600) n += f.should_retry(false, t);
        CHECK(n == 6);                                      // never more than 6 tries per open
        f.reset();
        CHECK(f.should_retry(false, 30000));
    }
    {   // Dolphin speed: committed from presses actually done, never assumed
        DolphinSpeed s;
        CHECK(dolphin_needs_apply(s, 4) && !dolphin_needs_apply(s, 2));
        auto p = dolphin_plan_for(s, 4);                    // 1x -> 2x: 10 increases, nothing mutated
        CHECK(p.size() == 10 && s.cur == 2 && s.tenths == 10 && s.known);
        dolphin_commit(s, 4, p, 10);
        CHECK(s.cur == 4 && s.tenths == 20 && s.known && !dolphin_needs_apply(s, 4));
        p = dolphin_plan_for(s, 2);                         // back to 1x: 10 decreases
        CHECK(p.size() == 10 && p[0] == DolphinKey::Decrease);
        dolphin_commit(s, 2, p, 4);                         // cut short: state unknown
        CHECK(!s.known && s.cur == 4 && dolphin_needs_apply(s, 2) && dolphin_needs_apply(s, 4));
        p = dolphin_plan_for(s, 2);                         // unknown: floor (90 decreases) then up to 1x (9 increases)
        CHECK((int)p.size() == kDolphinFloorPresses + 9 && p.front() == DolphinKey::Decrease && p.back() == DolphinKey::Increase);
        dolphin_commit(s, 2, p, (int)p.size());
        CHECK(s.known && s.tenths == 10 && s.cur == 2);
        CHECK(dolphin_retry_delay_ms(0) < dolphin_retry_delay_ms(1) && dolphin_retry_delay_ms(1) < dolphin_retry_delay_ms(2));
    }
    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail ? 1 : 0;
}
