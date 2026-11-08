/* Coverflow stage drawing (see shelf_draw.h): the blurred cover-colour background with its heavy vignette, the floor glow, the soft ellipse shadows and the
 * short faint reflection. Layout numbers adapted from PyroCade (MIT, Pyrodrifter). Meshes are written straight into the draw list: no allocations. */
#include "shelf_draw.h"
#include "cases_logic.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace shelfdraw {
namespace {

constexpr float kTau = 6.28318531f;
int c8(float v) { return v < 0.f ? 0 : v > 255.f ? 255 : (int)v; }
ImU32 mul_a(ImU32 c, float a) { return (c & 0x00FFFFFFu) | ((ImU32)c8((float)(c >> 24) * a) << 24); }

/* a radial gradient: a fan from the centre colour to the rim colour */
void fan(ImDrawList* dl, ImVec2 c, float rx, float ry, ImU32 in, ImU32 out, int seg = 48) {
    if (rx < 1.f || ry < 1.f) return;
    const ImVec2 uvw = dl->_Data->TexUvWhitePixel;
    dl->PrimReserve(seg * 3, seg + 1);
    const ImDrawIdx i0 = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(c, uvw, in);
    for (int k = 0; k < seg; ++k) { const float a = kTau * (float)k / (float)seg; dl->PrimWriteVtx(ImVec2(c.x + std::cos(a) * rx, c.y + std::sin(a) * ry), uvw, out); }
    ImDrawIdx* ix = dl->_IdxWritePtr;
    for (int k = 0; k < seg; ++k) { ix[k * 3] = i0; ix[k * 3 + 1] = (ImDrawIdx)(i0 + 1 + k); ix[k * 3 + 2] = (ImDrawIdx)(i0 + 1 + (k + 1) % seg); }
    dl->_IdxWritePtr += seg * 3;
}
/* the ring between two ellipses (radii scaled by t0 / t1) with a colour each */
void ring(ImDrawList* dl, ImVec2 c, float rx, float ry, float t0, float t1, ImU32 c0, ImU32 c1, int seg = 56) {
    const ImVec2 uvw = dl->_Data->TexUvWhitePixel;
    dl->PrimReserve(seg * 6, seg * 2);
    const ImDrawIdx i0 = (ImDrawIdx)dl->_VtxCurrentIdx;
    for (int k = 0; k < seg; ++k) {
        const float a = kTau * (float)k / (float)seg, ca = std::cos(a), sa = std::sin(a);
        dl->PrimWriteVtx(ImVec2(c.x + ca * rx * t0, c.y + sa * ry * t0), uvw, c0);
        dl->PrimWriteVtx(ImVec2(c.x + ca * rx * t1, c.y + sa * ry * t1), uvw, c1);
    }
    ImDrawIdx* ix = dl->_IdxWritePtr;
    for (int k = 0; k < seg; ++k) {
        const int j = (k + 1) % seg;
        const ImDrawIdx a0 = (ImDrawIdx)(i0 + k * 2), b0 = (ImDrawIdx)(i0 + k * 2 + 1), a1 = (ImDrawIdx)(i0 + j * 2), b1 = (ImDrawIdx)(i0 + j * 2 + 1);
        ix[k * 6] = a0; ix[k * 6 + 1] = b0; ix[k * 6 + 2] = b1; ix[k * 6 + 3] = a0; ix[k * 6 + 4] = b1; ix[k * 6 + 5] = a1;
    }
    dl->_IdxWritePtr += seg * 6;
}
ImVec2 proj(const CaseScene& c, float x, float y, float z) {
    const float k = c.dist / std::max(0.4f, c.dist + z);
    return ImVec2(c.cx + x * k * c.U, c.hy + (c.eye - y) * k * c.U);
}
/* A soft ellipse lying on the floor (the shadow plane turns with its case): full strength inside `plateau`, fading to nothing at `outer`. */
void soft_ellipse(const CaseScene& sc, float wx, float wy, float wz, float rx, float rz, float yaw, float plateau, float outer, float a) {
    constexpr int N = 28;
    ImDrawList* dl = sc.dl;
    const float ct = std::cos(yaw), st = std::sin(yaw);
    const ImVec2 uvw = dl->_Data->TexUvWhitePixel;
    const ImU32 in = IM_COL32(0, 0, 0, c8(a * 255.f)), out = IM_COL32(0, 0, 0, 0);
    dl->PrimReserve(N * 3 + N * 6, 1 + N * 2);
    const ImDrawIdx i0 = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(proj(sc, wx, wy, wz), uvw, in);
    for (int k = 0; k < N; ++k) {
        const float an = kTau * (float)k / (float)N, ca = std::cos(an), sa = std::sin(an);
        for (int r = 0; r < 2; ++r) {
            const float s = r == 0 ? plateau : outer;
            const float lx = rx * s * ca, lz = rz * s * sa;
            dl->PrimWriteVtx(proj(sc, wx + lx * ct + lz * st, wy, wz - lx * st + lz * ct), uvw, r == 0 ? in : out);
        }
    }
    ImDrawIdx* ix = dl->_IdxWritePtr;
    for (int k = 0; k < N; ++k) {
        const int j = (k + 1) % N;
        const ImDrawIdx in0 = (ImDrawIdx)(i0 + 1 + k * 2), ou0 = (ImDrawIdx)(i0 + 2 + k * 2), in1 = (ImDrawIdx)(i0 + 1 + j * 2), ou1 = (ImDrawIdx)(i0 + 2 + j * 2);
        ix[k * 3] = i0; ix[k * 3 + 1] = in0; ix[k * 3 + 2] = in1;
        ImDrawIdx* b = ix + N * 3 + k * 6;
        b[0] = in0; b[1] = ou0; b[2] = ou1; b[3] = in0; b[4] = ou1; b[5] = in1;
    }
    dl->_IdxWritePtr += N * 3 + N * 6;
}

}  // namespace

ImU32 flow_grid_at(const ImU32 grid[64], float u, float v) {
    const float fx = std::max(0.f, std::min(7.f, u * 8.f - 0.5f)), fy = std::max(0.f, std::min(7.f, v * 8.f - 0.5f));
    const int x0 = (int)fx, y0 = (int)fy, x1 = std::min(7, x0 + 1), y1 = std::min(7, y0 + 1);
    const float tx = fx - (float)x0, ty = fy - (float)y0;
    auto ch = [&](int s) {
        const float a = (float)((grid[y0 * 8 + x0] >> s) & 255) * (1.f - tx) + (float)((grid[y0 * 8 + x1] >> s) & 255) * tx;
        const float b = (float)((grid[y1 * 8 + x0] >> s) & 255) * (1.f - tx) + (float)((grid[y1 * 8 + x1] >> s) & 255) * tx;
        return c8(a * (1.f - ty) + b * ty);
    };
    return IM_COL32(ch(0), ch(8), ch(16), 255);
}

void flow_background(const CaseScene& sc, ImVec2 p0, ImVec2 p1, const ImU32 grid[64], ImU32 tint, float tint_k) {
    ImDrawList* dl = sc.dl;
    const float W = p1.x - p0.x, H = p1.y - p0.y;
    dl->AddRectFilled(p0, p1, IM_COL32(7, 8, 12, 255));
    {   // the cover's colours as a smooth mesh: the 8x8 average at the cell centres, the border cells stretched to the edges (a very blurred, slightly oversized picture)
        static const float cp[10] = {0.f, 1.f / 16.f, 3.f / 16.f, 5.f / 16.f, 7.f / 16.f, 9.f / 16.f, 11.f / 16.f, 13.f / 16.f, 15.f / 16.f, 1.f};
        const ImVec2 uvw = dl->_Data->TexUvWhitePixel;
        dl->PrimReserve(9 * 9 * 6, 100);
        const ImDrawIdx i0 = (ImDrawIdx)dl->_VtxCurrentIdx;
        for (int j = 0; j < 10; ++j)
            for (int i = 0; i < 10; ++i) {
                const int ci = std::max(0, std::min(7, i - 1)), cj = std::max(0, std::min(7, j - 1));
                dl->PrimWriteVtx(ImVec2(p0.x + cp[i] * W, p0.y + cp[j] * H), uvw, (grid[cj * 8 + ci] & 0x00FFFFFFu) | (232u << 24));
            }
        ImDrawIdx* ix = dl->_IdxWritePtr;
        int n = 0;
        for (int j = 0; j < 9; ++j)
            for (int i = 0; i < 9; ++i) {
                const ImDrawIdx a = (ImDrawIdx)(i0 + j * 10 + i), b = (ImDrawIdx)(a + 1), c = (ImDrawIdx)(a + 10), d = (ImDrawIdx)(a + 11);
                ix[n++] = a; ix[n++] = b; ix[n++] = d; ix[n++] = a; ix[n++] = d; ix[n++] = c;
            }
        dl->_IdxWritePtr += n;
    }
    const ImVec2 mid(p0.x + W * 0.5f, p0.y + H * 0.5f);
    // the cover's average colour as a glow behind the cases
    fan(dl, ImVec2(mid.x, p0.y + H * 0.42f), W * 0.62f, H * 0.62f, mul_a(tint, 0.22f * tint_k), mul_a(tint, 0.f));
    // heavy vignette: transparent up to 30% of the way to the corner, 75% black at the corners
    ring(dl, mid, W * 0.5f * 1.4142f, H * 0.5f * 1.4142f, 0.30f, 1.06f, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 191));
    // and a darkening at the top and the bottom
    dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, p0.y + H * 0.2f), IM_COL32(0, 0, 0, 102), IM_COL32(0, 0, 0, 102), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    dl->AddRectFilledMultiColor(ImVec2(p0.x, p0.y + H * 0.6f), p1, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 217), IM_COL32(0, 0, 0, 217));
}

void flow_floor_glow(const CaseScene& sc, ImVec2 p0, ImVec2 p1, ImU32 tint, float floor_y) {
    const float W = p1.x - p0.x;
    fan(sc.dl, ImVec2(sc.cx, floor_y + sc.U * 0.10f), W * 0.42f, sc.U * 0.55f, mul_a(tint, 0.26f), mul_a(tint, 0.f));
    fan(sc.dl, ImVec2(sc.cx, floor_y + sc.U * 0.06f), W * 0.18f, sc.U * 0.20f, IM_COL32(255, 255, 255, 14), IM_COL32(255, 255, 255, 0), 32);
}

void flow_shadow(const CaseScene& sc, hubui::shelf::Media m, const CasePose& ps, float base, float strength) {
    const float a = std::max(0.f, std::min(1.f, strength * ps.alpha));
    if (a <= 0.01f) return;
    const hubui::cases::CaseDims cd = hubui::cases::case_dims(m);
    const float hw = 0.5f * cd.w * ps.scale;
    soft_ellipse(sc, ps.x, base, ps.z - 0.035f * ps.scale, hw * 1.16f, 0.074f * ps.scale * 1.2f, ps.yaw, 0.55f, 1.2f, 0.60f * a);   // the broad blur
    soft_ellipse(sc, ps.x, base, ps.z - 0.012f * ps.scale, hw * 0.66f, 0.030f * ps.scale, ps.yaw, 0.55f, 1.15f, 0.80f * a);            // the tight core
}

void flow_reflection(const CaseScene& sc, const CaseDraw& d, const CasePose& ps, float strength) {
    if (strength <= 0.01f || ps.alpha <= 0.02f) return;
    ImVec2 mn, mx;
    cases_bounds(sc, d.media, ps, &mn, &mx);
    const float h = mx.y - mn.y;
    if (h < 8.f) return;
    CasePose pr = ps;
    pr.lift = -ps.lift;        // mirrored about the case's own bottom (cases_case mirrors about y = 0)
    pr.refl_detail = false;
    pr.dim = 0.f;
    constexpr int kBands = 6;
    const float y0 = mx.y - h * 0.03f, bh = h * 0.20f / (float)kBands;
    ImDrawList* dl = sc.dl;
    for (int k = 0; k < kBands; ++k) {
        const float f = 1.f - ((float)k + 0.5f) / (float)kBands;   // the mask: fading out over the 20%
        pr.refl = 0.22f * strength * std::pow(f, 1.25f);
        if (pr.refl <= 0.004f) continue;
        const float ya = k == 0 ? y0 : y0 + h * 0.03f + bh * (float)k, yb = y0 + h * 0.03f + bh * (float)(k + 1);
        dl->PushClipRect(ImVec2(mn.x - 8.f, ya), ImVec2(mx.x + 8.f, yb), true);
        cases_case(sc, d, pr, true);
        dl->PopClipRect();
    }
}

}  // namespace shelfdraw
