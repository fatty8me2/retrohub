// Console test for the quick menu's pure speed / volume logic (qm_logic.h).
#include "qm_logic.h"
#include "qm_card.h"
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
    {   // Dolphin pause / save / load chords: pairwise non-nested with each other and with the two speed chords (a chord fires every hotkey whose keys it contains)
        std::vector<std::vector<int>> all = {dolphin_speed_scans(DolphinKey::Increase), dolphin_speed_scans(DolphinKey::Decrease)};
        for (DolphinAct a : dolphin_acts()) all.push_back(dolphin_act_scans(a));
        auto subset = [](const std::vector<int>& x, const std::vector<int>& y) { for (int k : x) if (std::find(y.begin(), y.end(), k) == y.end()) return false; return true; };
        for (size_t i = 0; i < all.size(); ++i) for (size_t j = 0; j < all.size(); ++j) if (i != j) CHECK(!subset(all[i], all[j]));
        for (auto& c : all) for (int sc : c) CHECK(sc == kDolphinScanCtrl || sc == kDolphinScanShift || sc == 0x64 || sc == 0x65 || sc == 0x66);   // only DIK_F13..F15 and the modifiers
        CHECK(std::string(dolphin_act_hotkey_name(DolphinAct::Pause)) == "General/Toggle Pause" && std::string(dolphin_act_hotkey_expr(DolphinAct::Pause)) == "@(Ctrl+Shift+F15)");
        CHECK(std::string(dolphin_act_hotkey_name(DolphinAct::SaveState)) == "Save State/Save State Slot 1" && std::string(dolphin_act_hotkey_expr(DolphinAct::SaveState)) == "@(Ctrl+F14+F15)");
        CHECK(std::string(dolphin_act_hotkey_name(DolphinAct::LoadState)) == "Load State/Load State Slot 1" && std::string(dolphin_act_hotkey_expr(DolphinAct::LoadState)) == "@(Shift+F14+F15)");
        CHECK(dolphin_is_managed_hotkey("General/Toggle Pause") && dolphin_is_managed_hotkey("Emulation Speed/Increase Emulation Speed") && !dolphin_is_managed_hotkey("General/Stop"));
        // close plan: queued buttons go out; a paused game that is resumed gets one more Toggle Pause
        CHECK(dolphin_close_plan({}, false).acts.empty());
        CHECK(dolphin_close_plan({}, true).acts == std::vector<DolphinAct>{DolphinAct::Pause});
        CHECK(dolphin_close_plan({DolphinAct::Pause}, false).acts == std::vector<DolphinAct>{DolphinAct::Pause});
        CHECK((dolphin_close_plan({DolphinAct::SaveState}, true).acts == std::vector<DolphinAct>{DolphinAct::SaveState, DolphinAct::Pause}));
        CHECK(dolphin_close_plan({DolphinAct::SaveState}, false).acts == std::vector<DolphinAct>{DolphinAct::SaveState});
        // GameCube NetPlay lookup: A.proc wins; otherwise the NetPlay Dolphin; online even though A.run.online is unset
        CHECK(gc_menu_pid(true, 1234) == 0 && gc_menu_pid(false, 1234) == 1234 && gc_menu_pid(false, 0) == 0);
        CHECK(online_for_menu(false, 1234) && online_for_menu(true, 0) && !online_for_menu(false, 0));
    }
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
    {   // How do I play: callouts from the live profile
        auto c = hubqm::pad_callouts(hubctl::default_profile("n64"));
        CHECK(!c.empty() && c.count("ls") && c["ls"][0] == "Analog stick");
        CHECK(c.count("a") || c.count("b"));
        hubctl::Profile p = hubctl::default_profile("snes");
        p.buttons["start"] = "rightshoulder|lefttrigger";
        auto d = hubqm::pad_callouts(p);
        CHECK(d.count("rb") && d["rb"].back() == "Start" && d.count("lt") && d["lt"].back() == "Start");
        CHECK(hubqm::join_labels({"A", "B", "C", "D"}) == "A / B / C +1");
        hubctl::Profile q = hubctl::default_profile("n64");
        q.buttons["a"] = "bogus";
        CHECK(!hubqm::pad_callouts(q).count("bogus"));
    }
    {   // in-game chat OSD
        using namespace hubqm;
        CHECK(!chat_osd_wanted(kChatOff, false) && !chat_osd_wanted(kChatOff, true));
        CHECK(chat_osd_wanted(kChatRoom, false) && !chat_osd_wanted(kChatRoom, true));
        CHECK(chat_osd_wanted(kChatAll, true) && chat_osd_wanted(kChatAll, false));
        CHECK(chat_osd_text("Mia", "gg  wp") == "Mia: gg wp");
        CHECK(chat_osd_text("", "caf\xc3\xa9") == "caf?");
        CHECK(chat_osd_text("A", std::string(300, 'x')).size() == 118);
        CHECK(chat_osd_text("", "\xf0\x9f\x98\x80").empty() && chat_osd_text("", "   ").empty());
        ChatOsdLimiter l;
        int ok = 0;
        for (int i = 0; i < 10; ++i) ok += l.allow(1000 + i);
        CHECK(ok == 5 && !l.allow(2000) && l.allow(8000));
        CHECK(quick_phrases().size() == 4 && quick_phrases()[0] == "GG");
    }
    {   // Plan 4: pause row, pass controller, emotes, command pacing
        using namespace hubqm;
        CHECK(auto_pause_wanted(true, true, true, false));
        CHECK(!auto_pause_wanted(false, true, true, false));    // setting off
        CHECK(!auto_pause_wanted(true, true, true, true));      // never online
        CHECK(!auto_pause_wanted(true, false, true, false));    // menu closed
        CHECK(!auto_pause_wanted(true, true, false, false));    // no control channel
        CHECK(!pause_available(true, true) && pause_available(true, false) && !pause_available(false, false));
        CHECK(port_of_slot("0,1,2", 1) == 1 && port_of_slot("0,0,1,-1", 1) == 2 && port_of_slot("3,-1", 0) == -1 && port_of_slot("", 0) == -1);
        CHECK(port_of_slot("2,0,1", 0) == 1 && port_of_slot("0,1", 11) == -1 && port_of_slot("1,10", 1) == 0);
        CHECK(countdown_text("Mickey", 1) == "Passing to Mickey in 3");
        CHECK(countdown_text("Mickey", 2) == "Passing to Mickey in 3..2");
        CHECK(countdown_text("Mickey", 3) == "Passing to Mickey in 3..2..1" && countdown_text("M", 9) == "Passing to M in 3..2..1");
        CHECK(handoff_fail_text("unsupported") == kHandoffUnsupportedText && handoff_fail_text("") == kHandoffUnsupportedText);
        CHECK(std::string(kHandoffUnsupportedText) == "Passing the controller works in NES and SNES online games for now");
        CHECK(handoff_fail_text("busy") == "busy");
        CHECK(emotes().size() == 5 && std::string(emotes()[0].label) == "GG" && std::string(emotes()[4].label) == "ONE MORE?");
        CHECK(find_emote("gg") && find_emote("more") && !find_emote("GG") && !find_emote("") && !find_emote("gg\"}"));
        CHECK(emote_json("nice") == "{\"kind\":\"emote\",\"e\":\"nice\"}");
        EmoteLimiter el;   // one per 2 s
        CHECK(el.allow(100000) && !el.allow(100001) && !el.allow(101999) && el.allow(102000) && !el.allow(103000));
        EmoteInLimiter il;
        CHECK(il.allow(1, 5000) && !il.allow(1, 5500) && il.allow(2, 5500) && il.allow(1, 6000) && !il.allow(-1, 0) && !il.allow(8, 0));
        CmdPacer p;   // one command at a time
        CHECK(p.may_send(0));
        p.sent(5, 1000);
        CHECK(!p.may_send(1100));
        p.ack(4); CHECK(!p.may_send(1100));
        p.ack(5); CHECK(p.may_send(1100));
        p.sent(6, 2000); CHECK(!p.may_send(2500) && p.may_send(2700));   // a runner that never acks: go on after kMaxWaitMs
    }
    {   // extra rows: Rewind / Screen filter / Save a clip visibility
        const QmFeature R = QmFeature::Rewind, F = QmFeature::Filter, C = QmFeature::Clip;
        // (feature, has_ctl, online, is_n64, unsupported)
        CHECK(feature_row_visible(R, true, false, false, false) && feature_row_visible(F, true, false, false, false) && feature_row_visible(C, true, false, false, false));
        CHECK(!feature_row_visible(R, true, true, false, false) && !feature_row_visible(F, true, true, false, false));   // offline only
        CHECK(feature_row_visible(C, true, true, false, false) && feature_row_visible(C, true, true, true, false));      // clips are fine online, and on n64run
        CHECK(!feature_row_visible(R, true, false, true, false) && !feature_row_visible(F, true, false, true, false));    // n64run: unsupported
        CHECK(!feature_row_visible(R, false, false, false, false) && !feature_row_visible(F, false, false, false, false) && !feature_row_visible(C, false, false, false, false));   // external emulators
        CHECK(!feature_row_visible(R, true, false, false, true) && !feature_row_visible(C, true, false, false, true));   // runner said unsupported
        CHECK(feature_cmd(R) == runctl::Cmd::Rewind && feature_cmd(F) == runctl::Cmd::Shader && feature_cmd(C) == runctl::Cmd::Clip);
        CHECK(ack_means_unsupported("unsupported") && ack_means_unsupported("Unsupported") && ack_means_unsupported("unknown command") && ack_means_unsupported("Rewind is not available for this game"));
        CHECK(!ack_means_unsupported("") && !ack_means_unsupported("Hardcore: rewind is off") && !ack_means_unsupported("Instant replay is off. Turn it on in RetroHub Settings.") && !ack_means_unsupported("Not available in online sessions"));
        FeatureHide h;
        CHECK(!h.is(R) && !h.is(F) && !h.is(C));
        CHECK(!h.on_fail(runctl::Cmd::Clip, "Not available in online sessions") && !h.is(C));
        CHECK(std::string(kRewindHint).find("LT + View") != std::string::npos);   // the hint names the real retrorun hold (no Rewind row)
        CHECK(h.on_fail(runctl::Cmd::Rewind, "Rewind is not available for this game") && h.is(R) && !h.is(F) && !h.is(C));
        CHECK(!h.on_fail(runctl::Cmd::Speed, "unsupported"));   // not a row of ours
        CHECK(h.on_fail(runctl::Cmd::Clip, "unsupported") && h.is(C));
        h.reset();
        CHECK(!h.is(R) && !h.is(C));
        // the commands survive the control file round trip with the arguments the rows send
        runctl::Command q;
        CHECK(runctl::parse_control(runctl::make_control([] { runctl::Command x; x.seq = 1; x.cmd = runctl::Cmd::Shader; x.text = "next"; return x; }()), &q) && q.cmd == runctl::Cmd::Shader && q.text == "next");
        CHECK(runctl::parse_control(runctl::make_control([] { runctl::Command x; x.seq = 2; x.cmd = runctl::Cmd::Rewind; x.value = 1; return x; }()), &q) && q.cmd == runctl::Cmd::Rewind && q.value == 1);
        CHECK(runctl::parse_control(runctl::make_control([] { runctl::Command x; x.seq = 3; x.cmd = runctl::Cmd::Clip; return x; }()), &q) && q.cmd == runctl::Cmd::Clip);
    }
    {   // save page: replacing a used slot takes two presses
        using Res = OverwriteConfirm::Result;
        OverwriteConfirm o;
        CHECK(o.press(3, false, 1000, "slot3") == Res::Save && !o.is_armed(3));            // empty: one press
        CHECK(o.press(3, true, 1000, "slot3") == Res::Armed && o.is_armed(3));             // used: first press arms
        CHECK(o.press(3, true, 1500, "slot3") == Res::Save && !o.is_armed(3));             // second press saves
        CHECK(o.press(3, true, 2000, "slot3") == Res::Armed);                              // and it is armed again afterwards
        o.tick("slot3", 2100); CHECK(o.is_armed(3));                                       // same selection: stays
        o.tick("slot4", 2200); CHECK(!o.is_armed(3));                                      // navigation cancels
        CHECK(o.press(3, true, 3000, "slot3") == Res::Armed && o.press(3, true, 3000 + OverwriteConfirm::kExpiryMs + 1, "slot3") == Res::Armed);   // too slow: arms again, does not save
        o.cancel(); CHECK(!o.is_armed(3));
        CHECK(o.press(3, true, 9000, "slot3") == Res::Armed && o.press(4, true, 9100, "slot3") == Res::Armed && !o.is_armed(3) && o.is_armed(4));   // another slot: arms that one instead
        CHECK(o.press(3, true, 9200, "slot3") == Res::Armed);                              // and 3 is not confirmed by the old arm
        CHECK(o.press(5, false, 9300, "slot5") == Res::Save && !o.is_armed(3));            // an empty slot saves at once and clears the arm
        CHECK(o.press(-1, true, 9400, "slot10") == Res::Armed && o.is_armed(-1));          // the resume point is a slot too (never -2)
        o.tick("slot10", 9400 + OverwriteConfirm::kExpiryMs + 1); CHECK(!o.is_armed(-1));  // expiry on its own
    }
    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail ? 1 : 0;
}
