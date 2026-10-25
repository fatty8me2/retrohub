#include "username_logic.h"
#include "wizard_logic.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using namespace hubnet;
    using namespace hubui::un;
    // validation
    CHECK(username_validate("Kai") == NameCheck::Ok);
    CHECK(username_validate("kai_64") == NameCheck::Ok);
    CHECK(username_validate("") == NameCheck::Empty && username_validate("   ") == NameCheck::Empty);
    CHECK(username_validate("ab") == NameCheck::TooShort);
    CHECK(username_validate("abcdefghijklmnopq") == NameCheck::TooLong && username_validate("abcdefghijklmnop") == NameCheck::Ok);
    CHECK(username_validate("kai smith") == NameCheck::BadChars && username_validate("kai!") == NameCheck::BadChars);
    CHECK(username_validate("kai.64") == NameCheck::Ok && username_validate("ka-i_x") == NameCheck::Ok && username_validate("kai-") == NameCheck::BadEdge && username_validate("ka._i") == NameCheck::BadEdge);
    CHECK(username_validate("RH-7KQ2-M3XD") == NameCheck::Reserved && username_validate("rhabcdefg1") == NameCheck::Ok);
    CHECK(username_validate("_kai") == NameCheck::BadEdge && username_validate("kai_") == NameCheck::BadEdge && username_validate("ka__i") == NameCheck::BadEdge);
    CHECK(username_validate("Admin") == NameCheck::Reserved && username_validate("RETROHUB") == NameCheck::Reserved);
    CHECK(username_validate("  kai  ") == NameCheck::Ok);
    CHECK(username_validate("\xC3\xA9mile") == NameCheck::BadChars);
    // normalization
    CHECK(username_key("  Kai_64 ") == "kai_64" && username_key("KAI") == username_key("kai"));
    CHECK(username_trim("\t x y ") == "x y");
    CHECK(username_sanitize("Kai Smith") == "Kai_Smith");
    CHECK(username_sanitize("!!") == "Player");
    CHECK(username_sanitize("a") == "aPlayer");
    CHECK(username_sanitize("A very long windows account name").size() <= 16);
    CHECK(username_ok(username_sanitize("__x__")) && username_ok(username_sanitize("J. R. R.")));
    CHECK(username_initial("kai") == 'K' && username_initial("9lives") == '9' && username_initial("") == '?');
    // suggestions
    {   auto s = username_suggestions("kai");
        CHECK(s.size() == 3 && s[0] == "kai2");
        for (auto& e : s) CHECK(username_ok(e) && username_key(e) != "kai");
        CHECK(s[1].rfind("kai_", 0) == 0);
        auto l = username_suggestions("abcdefghijklmnop");
        CHECK(!l.empty()); for (auto& e : l) CHECK(username_ok(e) && e.size() <= 16);
        CHECK(username_suggestions("kai", 7) == username_suggestions("kai", 7));
        auto r = username_suggestions("admin"); CHECK(!r.empty() && username_key(r[0]) != "admin"); }
    // friend code vs username
    CHECK(username_looks_like_code("RH-7KQ2-M9XD") && username_looks_like_code("7kq2-m9xd") && !username_looks_like_code("kai_64"));
    CHECK(!username_looks_like_code("7KQ2M9XD") && !username_looks_like_code("RHINO"));

    // checker: debounce, stale answers, offline
    {   Checker c; double t = 10;
        c.set_input("", t); CHECK(c.state == Avail::Empty);
        c.set_input("ka", t); CHECK(c.state == Avail::Invalid && !status_text(c).empty());
        c.set_input("kai", t); CHECK(c.state == Avail::Typing);
        CHECK(!c.want_send(t + 0.1, true));
        CHECK(c.want_send(t + 0.5, true) && c.state == Avail::Checking);
        c.sent(5);
        CHECK(!c.on_result(4, true, "", {}) && c.state == Avail::Checking);      // stale ref
        CHECK(c.on_result(5, false, "taken", {}) && c.state == Avail::Taken);
        CHECK(status_text(c) == "taken" && status_tone(c.state) == Tone::Bad && shows_suggestions(c.state) && !c.suggestions.empty());
        c.set_input("kai9", t + 1); CHECK(c.state == Avail::Typing && c.suggestions.empty());
        CHECK(c.want_send(t + 2, true)); c.sent(6);
        c.set_input("kai99", t + 2.1);                                           // typed on while checking: old answer must not apply
        CHECK(!c.on_result(6, true, "", {}) && c.state == Avail::Typing);
        CHECK(c.want_send(t + 3, true)); c.sent(7);
        CHECK(c.on_result(7, true, "", {}) && c.state == Avail::Available && status_text(c) == "available" && status_tone(c.state) == Tone::Good);
        c.set_input("KAI99", t + 4); CHECK(c.state == Avail::Available);        // case-only edit keeps the answer
        c.set_input("kai98", t + 5); CHECK(!c.want_send(t + 5.1, false));
        CHECK(c.want_send(t + 6, false) == false && c.state == Avail::Offline);
        c.on_connection_back(t + 7); CHECK(c.state == Avail::Typing && c.want_send(t + 7.0, true));
        c.sent(0); CHECK(c.state == Avail::Offline);
        c.set_input("Admin", t + 9); CHECK(c.state == Avail::NotAllowed && shows_suggestions(c.state) && status_text(c) == "not allowed");
        c.mine_key = "kai"; c.set_input("Kai", t + 10); CHECK(c.state == Avail::Yours);
        Checker d; d.set_input("zed1", 0); d.want_send(1, true); d.sent(1);
        CHECK(d.on_result(1, false, "reserved", {}) && d.state == Avail::NotAllowed);
        d.set_input("zed3", 4); d.want_send(5, true); d.sent(3); CHECK(d.on_result(3, false, "held", {}) && d.state == Avail::Taken);
        d.set_input("zed4", 6); d.want_send(7, true); d.sent(4); CHECK(d.on_result(4, true, "", {}, true) && d.state == Avail::Yours);
        d.set_input("zed5", 8); d.want_send(9, true); d.sent(5); CHECK(d.on_result(5, false, "rate-limited", {}) && d.state == Avail::Offline);
        d.set_input("zed2", 2); d.want_send(3, true); d.sent(2); d.on_connection_lost(); CHECK(d.state == Avail::Offline); }

    // wizard name step
    CHECK(name_action(Avail::Available, false, false) == NameAction::Claim);
    CHECK(name_action(Avail::Available, false, true) == NameAction::Disabled);
    CHECK(name_action(Avail::Taken, false, false) == NameAction::Disabled);
    CHECK(name_action(Avail::NotAllowed, false, false) == NameAction::Disabled);
    CHECK(name_action(Avail::Invalid, false, false) == NameAction::Disabled);
    CHECK(name_action(Avail::Empty, false, false) == NameAction::Disabled);
    CHECK(name_action(Avail::Offline, false, false) == NameAction::ContinueOffline);
    CHECK(name_action(Avail::Yours, true, false) == NameAction::Continue);
    CHECK(name_action(Avail::Empty, true, false) == NameAction::Continue);
    CHECK(name_action_label(NameAction::Claim, false) == "Claim username" && name_action_label(NameAction::Claim, true) == "Claiming...");
    CHECK(name_action_label(NameAction::ContinueOffline, false).find("later") != std::string::npos);
    // banners / prompt
    CHECK(banner_text(true, false, true).empty());
    CHECK(!banner_text(false, false, false).empty() && banner_text(false, false, false) != banner_text(false, false, true));
    CHECK(banner_text(true, true, true).find("unique usernames") != std::string::npos);
    CHECK(should_prompt(true, false) && !should_prompt(true, true) && !should_prompt(false, false));
    CHECK(hold_rule_text().find("7 days") != std::string::npos);
    CHECK(claim_error_text("taken").find("took") != std::string::npos);
    CHECK(countdown_text(545) == "9:05" && countdown_text(-3) == "0:00" && countdown_text(0.2) == "0:01");
    CHECK(link_code_complete("k7m2-qx9p") && link_code_complete("K7M2QX9P") && !link_code_complete("K7M2-QX9") && link_code_complete("K7M2-QX9P-4HTW"));
    // wizard flow: 6 steps (cloud after username) and names the username step
    CHECK(hubui::wiz::kStepCount == 6 && hubui::wiz::step_title(hubui::wiz::Step::Name) == "Pick your username");
    CHECK(hubui::wiz::name_ok("kai") && !hubui::wiz::name_ok("ab") && !hubui::wiz::name_ok("a b c"));
    CHECK(hubui::wiz::returning_blurb().find("another PC") != std::string::npos);
    std::printf(fails ? "FAILED\n" : "username tests passed\n");
    return fails ? 1 : 0;
}
