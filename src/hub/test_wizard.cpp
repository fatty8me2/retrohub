#include "wizard_logic.h"
#include "logic.h"
#include <cstdio>
#include <string>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using namespace hubui::wiz;
    CHECK(next(Step::Welcome) == Step::Controller);
    CHECK(next(Step::Name) == Step::Cloud && next(Step::Cloud) == Step::Done && back(Step::Done) == Step::Cloud);
    CHECK(next(Step::Done) == Step::Done);
    CHECK(back(Step::Welcome) == Step::Welcome);
    CHECK(back(Step::Games) == Step::Controller);
    CHECK(from_index(-3) == Step::Welcome && from_index(99) == Step::Done && from_index(2) == Step::Games);
    CHECK(should_autorun(false, false, false));
    CHECK(!should_autorun(true, false, false));
    CHECK(!should_autorun(false, true, false));
    CHECK(!should_autorun(false, false, true));
    CHECK(needs_setup(false, false, false) && !needs_setup(true, false, false) && !needs_setup(false, true, false) && !needs_setup(false, false, true));

    /* H4: the installer pre-fills a games folder, which must not make a brand-new install look "already set up". */
    {
        using hubui::Settings;
        // the exact steps main() takes: merge the hand-off folder, then decide from the folders the PLAYER chose
        auto decide = [](Settings& s, const char* hand_text) {
            Settings probe;
            const bool usable = hubui::merge_installer_games_dir(&probe, hand_text);
            hubui::merge_installer_games_dir(&s, hand_text);
            return fresh_install(s.setup_done, s.find_games_done, s.rom_folders.size(), usable);
        };
        const char* hand = "C:\\Users\\Kai\\Documents\\RetroHub Games\r\n";
        {   Settings s;                                     // a fresh installer install: no hub.json yet
            CHECK(decide(s, hand) && s.rom_folders.size() == 1);   // the folder is kept as the wizard's pre-filled answer
        }
        {   Settings s; s.rom_folders = {"D:\\Roms"};       // the player already had games of their own: not fresh
            CHECK(!decide(s, hand) && s.rom_folders.size() == 2);
        }
        {   Settings s; s.setup_done = true;                // the wizard was finished before (reinstall over kept user data)
            CHECK(!decide(s, hand));
        }
        {   Settings s; s.find_games_done = true;           // "Find my games" was done before
            CHECK(!decide(s, hand));
        }
        {   Settings s; s.rom_folders = {"c:/users/kai/documents/retrohub games/"};   // second start after quitting mid-wizard: only the installer's folder is listed
            CHECK(decide(s, hand) && s.rom_folders.size() == 1);   // still fresh: the wizard runs again until it is finished or skipped
        }
        {   Settings s;                                     // a hand-off file that names no real folder
            CHECK(decide(s, "relative\\path") && s.rom_folders.empty());   // nothing merged, and a player with no folders is fresh too
        }
        CHECK(!fresh_install(false, false, 2, true) && fresh_install(false, false, 1, true) && fresh_install(false, false, 0, false));
        CHECK(!fresh_install(false, false, 1, false));       // one folder that is NOT the installer's: the player's own
        // the flag the whole first-run UI reads (the wizard's autorun and the "What's new" popup that waits for a fresh install)
        CHECK(!installer_prefill());
        CHECK(!should_autorun(false, false, true));
        set_installer_prefill(true);
        CHECK(should_autorun(false, false, true));           // folders present, but only the installer's: still a fresh install
        CHECK(should_autorun(false, false, false));
        CHECK(!should_autorun(true, false, true) && !should_autorun(false, true, true));   // finished or skipped: never again
        set_installer_prefill(false);
        CHECK(!should_autorun(false, false, true));
    }
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
