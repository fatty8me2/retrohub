// Console test: game night plans (S6): DM tag round trip, reminder/start timing, dedupe, persistence.
#include "gamenight_logic.h"

#include <cstdio>
#include <filesystem>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
using namespace hubui::night;

int main() {
    const int64_t M = 60000, H = 60 * M, now = 1000000LL * 1000;
    Night n; n.host_uid = "u1"; n.host_name = "Mia"; n.game_id = "mk64"; n.game_title = "Mario Kart 64"; n.start_ms = now + 30 * M;
    n.invitees = {"u2"};
    std::string t = encode(n, "Fri 8:00 PM");
    CHECK(t.size() < 500 && t.find("Game night: Mario Kart 64 on Fri 8:00 PM") == 0);
    Night p;
    CHECK(parse(t, &p) && p.game_id == "mk64" && p.game_title == "Mario Kart 64" && p.start_ms == n.start_ms);
    CHECK(display_text(t) == "Game night: Mario Kart 64 on Fri 8:00 PM - open RetroHub to join");
    CHECK(!parse("hello", &p) && !parse("[RHGN1|abc|x|y]", &p) && !parse("[RHGN1|123|x]", &p) && display_text("hi") == "hi");
    n.game_title = "Evil|]Title"; CHECK(parse(encode(n, "x"), &p) && p.game_title == "Evil  Title");

    n.game_title = "Mario Kart 64"; n.id = make_id("u1", n.start_ms);
    CHECK(phase_of(n, now) == Phase::Upcoming && !reminder_due(n, now));
    CHECK(phase_of(n, now + 20 * M) == Phase::Reminder && reminder_due(n, now + 20 * M) && !start_due(n, now + 20 * M));
    CHECK(phase_of(n, now + 30 * M) == Phase::Live && start_due(n, now + 30 * M) && reminder_due(n, now + 30 * M));  // PC was off: still reminded once
    CHECK(phase_of(n, now + 30 * M + kLiveMs) == Phase::Over);
    n.reminded = true; CHECK(!reminder_due(n, now + 25 * M));
    n.dismissed = true; CHECK(phase_of(n, now + 30 * M) == Phase::Over);
    n.dismissed = false;
    CHECK(when_text(n, now) == "in 30 mins" && when_text(n, now + 30 * M) == "now" && when_text(n, now + 42 * M) == "started 12 mins ago");
    CHECK(plan_error(n, now).empty());
    Night bad = n; bad.start_ms = now + 10000; CHECK(!plan_error(bad, now).empty());
    bad = n; bad.start_ms = now + 20 * 24 * H; CHECK(!plan_error(bad, now).empty());
    bad = n; bad.invitees.clear(); CHECK(!plan_error(bad, now).empty());
    bad = n; bad.game_id.clear(); CHECK(!plan_error(bad, now).empty());

    Plans pl;
    CHECK(pl.add(n, now) && !pl.add(n, now));          // a re-delivered DM does not re-add
    Night old = n; old.id = "x@1"; old.start_ms = now - 5 * H; CHECK(!pl.add(old, now));
    Night later = n; later.id = "y@2"; later.start_ms = now + 2 * H; pl.add(later, now);
    CHECK(pl.v.size() == 2 && pl.v[0].id == n.id && pl.active(now).size() == 2);
    pl.find(n.id)->reminded = true;
    std::string path = (std::filesystem::temp_directory_path() / "gn_test" / "gamenights.json").string();
    CHECK(save(path, pl));
    Plans r; CHECK(load(path, &r) && r.v.size() == 2 && r.v[0].reminded && r.v[0].invitees.size() == 1 && r.v[1].id == "y@2");
    std::filesystem::remove_all(std::filesystem::path(path).parent_path());
    CHECK(!from_json("garbage", &r));
    std::printf(fails ? "FAILED (%d)\n" : "gamenight tests passed\n", fails);
    return fails ? 1 : 0;
}
