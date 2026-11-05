#pragma once
/* Plan 4 Wave 3 D1/D2: pure window-placement and UI-scale logic (no SDL, no Win32), unit tested in test_display.cpp.
 * display.cpp turns the real monitors and window into these types. All numbers are physical pixels in virtual-screen coordinates. */
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace hubdisplay {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
};

struct Monitor {
    std::string name;      // device name, e.g. "\\.\DISPLAY2"
    Rect bounds;           // the whole monitor
    Rect work;             // minus the taskbar
    bool primary = false;
};

/* What hub.json remembers about the window (Settings win_x/y/w/h/win_max/win_monitor). */
struct Saved {
    Rect rect;             // the NORMAL (not maximized) rect; w/h 0 = never saved
    bool maximized = false;
    std::string monitor;   // "" = primary
};

struct Placement {
    Rect rect;             // where to create the window (the normal rect)
    bool maximized = false;
    std::string monitor;   // device name of the monitor it lands on
    bool restored = false; // true = the saved position was used as is; false = centred (first run, monitor gone, off screen)
};

constexpr int kMinW = 960, kMinH = 600;     // design minimum at scale 1.0
constexpr int kDefaultW = 1280, kDefaultH = 780;

inline Rect intersect(const Rect& a, const Rect& b) {
    const int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y), x1 = std::min(a.right(), b.right()), y1 = std::min(a.bottom(), b.bottom());
    Rect r;
    if (x1 > x0 && y1 > y0) { r.x = x0; r.y = y0; r.w = x1 - x0; r.h = y1 - y0; }
    return r;
}

inline const Monitor* find_monitor(const std::vector<Monitor>& mons, const std::string& name) {
    if (name.empty()) return nullptr;
    for (const Monitor& m : mons) if (m.name == name) return &m;
    return nullptr;
}

inline const Monitor* primary_monitor(const std::vector<Monitor>& mons) {
    for (const Monitor& m : mons) if (m.primary) return &m;
    return mons.empty() ? nullptr : &mons[0];
}

/* The window is "visible" when enough of it is on the monitor's work area to grab and move: at least 200x100 px (or half of a smaller
 * window) overlaps, and its title bar (the top edge) is inside the work area vertically. */
inline bool rect_visible_on(const Rect& r, const Monitor& m) {
    if (r.w <= 0 || r.h <= 0) return false;
    const Rect o = intersect(r, m.work);
    if (o.w < std::min(200, r.w / 2) || o.h < std::min(100, r.h / 2)) return false;
    return r.y >= m.work.y && r.y < m.work.bottom() - 40;
}

inline bool visible_on_any(const Rect& r, const std::vector<Monitor>& mons) {
    for (const Monitor& m : mons) if (rect_visible_on(r, m)) return true;
    return false;
}

/* The monitor holding most of a rect (by overlap with the monitor, not the work area); null when it is off every screen. */
inline const Monitor* monitor_of(const Rect& r, const std::vector<Monitor>& mons) {
    const Monitor* best = nullptr;
    long long best_area = 0;
    for (const Monitor& m : mons) {
        const Rect o = intersect(r, m.bounds);
        const long long a = (long long)o.w * o.h;
        if (a > best_area) { best_area = a; best = &m; }
    }
    return best;
}

/* Minimum window size in px for a UI scale, clamped to what the work area can hold (L3: a 768-high panel at 125%, or 200% on 1080p,
 * could not fit the old fixed minimum). */
inline Rect min_size_for(float scale, const Rect& work) {
    Rect r;
    r.w = (int)std::lround(kMinW * scale);
    r.h = (int)std::lround(kMinH * scale);
    if (work.w > 0) r.w = std::min(r.w, work.w);
    if (work.h > 0) r.h = std::min(r.h, work.h);
    r.w = std::max(r.w, 320);
    r.h = std::max(r.h, 240);
    return r;
}

inline Rect centred_on(const Monitor& m, int w, int h) {
    Rect r;
    r.w = std::max(1, std::min(w, m.work.w));
    r.h = std::max(1, std::min(h, m.work.h));
    r.x = m.work.x + (m.work.w - r.w) / 2;
    r.y = m.work.y + (m.work.h - r.h) / 2;
    return r;
}

/* Where to open at start-up.
 *   - remember off, or nothing saved: centred on the chosen monitor (saved.monitor) when it still exists, else the primary one;
 *   - the saved monitor is gone: centred on the primary monitor (saved size kept, clamped to fit);
 *   - the saved rect is no longer visible on that monitor (resolution or layout changed): centred on the primary monitor;
 *   - maximized: only the monitor has to exist (the rect is just the size to return to), the window opens maximized on it;
 *   - otherwise: exactly the saved rect, clamped to the work area. */
inline Placement plan_start(const Saved& saved, bool remember, const std::vector<Monitor>& mons, int def_w, int def_h) {
    Placement p;
    const Monitor* prim = primary_monitor(mons);
    if (!prim) {   // no monitor info at all: let the caller fall back to its own default
        p.rect = Rect{0, 0, def_w, def_h};
        return p;
    }
    const Monitor* chosen = find_monitor(mons, saved.monitor);
    const bool have_size = saved.rect.w > 0 && saved.rect.h > 0;
    if (!remember || !have_size) {
        const Monitor* t = chosen ? chosen : prim;
        p.rect = centred_on(*t, def_w, def_h);
        p.monitor = t->name;
        return p;
    }
    const int w = saved.rect.w, h = saved.rect.h;
    if (!chosen) {   // that monitor was unplugged (or never named): the primary one
        p.rect = centred_on(*prim, w, h);
        p.monitor = prim->name;
        p.maximized = saved.maximized;
        return p;
    }
    if (saved.maximized) {   // open maximized on the saved monitor; the normal rect is only where "restore" goes
        Rect r = saved.rect;
        if (!rect_visible_on(r, *chosen)) r = centred_on(*chosen, w, h);
        p.rect = r;
        p.maximized = true;
        p.monitor = chosen->name;
        p.restored = true;
        return p;
    }
    if (!rect_visible_on(saved.rect, *chosen)) {
        p.rect = centred_on(*prim, w, h);
        p.monitor = prim->name;
        return p;
    }
    Rect r = saved.rect;
    r.w = std::min(r.w, chosen->work.w);
    r.h = std::min(r.h, chosen->work.h);
    p.rect = r;
    p.monitor = chosen->name;
    p.restored = true;
    return p;
}

/* Settings > "Open on": the monitor to move the window to now. Returns the centred rect on the named monitor (keeping the current size
 * when it fits), or an empty rect (w == 0) when that monitor does not exist (the caller then does nothing). */
inline Rect move_target(const std::string& monitor, const Rect& cur, const std::vector<Monitor>& mons) {
    const Monitor* m = monitor.empty() ? primary_monitor(mons) : find_monitor(mons, monitor);
    if (!m) return Rect{};
    return centred_on(*m, cur.w > 0 ? cur.w : kDefaultW, cur.h > 0 ? cur.h : kDefaultH);
}

/* Final scale: the monitor's DPI scale times the player's UI size (percent, 80..200 in steps of 10). */
inline int clamp_ui_scale(int pct) {
    pct = std::max(80, std::min(200, pct));
    return ((pct + 5) / 10) * 10;
}
inline float effective_scale(float dpi_scale, int ui_scale_pct) {
    return std::max(0.5f, dpi_scale) * (float)clamp_ui_scale(ui_scale_pct) / 100.f;
}

/* Windows "Show animations" off means the player wants calm: the FIRST time only (no reduce_motion key in hub.json yet) it becomes the
 * initial value of the Reduce motion setting. After that the setting is the player's own. */
inline bool initial_reduce_motion(bool key_already_saved, bool saved_value, bool os_animations_on) {
    return key_already_saved ? saved_value : !os_animations_on;
}

}  // namespace hubdisplay
