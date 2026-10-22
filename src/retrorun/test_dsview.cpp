// Console test for retrorun's DS helpers (dsview.h). Exit code 0 = all passed.
#include "dsview.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <map>
#include <string>

static int g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } \
    } while (0)

int main() {
    using namespace dsview;
    // Fractional fit: 512x192 (left-right) in 1920x1080 -> 1920x720, centered vertically.
    Rect r = fit(1920, 1080, 192, 512.0 / 192.0, false);
    CHECK(r.w == 1920 && r.h == 720 && r.x == 0 && r.y == 180);
    // Integer fit keeps whole multiples (GBA 240x160 in 1920x1080 -> 6x = 1440x960).
    Rect g = fit(1920, 1080, 160, 1.5, true);
    CHECK(g.w == 1440 && g.h == 960 && g.x == 240 && g.y == 60);
    // Smaller than 1x never snaps up.
    Rect s = fit(100, 100, 160, 1.5, true);
    CHECK(s.w <= 100 && s.h <= 100 && s.w > 0);
    CHECK(fit(0, 10, 10, 1.0, false).w == 0);

    // Pointer mapping across the displayed frame.
    int px = 0, py = 0;
    CHECK(pointer_from_window(960, 540, r, &px, &py));
    CHECK(std::abs(px) <= 100 && std::abs(py) <= 100);      // frame centre
    CHECK(pointer_from_window(0, 180, r, &px, &py) && px < -32000 && py < -32000);   // top-left corner
    CHECK(pointer_from_window(1919, 899, r, &px, &py) && px > 32000 && py > 32000);  // bottom-right corner
    px = 7; py = 9;
    CHECK(!pointer_from_window(10, 100, r, &px, &py) && px == 7 && py == 9);          // letterbox bar: not touching
    CHECK(!pointer_from_window(1920, 500, r, &px, &py));

    // Layout cycle wraps and starts as a vertical stack (top screen over the touch screen).
    CHECK(std::string(layout_name(0)) == "top-bottom" && std::string(layout_name(1)) == "left-right");
    CHECK(layout_index("hybrid-top") == 3 && layout_index("bogus") == -1 && layout_index(layout_name(2)) == 2);
    CHECK(std::string(layout_name(layout_count())) == layout_name(0));
    CHECK(std::string(layout_name(-1)) == layout_name(layout_count() - 1));

    // Defaults: software rendering, no user BIOS, no SD card images, single layout slot.
    std::map<std::string, std::string> d;
    for_each_default_option([&](const char* k, const char* v) { d[k] = v; });
    CHECK(d["melonds_render_mode"] == "software");
    CHECK(d["melonds_sysfile_mode"] == "builtin");
    CHECK(d["melonds_console_mode"] == "ds");
    CHECK(d["melonds_homebrew_sdcard"] == "disabled" && d["melonds_dsi_sdcard"] == "disabled");
    CHECK(d["melonds_number_of_screen_layouts"] == "1" && d["melonds_screen_layout1"] == "top-bottom");
    CHECK(d["melonds_start_time_mode"] == "real");

    // Stylus latch: a press that begins outside the frame never touches, even after dragging inside; one that begins
    // inside keeps touching only while over the frame.
    {
        StylusLatch l;
        CHECK(!l.update(false, false));
        CHECK(!l.update(true, false));    // button down outside
        CHECK(!l.update(true, true));     // dragged in: still no touch
        CHECK(!l.update(false, true));    // released
        CHECK(l.update(true, true));      // new press inside
        CHECK(l.update(true, true));      // repeated poll in the same frame
        CHECK(!l.update(true, false));    // dragged out: lifts
        CHECK(l.update(true, true));      // back in while still held: touches again
        CHECK(!l.update(false, true));
    }
    // Every option key/value retrorun relies on is listed for the runtime check against the real core, with no duplicates of a
    // layout name missing: all four F9 layouts must be covered.
    {
        int n = 0;
        const ReqOpt* q = required_options(&n);
        for (int i = 0; i < layout_count(); ++i) {
            bool found = false;
            for (int k = 0; k < n; ++k)
                if (std::string(q[k].key) == "melonds_screen_layout1" && std::string(q[k].value) == layout_name(i)) found = true;
            CHECK(found);
        }
        std::map<std::string, std::string> dd;
        for_each_default_option([&](const char* k, const char* v) { dd[k] = v; });
        for (auto& kv : dd) {
            bool found = false;
            for (int k = 0; k < n; ++k)
                if (kv.first == q[k].key && kv.second == q[k].value) found = true;
            if (kv.first == "melonds_screen_layout1") found = true;   // covered above
            if (kv.first == "melonds_show_bios_warnings" || kv.first == "melonds_show_unsupported_features" || kv.first == "melonds_boot_mode") found = true;
            CHECK(found);   // each default is also verified against the core at startup
        }
    }

    std::printf(g_fail ? "retrorun_ds_test: %d FAILED\n" : "retrorun_ds_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
