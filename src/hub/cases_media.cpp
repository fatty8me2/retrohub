/* Coverflow: the disc / cartridge that slides out from behind the selected case (see shelf_draw.h, cases_logic.h cf::). Drawn from ImDrawList meshes
 * projected with the carousel's own camera: a disc is a metallic annulus with the cover art as its label (a real hole in the middle) and a slow
 * iridescent sheen; a cartridge is a small extruded shell in the console's plastic with a label well showing the art. No allocations per frame. */
#include "shelf_draw.h"
#include "cases_logic.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace shelfdraw {
namespace {

using hubui::cases::CaseDims;
using hubui::shelf::Media;
namespace cf = hubui::cases::cf;

constexpr float kTau = 6.28318531f;
constexpr int kSeg = 64;

ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
int c8(float v) { return v < 0.f ? 0 : v > 255.f ? 255 : (int)v; }
ImU32 shade(ImU32 c, float k) { return IM_COL32(c8((float)(c & 255) * k), c8((float)((c >> 8) & 255) * k), c8((float)((c >> 16) & 255) * k), (int)(c >> 24)); }
ImU32 mixc(ImU32 a, ImU32 b, float t) {
    auto m = [&](int s) { return c8((float)((a >> s) & 255) + (float)(((int)((b >> s) & 255) - (int)((a >> s) & 255))) * t); };
    return IM_COL32(m(0), m(8), m(16), m(24));
}
ImU32 with_a(ImU32 c, float a) { return (c & 0x00FFFFFFu) | ((ImU32)c8(a * 255.f) << 24); }

struct Frame {   // where the media's plane sits: centre, the turn (roll about its own axis, then yaw), and the camera
    const CaseScene* sc = nullptr;
    float cx = 0, cy = 0, cz = 0, yc = 1, ys = 0, rc = 1, rs = 0;
    ImVec2 at(float u, float v, float w = 0.f) const {   // u right, v up on the plane, w = depth behind the plane (away from the camera)
        const float x1 = u * rc + v * rs, y1 = -u * rs + v * rc;
        const float x2 = x1 * yc + w * ys, z2 = -x1 * ys + w * yc;
        const float z = cz + z2, k = sc->dist / std::max(0.4f, sc->dist + z);
        return ImVec2(sc->cx + (cx + x2) * k * sc->U, sc->hy + (sc->eye - (cy + y1)) * k * sc->U);
    }
};

/* the metallic stops of a disc (conic): #dce4ec #767e95 #e8eff3 #b0c3d0 #737d94 #e0e6ed, going round */
ImU32 metal_at(float ang01) {
    static const ImU32 st[6] = {IM_COL32(220, 228, 236, 255), IM_COL32(118, 126, 149, 255), IM_COL32(232, 239, 243, 255), IM_COL32(176, 195, 208, 255), IM_COL32(115, 125, 148, 255), IM_COL32(224, 230, 237, 255)};
    const float f = (ang01 - std::floor(ang01)) * 6.f;
    const int i = (int)f % 6;
    return mixc(st[i], st[(i + 1) % 6], f - std::floor(f));
}

void strip(ImDrawList* dl, int n, const ImVec2* a, const ImVec2* b, const ImU32* ca, const ImU32* cb) {   // a closed ring between two rings of n points
    const ImVec2 uvw = dl->_Data->TexUvWhitePixel;
    dl->PrimReserve(n * 6, n * 2);
    const ImDrawIdx i0 = (ImDrawIdx)dl->_VtxCurrentIdx;
    for (int i = 0; i < n; ++i) { dl->PrimWriteVtx(a[i], uvw, ca[i]); dl->PrimWriteVtx(b[i], uvw, cb[i]); }
    ImDrawIdx* ix = dl->_IdxWritePtr;
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        const ImDrawIdx a0 = (ImDrawIdx)(i0 + i * 2), b0 = (ImDrawIdx)(i0 + i * 2 + 1), a1 = (ImDrawIdx)(i0 + j * 2), b1 = (ImDrawIdx)(i0 + j * 2 + 1);
        ix[i * 6] = a0; ix[i * 6 + 1] = b0; ix[i * 6 + 2] = b1; ix[i * 6 + 3] = a0; ix[i * 6 + 4] = b1; ix[i * 6 + 5] = a1;
    }
    dl->_IdxWritePtr += n * 6;
}

void disc(const CaseScene& sc, const MediaDraw& d, const Frame& fr, float R, float time) {
    ImDrawList* dl = sc.dl;
    ImVec2 ring[kSeg + 1], inner[kSeg + 1], mid[kSeg + 1];
    ImU32 ca[kSeg + 1], cb[kSeg + 1];
    // the disc's thickness: a darker copy just behind
    {
        ImVec2 p[kSeg];
        for (int i = 0; i < kSeg; ++i) { const float a = kTau * (float)i / (float)kSeg; p[i] = fr.at(std::cos(a) * R, std::sin(a) * R, R * 0.035f); }
        dl->AddConvexPolyFilled(p, kSeg, rgb(70, 76, 92, 255));
    }
    const float r_hole = 0.075f, r_lab0 = 0.17f, r_lab1 = 0.965f;
    for (int i = 0; i < kSeg; ++i) {
        const float a = kTau * (float)i / (float)kSeg, ct = std::cos(a), st = std::sin(a);
        ring[i] = fr.at(ct * R, st * R);
        inner[i] = fr.at(ct * R * r_hole, st * R * r_hole);
        ca[i] = metal_at(a / kTau + 0.08f); cb[i] = metal_at(a / kTau + 0.08f + 0.02f);
    }
    strip(dl, kSeg, inner, ring, ca, cb);   // the metal: hub ring and rim (the hole stays empty)
    // the label: the cover art (or a plain colour) between the hub ring and the rim
    ImVec2 l0[kSeg + 1], l1[kSeg + 1], u0[kSeg + 1], u1[kSeg + 1];
    float ku = 0.5f, kv = 0.5f;   // half the texture span the label covers: a square from the middle of the art
    if (d.art && d.art_w > 0 && d.art_h > 0) { const float asp = (float)d.art_w / (float)d.art_h; if (asp > 1.f) ku = 0.5f / asp; else kv = 0.5f * asp; }
    for (int i = 0; i < kSeg; ++i) {
        const float a = kTau * (float)i / (float)kSeg, ct = std::cos(a), st = std::sin(a);
        l0[i] = fr.at(ct * R * r_lab0, st * R * r_lab0); l1[i] = fr.at(ct * R * r_lab1, st * R * r_lab1);
        u0[i] = ImVec2(0.5f + ct * r_lab0 * ku, 0.5f - st * r_lab0 * kv);
        u1[i] = ImVec2(0.5f + ct * r_lab1 * ku, 0.5f - st * r_lab1 * kv);
    }
    if (d.art) {
        dl->PushTextureID((ImTextureID)d.art);
        dl->PrimReserve(kSeg * 6, kSeg * 2);
        const ImDrawIdx i0 = (ImDrawIdx)dl->_VtxCurrentIdx;
        for (int i = 0; i < kSeg; ++i) { dl->PrimWriteVtx(l0[i], u0[i], IM_COL32_WHITE); dl->PrimWriteVtx(l1[i], u1[i], IM_COL32_WHITE); }
        ImDrawIdx* ix = dl->_IdxWritePtr;
        for (int i = 0; i < kSeg; ++i) {
            const int j = (i + 1) % kSeg;
            const ImDrawIdx a0 = (ImDrawIdx)(i0 + i * 2), b0 = (ImDrawIdx)(i0 + i * 2 + 1), a1 = (ImDrawIdx)(i0 + j * 2), b1 = (ImDrawIdx)(i0 + j * 2 + 1);
            ix[i * 6] = a0; ix[i * 6 + 1] = b0; ix[i * 6 + 2] = b1; ix[i * 6 + 3] = a0; ix[i * 6 + 4] = b1; ix[i * 6 + 5] = a1;
        }
        dl->_IdxWritePtr += kSeg * 6;
        dl->PopTextureID();
    } else {
        const ImU32 lc = mixc(d.c0 ? d.c0 : rgb(60, 70, 110), rgb(236, 238, 246), 0.3f);
        for (int i = 0; i < kSeg; ++i) { ca[i] = lc; cb[i] = shade(lc, 0.8f); }
        strip(dl, kSeg, l0, l1, ca, cb);
    }
    // a soft shade over the label (it is printed, not mirrored) and the hub's inner edge
    for (int i = 0; i < kSeg; ++i) { ca[i] = rgb(0, 0, 0, 0); cb[i] = rgb(0, 0, 0, 38); }
    for (int i = 0; i < kSeg; ++i) { const float a = kTau * (float)i / (float)kSeg; mid[i] = fr.at(std::cos(a) * R * 0.99f, std::sin(a) * R * 0.99f); }
    strip(dl, kSeg, l0, mid, ca, cb);
    // the iridescent sheen: lobes of colour turning once per 18 s over the whole disc
    {
        const float rot = (float)std::fmod((double)time / 18.0, 1.0) * kTau;
        for (int i = 0; i < kSeg; ++i) {
            const float a = kTau * (float)i / (float)kSeg;
            const float lobe = std::max(0.f, std::sin(2.f * (a - rot))), lobe2 = std::max(0.f, std::sin(3.f * (a - rot) + 1.7f));
            float r, g, b;
            ImGui::ColorConvertHSVtoRGB(std::fmod(0.55f + 0.35f * std::sin(a - rot) + 1.f, 1.f), 0.55f, 1.f, r, g, b);
            ca[i] = IM_COL32(c8(r * 255.f), c8(g * 255.f), c8(b * 255.f), c8(255.f * (0.08f * lobe + 0.05f * lobe2)));
            cb[i] = IM_COL32(c8(r * 255.f), c8(g * 255.f), c8(b * 255.f), c8(255.f * (0.30f * lobe + 0.12f * lobe2)));
        }
        strip(dl, kSeg, inner, ring, ca, cb);
    }
    dl->AddPolyline(ring, kSeg, rgb(255, 255, 255, 120), ImDrawFlags_Closed, 1.f);
    dl->AddPolyline(inner, kSeg, rgb(20, 22, 30, 160), ImDrawFlags_Closed, 1.f);
}

/* ---- cartridges ---- */
struct Shell {
    ImU32 col = 0, trim = 0;
    float ch[4] = {0, 0, 0, 0};          // corner cuts (fractions of the shorter side): TL TR BR BL
    float lu0 = 0.1f, lv0 = 0.1f, lu1 = 0.9f, lv1 = 0.6f;   // the label well, fractions of the face (v down)
    bool contacts = false;               // a metal band along the bottom edge
};
Shell shell_of(Media m, bool nx2) {
    Shell s;
    switch (m) {
        case Media::SwitchCase:
            s.col = nx2 ? rgb(196, 24, 42) : rgb(44, 46, 54); s.trim = rgb(200, 204, 214); s.ch[1] = 0.10f; s.ch[3] = 0.16f; s.lu0 = 0.10f; s.lv0 = 0.07f; s.lu1 = 0.90f; s.lv1 = 0.66f; s.contacts = true; break;
        case Media::DsCase: s.col = rgb(110, 112, 120); s.trim = rgb(200, 202, 210); s.ch[1] = 0.12f; s.lu0 = 0.10f; s.lv0 = 0.12f; s.lu1 = 0.90f; s.lv1 = 0.66f; s.contacts = true; break;
        case Media::N3dsCase: s.col = rgb(40, 42, 50); s.trim = rgb(190, 194, 204); s.ch[1] = 0.12f; s.lu0 = 0.10f; s.lv0 = 0.12f; s.lu1 = 0.90f; s.lv1 = 0.66f; s.contacts = true; break;
        case Media::N64Cart: s.col = rgb(112, 114, 120); s.trim = rgb(214, 216, 222); s.ch[0] = 0.04f; s.ch[1] = 0.04f; s.lu0 = 0.13f; s.lv0 = 0.10f; s.lu1 = 0.87f; s.lv1 = 0.66f; break;
        case Media::SnesCart: s.col = rgb(158, 160, 168); s.trim = rgb(220, 222, 228); s.ch[2] = 0.10f; s.ch[3] = 0.10f; s.lu0 = 0.09f; s.lv0 = 0.10f; s.lu1 = 0.91f; s.lv1 = 0.62f; break;
        case Media::NesCart: s.col = rgb(98, 98, 104); s.trim = rgb(180, 180, 188); s.ch[2] = 0.06f; s.ch[3] = 0.06f; s.lu0 = 0.12f; s.lv0 = 0.12f; s.lu1 = 0.88f; s.lv1 = 0.62f; break;
        case Media::GbCart: s.col = rgb(168, 170, 166); s.trim = rgb(220, 222, 220); s.ch[1] = 0.12f; s.lu0 = 0.12f; s.lv0 = 0.12f; s.lu1 = 0.88f; s.lv1 = 0.62f; s.contacts = true; break;
        case Media::GbcCart: s.col = rgb(122, 86, 196); s.trim = rgb(220, 214, 240); s.ch[1] = 0.12f; s.lu0 = 0.12f; s.lv0 = 0.12f; s.lu1 = 0.88f; s.lv1 = 0.62f; s.contacts = true; break;
        case Media::GbaCart: s.col = rgb(120, 116, 168); s.trim = rgb(214, 214, 236); s.ch[1] = 0.10f; s.ch[0] = 0.10f; s.lu0 = 0.10f; s.lv0 = 0.14f; s.lu1 = 0.90f; s.lv1 = 0.84f; s.contacts = true; break;
        default: s.col = rgb(30, 32, 40); s.trim = rgb(176, 184, 206); s.ch[2] = 0.10f; s.lu0 = 0.08f; s.lv0 = 0.08f; s.lu1 = 0.92f; s.lv1 = 0.92f; break;   // the UMD
    }
    return s;
}

void card(const CaseScene& sc, const MediaDraw& d, const Frame& fr, float w, float h, float thick) {
    ImDrawList* dl = sc.dl;
    const Shell s = shell_of(d.media, d.nx2);
    const float hw = w * 0.5f, hh = h * 0.5f, m = std::min(w, h);
    const float c0 = s.ch[0] * m, c1 = s.ch[1] * m, c2 = s.ch[2] * m, c3 = s.ch[3] * m;
    const float px[8] = {-hw + c0, hw - c1, hw, hw, hw - c2, -hw + c3, -hw, -hw};
    const float py[8] = {hh, hh, hh - c1, -hh + c2, -hh, -hh, -hh + c3, hh - c0};
    ImVec2 fp[8], bp[8];
    for (int i = 0; i < 8; ++i) { fp[i] = fr.at(px[i], py[i]); bp[i] = fr.at(px[i], py[i], thick); }
    dl->AddConvexPolyFilled(bp, 8, shade(s.col, 0.45f));
    for (int i = 0; i < 8; ++i) {   // the edge: every side quad (the far ones sit behind the face)
        const int j = (i + 1) % 8;
        const ImVec2 q[4] = {fp[i], fp[j], bp[j], bp[i]};
        const float lit = (i == 0 || i == 7) ? 1.05f : (i == 2 || i == 3) ? 0.62f : 0.8f;
        dl->AddConvexPolyFilled(q, 4, shade(s.col, 0.55f * lit));
    }
    dl->AddConvexPolyFilled(fp, 8, s.col);
    // bevel: a lighter upper-left rim, darker lower-right
    dl->AddPolyline(fp, 8, with_a(s.trim, 0.45f), ImDrawFlags_Closed, 1.f);
    // the sheen: a diagonal gradient over the shell
    {
        const ImVec2 a = fr.at(-hw, hh), b = fr.at(hw, hh), c = fr.at(hw, -hh), e = fr.at(-hw, -hh);
        const ImVec2 uvw = dl->_Data->TexUvWhitePixel;
        dl->PrimReserve(6, 4);
        const ImDrawIdx i0 = (ImDrawIdx)dl->_VtxCurrentIdx;
        dl->PrimWriteVtx(a, uvw, rgb(255, 255, 255, 70)); dl->PrimWriteVtx(b, uvw, rgb(255, 255, 255, 14)); dl->PrimWriteVtx(c, uvw, rgb(0, 0, 0, 40)); dl->PrimWriteVtx(e, uvw, rgb(255, 255, 255, 10));
        ImDrawIdx* ix = dl->_IdxWritePtr;
        ix[0] = i0; ix[1] = (ImDrawIdx)(i0 + 1); ix[2] = (ImDrawIdx)(i0 + 2); ix[3] = i0; ix[4] = (ImDrawIdx)(i0 + 2); ix[5] = (ImDrawIdx)(i0 + 3);
        dl->_IdxWritePtr += 6;
    }
    // the label well
    const float u0 = -hw + s.lu0 * w, u1 = -hw + s.lu1 * w, v0 = hh - s.lv0 * h, v1 = hh - s.lv1 * h;
    const ImVec2 q0 = fr.at(u0, v0), q1 = fr.at(u1, v0), q2 = fr.at(u1, v1), q3 = fr.at(u0, v1);
    dl->AddQuadFilled(q0, q1, q2, q3, d.art ? rgb(236, 236, 240) : mixc(s.col, d.c0 ? d.c0 : s.col, 0.7f));
    if (d.art && d.art_w > 0 && d.art_h > 0) {
        const float ta = (u1 - u0) / std::max(0.0001f, v0 - v1), asp = (float)d.art_w / (float)d.art_h;   // fill the well: crop the art to its shape
        float a0 = 0.f, b0 = 0.f, a1 = 1.f, b1 = 1.f;
        if (asp > ta) { const float k = ta / asp; a0 = (1.f - k) * 0.5f; a1 = a0 + k; }
        else { const float k = asp / ta; b0 = (1.f - k) * 0.5f * 0.7f; b1 = b0 + k; }
        dl->AddImageQuad((ImTextureID)d.art, q0, q1, q2, q3, ImVec2(a0, b0), ImVec2(a1, b0), ImVec2(a1, b1), ImVec2(a0, b1), IM_COL32_WHITE);
    }
    dl->AddQuad(q0, q1, q2, q3, rgb(0, 0, 0, 90), 1.f);
    if (s.contacts) {   // the metal contacts along the bottom edge
        const float cy0 = -hh + 0.02f * h, cy1 = -hh + 0.11f * h;
        const ImVec2 k0 = fr.at(-hw + 0.18f * w, cy1), k1 = fr.at(hw - 0.18f * w, cy1), k2 = fr.at(hw - 0.18f * w, cy0), k3 = fr.at(-hw + 0.18f * w, cy0);
        dl->AddQuadFilled(k0, k1, k2, k3, rgb(226, 190, 96, 230));
    }
    // a lighter strip under the label, the finger ridges on a Switch / DS card
    if (d.media == Media::SwitchCase || d.media == Media::DsCase || d.media == Media::N3dsCase) {
        const ImVec2 r0 = fr.at(-hw + 0.1f * w, v1 - 0.06f * h), r1 = fr.at(hw - 0.1f * w, v1 - 0.06f * h);
        dl->AddLine(r0, r1, rgb(255, 255, 255, 50), 1.f);
    }
}

}  // namespace

void cases_media(const CaseScene& sc, const MediaDraw& d) {
    if (d.amount <= 0.004f || d.alpha <= 0.01f) return;
    const hubui::cases::cf::MediaSpec spec = cf::media_spec(d.media);
    if (spec.kind == cf::MediaKind::None) return;
    const CaseDims cd = hubui::cases::case_dims(d.media);
    const float S = d.scale;   // the case's whole scale (display scale x slot scale)
    float mw, mh, thick;
    if (spec.kind == cf::MediaKind::Disc) { mw = mh = cf::disc_dia(spec, cd) * S; thick = spec.thick * S; }
    else { mw = spec.w * S; mh = spec.h * S; thick = spec.thick * S; }
    const cf::MediaOffset off = cf::media_offset(spec, cd, S, mw, mh, d.x, d.far_limit);
    // the item's own turn (the slot turn, without the selected case's 22 degree sway): the media slides along it
    const float iy = d.item_yaw, ict = std::cos(iy), ist = std::sin(iy);
    const float ox = off.out * d.amount, oy = off.up * d.amount, oz = (0.5f * cd.d * S + 0.5f * thick + 0.004f);
    Frame fr;
    fr.sc = &sc;
    fr.cx = d.x + ox * ict + oz * ist;
    fr.cz = d.z + (-ox * ist + oz * ict);
    fr.cy = d.centre_y + oy;
    const float yaw = iy + spec.yaw * d.amount, roll = spec.roll * d.amount;
    fr.yc = std::cos(yaw); fr.ys = std::sin(yaw);
    fr.rc = std::cos(roll); fr.rs = std::sin(roll);
    if (spec.kind == cf::MediaKind::Disc) disc(sc, d, fr, mw * 0.5f, (float)d.time);
    else card(sc, d, fr, mw, mh, thick * 1.2f);
}

}  // namespace shelfdraw
