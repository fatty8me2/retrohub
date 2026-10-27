#pragma once
/* Attract mode (Wave 3 L6), pure logic: the idle timer and the slideshow maths. No ImGui, no files.
 *
 * After kIdleSecs without any input (window focused, Home page, nothing else going on) the Home hero turns into a slow
 * Ken Burns slideshow of the player's own save-state thumbnails and screenshots. Any input ends it at once. */

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace hubui::attract {

constexpr double kIdleSecs = 60.0;    // idle time before attract mode starts
constexpr double kSlideSecs = 7.0;    // time one picture stays (including its fade)
constexpr double kFadeSecs = 1.4;     // crossfade length at the end of a slide
constexpr size_t kMaxGames = 8;       // recently played games to draw pictures from
constexpr size_t kPerGame = 3;        // pictures per game

/* Idle tracker. step() is called once per frame; returns true on the frame attract mode ENDS (so the caller can hand the focus back). */
struct Idle {
    double idle = 0;
    bool active = false;
    double shown = 0;   // seconds attract mode has been running (drives the slideshow clock)
    bool step(double dt, bool input, bool eligible, double threshold = kIdleSecs) {
        if (!eligible || input) {
            const bool was = active;
            idle = 0;
            shown = 0;
            active = false;
            return was;
        }
        idle += dt;
        if (active) shown += dt;
        else if (idle >= threshold) { active = true; shown = 0; }
        return false;
    }
};

/* Which slide is showing at time t and how far the crossfade to the next one has got (0 = not fading). */
struct SlideFrame { int cur = -1, next = -1; float mix = 0.f; };
inline SlideFrame slide_at(double t, int n, double slide = kSlideSecs, double fade = kFadeSecs) {
    SlideFrame f;
    if (n <= 0 || t < 0) return f;
    long long k = (long long)std::floor(t / slide);
    double local = t - (double)k * slide;
    f.cur = (int)(k % n);
    f.next = (f.cur + 1) % n;
    if (n > 1 && local > slide - fade) f.mix = (float)((local - (slide - fade)) / fade);
    if (f.mix > 1.f) f.mix = 1.f;
    return f;
}

/* Texture window (uv rectangle). */
struct UV { float u0 = 0, v0 = 0, u1 = 1, v1 = 1; };

/* The part of a texture that fills a box without stretching (what "cover" shows). */
inline UV cover_uv(float tex_w, float tex_h, float box_w, float box_h) {
    UV r;
    if (tex_w <= 0 || tex_h <= 0 || box_w <= 0 || box_h <= 0) return r;
    const float ta = tex_w / tex_h, ba = box_w / box_h;
    if (ta > ba) {   // texture is wider: crop the sides
        const float w = ba / ta;
        r.u0 = (1.f - w) * 0.5f;
        r.u1 = r.u0 + w;
    } else {         // taller: crop top and bottom
        const float h = ta / ba;
        r.v0 = (1.f - h) * 0.5f;
        r.v1 = r.v0 + h;
    }
    return r;
}

inline float smooth01(float u) { u = std::max(0.f, std::min(1.f, u)); return u * u * (3.f - 2.f * u); }

/* Ken Burns window for picture number `idx` at progress u (0..1 over the time it is on screen, fades included).
 * Alternates zooming in and out; the pan direction depends on the picture. Always stays inside the texture. */
inline UV ken_burns(int idx, float u, const UV& cover) {
    const float s = smooth01(u);
    const bool in = (idx & 1) == 0;
    const float zoom = in ? 1.0f + 0.14f * s : 1.14f - 0.14f * s;   // 1.0 .. 1.14
    static const float dx[4] = {1.f, -1.f, 1.f, -1.f}, dy[4] = {-1.f, -1.f, 1.f, 1.f};
    const float pan = in ? s : 1.f - s;   // 0..1 along the pan path
    const float cw = cover.u1 - cover.u0, ch = cover.v1 - cover.v0;
    const float w = cw / zoom, h = ch / zoom;
    const float slack_x = cw - w, slack_y = ch - h;   // room left inside the cover window
    const float cx = (cover.u0 + cover.u1) * 0.5f + dx[idx & 3] * (pan - 0.5f) * slack_x;
    const float cy = (cover.v0 + cover.v1) * 0.5f + dy[idx & 3] * (pan - 0.5f) * slack_y;
    UV r;
    r.u0 = std::max(cover.u0, std::min(cover.u1 - w, cx - w * 0.5f));
    r.v0 = std::max(cover.v0, std::min(cover.v1 - h, cy - h * 0.5f));
    r.u1 = r.u0 + w;
    r.v1 = r.v0 + h;
    return r;
}

/* One picture in the slideshow. */
struct Slide {
    std::string key, title, path;
    long long when = 0;      // unix seconds the picture was made (0 = unknown)
    bool is_state = false;   // save-state thumbnail (else a screenshot)
};

/* Interleaves the pictures of several games so consecutive slides come from different games:
 * game1.pic1, game2.pic1, ..., game1.pic2, ... `per_game` is each game's list, most recent game first. */
inline std::vector<Slide> interleave(const std::vector<std::vector<Slide>>& per_game) {
    std::vector<Slide> out;
    for (size_t round = 0;; ++round) {
        bool any = false;
        for (auto& g : per_game)
            if (round < g.size()) { out.push_back(g[round]); any = true; }
        if (!any) break;
    }
    return out;
}

}  // namespace hubui::attract
