// Console test: recent groups + notification history persistence, merge and expiry (scratch dirs only).
#include "group_logic.h"

#include <cstdio>
#include <filesystem>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubui::group;
namespace fs = std::filesystem;

static RecentGroup grp(std::vector<Member> m, const char* gid, const char* title, int64_t at) {
    RecentGroup g; g.members = m; g.game_id = gid; g.game_title = title; g.at = at; return g;
}

int main() {
    const int64_t H = 3600000LL, D = kDayMs, now = 100 * D;
    CHECK(ago_text(now - 10000, now) == "just now" && ago_text(now - 5 * 60000, now) == "5 min ago");
    CHECK(ago_text(now - H, now) == "1 hour ago" && ago_text(now - 3 * H, now) == "3 hours ago");
    CHECK(ago_text(now - D, now) == "yesterday" && ago_text(now - 2 * D, now) == "2 days ago");
    CHECK(ago_text(now - 21 * D, now) == "3 weeks ago" && ago_text(now - 90 * D, now) == "3 months ago" && ago_text(0, now).empty());

    RecentGroups r;
    CHECK(!r.record(grp({}, "a", "A", 5)));   // nobody else: ignored
    for (int i = 0; i < 7; ++i) CHECK(r.record(grp({{"u" + std::to_string(i), "N"}}, "mk64", "Mario Kart 64", 1000 + i)));
    CHECK(r.v.size() == kMaxGroups && r.last()->at == 1006 && r.v.back().at == 1002);
    // same people + game replaces the older entry and moves to front
    CHECK(r.record(grp({{"u3", "N"}}, "mk64", "Mario Kart 64", 2000)));
    CHECK(r.v.size() == kMaxGroups && r.last()->members[0].uid == "u3" && r.last()->at == 2000);
    CHECK(r.last_with("u6") && r.last_with("u6")->at == 1006 && !r.last_with("zz"));
    CHECK(together_text(r.last_with("u6"), 1006 + 2 * D) == "Last played together: Mario Kart 64, 2 days ago");
    CHECK(together_text(nullptr, now).empty());
    RecentGroup big = grp({{"a", "Ann"}, {"b", "Bob"}, {"c", "Cy"}, {"d", "Di"}, {"e", "Ed"}}, "g", "G", 1);
    CHECK(group_names(big) == "Ann, Bob, Cy +2");
    CHECK((invitable(big, {"a", "b", "c", "e"}, {"b"}) == std::vector<std::string>{"a", "c", "e"}));

    // persistence: two copies (PC + vault) union
    fs::path base = fs::temp_directory_path() / "rh_group_test";
    fs::remove_all(base);
    std::string pc = (base / "pc").string(), vault = (base / "vault").string();
    CHECK(save_groups(r, pc, ""));
    RecentGroups other; other.record(grp({{"zz", "Zed"}}, "smash64", "Smash 64", 5000));
    CHECK(save_groups(other, "", vault));
    RecentGroups l = load_groups(pc, vault);
    CHECK(l.v.size() == kMaxGroups && l.last()->members[0].uid == "zz" && l.v[1].at == 2000);
    CHECK(load_groups((base / "nope").string(), "").v.empty());
    RecentGroups rt;
    CHECK(groups_from_json(groups_to_json(l), &rt) && rt.v.size() == l.v.size() && rt.v[0].game_title == "Smash 64");
    CHECK(!groups_from_json("not json", &rt));

    // notification history
    NotifHistory h;
    World w; w.now = now; w.pending_request_uids = {"r1"};
    Entry inv; inv.kind = Kind::Invite; inv.uid = "u1"; inv.name = "Mia"; inv.title = "Smash 64"; inv.code = "ABC234"; inv.at = now - 60000; inv.expires_at = now + 600000;
    h.add(inv);
    Entry rq; rq.kind = Kind::FriendRequest; rq.uid = "r1"; rq.name = "Kai"; rq.at = now - 30000;
    h.add(rq);
    Entry cf; cf.kind = Kind::CloudConflict; cf.text = "Save conflict"; cf.at = now - 20000;
    h.add(cf);
    CHECK(h.v.size() == 3 && h.unread() == 3 && h.v[0].kind == Kind::CloudConflict);
    CHECK(state_of(inv, w) == State::Active && action_label(inv, State::Active) == "Join");
    CHECK(state_of(rq, w) == State::Active && action_label(rq, State::Active) == "Accept");
    CHECK(state_of(cf, w) == State::Expired);   // no conflicts left in the world
    w.conflicts = 1; CHECK(state_of(cf, w) == State::Active);
    w.now = inv.expires_at; CHECK(state_of(inv, w) == State::Expired && action_label(inv, State::Expired).empty() && std::string(state_label(State::Expired)) == "Expired");
    w.now = now; w.pending_request_uids.clear();
    CHECK(state_of(rq, w) == State::Expired);   // answered elsewhere
    w.pending_request_uids = {"r1"};
    CHECK(h.active(w) == 3);
    h.resolve(Kind::FriendRequest, "r1");
    CHECK(state_of(h.v[1], w) == State::Done && h.active(w) == 2 && h.unread() == 2);
    // same invite again replaces the old row
    Entry inv2 = inv; inv2.at = now; h.add(inv2);
    CHECK(h.v.size() == 3 && h.v[0].at == now);
    h.mark_all_read(); CHECK(h.unread() == 0);
    CHECK(headline(inv) == "Mia invited you to play Smash 64" && headline(rq) == "Kai wants to be friends");

    // cap 50, newest kept
    NotifHistory big_h;
    for (int i = 0; i < 80; ++i) { Entry e; e.kind = Kind::Info; e.text = "n" + std::to_string(i); e.at = 1000 + i; big_h.add(e); }
    CHECK(big_h.v.size() == kMaxNotifs && big_h.v[0].text == "n79" && big_h.v.back().text == "n30");

    // persistence + merge of read/done flags
    CHECK(save_history(h, pc, ""));
    NotifHistory h2 = h; h2.v[0].read = false; for (auto& e : h2.v) e.done = e.kind == Kind::Invite;
    CHECK(save_history(h2, "", vault));
    NotifHistory lh = load_history(pc, vault);
    CHECK(lh.v.size() == 3 && lh.unread() == 0);
    bool inv_done = false; for (auto& e : lh.v) if (e.kind == Kind::Invite) inv_done = e.done;
    CHECK(inv_done);
    NotifHistory rh;
    CHECK(history_from_json(history_to_json(lh), &rh) && rh.v.size() == 3 && rh.v[0].code == "ABC234");
    CHECK(!history_from_json("{", &rh));

    // play again banner
    CHECK(play_again_label("LAUNCH", true, false, true, true) == "PLAY AGAIN");
    CHECK(play_again_label("LAUNCH", true, false, false, true) == "LAUNCH");
    CHECK(play_again_label("READY", false, false, true, true) == "PLAY AGAIN");
    CHECK(play_again_label("NOT READY", false, true, true, true) == "NOT READY");
    CHECK(play_again_label("LAUNCH", true, false, true, false) == "LAUNCH");
    CHECK(play_again_applies(true, false, false) && !play_again_applies(true, true, false) && !play_again_applies(true, false, true));

    fs::remove_all(base);
    if (fails) { std::printf("%d failures\n", fails); return 1; }
    std::printf("hub_group_test: ok\n");
    return 0;
}
