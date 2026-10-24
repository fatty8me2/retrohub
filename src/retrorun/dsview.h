#pragma once
/* Pure helpers for Nintendo DS support in retrorun (no SDL/windows dependencies, unit-tested by retrorun_ds_test). */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace dsview {

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

/* Largest rect of aspect `aspect` (w/h) fitting an ow x oh output, centered. `integer` snaps the scale down to a whole
 * multiple of `src_h` when it is >= 1 (crisp pixels for GB/GBA); DS uses fractional scaling with linear filtering. */
inline Rect fit(int ow, int oh, int src_h, double aspect, bool integer) {
    Rect r;
    if (ow <= 0 || oh <= 0 || src_h <= 0 || aspect <= 0) return r;
    double ks = std::min(ow / (src_h * aspect), (double)oh / src_h);
    double k = (integer && ks >= 1.0) ? std::floor(ks) : ks;
    r.h = (int)std::lround(src_h * k);
    r.w = (int)std::lround(src_h * k * aspect);
    r.x = (ow - r.w) / 2;
    r.y = (oh - r.h) / 2;
    return r;
}

/* Window mouse position -> libretro pointer coordinates (-32767..32767 across the displayed frame).
 * Returns false (and leaves the outputs untouched) when the position is outside the frame. */
inline bool pointer_from_window(int mx, int my, const Rect& dst, int* px, int* py) {
    if (dst.w <= 0 || dst.h <= 0) return false;
    if (mx < dst.x || my < dst.y || mx >= dst.x + dst.w || my >= dst.y + dst.h) return false;
    double nx = (mx - dst.x + 0.5) / dst.w, ny = (my - dst.y + 0.5) / dst.h;
    *px = (int)std::lround(nx * 65534.0 - 32767.0);
    *py = (int)std::lround(ny * 65534.0 - 32767.0);
    return true;
}

/* Screen layouts cycled by F9. Index 0 is the default: top screen over the touch screen, the way DS games are designed.
 * (F9 offers side by side for wide windows, then two layouts with one large screen.) */
inline const char* layout_name(int i) {
    static const char* const k[] = {"top-bottom", "left-right", "largescreen-top", "hybrid-top"};
    return k[((i % 4) + 4) % 4];
}
inline int layout_count() { return 4; }
inline int layout_index(const std::string& name) {
    for (int i = 0; i < layout_count(); ++i)
        if (name == layout_name(i)) return i;
    return -1;
}

/* A stylus press only counts when it began over the frame, so dragging in from outside the window area does not touch. */
struct StylusLatch {
    bool prev_down = false, latched = false;
    /* Idempotent for repeated polls within a frame. */
    bool update(bool down, bool inside) {
        if (!down) latched = false;
        else if (!prev_down) latched = inside;
        prev_down = down;
        return down && latched && inside;
    }
};

/* Gamepad stylus: the right stick moves a cursor across the displayed frame (pointer coordinates -32767..32767), the
 * profile's R3 control touches. Pure; `active` turns on when the stick moves and off again when the mouse takes over. */
struct PadCursor {
    double x = 0, y = 0;     // -1..1 across the frame
    bool active = false;
    /* sx, sy: stick -1..1 (SDL orientation, down = +y); dt: seconds since the last call. */
    void step(double sx, double sy, double dt) {
        const double dead = 0.2;
        double m = std::sqrt(sx * sx + sy * sy);
        if (m <= dead) return;
        double k = (m - dead) / (1.0 - dead);
        if (k > 1.0) k = 1.0;
        k /= m;
        active = true;
        x = std::max(-1.0, std::min(1.0, x + sx * k * 1.6 * dt));
        y = std::max(-1.0, std::min(1.0, y + sy * k * 1.6 * dt));
    }
    int px() const { return (int)std::lround(x * 32767.0); }
    int py() const { return (int)std::lround(y * 32767.0); }
};

/* Core option defaults retrorun applies for the DS core (melonDS DS). Written into a callback so the test can
 * inspect them without a Core. User ini / --opt values are applied afterwards and win. */
template <class F> void for_each_default_option(F&& f) {
    f("melonds_render_mode", "software");           // no OpenGL context in retrorun
    f("melonds_console_mode", "ds");                // DSi-enhanced carts (Black/White) run in DS mode: no DSi BIOS/NAND needed
    f("melonds_sysfile_mode", "builtin");           // FreeBIOS + generated firmware: no user BIOS files
    f("melonds_boot_mode", "direct");
    f("melonds_screen_layout1", layout_name(0));   // top-bottom
    f("melonds_number_of_screen_layouts", "1");
    f("melonds_show_cursor", "disabled");           // the OS cursor is the stylus
    f("melonds_touch_mode", "touch");
    f("melonds_homebrew_sdcard", "disabled");       // no SD card images created next to the saves
    f("melonds_dsi_sdcard", "disabled");
    f("melonds_network_mode", "disabled");          // no Wi-Fi/PCap probing
    f("melonds_mic_input", "silence");
    f("melonds_battery_update_interval", "1");      // flush cart save RAM to the exposed buffer every second
    f("melonds_show_bios_warnings", "disabled");
    f("melonds_show_unsupported_features", "disabled");
    f("melonds_start_time_mode", "real");           // the RTC follows the PC clock (Pokemon day/night, events)
}

/* Options retrorun sets or changes at run time, with a value each must accept (checked against the loaded core). */
struct ReqOpt { const char* key; const char* value; };
inline const ReqOpt* required_options(int* n) {
    static const ReqOpt k[] = {
        {"melonds_screen_layout1", "top-bottom"}, {"melonds_screen_layout1", "left-right"},
        {"melonds_screen_layout1", "largescreen-top"}, {"melonds_screen_layout1", "hybrid-top"},
        {"melonds_number_of_screen_layouts", "1"}, {"melonds_touch_mode", "touch"},
        {"melonds_render_mode", "software"}, {"melonds_console_mode", "ds"}, {"melonds_sysfile_mode", "builtin"},
        {"melonds_show_cursor", "always"}, {"melonds_show_cursor", "disabled"}, {"melonds_battery_update_interval", "1"},
        {"melonds_homebrew_sdcard", "disabled"}, {"melonds_dsi_sdcard", "disabled"}, {"melonds_network_mode", "disabled"},
        {"melonds_mic_input", "silence"}, {"melonds_start_time_mode", "real"},
    };
    *n = (int)(sizeof k / sizeof k[0]);
    return k;
}

} // namespace dsview
