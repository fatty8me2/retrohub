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
    CHECK(!join_needs_confirm(false, 0, false) && !join_needs_confirm(true, 1, true) && !join_needs_confirm(false, 3, true));
    CHECK(join_needs_confirm(true, 2, true) && join_needs_confirm(true, 3, false));
    CHECK(join_confirm_text("Kai", false) == "Leave your room and join Kai's?");
    CHECK(join_confirm_text("Kai", true) == "Leave your room and join Kai's? Your guests will stay behind.");
    CHECK(join_confirm_text("", false) == "Leave your room and join that room?");
    CHECK(missing_game_notice("Mario Kart 64") == "You can join and chat, but you can't ready up until you add Mario Kart 64 with Add games in your Library.");
    CHECK(missing_game_notice("") .find("this game") != std::string::npos);
    CHECK(join_route(true, true, false) == JoinRoute::SilentBell);      // session running, game in front: silent
    CHECK(join_route(true, false, false) == JoinRoute::SilentBell);     // full-screen game, no hub session: silent, invite kept in the bell
    CHECK(join_route(true, false, true) == JoinRoute::SilentBell);      // a game with no hub session never gets a toast
    CHECK(join_route(true, true, true) == JoinRoute::QuietToast);       // hub in front, background session: quiet toast
    CHECK(join_route(false, false, true) == JoinRoute::JoinNow && join_route(false, false, false) == JoinRoute::JoinNow);
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
    { /* uid matching (SO-F10): presence in THIS room only, never the display name */
      std::vector<FriendRef> fr = {{"u1", "Mia", "ROOM01"}, {"u2", "Jake", ""}, {"u3", "Sam", ""}, {"u4", "Sam", ""}, {"u5", "Zed", "OTHER1"}};
      auto m = match_friends({"mia2", "jake", "sam", "Zed"}, fr, "ROOM01");
      CHECK(m.size() == 1 && m[0].uid == "u1");   /* mia2 renamed: credited by presence; same-named strangers jake/Zed/sam: nobody */
      CHECK(match_friends({"Jake"}, fr, "").empty());
      /* a stranger called Mickey sits in the room; friend Mickey is online elsewhere or offline: not credited */
      std::vector<FriendRef> mk = {{"umk", "Mickey", "OTHER9"}, {"ucam", "camer", "ROOM01"}};
      CHECK(friend_for_member("Mickey", mk, "ROOM01") == nullptr);
      CHECK(friend_for_member("mickey", {{"umk", "Mickey", ""}}, "ROOM01") == nullptr);
      CHECK(match_friends({"Mickey", "camer"}, mk, "ROOM01").size() == 1);
      /* the friend really is in the room: credited by uid (case-insensitive name picks the seat) */
      const FriendRef* c = friend_for_member("CAMER", mk, "ROOM01");
      CHECK(c && c->uid == "ucam");
      /* two friends with the same name in the room: ambiguous, nobody */
      CHECK(friend_for_member("Sam", {{"a", "Sam", "ROOM01"}, {"b", "Sam", "ROOM01"}}, "ROOM01") == nullptr);
      CHECK(friend_for_member("", mk, "ROOM01") == nullptr);
    }

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

    {   // "Notify me when they come online": per-friend toggle list
        std::string w;
        CHECK(!watch_has(w, "u1") && watch_split("").empty());
        CHECK(watch_set(&w, "u1", true) && watch_has(w, "u1") && w == "u1");
        CHECK(watch_set(&w, "u2", true) && watch_set(&w, "u2", true) && w == "u1,u2");   // no duplicates
        CHECK(watch_set(&w, "u1", false) && w == "u2" && !watch_has(w, "u1"));
        CHECK(watch_set(&w, "nobody", false) && w == "u2");
        std::string full;
        for (size_t i = 0; i < kWatchMax; ++i) full += (i ? "," : "") + std::string("f") + std::to_string(i);
        CHECK(watch_split(full).size() == kWatchMax);
        CHECK(!watch_set(&full, "extra", true) && !watch_has(full, "extra") && watch_split(full).size() == kWatchMax);   // at the cap: refused
        std::vector<std::string> keep = {"f0", "f1"};
        CHECK(watch_set(&full, "extra", true, &keep) && watch_split(full).size() == 3 && watch_has(full, "extra"));       // removed friends make room
    }

    {   // owner bell entries: someone waiting (Approve / Deny while still waiting) and an invite used
        NotifHistory h;
        Entry w; w.kind = Kind::JoinWaiting; w.uid = "u1"; w.name = "newguy"; w.code = "RH-7KQ2-MNXD"; w.text = "newguy (RH-7KQ2-MNXD) wants to join"; w.at = 100;
        Entry w2 = w; w2.at = 200;   // the same person again replaces the older entry
        Entry u; u.kind = Kind::InviteUsed; u.uid = "u2"; u.name = "Kai"; u.code = "K7QM-2XDT"; u.text = "Kai joined with invite K7QM-2XDT"; u.at = 150;
        h.add(w); h.add(u); h.add(w2);
        CHECK(h.v.size() == 2 && h.v[0].at == 200);
        World wo; wo.now = 300; wo.admin_waiting = 1;
        CHECK(state_of(h.v[0], wo) == State::Active && action_label(h.v[0], State::Active) == "Approve");
        wo.admin_waiting = 0;
        CHECK(state_of(h.v[0], wo) == State::Expired);   // approved elsewhere: nobody waits any more
        wo.admin_waiting = 2; h.resolve(Kind::JoinWaiting, "u1");
        CHECK(state_of(h.v[0], wo) == State::Done);
        CHECK(state_of(h.v[1], wo) == State::Info && action_label(h.v[1], State::Info).empty());
        CHECK(kind_from(kind_name(Kind::JoinWaiting)) == Kind::JoinWaiting && kind_from(kind_name(Kind::InviteUsed)) == Kind::InviteUsed);
        CHECK(headline(h.v[1]) == "Kai joined with invite K7QM-2XDT");
    }

    {   // invites / came-online while a game runs: never a Windows toast; bell always; OSD only when the runner can show one
        EventRoute a = route_event(true, false, true, false);
        CHECK(a.bell && !a.windows_toast && a.osd);   // game running: no toast, OSD
        EventRoute b = route_event(false, true, false, false);   // session without a runner channel: bell only
        CHECK(b.bell && !b.windows_toast && !b.osd);
        EventRoute c = route_event(false, false, true, false);   // idle hub: the in-hub toast covers it
        CHECK(c.bell && c.windows_toast && !c.osd);   // nothing running: the opt-in toast is allowed
        EventRoute d = route_event(true, true, true, true);      // headless: nothing external
        CHECK(d.bell && !d.windows_toast && !d.osd);
    }

    {   // in-hub notifications are held while a game runs; one quiet summary afterwards
        InHubRoute idle = in_hub_route(false, false, false);
        CHECK(idle.sound && idle.toast && !idle.hold);
        InHubRoute g = in_hub_route(true, false, false), s = in_hub_route(false, true, false), both = in_hub_route(true, true, false);
        CHECK(!g.sound && !g.toast && g.hold && !s.sound && !s.toast && s.hold && both.hold);
        InHubRoute hl = in_hub_route(true, true, true);   // headless screenshots keep their toasts
        CHECK(hl.sound && hl.toast && !hl.hold);
        CHECK(online_alert_wanted(true) && !online_alert_wanted(false));

        /* windowed game with the hub in front: the ask card may show, but never a sound; without focus it is held */
        InHubRoute af = in_hub_route(true, false, false, true, true), an = in_hub_route(true, false, false, false, true), ad = in_hub_route(true, false, false, true, false);
        CHECK(!af.sound && af.toast && !af.hold);
        CHECK(!an.sound && !an.toast && an.hold);
        CHECK(!ad.sound && !ad.toast && ad.hold);   // other kinds ignore focus

        HeldNotices h;
        CHECK(h.tick(false).empty());
        h.note(true);                               // while idle nothing is held (callers only note while busy)
        CHECK(h.tick(false).empty() && h.count == 0);
        CHECK(h.tick(true).empty());
        h.note(true); h.note(true); h.note(true);
        CHECK(h.tick(true).empty());                // still playing: nothing yet
        CHECK(h.tick(false, false).empty());        // play over but the hub is not in front: stays pending
        CHECK(h.tick(false, false).empty());
        std::string sum = h.tick(false, true);      // in front: exactly one summary
        CHECK(sum == "3 new notifications while you played. Open the bell to see them.");
        CHECK(h.tick(false).empty() && h.tick(false).empty());   // never again
        h.tick(true); h.note(true);
        CHECK(h.tick(false) == "1 new notification while you played. Open the bell to see it.");
        h.tick(true); h.note(true); h.note(false);  // a DM among them: no pointer to the bell
        CHECK(h.tick(false) == "2 new notifications while you played.");
        h.tick(true);
        CHECK(h.tick(false).empty());               // played with nothing arriving: silent

        FriendToastDedup d;                         // one friend toast per person per few seconds
        CHECK(d.allow("u1", 1000));
        CHECK(!d.allow("u1", 3000));                // second source for the same friend
        CHECK(d.allow("u2", 3000));
        CHECK(d.allow("u1", 7000));                 // later: a new event is fine
    }

    fs::remove_all(base);
    if (fails) { std::printf("%d failures\n", fails); return 1; }
    std::printf("hub_group_test: ok\n");
    return 0;
}
