/* Window placement and monitor glue: see display.h. */
#include "app.h"
#include "display.h"

namespace hubdisplay {

namespace {

Rect to_rect(const RECT& r) { return Rect{(int)r.left, (int)r.top, (int)(r.right - r.left), (int)(r.bottom - r.top)}; }

BOOL CALLBACK enum_cb(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(hm, &mi)) return TRUE;
    Monitor m;
    m.name = U(std::wstring(mi.szDevice));
    m.bounds = to_rect(mi.rcMonitor);
    m.work = to_rect(mi.rcWork);
    m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    ((std::vector<Monitor>*)lp)->push_back(m);
    return TRUE;
}

float g_dpi = 1.f;
bool g_changed = false, g_display_changed = false;
ULONGLONG g_event_ms = 0;
std::string g_last_monitor_seen;

bool fl_normal(Uint32 fl) { return !(fl & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)); }

const Monitor* monitor_of_window(SDL_Window* win, const std::vector<Monitor>& mons) {
    if (!win) return nullptr;
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(win, &x, &y);
    SDL_GetWindowSize(win, &w, &h);
    return monitor_of(Rect{x, y, w, h}, mons);
}

}  // namespace

std::vector<Monitor> monitors() {
    std::vector<Monitor> v;
    EnumDisplayMonitors(nullptr, nullptr, enum_cb, (LPARAM)&v);
    std::stable_sort(v.begin(), v.end(), [](const Monitor& a, const Monitor& b) { return a.name < b.name; });
    return v;
}

std::string monitor_label(const Monitor& m) {
    int n = 0;
    for (char c : m.name) if (c >= '0' && c <= '9') n = n * 10 + (c - '0');
    std::string s = "Monitor " + std::to_string(n > 0 ? n : 1) + " (" + std::to_string(m.bounds.w) + "x" + std::to_string(m.bounds.h) + ")";
    if (m.primary) s += " - main";
    return s;
}

float dpi_scale_for(const Monitor* m) {
    if (!m) return 1.f;
    const int n = SDL_GetNumVideoDisplays();
    for (int i = 0; i < n; ++i) {
        SDL_Rect b;
        if (SDL_GetDisplayBounds(i, &b) == 0 && b.x == m->bounds.x && b.y == m->bounds.y) {
            float d = 96.f;
            if (SDL_GetDisplayDPI(i, &d, nullptr, nullptr) == 0 && d > 0.f) return std::max(1.0f, d / 96.0f);
        }
    }
    return 1.f;
}

float dpi_scale() { return g_dpi; }
void set_dpi_scale(float s) { g_dpi = std::max(0.5f, s); }

bool os_animations_on() {
    BOOL on = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) return on != FALSE;
    return true;
}

Rect work_area_of(SDL_Window* win) {
    const std::vector<Monitor> mons = monitors();
    const Monitor* m = monitor_of_window(win, mons);
    if (!m) m = primary_monitor(mons);
    return m ? m->work : Rect{};
}

void apply_min_size(SDL_Window* win, float scale) {
    if (!win) return;
    const Rect mn = min_size_for(scale, work_area_of(win));
    SDL_SetWindowMinimumSize(win, mn.w, mn.h);
}

void note_event(const SDL_Event& e) {
    if (g_headless || e.type != SDL_WINDOWEVENT) return;
    switch (e.window.event) {
        case SDL_WINDOWEVENT_MOVED: case SDL_WINDOWEVENT_RESIZED: case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_MAXIMIZED: case SDL_WINDOWEVENT_RESTORED:
            g_changed = true;
            g_event_ms = GetTickCount64();
            break;
        case SDL_WINDOWEVENT_DISPLAY_CHANGED:
            g_display_changed = true;
            g_changed = true;
            g_event_ms = GetTickCount64();
            break;
        default: break;
    }
}

bool tick(SDL_Window* win, bool force, bool tv_active) {
    if (g_headless || !win || !A.st.remember_window) return false;
    if (!force && (!g_changed || GetTickCount64() - g_event_ms < 400)) return false;   // let a drag settle first
    g_changed = false;
    if (tv_active) return false;   // the fullscreen rect is not the window
    const Uint32 fl = SDL_GetWindowFlags(win);
    if (fl & SDL_WINDOW_MINIMIZED) return false;
    if (fl & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) return false;
    const std::vector<Monitor> mons = monitors();
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(win, &x, &y);
    SDL_GetWindowSize(win, &w, &h);
    const Monitor* m = monitor_of(Rect{x, y, w, h}, mons);
    const std::string name = m ? m->name : A.st.win_monitor;
    bool changed = false;
    const bool maxd = (fl & SDL_WINDOW_MAXIMIZED) != 0;
    if (A.st.win_max != maxd) { A.st.win_max = maxd; changed = true; }
    if (name != A.st.win_monitor) { A.st.win_monitor = name; changed = true; }
    if (fl_normal(fl) && w > 0 && h > 0 && (A.st.win_x != x || A.st.win_y != y || A.st.win_w != w || A.st.win_h != h)) {
        A.st.win_x = x; A.st.win_y = y; A.st.win_w = w; A.st.win_h = h;
        changed = true;
    }
    if (changed) mark_dirty();
    return changed;
}

float take_display_changed(SDL_Window* win) {
    if (!g_display_changed) return 0.f;
    g_display_changed = false;
    const std::vector<Monitor> mons = monitors();
    const Monitor* m = monitor_of_window(win, mons);
    if (!m) return 0.f;
    if (m->name == g_last_monitor_seen) return 0.f;
    const bool first = g_last_monitor_seen.empty();
    g_last_monitor_seen = m->name;
    return first ? 0.f : dpi_scale_for(m);
}

bool move_to_monitor(SDL_Window* win, const std::string& monitor) {
    if (g_headless || !win) return false;
    const std::vector<Monitor> mons = monitors();
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(win, &x, &y);
    SDL_GetWindowSize(win, &w, &h);
    const Rect t = move_target(monitor, Rect{x, y, w, h}, mons);
    if (t.w <= 0) return false;
    const Uint32 fl = SDL_GetWindowFlags(win);
    if (fl & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MINIMIZED)) return false;
    if (!(fl & SDL_WINDOW_MAXIMIZED)) SDL_SetWindowSize(win, t.w, t.h);
    SDL_SetWindowPosition(win, t.x, t.y);   // a maximized window stays maximized and follows to the new monitor
    if (fl & SDL_WINDOW_MAXIMIZED) SDL_MaximizeWindow(win);
    g_changed = true;
    g_event_ms = GetTickCount64();
    return true;
}

void prepare_fullscreen_monitor(SDL_Window* win) {
    if (g_headless || !win || A.st.win_monitor.empty()) return;
    const std::vector<Monitor> mons = monitors();
    const Monitor* want = find_monitor(mons, A.st.win_monitor);
    if (!want) return;
    const Monitor* cur = monitor_of_window(win, mons);
    if (cur && cur->name == want->name) return;
    SDL_SetWindowPosition(win, want->work.x + 40, want->work.y + 40);
}

}  // namespace hubdisplay
