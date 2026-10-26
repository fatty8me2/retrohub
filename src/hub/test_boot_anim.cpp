/* Console test for the game start animation: media mapping, timeline, launch-mode rules, state machine and the settings round trip. */
#include <cstdio>
#include <cmath>
#include "boot_logic.h"
#include "logic.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace bootanim;

int main() {
    /* media per system */
    CHECK(media_for(hub::System::N64) == Media::Cartridge);
    CHECK(media_for(hub::System::GB) == Media::SmallCart && media_for(hub::System::GBC) == Media::SmallCart && media_for(hub::System::GBA) == Media::SmallCart);
    CHECK(media_for(hub::System::NDS) == Media::Card && media_for(hub::System::N3DS) == Media::Card);
    CHECK(media_for(hub::System::GC) == Media::MiniDisc);
    CHECK(media_for(hub::System::PS1) == Media::Disc && media_for(hub::System::PS2) == Media::Disc && media_for(hub::System::WiiU) == Media::Disc);
    CHECK(media_for(hub::System::Unknown) == Media::None);

    /* timeline: full is 2.5-3.5 s, short ~1 s, phases in order and contiguous */
    Timeline full = timeline_for(false), shrt = timeline_for(true);
    CHECK(full.total() >= 4.5f && full.total() <= 6.0f);
    CHECK(std::fabs(shrt.total() - 1.0f) < 0.01f);
    CHECK(phase_at(full, -0.1f) == Phase::Idle);
    CHECK(phase_at(full, 0.f) == Phase::Slide);
    CHECK(phase_at(full, full.slide + 0.001f) == Phase::Fly);
    CHECK(phase_at(full, full.slide + full.fly + 0.001f) == Phase::Insert);
    CHECK(phase_at(full, full.slide + full.fly + full.insert + 0.001f) == Phase::Power);
    CHECK(phase_at(full, full.total() - 0.001f) == Phase::Fade);
    CHECK(phase_at(full, full.total()) == Phase::Done);
    CHECK(std::fabs(phase_progress(full, full.slide * 0.5f) - 0.5f) < 1e-4f);
    CHECK(phase_progress(full, full.total() + 1.f) == 1.f);
    float last = -1;
    for (float t = 0; t < full.total(); t += 0.01f) { int p = (int)phase_at(full, t); CHECK(p >= last); last = (float)p; }

    /* easing */
    CHECK(ease_in_out(0.f) == 0.f && ease_in_out(1.f) == 1.f && std::fabs(ease_in_out(0.5f) - 0.5f) < 1e-5f);
    CHECK(std::fabs(ease_out_back(1.f) - 1.f) < 1e-4f && ease_out_back(0.f) == 0.f && ease_out_back(0.7f) > 1.f);
    CHECK(ease_in_out(-3.f) == 0.f && ease_in_out(9.f) == 1.f);

    /* launch mode */
    Options o;
    CHECK(choose_mode(o, false, false) == Mode::Full);
    CHECK(choose_mode(o, false, true) == Mode::Short);
    CHECK(choose_mode(o, true, false) == Mode::Skip);   // online skipped by default
    o.skip_online = false;
    CHECK(choose_mode(o, true, false) == Mode::Short);  // never the full version online
    CHECK(choose_mode(o, true, true) == Mode::Short);
    o.short_version = true;
    CHECK(choose_mode(o, false, false) == Mode::Short);
    o.enabled = false;
    CHECK(choose_mode(o, false, false) == Mode::Skip && choose_mode(o, true, true) == Mode::Skip);

    /* state machine: normal run finishes exactly once, sound starts once at the Power phase */
    {
        Runner r;
        r.begin(Mode::Full);
        CHECK(r.running && r.phase() == Phase::Slide);
        int fin = 0, snd = 0;
        float tt = 0;
        for (int i = 0; i < 1000 && r.running; ++i) {
            if (r.want_sound()) { ++snd; CHECK(r.phase() == Phase::Power); }
            if (r.tick(1.f / 60.f)) ++fin;
            tt += 1.f / 60.f;
        }
        CHECK(fin == 1 && snd == 1 && !r.running && !r.skipped);
        CHECK(tt > full.total() - 0.1f && tt < full.total() + 0.2f);
        CHECK(!r.tick(0.016f) && !r.skip());   // no second completion
    }
    /* a skip on the very first frame is ignored (the press that started it), later skips finish immediately and once */
    {
        Runner r;
        r.begin(Mode::Full);
        CHECK(!r.skip() && r.running);
        CHECK(!r.tick(0.016f) && r.t == 0.f);
        CHECK(!r.tick(0.5f) && r.running);
        CHECK(r.skip() && !r.running && r.skipped);
        CHECK(!r.skip());
    }
    /* a long hitch cannot eat the whole animation */
    {
        Runner r;
        r.begin(Mode::Full);
        r.tick(0.016f);
        r.tick(5.f);
        CHECK(r.running && r.t <= 0.11f);
    }
    /* Skip mode is finished immediately; short run is ~1 s */
    {
        Runner r;
        r.begin(Mode::Skip);
        CHECK(!r.running && r.finished_ && r.phase() == Phase::Done);
        Runner s;
        s.begin(Mode::Short);
        int n = 0;
        while (s.running && n < 1000) { s.tick(1.f / 60.f); ++n; }
        CHECK(n >= 55 && n <= 70);
    }

    /* settings: defaults, round trip, clamping */
    {
        hubui::Settings d;
        CHECK(d.boot_anim && d.boot_sounds && !d.boot_short && d.boot_skip_online && d.boot_volume == 70);
        hubui::Settings g;
        g.boot_anim = false; g.boot_sounds = false; g.boot_short = true; g.boot_skip_online = false; g.boot_volume = 25;
        hubui::Settings h;
        CHECK(hubui::settings_from_json(hubui::settings_to_json(g), &h));
        CHECK(!h.boot_anim && !h.boot_sounds && h.boot_short && !h.boot_skip_online && h.boot_volume == 25);
        hubui::Settings k;
        CHECK(hubui::settings_from_json("{\"name\":\"x\"}", &k) && k.boot_anim && k.boot_volume == 70);   // old settings files keep the defaults
        hubui::Settings m;
        CHECK(hubui::settings_from_json("{\"boot_volume\": 900}", &m) && m.boot_volume == 100);
    }

    std::printf(fails ? "boot_anim test: %d FAILED\n" : "boot_anim test: all passed\n", fails);
    return fails ? 1 : 0;
}
