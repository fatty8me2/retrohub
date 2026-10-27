#pragma once
/* TV mode: pure logic (no ImGui / SDL) so the console test can check it. */
#include <algorithm>
#include <cstdio>
#include <string>

namespace hubtv {

/* The 10-foot UI is the normal UI at a larger scale: 1.0 of "design height" is 720 px, so a 1080p screen is 1.5x and 4K is 3x. */
constexpr float kDesignH = 720.f;
constexpr float kMaxScale = 4.f;

/* Scale used in TV mode: never smaller than the desktop scale, grows with the screen height. */
inline float tv_scale(float base, int display_h) {
    float s = display_h > 0 ? (float)display_h / kDesignH : base;
    return std::min(kMaxScale, std::max(base, s));
}

/* Hero banner height: in TV mode the hero is taller (up to ~half the free height, never more than 44% of the screen). */
inline float hero_height(bool tv, float normal, float avail_h, float screen_h) {
    if (!tv) return normal;
    return std::max(normal, std::min(avail_h * 0.5f, screen_h * 0.44f));
}

/* The HKCU Run value: the exe, quoted, plus the flag that starts straight into TV mode. */
inline std::wstring run_value(const std::wstring& exe) { return L"\"" + exe + L"\" --tv"; }

/* "--tv" present as its own argument on a command line. */
inline bool has_tv_flag(const std::wstring& cmdline) {
    size_t pos = 0;
    while ((pos = cmdline.find(L"--tv", pos)) != std::wstring::npos) {
        bool left = pos == 0 || cmdline[pos - 1] == L' ' || cmdline[pos - 1] == L'"';
        size_t e = pos + 4;
        bool right = e >= cmdline.size() || cmdline[e] == L' ' || cmdline[e] == L'"';
        if (left && right) return true;
        pos = e;
    }
    return false;
}

inline bool start_in_tv(bool flag, bool setting) { return flag || setting; }

/* "3:07 PM" (12 h, no leading zero) or "15:07". */
inline std::string clock_text(int hour24, int minute, bool use24 = false) {
    hour24 = std::max(0, std::min(23, hour24));
    minute = std::max(0, std::min(59, minute));
    char b[16];
    if (use24) { std::snprintf(b, sizeof b, "%02d:%02d", hour24, minute); return b; }
    int h = hour24 % 12 == 0 ? 12 : hour24 % 12;
    std::snprintf(b, sizeof b, "%d:%02d %s", h, minute, hour24 < 12 ? "AM" : "PM");
    return b;
}

/* Online-friend avatars in the corner: how many circles to draw and the "+N" overflow. */
struct AvatarPlan { int shown = 0; int extra = 0; };
inline AvatarPlan avatar_plan(int online, int max_shown) {
    AvatarPlan p;
    online = std::max(0, online);
    max_shown = std::max(0, max_shown);
    if (online <= max_shown) { p.shown = online; return p; }
    p.shown = std::max(0, max_shown - 1);   // the last slot becomes the "+N" bubble
    p.extra = online - p.shown;
    return p;
}

/* The window rectangle to put back when TV mode ends. */
struct WinRect { int x = 0, y = 0, w = 0, h = 0; bool maximized = false; bool valid = false; };

}  // namespace hubtv
