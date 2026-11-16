/* hub_option_sections_test: option_sections.h (which option sections start open, the closed-section summary, the open / closed memory in
 * hub.json, which kit control each stored option gets, the status pill words) against the real option table, and the hub.json round trip
 * of "option_sections". No UI, no files. */
#include "option_sections.h"
#include "option_rows.h"
#include "logic.h"

#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubui;

/* The headings of a page in drawn order and how many rows each holds (the per-system defaults: no "Plays with", no Fritchii rows). */
struct Head { std::string name; int rows = 0; bool runner = false; };
static std::vector<Head> page_of(hub::System s, bool with_runner) {
    std::vector<std::string> present;
    std::vector<const hub::gs::Option*> shown;
    for (const hub::gs::Option* o : hub::gs::options_for(s)) {
        if (o->key == "switch.runner" && !with_runner) continue;
        if (o->key.rfind("switch.oot_", 0) == 0) continue;
        shown.push_back(o);
        present.push_back(option_group_label(o->group));
    }
    std::vector<Head> out;
    for (const std::string& g : option_group_order(present)) {
        Head h;
        h.name = g;
        for (const hub::gs::Option* o : shown)
            if (option_group_label(o->group) == g) { ++h.rows; h.runner = h.runner || o->key == "switch.runner"; }
        out.push_back(h);
    }
    return out;
}
static int rows_of(const std::vector<Head>& p) { int n = 0; for (auto& h : p) n += h.rows; return n; }

int main() {
    /* ---- which sections start open ---- */
    CHECK(option_section_default_open("Graphics", 0, false, 30));          // Graphics: always
    CHECK(option_section_default_open("Graphics", 3, false, 30));
    CHECK(!option_section_default_open("Advanced", 1, false, 5));          // Advanced: closed even on a short page
    CHECK(option_section_default_open("Gameplay", 4, true, 30));           // the "Plays with" section: always
    CHECK(option_section_default_open("Audio", 1, false, 12));             // a short page (12 rows or fewer): everything else open
    CHECK(option_section_default_open("Saves", 5, false, 12));
    CHECK(option_section_default_open("Audio", 1, false, 13));             // a long page: only the first two
    CHECK(!option_section_default_open("Gameplay", 2, false, 13));
    CHECK(!option_section_default_open("Controls", 3, false, 40));
    CHECK(!option_section_default_open("Advanced", 0, false, 40));
    {   /* the real pages */
        const std::vector<Head> n64 = page_of(hub::System::N64, false);   // 11 rows: all open but Advanced
        CHECK(rows_of(n64) <= kOptBigPage);
        for (int i = 0; i < (int)n64.size(); ++i)
            CHECK(option_section_default_open(n64[i].name, i, n64[i].runner, rows_of(n64)) == (n64[i].name != "Advanced"));
        const std::vector<Head> sw = page_of(hub::System::Switch, true);  // Ocarina of Time: many rows, Graphics + Plays with open
        CHECK(rows_of(sw) > kOptBigPage);
        bool runner_open = false, all_closed_else = true;
        for (int i = 0; i < (int)sw.size(); ++i) {
            const bool open = option_section_default_open(sw[i].name, i, sw[i].runner, rows_of(sw));
            if (sw[i].runner) runner_open = open;
            else if (sw[i].name != "Graphics" && i >= 2) all_closed_else = all_closed_else && !open;
        }
        CHECK(runner_open && all_closed_else);
        CHECK(sw[0].name == "Graphics");
    }

    /* ---- closed-section summary ---- */
    CHECK(option_section_summary(3, 1) == "3 settings, 1 changed");
    CHECK(option_section_summary(1, 0) == "1 setting");
    CHECK(option_section_summary(4, 0) == "4 settings");
    CHECK(option_section_summary(2, 2) == "2 settings, 2 changed");

    /* ---- open / closed memory ---- */
    CHECK(section_state_get("", "Advanced") == -1);
    std::string st = section_state_put("", "Advanced", true);
    CHECK(st == "Advanced=1" && section_state_get(st, "Advanced") == 1);
    st = section_state_put(st, "Audio", false);
    CHECK(section_state_get(st, "Audio") == 0 && section_state_get(st, "Advanced") == 1 && section_state_get(st, "Graphics") == -1);
    st = section_state_put(st, "Advanced", false);
    CHECK(section_state_get(st, "Advanced") == 0 && st == "Advanced=0,Audio=0");
    CHECK(section_state_put(st, "", true) == st && section_state_put(st, "a,b", true) == st && section_state_put(st, "a=b", true) == st);
    CHECK(section_state_get("Advanced=2,=1,Audio=1,junk", "Audio") == 1 && section_state_get("Advanced=2", "Advanced") == -1);   // bad tokens are skipped
    {
        std::string big;
        for (int i = 0; i < (int)kSectionStateMax + 5; ++i) big = section_state_put(big, "k" + std::to_string(i), true);
        CHECK(section_states_parse(big).size() == kSectionStateMax);   // bounded
        CHECK(section_state_put(big, "k0", false) != big);             // an existing key still changes
    }
    {   /* hub.json round trip; an older file without the key keeps everything at the defaults */
        hubui::Settings s;
        CHECK(s.option_sections.empty());
        s.option_sections = "Advanced=1,Audio=0";
        hubui::Settings r;
        CHECK(hubui::settings_from_json(hubui::settings_to_json(s), &r) && r.option_sections == "Advanced=1,Audio=0");
        hubui::Settings old;
        CHECK(hubui::settings_from_json("{\"name\":\"x\"}", &old) && old.option_sections.empty());
    }

    /* ---- controls: every option gets one, the stored values stay the same ---- */
    for (const hub::gs::Option& o : hub::gs::options()) {
        const OptCtl c = option_control(o);
        if (o.kind == hub::gs::Kind::Text) CHECK(c == OptCtl::Text);
        if (o.kind == hub::gs::Kind::Bool) CHECK(c == OptCtl::Toggle);
        if (c == OptCtl::Toggle && o.kind == hub::gs::Kind::Choice) {   // an On / Off choice as a toggle writes exactly its own two values
            std::string a, b;
            CHECK(hub::gs::normalize(o, option_toggle_value(o, true), &a) && a == "on");
            CHECK(hub::gs::normalize(o, option_toggle_value(o, false), &b) && b == "off");
            CHECK(option_toggle_on(o, "on") && !option_toggle_on(o, "off"));
        }
        if (o.kind == hub::gs::Kind::Bool) {
            std::string a;
            CHECK(hub::gs::normalize(o, option_toggle_value(o, true), &a) && a == "true" && option_toggle_on(o, "true") && !option_toggle_on(o, "false"));
        }
        if (c == OptCtl::Stepper) CHECK(o.kind == hub::gs::Kind::Choice && o.choices.size() >= 3 && o.choices.size() <= 12);
        if (o.kind == hub::gs::Kind::Choice) {
            CHECK(option_choice_index(o, o.def) >= 0);   // the default is always one of the choices (a dropdown never shows an empty value)
            CHECK(option_choice_index(o, "no such value") == -1);
        }
    }
    CHECK(option_control(*hub::gs::find_option("volume")) == OptCtl::Stepper);
    CHECK(option_control(*hub::gs::find_option("switch.res")) == OptCtl::Stepper);
    CHECK(option_control(*hub::gs::find_option("switch.fast_reads")) == OptCtl::Toggle);
    CHECK(option_control(*hub::gs::find_option("switch.repair_damaged")) == OptCtl::Toggle);
    CHECK(option_control(*hub::gs::find_option("fullscreen")) == OptCtl::Toggle);
    CHECK(option_control(*hub::gs::find_option("switch.runner")) == OptCtl::Dropdown);
    CHECK(option_control(*hub::gs::find_option("n64.texpack")) == OptCtl::Text);
    CHECK(option_control(*hub::gs::find_option("play.rewind")) == OptCtl::Dropdown);   // Automatic / Off is not an On / Off pair
    {   /* a value that is not one of the choices (the Game defaults volume follows Settings > Video & Games, any whole percent) */
        const hub::gs::Option& vol = *hub::gs::find_option("volume");
        CHECK(option_choice_nearest(vol, "70") == option_choice_index(vol, "70"));
        CHECK(vol.choices[(size_t)option_choice_nearest(vol, "75")].value == "70");   // tie: the lower one
        CHECK(vol.choices[(size_t)option_choice_nearest(vol, "77")].value == "80");
        CHECK(vol.choices[(size_t)option_choice_nearest(vol, "100")].value == "100");
        CHECK(option_choice_nearest(vol, "loud") == -1 && option_choice_nearest(vol, "") == -1);
        CHECK(option_value_text(vol, "75") == "75%" && option_value_text(vol, "70") == "70%" && option_value_text(vol, "0") == "0% (silent)");
        const hub::gs::Option& vs = *hub::gs::find_option("switch.vsync");
        CHECK(option_value_text(vs, "fifo") == "On (FIFO)" && option_value_text(vs, "weird") == "weird" && option_choice_nearest(vs, "weird") == -1);
    }

    /* ---- pills: plain words, sentence case, no dashes ---- */
    CHECK(std::string(option_changed_pill(true, false)) == "Changed" && option_changed_pill(false, false) == nullptr);
    CHECK(std::string(option_changed_pill(false, true)) == "Uses Switch setting");
    CHECK(option_online_pill(hub::gs::Scope::Visual) == nullptr);
    for (const char* p : {option_changed_pill(true, false), option_changed_pill(false, true), option_online_pill(hub::gs::Scope::Sync), option_online_pill(hub::gs::Scope::Locked)}) {
        CHECK(p && p[0] >= 'A' && p[0] <= 'Z');
        for (const char* c = p; *c; ++c) CHECK((unsigned char)*c < 0x80 && *c != '-' && *c != '!');
    }
    CHECK(std::string(option_online_pill(hub::gs::Scope::Sync)) != option_online_pill(hub::gs::Scope::Locked));

    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("hub_option_sections_test: all passed\n");
    return 0;
}
