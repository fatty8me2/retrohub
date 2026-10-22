// Console test for retrorun's DS helpers (dsview.h). Exit code 0 = all passed.
#include "dsview.h"

#include <cstdio>
#include <cstdlib>
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

    // Layout cycle wraps and starts side-by-side.
    CHECK(std::string(layout_name(0)) == "left-right");
    CHECK(std::string(layout_name(layout_count())) == layout_name(0));
    CHECK(std::string(layout_name(-1)) == layout_name(layout_count() - 1));

    // Defaults: software rendering, no user BIOS, no SD card images, single layout slot.
    std::map<std::string, std::string> d;
    for_each_default_option([&](const char* k, const char* v) { d[k] = v; });
    CHECK(d["melonds_render_mode"] == "software");
    CHECK(d["melonds_sysfile_mode"] == "builtin");
    CHECK(d["melonds_console_mode"] == "ds");
    CHECK(d["melonds_homebrew_sdcard"] == "disabled" && d["melonds_dsi_sdcard"] == "disabled");
    CHECK(d["melonds_number_of_screen_layouts"] == "1" && d["melonds_screen_layout1"] == "left-right");
    CHECK(d["melonds_start_time_mode"] == "real");

    std::printf(g_fail ? "retrorun_ds_test: %d FAILED\n" : "retrorun_ds_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
