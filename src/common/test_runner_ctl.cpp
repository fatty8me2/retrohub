#include <chrono>
#include <cstdio>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <filesystem>

#include "runner_ctl.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using namespace runctl;
    Command c;
    CHECK(parse_control("{\"seq\":3,\"cmd\":\"save_state\",\"slot\":3}", &c) && c.seq == 3 && c.cmd == Cmd::SaveState && c.slot == 3);
    CHECK(parse_control("{\"seq\":1,\"cmd\":\"screenshot\"}", &c) && c.cmd == Cmd::Screenshot);
    CHECK(parse_control("{\"seq\":1,\"cmd\":\"load_state\",\"slot\":-1}", &c) && c.slot == kAutoSlot);
    CHECK(!parse_control("{\"seq\":1,\"cmd\":\"save_state\",\"slot\":10}", &c));
    CHECK(!parse_control("{\"seq\":1,\"cmd\":\"save_state\",\"slot\":-1}", &c));
    CHECK(!parse_control("{\"seq\":0,\"cmd\":\"quit\"}", &c));
    CHECK(!parse_control("{\"seq\":1,\"cmd\":\"format_c\"}", &c));
    CHECK(!parse_control("garbage", &c));
    Command r;
    CHECK(parse_control(make_control({7, Cmd::Quit, 0}), &r) && r.seq == 7 && r.cmd == Cmd::Quit);
    {   // OSD / audio / speed commands
        Command q;
        CHECK(parse_control("{\"seq\":1,\"cmd\":\"volume\",\"value\":40}", &q) && q.cmd == Cmd::Volume && q.value == 40);
        CHECK(!parse_control("{\"seq\":1,\"cmd\":\"volume\",\"value\":101}", &q));
        CHECK(parse_control("{\"seq\":1,\"cmd\":\"mute\",\"value\":1}", &q) && q.cmd == Cmd::Mute && q.value == 1);
        CHECK(parse_control("{\"seq\":1,\"cmd\":\"ff\",\"value\":0}", &q) && q.cmd == Cmd::Ff && q.value == 0);
        CHECK(!parse_control("{\"seq\":1,\"cmd\":\"ff\",\"value\":2}", &q));
        CHECK(parse_control("{\"seq\":1,\"cmd\":\"speed\",\"speed\":1.5}", &q) && q.speed == 1.5);
        CHECK(parse_control("{\"seq\":1,\"cmd\":\"speed\",\"speed\":\"max\"}", &q) && q.speed == 0);
        CHECK(!parse_control("{\"seq\":1,\"cmd\":\"speed\",\"speed\":4}", &q));
        CHECK(parse_control("{\"seq\":1,\"cmd\":\"osd_msg\",\"text\":\"Hello\"}", &q) && q.text == "Hello");
        CHECK(!parse_control("{\"seq\":1,\"cmd\":\"osd_msg\"}", &q));
        CHECK(parse_control("{\"seq\":1,\"cmd\":\"shader\",\"text\":\"crt\"}", &q) && q.cmd == Cmd::Shader);
        CHECK(parse_control("{\"seq\":1,\"cmd\":\"clip\"}", &q) && q.cmd == Cmd::Clip);
        CHECK(parse_control("{\"seq\":1,\"cmd\":\"overlay\",\"value\":1}", &q) && q.cmd == Cmd::Overlay);
        CHECK(parse_control("{\"seq\":1,\"cmd\":\"rewind\",\"value\":1}", &q) && q.cmd == Cmd::Rewind);
        double sp = -1;
        CHECK(parse_speed_arg("2x", &sp) && sp == 2 && parse_speed_arg("max", &sp) && sp == 0 && parse_speed_arg("0.25", &sp) && sp == 0.25);
        CHECK(!parse_speed_arg("4", &sp) && !parse_speed_arg("fast", &sp) && !parse_speed_arg("", &sp));
        CHECK(speed_label(2) == "Speed 2x" && speed_label(0.5) == "Speed 0.5x" && speed_label(0) == "Speed max");
        CHECK(!offline_only(Cmd::Volume) && !offline_only(Cmd::Mute) && offline_only(Cmd::Speed) && !offline_only(Cmd::Overlay) && offline_only(Cmd::Ff) && offline_only(Cmd::SaveState) && !offline_only(Cmd::OsdMsg) && !offline_only(Cmd::Quit));
        CHECK(!offline_only(Cmd::Clip));   // clips are cosmetic: allowed online
        Command w; w.cmd = Cmd::OsdMsg; w.text = "a \"q\""; w.seq = 9;
        CHECK(parse_control(make_control(w), &q) && q.text == w.text && q.seq == 9);
        w = Command(); w.cmd = Cmd::Speed; w.speed = 0.25; w.seq = 2;
        CHECK(parse_control(make_control(w), &q) && q.speed == 0.25);
    }
    {   // Plan 4: pause, corner text, controller hand-off
        Command q;
        CHECK(parse_control("{\"seq\":1,\"cmd\":\"pause\",\"value\":1}", &q) && q.cmd == Cmd::Pause && q.value == 1);
        CHECK(!parse_control("{\"seq\":1,\"cmd\":\"pause\",\"value\":2}", &q));
        CHECK(parse_control("{\"seq\":1,\"cmd\":\"osd_corner\",\"slot\":1,\"text\":\"1 Mia 6\"}", &q) && q.cmd == Cmd::OsdCorner && q.slot == 1 && q.text == "1 Mia 6");
        CHECK(parse_control("{\"seq\":1,\"cmd\":\"osd_corner\",\"slot\":0,\"text\":\"\"}", &q) && q.text.empty());   // empty text clears the corner
        CHECK(!parse_control("{\"seq\":1,\"cmd\":\"osd_corner\",\"slot\":4,\"text\":\"x\"}", &q));
        CHECK(!parse_control("{\"seq\":1,\"cmd\":\"osd_corner\",\"slot\":0,\"value\":5000,\"text\":\"x\"}", &q));
        CHECK(parse_control("{\"seq\":1,\"cmd\":\"handoff\",\"slot\":0,\"value\":2}", &q) && q.cmd == Cmd::Handoff && q.slot == 0 && q.value == 2);
        CHECK(!parse_control("{\"seq\":1,\"cmd\":\"handoff\",\"slot\":0,\"value\":5}", &q));
        CHECK(offline_only(Cmd::Pause) && !offline_only(Cmd::OsdCorner) && !offline_only(Cmd::Handoff));
        // live controls: reload_controls takes no arguments, round-trips, and is allowed online (the mapping is local)
        CHECK(parse_control("{\"seq\":1,\"cmd\":\"reload_controls\"}", &q) && q.cmd == Cmd::ReloadControls);
        CHECK(!offline_only(Cmd::ReloadControls) && std::string(cmd_name(Cmd::ReloadControls)) == "reload_controls" && cmd_from_name("reload_controls") == Cmd::ReloadControls);
        { Command r; r.cmd = Cmd::ReloadControls; r.seq = 7; CHECK(parse_control(make_control(r), &q) && q.cmd == Cmd::ReloadControls && q.seq == 7); }
        Command w; w.cmd = Cmd::OsdCorner; w.slot = 3; w.value = 10; w.text = "02:13"; w.seq = 4;
        CHECK(parse_control(make_control(w), &q) && q.cmd == Cmd::OsdCorner && q.slot == 3 && q.value == 10 && q.text == "02:13");
    }
    Ack a;
    CHECK(parse_ack(make_ack({5, true, "x \"y\""}), &a) && a.seq == 5 && a.ok && a.msg == "x \"y\"");
    CHECK(state_file("d/", 3) == "d/slot3.state" && state_file("d", kAutoSlot) == "d/auto.state");
    CHECK(thumb_file("d", 0) == "d/slot0.png" && sidecar_file("d", 9) == "d/slot9.json");
    CHECK(shots_dir("v/n64/k/") == "v/n64/k/shots");
    CHECK(shot_name(1000000000).size() == 19);
    Sidecar s;
    CHECK(parse_sidecar(make_sidecar({"k", 10, 20, 1}), &s) && s.game_key == "k" && s.time == 10 && s.playtime == 20 && s.disc == 1);

    namespace fs = std::filesystem;
    fs::path d = fs::temp_directory_path() / "runctl_test";
    fs::remove_all(d);
    std::string ctl = (d / "control.json").u8string();
    CHECK(ack_path_for(ctl) == (d / "control_ack.json").u8string());
    CHECK(send_command(ctl, Cmd::SaveState, 2) == 1);   // pre-existing at start-up: handled
    Poller p(ctl);
    CHECK(p.last_seq() == 1);
    CHECK(!p.poll(1000, &c));
    CHECK(send_command(ctl, Cmd::LoadState, 4) == 2);
    CHECK(!p.poll(1100, &c));   // inside the poll interval
    CHECK(p.poll(1300, &c) && c.seq == 2 && c.cmd == Cmd::LoadState && c.slot == 4);
    p.ack(true, "loaded");
    std::string t;
    CHECK(read_text(ack_path_for(ctl), &t) && parse_ack(t, &a) && a.seq == 2 && a.ok && a.msg == "loaded");
    CHECK(!p.poll(2000, &c));   // once per seq
    write_text_atomic(ctl, "{\"seq\":2,\"cmd\":\"quit\"}");   // same seq rewritten: ignored
    CHECK(!p.poll(3000, &c));
    write_text_atomic(ctl, "not json");
    CHECK(!p.poll(4000, &c));
    CHECK(send_command(ctl, Cmd::Screenshot) > 2);   // corrupt file: clock-based seq
    CHECK(p.poll(5000, &c) && c.cmd == Cmd::Screenshot);
    fs::remove_all(d);

    {   // runners-H1: the one-command file never loses an un-acked Quit
        fs::path d2 = fs::temp_directory_path() / "runctl_test_quit";
        fs::remove_all(d2);
        std::string c2 = (d2 / "control.json").u8string();
        CHECK(send_command(c2, Cmd::Quit) == 1);
        Command in; in.cmd = Cmd::Input; in.value = 1;
        CHECK(send_command_ex(c2, in) == 0);          // the menu's "input on" right after Quit is dropped
        std::string t2; Command now;
        CHECK(read_text(c2, &t2) && parse_control(t2, &now) && now.cmd == Cmd::Quit && now.seq == 1);
        CHECK(send_command(c2, Cmd::Quit) == 2);      // a Quit may replace a Quit
        write_text_atomic(ack_path_for(c2), make_ack({2, true, "quitting"}));
        CHECK(send_command_ex(c2, in) == 3);          // acked: commands flow again
        fs::remove_all(d2);
    }
#ifdef _WIN32
    {   // a reader holding the ack file open (no delete sharing) blocks the rename: no sleeping, no lost ack
        fs::path d3 = fs::temp_directory_path() / "runctl_test_held";
        fs::remove_all(d3);
        fs::create_directories(d3);
        std::string ctl3 = (d3 / "control.json").u8string();
        std::string ap = ack_path_for(ctl3);
        CHECK(write_text_atomic(ap, make_ack({1, true, "old"})));
        CHECK(send_command(ctl3, Cmd::Screenshot) == 1);
        Poller pl(ctl3);   // seq 1 is already there: handled
        CHECK(send_command(ctl3, Cmd::Pause, 0) == 2);
        Command got;
        CHECK(pl.poll(1000, &got) && got.seq == 2);
        HANDLE h = CreateFileW((d3 / "control_ack.json").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        auto t0 = std::chrono::steady_clock::now();
        CHECK(!write_text_atomic(ap, make_ack({9, true, "x"})));   // single attempt: fails fast while held
        pl.ack(true, "paused");                                    // held: the ack is kept, not written
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200));   // nothing slept
        std::string tt; Ack aa;
        CHECK(read_text(ap, &tt) && parse_ack(tt, &aa) && aa.seq == 1 && aa.msg == "old");   // old file intact
        CHECK(!fs::exists(d3 / "control_ack.json.tmp"));
        CHECK(!pl.poll(1300, &got));   // next tick, still held: still not written
        CHECK(read_text(ap, &tt) && parse_ack(tt, &aa) && aa.seq == 1);
        CloseHandle(h);
        CHECK(!pl.poll(1600, &got));   // released: this tick writes the pending ack
        CHECK(read_text(ap, &tt) && parse_ack(tt, &aa) && aa.seq == 2 && aa.ok && aa.msg == "paused");
        CHECK(!fs::exists(d3 / "control_ack.json.tmp"));
        // held for good: the retry gives up after kAckRetryTicks, the old ack stays, the poll never blocks
        CHECK(send_command(ctl3, Cmd::Pause, 0) == 3);
        CHECK(pl.poll(2000, &got) && got.seq == 3);
        h = CreateFileW((d3 / "control_ack.json").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        pl.ack(true, "never");
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kAckRetryTicks + 5; ++i) pl.poll(3000 + (uint64_t)i * 300, &got);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
        CloseHandle(h);
        pl.poll(20000, &got);   // gave up: the abandoned ack does not appear later
        CHECK(read_text(ap, &tt) && parse_ack(tt, &aa) && aa.seq == 2);
        fs::remove_all(d3);
    }
#endif
    if (!fails) std::printf("runner_ctl_test: all passed\n");
    return fails ? 1 : 0;
}
