#pragma once
/* "Welcome back" card after an update: pure logic (no UI, no files, no clock). Unit-tested by hub_welcome_test.
 *
 *  - which changelog versions are new since the last one the player saw (collapse + cap),
 *  - "Label: text" item splitting into a bold card title and a body,
 *  - keyword -> topic/page table (a "Show me" button only when it maps cleanly),
 *  - whether to show now, silently remember the version, or wait (fresh install, game running, lock screen, wizard...).
 * welcome_ui.cpp draws it. The hard rule (popup_guard.h): never over a running game, never raising or focusing the window. */

#include "changelog.h"
#include "logic.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace hubwelcome {

namespace cl = hubui::changelog;

/* What a card is about: drives its icon (welcome_ui.cpp) and the optional "Show me" page. */
enum class Topic { Generic, Discover, Admin, Saves, Controls, Online, Library, Look, Settings, Fix };
enum class Target { None, Library, Discover, Online, Saves, Controllers, Settings, Updates, Admin };

struct Card {
    std::string title;   // bold heading ("Invite codes"); empty when the item has no "Label:" prefix
    std::string body;    // the rest, first letter capitalised, no trailing full stop
    Topic topic = Topic::Generic;
    Target target = Target::None;   // owner-only targets already resolved for this player
    std::string version;            // the release it came from
};

/* ---- versions ---- */
/* a < b: -1, equal 0, > 1, either side unparsable ("dev") 2. "v1.2.3", "1.2.3" and "1.2.3-beta" all parse (leading v tolerated). */
inline int cmp(const std::string& a, const std::string& b) { return hubui::compare_app_versions(a, b); }

/* The entries newer than `last_seen` and not newer than `current`, newest first (the order of the file). last_seen empty or unparsable
 * gives only the entry for `current`. */
inline std::vector<const cl::Entry*> versions_since(const std::vector<cl::Entry>& all, const std::string& last_seen, const std::string& current) {
    std::vector<const cl::Entry*> out;
    const bool have_last = cmp(last_seen, "0") != 2;
    for (const cl::Entry& e : all) {
        if (cmp(e.version, current) == 1) continue;      // newer than this build (notes written ahead): not yet
        if (have_last ? cmp(e.version, last_seen) <= 0 : cmp(e.version, current) != 0) continue;
        out.push_back(&e);
    }
    return out;
}

/* ---- label split ---- */
/* "Invite codes: the owner makes a code" -> {"Invite codes", "The owner makes a code"}. A label is the text before the first ": " when it is
 * short (<= 32 chars, <= 4 words) and holds no quote/bracket; otherwise there is no title. */
inline void split_label(const std::string& item, std::string* title, std::string* body) {
    std::string t, b = item;
    size_t p = item.find(": ");
    if (p != std::string::npos && p > 0 && p <= 32) {
        std::string lab = item.substr(0, p);
        int words = 1;
        bool ok = true;
        for (char c : lab) {
            if (c == ' ') ++words;
            if (c == '"' || c == '<' || c == '(' || c == '\'' || c == ',') ok = false;
        }
        if (ok && words <= 4 && item.size() > p + 2) {
            t = lab;
            b = item.substr(p + 2);
        }
    }
    while (!b.empty() && (b.back() == '.' || b.back() == ' ')) b.pop_back();
    if (!b.empty()) b[0] = (char)std::toupper((unsigned char)b[0]);
    *title = t;
    *body = b;
}

/* ---- keyword -> topic/page ---- */
struct Rule { const char* kw; Topic topic; Target target; bool owner_only; };
inline const Rule* rules(int* n) {
    /* First match wins, so the specific words come first. Matched on the lower-cased title + body. */
    static const Rule k[] = {
        {"discover", Topic::Discover, Target::Discover, false},
        {"invite", Topic::Admin, Target::Admin, true},
        {"admin", Topic::Admin, Target::Admin, true},
        {"approv", Topic::Admin, Target::Admin, true},
        {"banned", Topic::Admin, Target::Admin, true},
        {"cloud", Topic::Saves, Target::Saves, false},
        {"save", Topic::Saves, Target::Saves, false},
        {"controller", Topic::Controls, Target::Controllers, false},
        {"controls", Topic::Controls, Target::Controllers, false},
        {"online", Topic::Online, Target::Online, false},
        {"room", Topic::Online, Target::Online, false},
        {"friend", Topic::Online, Target::Online, false},
        {"library", Topic::Library, Target::Library, false},
        {"logo", Topic::Look, Target::None, false},
        {"theme", Topic::Look, Target::None, false},
        {"settings", Topic::Settings, Target::Settings, false},
    };
    *n = (int)(sizeof k / sizeof k[0]);
    return k;
}

inline std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

/* Topic and (player-specific) target for a card text. Owner-only targets are dropped for everyone else. */
inline void classify(const std::string& text, bool is_owner, Topic* topic, Target* target) {
    const std::string t = lower(text);
    int n = 0;
    const Rule* r = rules(&n);
    *topic = Topic::Generic;
    *target = Target::None;
    for (int i = 0; i < n; ++i) {
        if (t.find(r[i].kw) == std::string::npos) continue;
        *topic = r[i].topic;
        *target = (r[i].owner_only && !is_owner) ? Target::None : r[i].target;
        return;
    }
}

/* ---- cards ---- */
inline constexpr int kMaxCards = 5;
inline constexpr int kMinCards = 3;

/* 3..5 highlight cards from the "new" items of every version since last_seen. Several versions are collapsed round-robin (every release gets
 * a card before any gets a second), newest release first; when "new" gives fewer than kMinCards, "improved" items fill up. Duplicate titles
 * are dropped. An empty result means there is nothing worth a pop-up (the version is then just remembered). */
inline std::vector<Card> collect_cards(const std::vector<const cl::Entry*>& vs, bool is_owner, int cap = kMaxCards) {
    std::vector<Card> out;
    cap = std::max(1, std::min(cap, kMaxCards));
    auto take = [&](int section) {
        size_t depth = 0;
        for (const cl::Entry* e : vs) depth = std::max(depth, e->items[section].size());
        for (size_t round = 0; round < depth && (int)out.size() < cap; ++round) {
            for (const cl::Entry* e : vs) {
                if ((int)out.size() >= cap) break;
                if (round >= e->items[section].size()) continue;
                Card c;
                split_label(e->items[section][round], &c.title, &c.body);
                if (c.body.empty()) continue;
                bool dup = false;
                for (const Card& o : out)
                    if ((!c.title.empty() && o.title == c.title) || o.body == c.body) dup = true;
                if (dup) continue;
                c.version = e->version;
                classify(c.title + " " + c.body, is_owner, &c.topic, &c.target);
                out.push_back(std::move(c));
            }
        }
    };
    take(cl::New);
    if ((int)out.size() < kMinCards) take(cl::Improved);
    return out;
}

/* ---- when to show ---- */
enum class Action { None, MarkSeen, Show };

struct Context {
    std::string last_seen;        // Settings.changelog_seen ("" = none recorded)
    std::string current;          // this build ("v1.31.13", "dev")
    bool enabled = true;          // Settings "Show what's new after updates"
    bool fresh_install = false;   // the first-run setup is still to come (wizard rule)
    bool has_cards = false;       // collect_cards() found something to show
    /* Reasons to wait (the version stays unseen and it is offered later): */
    bool game_active = false;     // any session running, or another full-screen / busy app
    bool hub_focused = true;      // never needs the focus taken: it waits until the hub has it
    bool locked = false;          // lock screen (pending / banned / setup)
    bool wizard_open = false;     // first-run wizard or another startup dialog
    bool other_popup = false;     // some other modal / menu is open
    bool not_ready = false;       // library scan not finished yet (the hub is still booting)
    /* Never at all (the version is NOT recorded, so a real run still gets it): */
    bool headless = false;        // screenshot / headless run
    bool qa_env = false;          // RETROHUB_QA / RETROHUB_TEST
    bool autotest = false;        // RETROHUB_AUTOTEST
};

inline Action decide(const Context& c) {
    if (c.headless || c.qa_env || c.autotest) return Action::None;
    if (cmp(c.current, "0") == 2) return Action::None;                                    // "dev" or unparsable build
    if (cmp(c.last_seen, "0") == 2) return Action::MarkSeen;                              // nothing recorded: a fresh install; remember, no pop-up
    if (cmp(c.last_seen, c.current) >= 0) return Action::None;                            // already seen (or a downgrade)
    if (c.fresh_install) return Action::MarkSeen;
    if (!c.enabled || !c.has_cards) return Action::MarkSeen;
    if (c.game_active || !c.hub_focused || c.locked || c.wizard_open || c.other_popup || c.not_ready) return Action::None;
    return Action::Show;
}

/* ---- animation ---- */
inline constexpr float kAnimSecs = 1.0f;
/* Eased 0..1 progress of the entrance; reduced = no animation at all (settled at once). */
inline float anim_progress(float t, bool reduced) {
    if (reduced) return 1.f;
    float x = std::max(0.f, std::min(1.f, t / kAnimSecs));
    return x;
}
/* Logo scale with a little overshoot ("bounce"): 0.6 -> ~1.08 -> 1.0. */
inline float bounce_scale(float p) {
    if (p >= 1.f) return 1.f;
    const float c1 = 1.70158f, c3 = c1 + 1.f, q = p - 1.f;
    const float e = 1.f + c3 * q * q * q + c1 * q * q;   // easeOutBack
    return 0.6f + 0.4f * e;
}

}  // namespace hubwelcome
