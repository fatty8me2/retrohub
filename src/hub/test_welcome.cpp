// Console test: "Welcome back" card logic (versions since, collapse/cap, label split, keyword->page, show/defer rules).
#include "welcome_logic.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubwelcome;

static cl::Entry mk(const char* v, const char* title, std::vector<std::string> nw, std::vector<std::string> imp = {}) {
    cl::Entry e; e.version = v; e.title = title; e.items[cl::New] = nw; e.items[cl::Improved] = imp; return e;
}

int main() {
    // version compare
    CHECK(cmp("v1.31.13", "1.31.13") == 0 && cmp("1.31.9", "1.31.13") == -1 && cmp("v2.0", "1.99.99") == 1 && cmp("dev", "1.0") == 2);
    CHECK(cmp("1.31.13-beta", "1.31.13") == 0 && cmp("", "1.0") == 2);

    std::vector<cl::Entry> all = {
        mk("1.31.13", "Invites", {"Invite codes: the owner makes a code.", "Admin: waiting list first"}),
        mk("1.31.12", "Smash accounts", {"Smash Classic online now checks your account"}),
        mk("1.31.11", "Approve", {"Banned accounts see only a lock screen", "Second"}),
        mk("1.31.10", "Owner", {"Owner-only Admin page"}),
        mk("1.31.9", "Discover", {"Discover tab: highly rated games", "Extra thing: x"}, {"Imp one"}),
        mk("1.31.8", "Old", {"Old thing"}),
    };
    // versions since
    auto vs = versions_since(all, "1.31.9", "v1.31.13");
    CHECK(vs.size() == 4 && vs[0]->version == "1.31.13" && vs[3]->version == "1.31.10");
    CHECK(versions_since(all, "1.31.13", "1.31.13").empty());
    CHECK(versions_since(all, "1.31.11", "1.31.12").size() == 1);
    CHECK(versions_since(all, "", "1.31.12").size() == 1 && versions_since(all, "dev", "1.31.12")[0]->version == "1.31.12");
    CHECK(versions_since(all, "1.31.14", "1.31.13").empty());   // downgrade

    // label split
    std::string t, b;
    split_label("Invite codes: the owner makes a code.", &t, &b);
    CHECK(t == "Invite codes" && b == "The owner makes a code");
    split_label("New Candy D-pad logo and app icon", &t, &b);
    CHECK(t.empty() && b == "New Candy D-pad logo and app icon");
    split_label("Owner notifications: \"<name> wants to join\" with Approve", &t, &b);
    CHECK(t == "Owner notifications" && b.rfind("\"<name>", 0) == 0);
    split_label("Smash Classic online now checks your RetroHub account: banned or waiting accounts can't join", &t, &b);
    CHECK(t.empty());   // label too long -> no title
    split_label("Something (like this): more", &t, &b);
    CHECK(t.empty());
    split_label("Trailing: ", &t, &b);
    CHECK(t.empty());

    // collapse + cap
    auto cards = collect_cards(vs, true);
    CHECK(cards.size() == 5);
    CHECK(cards[0].version == "1.31.13" && cards[1].version == "1.31.12" && cards[2].version == "1.31.11" && cards[3].version == "1.31.10" &&
          cards[4].version == "1.31.13");
    CHECK(collect_cards(vs, true, 3).size() == 3 && collect_cards(vs, true, 99).size() == 5);
    auto one = collect_cards(versions_since(all, "1.31.12", "1.31.13"), true);
    CHECK(one.size() == 2);
    auto few = collect_cards(versions_since(all, "1.31.8", "1.31.9"), true);
    CHECK(few.size() == 3 && few[2].body == "Imp one");   // 2 new + improved fills to the minimum of 3
    std::vector<const cl::Entry*> dup = {&all[0], &all[0]};
    CHECK(collect_cards(dup, true).size() == 2);   // identical items are not repeated
    CHECK(collect_cards({}, true).empty());

    // keyword -> page
    Topic tp; Target tg;
    classify("Discover tab: highly rated games", false, &tp, &tg);
    CHECK(tp == Topic::Discover && tg == Target::Discover);
    classify("Invite codes: the owner makes a code", true, &tp, &tg);
    CHECK(tp == Topic::Admin && tg == Target::Admin);
    classify("Invite codes: the owner makes a code", false, &tp, &tg);
    CHECK(tp == Topic::Admin && tg == Target::None);   // owner-only page is never offered to others
    classify("Cloud saves sync", false, &tp, &tg);
    CHECK(tg == Target::Saves);
    classify("New Candy D-pad logo", false, &tp, &tg);
    CHECK(tp == Topic::Look && tg == Target::None);
    classify("Faster startup", false, &tp, &tg);
    CHECK(tp == Topic::Generic && tg == Target::None);
    CHECK(collect_cards(vs, false)[0].target == Target::None);

    // show / defer rules
    Context c;
    c.last_seen = "1.31.9"; c.current = "v1.31.13"; c.has_cards = true;
    CHECK(decide(c) == Action::Show);
    Context f = c; f.last_seen = "";
    CHECK(decide(f) == Action::MarkSeen);   // fresh install: silent
    f = c; f.fresh_install = true;
    CHECK(decide(f) == Action::MarkSeen);
    f = c; f.last_seen = "1.31.13";
    CHECK(decide(f) == Action::None);
    f = c; f.last_seen = "1.32.0";
    CHECK(decide(f) == Action::None);
    f = c; f.current = "dev";
    CHECK(decide(f) == Action::None);
    f = c; f.enabled = false;
    CHECK(decide(f) == Action::MarkSeen);   // toggle off: remembered, never shown
    f = c; f.has_cards = false;
    CHECK(decide(f) == Action::MarkSeen);
    // deferral: the version stays unseen
    f = c; f.game_active = true; CHECK(decide(f) == Action::None);
    f = c; f.hub_focused = false; CHECK(decide(f) == Action::None);
    f = c; f.locked = true; CHECK(decide(f) == Action::None);
    f = c; f.wizard_open = true; CHECK(decide(f) == Action::None);
    f = c; f.other_popup = true; CHECK(decide(f) == Action::None);
    f = c; f.not_ready = true; CHECK(decide(f) == Action::None);
    f = c; f.game_active = false; CHECK(decide(f) == Action::Show);   // game exited: offered now
    // never in headless / QA / autotest, and not even remembered there
    f = c; f.headless = true; CHECK(decide(f) == Action::None);
    f = c; f.qa_env = true; CHECK(decide(f) == Action::None);
    f = c; f.autotest = true; CHECK(decide(f) == Action::None);
    f = c; f.headless = true; f.last_seen = ""; CHECK(decide(f) == Action::None);

    // animation
    CHECK(anim_progress(0.f, false) == 0.f && anim_progress(5.f, false) == 1.f && anim_progress(0.1f, true) == 1.f);
    CHECK(bounce_scale(0.f) > 0.5f && bounce_scale(0.f) < 0.7f && bounce_scale(1.f) == 1.f);
    float mx = 0;
    for (int i = 0; i <= 100; ++i) mx = std::max(mx, bounce_scale(i / 100.f));
    CHECK(mx > 1.0f && mx < 1.2f);   // a little overshoot

    std::printf(fails ? "welcome test: FAILED\n" : "welcome test: OK\n");
    return fails ? 1 : 0;
}
