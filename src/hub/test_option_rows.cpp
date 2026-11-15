/* hub_option_rows_test: option_rows.h (row layout, heading order, the "Plays with" value shown) and the option table's headings and help texts
 * (game_settings.cpp). No UI, no files. */
#include "option_rows.h"
#include "game_settings.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubui;

static std::vector<std::string> headings_of(hub::System s) {
    std::vector<std::string> v;
    for (const hub::gs::Option* o : hub::gs::options_for(s)) v.push_back(option_group_label(o->group));
    return v;
}
static bool has(const std::vector<std::string>& v, const char* s) { for (auto& x : v) if (x == s) return true; return false; }
static int index_of(const std::vector<std::string>& v, const char* s) { for (size_t i = 0; i < v.size(); ++i) if (v[i] == s) return (int)i; return -1; }

int main() {
    /* ---- row layout: label column from the longest label, control from the longest value, stacked when they do not fit ---- */
    {
        OptRowIn in; in.label_max = 200; in.value_max = 150; in.avail = 600; in.gap = 16; in.min_control = 96;
        OptRowLayout r = option_row_layout(in);
        CHECK(!r.stacked && r.control_x == 216.f && r.control_w == 150.f);
        in.value_max = 40;   // short values ("On" / "Off") still get a usable combo
        r = option_row_layout(in);
        CHECK(!r.stacked && r.control_w == 96.f);
        in.value_max = 384; in.avail = 600;   // exactly fits: 200 + 16 + 384
        r = option_row_layout(in);
        CHECK(!r.stacked && r.control_x + r.control_w == 600.f);
        in.avail = 599;   // one pixel short: the control goes under the label at its full width
        r = option_row_layout(in);
        CHECK(r.stacked && r.control_x == 0.f && r.control_w == 384.f);
        in.value_max = 700;   // wider than the row itself: cut to the row
        r = option_row_layout(in);
        CHECK(r.stacked && r.control_w == 599.f);
        in.avail = -5;   // nothing to work with: no negative widths
        r = option_row_layout(in);
        CHECK(r.stacked && r.control_w == 0.f);
    }
    {   // the clipped cases from the audit, with an 8 px per character stand-in font and a 40 px combo frame at a 1280 px window's ~860 px panel
        const float ch = 8.f, chrome = 40.f;
        OptRowIn in;
        in.label_max = std::strlen("Frames ahead (smoother on some games)") * ch;
        in.value_max = std::strlen("Smooth 60 (steady 30 + frame generation)") * ch + chrome;
        in.gap = 16; in.min_control = 96; in.avail = 860;
        OptRowLayout r = option_row_layout(in);
        CHECK(!r.stacked && r.control_x >= in.label_max && r.control_w >= in.value_max);   // nothing runs under the combo, no value is cut
        in.avail = 520;   // a narrow window: under the label, still the full value
        r = option_row_layout(in);
        CHECK(r.stacked && r.control_w == in.value_max);
    }
    /* trailing notes ("overridden", "host decides online", reset) */
    CHECK(option_trail_fits(500, 8, 100, 608) && !option_trail_fits(500, 8, 101, 608) && option_trail_fits(900, 8, 0, 100));

    /* ---- headings: every group an option uses is drawn (Multitap and Memory card 2 were never shown), known order first ---- */
    CHECK(option_group_label("Video") == "Graphics" && option_group_label("Color") == "Display" && option_group_label("Emulation") == "Gameplay");
    CHECK(option_group_label("Controls") == "Controls" && option_group_label("Saves") == "Saves" && option_group_label("Audio") == "Audio");
    {
        const std::vector<std::string> o = option_group_order({"Saves", "Graphics", "Mystery", "Controls", "Graphics", "Audio"});
        CHECK(o.size() == 5 && o[0] == "Graphics" && o[1] == "Audio" && o[2] == "Controls" && o[3] == "Saves" && o[4] == "Mystery");
        CHECK(option_group_order({}).empty());
    }
    {
        using hub::System;
        for (System s : {System::N64, System::GB, System::GBC, System::GBA, System::NDS, System::GC, System::PS1, System::N3DS, System::WiiU,
                         System::PS2, System::NES, System::SNES, System::PSP, System::Switch}) {
            const std::vector<std::string> h = headings_of(s);
            const std::vector<std::string> order = option_group_order(h);
            for (const std::string& x : h) CHECK(has(order, x.c_str()));   // no option without a drawn heading
        }
        const std::vector<std::string> ps1 = option_group_order(headings_of(System::PS1));
        CHECK(has(ps1, "Controls") && has(ps1, "Saves") && index_of(ps1, "Gameplay") < index_of(ps1, "Controls") && index_of(ps1, "Controls") < index_of(ps1, "Saves"));
        CHECK(has(option_group_order(headings_of(System::SNES)), "Controls"));
        const hub::gs::Option* mt = hub::gs::find_option("ps1.multitap");
        const hub::gs::Option* mc = hub::gs::find_option("ps1.memcard2");
        CHECK(mt && option_group_label(mt->group) == "Controls" && mc && option_group_label(mc->group) == "Saves");
    }
    /* Run-ahead and Rewind show under Gameplay; their keys and values are unchanged */
    for (const char* k : {"play.runahead", "play.rewind"}) {
        const hub::gs::Option* o = hub::gs::find_option(k);
        CHECK(o && option_group_label(o->group) == "Gameplay");
    }
    {
        const hub::gs::Option* ra = hub::gs::find_option("play.runahead");
        CHECK(ra && ra->def == "off" && ra->choices.size() == 4 && ra->choices[3].value == "auto");
        hub::gs::Store st;
        CHECK(st.set_game("g", hub::System::SNES, "play.runahead", "2") && st.resolve(hub::System::SNES, "g").at("play.runahead") == "2");
    }

    /* ---- help: one plain sentence for every option a player would not understand ---- */
    {
        const char* keys[] = {"n64.fb_emulation", "n64.copy_color", "n64.copy_depth", "n64.aa", "n64.fxaa", "n64.texfilter", "scale.integer",
                              "play.runahead", "nes.region", "snes.region", "gb.color", "wiiu.cpu_mode", "ps2.renderer", "switch.vsync",
                              "switch.async_shaders", "switch.preload_shaders", "switch.fast_reads", "switch.gpu_accuracy", "switch.frames_ahead",
                              "switch.framegen", "switch.pacing", "switch.runner"};
        for (const char* k : keys) {
            const hub::gs::Option* o = hub::gs::find_option(k);
            CHECK(o != nullptr);
            if (!o) continue;
            const std::string& h = o->help;
            bool ok = !h.empty() && h.size() < 200 && h.back() == '.' && h.find(". ") == std::string::npos && h.find('!') == std::string::npos;
            for (unsigned char c : h) if (c >= 0x80 || c < 0x20) ok = false;   // plain ASCII: no em or en dashes
            if (!ok) std::printf("bad help for %s: \"%s\"\n", k, h.c_str());
            CHECK(ok);
            CHECK(h[0] >= 'A' && h[0] <= 'Z' ? true : (h[0] >= '0' && h[0] <= '9'));   // sentence case
        }
        CHECK(hub::gs::find_option("fullscreen")->help.empty());   // obvious options stay without a tip
    }

    /* ---- Switch: the "Plays with" value shown is what runs ---- */
    CHECK(runner_shown("lichii", true) == "fritchii");     // Ocarina of Time, nothing chosen: Play starts Fritchii, so the combo says Fritchii
    CHECK(runner_shown("lichii", false) == "lichii");
    CHECK(runner_shown("fritchii", true) == "fritchii");
    CHECK(runner_shown("fritchii", false) == "lichii");    // (an online launch would get Lichii; the settings rows ask for offline)
    CHECK(runner_shown("hookshot", false) == "hookshot" && runner_shown("longshot", true) == "longshot");
    CHECK(runner_shown("", false) == "lichii");
    CHECK(switch_wide_runner_preview("Lichii (emulator)", true, true) == "Lichii (emulator)");
    CHECK(switch_wide_runner_preview("Lichii (emulator)", false, false) == "Lichii (emulator)");
    CHECK(switch_wide_runner_preview("Lichii (emulator)", false, true) != "Lichii (emulator)" &&
          switch_wide_runner_preview("Lichii (emulator)", false, true).find("Fritchii") != std::string::npos);

    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("hub_option_rows_test: all passed\n");
    return 0;
}
