#include "shelf_logic.h"

#include <algorithm>
#include <cmath>

namespace hubui::shelf {

namespace {
constexpr float kPi = 3.14159265358979f;
}

Media media_for(const std::string& s) {
    if (s == "nes") return Media::NesCart;
    if (s == "snes") return Media::SnesCart;
    if (s == "n64") return Media::N64Cart;
    if (s == "gb") return Media::GbCart;
    if (s == "gbc") return Media::GbcCart;
    if (s == "gba") return Media::GbaCart;
    if (s == "nds") return Media::DsCase;
    if (s == "3ds") return Media::N3dsCase;
    if (s == "ps1") return Media::Ps1Jewel;
    if (s == "ps2") return Media::Ps2Case;
    if (s == "gc") return Media::GcCase;
    if (s == "wiiu") return Media::WiiUCase;
    if (s == "psp") return Media::PspCase;
    if (s == "switch") return Media::SwitchCase;
    if (s == "switch2") return Media::SwitchCase;   // a console id: the Switch 2 case is the Switch case with the Switch 2 banner
    return Media::Generic;
}

const char* media_key(Media m) {
    switch (m) {
        case Media::NesCart: return "nes-cart";
        case Media::SnesCart: return "snes-cart";
        case Media::N64Cart: return "n64-cart";
        case Media::GbCart: return "gb-cart";
        case Media::GbcCart: return "gbc-cart";
        case Media::GbaCart: return "gba-cart";
        case Media::DsCase: return "ds-case";
        case Media::N3dsCase: return "3ds-case";
        case Media::Ps1Jewel: return "ps1-jewel";
        case Media::Ps2Case: return "ps2-case";
        case Media::GcCase: return "gc-case";
        case Media::WiiUCase: return "wiiu-case";
        case Media::PspCase: return "psp-case";
        case Media::SwitchCase: return "switch-case";
        default: return "keep-case";
    }
}

bool is_cart(Media m) { return m >= Media::NesCart && m <= Media::GbaCart; }

Dims base_dims(Media m) {
    const float cart = 0.17f, face = 0.f, spine = -kPi * 0.5f;
    switch (m) {
        case Media::NesCart:  return {0.84f, 0.94f, cart, face};
        case Media::SnesCart: return {0.98f, 0.80f, cart, face};
        case Media::N64Cart:  return {0.86f, 0.68f, cart, face};
        case Media::GbCart:   return {0.60f, 0.70f, 0.13f, face};
        case Media::GbcCart:  return {0.60f, 0.70f, 0.13f, face};
        case Media::GbaCart:  return {0.64f, 0.54f, 0.12f, face};
        case Media::DsCase:   return {0.56f, 0.70f, 0.145f, spine};
        case Media::N3dsCase: return {0.56f, 0.70f, 0.14f, spine};
        case Media::Ps1Jewel: return {0.74f, 0.80f, 0.145f, spine};
        case Media::Ps2Case:  return {0.70f, 1.00f, 0.17f, spine};
        case Media::GcCase:   return {0.62f, 0.84f, 0.155f, spine};
        case Media::WiiUCase: return {0.74f, 0.98f, 0.17f, spine};
        case Media::PspCase:  return {0.54f, 0.76f, 0.145f, spine};
        case Media::SwitchCase: return {0.52f, 0.90f, 0.12f, spine};
        default:              return {0.70f, 0.94f, 0.155f, spine};
    }
}

uint32_t hash32(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}

Dims item_dims(Media m, uint32_t seed) {
    Dims d = base_dims(m);
    const float a = (float)(seed & 0xFF) / 255.f, b = (float)((seed >> 8) & 0xFF) / 255.f;   // 0..1
    if (is_cart(m)) {
        d.d *= 0.92f + 0.16f * b;
    } else {
        d.h *= 0.95f + 0.05f * a;     // cases differ a little in height ...
        d.d *= 0.90f + 0.20f * b;     // ... and thickness
    }
    return d;
}

float rest_width(const Dims& d) { return d.rest_theta < -0.5f ? d.d : d.w; }

float slot_gap(Media m) { return is_cart(m) ? 0.06f : 0.012f; }

/* ---------------------------------- rows ---------------------------------- */

Layout pack_rows(const std::vector<float>& slot_w, const std::vector<int>& sec_sizes, float inner_w, float margin_x, float row_h, float top, float bottom) {
    Layout out;
    const int n = (int)slot_w.size();
    out.x.assign((size_t)n, 0.f);
    out.w = slot_w;
    out.L.item_row.assign((size_t)n, 0);
    out.L.cols = 1;
    const float right = inner_w - margin_x;
    int item = 0;
    for (size_t s = 0; s < sec_sizes.size(); ++s) {
        const int cnt = std::min(sec_sizes[s], n - item);
        if (cnt <= 0) continue;
        bool first = true;
        float cx = margin_x;
        VRow row;
        row.kind = 1;
        row.first = item;
        row.count = 0;
        for (int k = 0; k < cnt; ++k, ++item) {
            const float w = slot_w[(size_t)item];
            if (row.count > 0 && cx + w > right) {   // this one does not fit: close the row, open the next
                out.L.rows.push_back(row);
                out.row_sec.push_back((int)s);
                out.row_first.push_back(first ? 1 : 0);
                first = false;
                row = VRow();
                row.kind = 1;
                row.first = item;
                row.count = 0;
                cx = margin_x;
            }
            out.x[(size_t)item] = cx;
            out.L.item_row[(size_t)item] = (int)out.L.rows.size();
            cx += w;
            ++row.count;
        }
        out.L.rows.push_back(row);
        out.row_sec.push_back((int)s);
        out.row_first.push_back(first ? 1 : 0);
    }
    for (size_t r = 0; r < out.L.rows.size(); ++r) {
        out.L.rows[r].y = top + (float)r * row_h;
        out.L.rows[r].h = row_h;
    }
    out.L.total_h = top + (float)out.L.rows.size() * row_h + bottom;
    return out;
}

int row_of(const Layout& l, int item) {
    if (l.L.item_row.empty()) return 0;
    return l.L.item_row[(size_t)std::max(0, std::min(item, (int)l.L.item_row.size() - 1))];
}

int move_focus(const Layout& l, int cur, Dir dir) {
    const int n = (int)l.L.item_row.size();
    if (n == 0 || cur < 0 || cur >= n) return cur;
    switch (dir) {
        case Dir::Left: return cur > 0 ? cur - 1 : cur;
        case Dir::Right: return cur < n - 1 ? cur + 1 : cur;
        default: break;
    }
    const int r = l.L.item_row[(size_t)cur] + (dir == Dir::Down ? 1 : -1);
    if (r < 0 || r >= (int)l.L.rows.size()) return cur;
    const VRow& row = l.L.rows[(size_t)r];
    const float cx = l.x[(size_t)cur] + l.w[(size_t)cur] * 0.5f;
    int best = row.first;
    float bd = 1e30f;
    for (int i = row.first; i < row.first + row.count; ++i) {
        const float d = std::fabs(l.x[(size_t)i] + l.w[(size_t)i] * 0.5f - cx);
        if (d < bd - 1e-4f) { bd = d; best = i; }
    }
    return best;
}

void visible_rows(const Layout& l, float y0, float y1, int* first_row, int* last_row) { rows_in_range(l.L, y0, y1, first_row, last_row); }

/* -------------------------------- slide-out -------------------------------- */

void spring_step(Spring& s, float target, float dt, bool reduced_motion) {
    if (reduced_motion || !(dt > 0.f)) { if (reduced_motion) { s.x = target; s.v = 0; } return; }
    dt = std::min(dt, 0.25f);
    const float k = 190.f, c = 17.f;   // zeta ~ 0.62: a small overshoot, settles in about a third of a second
    const int steps = std::max(1, (int)std::ceil(dt / (1.f / 120.f)));
    const float h = dt / (float)steps;
    for (int i = 0; i < steps; ++i) {
        s.v += (k * (target - s.x) - c * s.v) * h;
        s.x += s.v * h;
    }
    if (std::fabs(target - s.x) < 0.0006f && std::fabs(s.v) < 0.01f) { s.x = target; s.v = 0; }
}

bool spring_settled(const Spring& s, float target) { return s.x == target && s.v == 0.f; }

float slide_target(bool active, bool selected) { return active ? 1.f : selected ? 0.42f : 0.f; }

SlidePose slide_pose(Media m, const Dims& d, float s, float v, bool reduced_motion) {
    SlidePose p;
    if (reduced_motion) { s = std::max(0.f, std::min(1.f, s)); v = 0; }
    const float sc = std::max(-0.2f, std::min(s, 1.25f));   // a little overshoot is the fun part
    const float t = std::max(0.f, sc);
    p.scale = 1.f + 0.40f * sc;
    p.lift = 0.10f * sc;
    p.shadow = std::max(0.f, sc);
    p.persp = 0.45f * t;
    const float lean = reduced_motion ? 0.f : std::max(-0.12f, std::min(0.12f, v * 0.028f));   // leans into the motion
    if (is_cart(m)) {
        const float to = -0.40f;                    // turns to show its left edge a little
        p.theta = d.rest_theta + 0.12f * (1.f - std::min(t, 1.f)) + (to - 0.12f) * sc + lean;
    } else {
        const float to = -0.50f;                    // the spine swings toward the viewer's left, showing the cover
        p.theta = d.rest_theta + (to - d.rest_theta) * std::min(1.f, sc * 1.05f) + lean;
        if (sc > 1.f) p.theta += (sc - 1.f) * (to - d.rest_theta) * 1.05f;
    }
    p.card = (m == Media::DsCase || m == Media::N3dsCase) ? std::max(0.f, std::min(1.f, (sc - 0.35f) / 0.65f)) : 0.f;
    return p;
}

}  // namespace hubui::shelf
