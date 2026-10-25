#include "wizard_logic.h"
#include "logic.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using namespace hubui::wiz;
    CHECK(next(Step::Welcome) == Step::Controller);
    CHECK(next(Step::Name) == Step::Done);
    CHECK(next(Step::Done) == Step::Done);
    CHECK(back(Step::Welcome) == Step::Welcome);
    CHECK(back(Step::Games) == Step::Controller);
    CHECK(from_index(-3) == Step::Welcome && from_index(99) == Step::Done && from_index(2) == Step::Games);
    CHECK(should_autorun(false, false, false));
    CHECK(!should_autorun(true, false, false));
    CHECK(!should_autorun(false, true, false));
    CHECK(!should_autorun(false, false, true));
    CHECK(pad_headline(0) == "No controller found");
    CHECK(pad_headline(1) == "Your controller is ready");
    CHECK(pad_headline(3) == "3 controllers are ready");
    CHECK(pad_hint(0, true).find("Plug in a controller") != std::string::npos);
    CHECK(pad_hint(1, true) == "Press A to continue.");
    CHECK(player_label(1) == "Player 2");
    CHECK(name_ok("Kai") && !name_ok("") && !name_ok("  	") && !name_ok("ab") && !name_ok("a b"));
    {   hubui::Settings a, b; a.setup_done = true;
        CHECK(hubui::settings_from_json(hubui::settings_to_json(a), &b) && b.setup_done);
        hubui::Settings c; CHECK(!c.setup_done && !c.username_claimed);
        a.username_claimed = true; a.name = "kai_64";
        hubui::Settings d; CHECK(hubui::settings_from_json(hubui::settings_to_json(a), &d) && d.username_claimed && d.name == "kai_64");
        CHECK(hubui::friends_error_text("unknown-username", "") == "No player has that username."); }
    std::printf(fails ? "FAILED\n" : "wizard tests passed\n");
    return fails ? 1 : 0;
}
