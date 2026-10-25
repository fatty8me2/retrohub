#include <cstdio>
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
    Ack a;
    CHECK(parse_ack(make_ack({5, true, "x \"y\""}), &a) && a.seq == 5 && a.ok && a.msg == "x \"y\"");
    CHECK(state_file("d/", 3) == "d/slot3.state" && state_file("d", kAutoSlot) == "d/auto.state");
    CHECK(thumb_file("d", 0) == "d/slot0.png" && sidecar_file("d", 9) == "d/slot9.json");
    CHECK(shots_dir("v\\", "n64_x") == "v/n64_x/shots");
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
    if (!fails) std::printf("runner_ctl_test: all passed\n");
    return fails ? 1 : 0;
}
