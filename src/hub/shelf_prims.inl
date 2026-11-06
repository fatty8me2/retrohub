// Shared drawing primitives of shelf_draw.cpp and cases_draw.cpp (included inside each file's anonymous namespace).
inline ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
inline int cr(ImU32 c) { return (int)(c & 0xFF); }
inline int cg(ImU32 c) { return (int)((c >> 8) & 0xFF); }
inline int cb(ImU32 c) { return (int)((c >> 16) & 0xFF); }
inline int ca(ImU32 c) { return (int)((c >> 24) & 0xFF); }
inline int cl8(float v) { return v < 0 ? 0 : v > 255 ? 255 : (int)v; }
ImU32 shade(ImU32 c, float k) { return IM_COL32(cl8(cr(c) * k), cl8(cg(c) * k), cl8(cb(c) * k), ca(c)); }
ImU32 alpha(ImU32 c, float a) { return IM_COL32(cr(c), cg(c), cb(c), cl8(ca(c) * a)); }
ImU32 lerpc(ImU32 a, ImU32 b, float t) {
    return IM_COL32(cl8(cr(a) + (cr(b) - cr(a)) * t), cl8(cg(a) + (cg(b) - cg(a)) * t), cl8(cb(a) + (cb(b) - cb(a)) * t), cl8(ca(a) + (ca(b) - ca(a)) * t));
}
float lum(ImU32 c) { return (0.2126f * cr(c) + 0.7152f * cg(c) + 0.0722f * cb(c)) / 255.f; }
ImVec2 lerp2(ImVec2 a, ImVec2 b, float t) { return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t); }
ImU32 hsv(float h, float s, float v) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h - std::floor(h), s, v, r, g, b);
    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
}

/* ---------------------------- primitives on a quad ---------------------------- */

struct Face {
    ImVec2 q[4];       // TL TR BR BL on screen
    float W = 1, H = 1;// local size in px (for text and px-based details)
    ImVec2 at(float u, float v) const { return lerp2(lerp2(q[0], q[1], u), lerp2(q[3], q[2], u), v); }
};
struct Ctx {
    ImDrawList* dl;
    float ghost = 0;   // 0..1: desaturate toward grey
    float a = 1;       // alpha multiplier (edge fade, reflections)
    bool detail = true;// false: skip text (far reflections)
    ImU32 c(ImU32 col) const {
        if (ghost > 0.f) {
            const ImU32 g = IM_COL32(104, 107, 116, ca(col));
            col = lerpc(col, g, ghost);
        }
        return a < 1.f ? alpha(col, a) : col;
    }
};

void quad(Ctx& x, const Face& f, float u0, float v0, float u1, float v1, ImU32 col) {
    x.dl->AddQuadFilled(f.at(u0, v0), f.at(u1, v0), f.at(u1, v1), f.at(u0, v1), x.c(col));
}
void outline(Ctx& x, const Face& f, float u0, float v0, float u1, float v1, ImU32 col, float th = 1.f) {
    x.dl->AddQuad(f.at(u0, v0), f.at(u1, v0), f.at(u1, v1), f.at(u0, v1), x.c(col), th);
}
void poly(Ctx& x, const Face& f, std::initializer_list<ImVec2> uv, ImU32 col) {
    ImVec2 p[10];
    int n = 0;
    for (const ImVec2& a : uv) if (n < 10) p[n++] = f.at(a.x, a.y);
    x.dl->AddConvexPolyFilled(p, n, x.c(col));
}
void line(Ctx& x, const Face& f, float u0, float v0, float u1, float v1, ImU32 col, float th = 1.f) {
    x.dl->AddLine(f.at(u0, v0), f.at(u1, v1), x.c(col), th);
}
/* A quad with a colour per corner (no anti-aliasing: used inside shapes). */
void grad(Ctx& x, const Face& f, float u0, float v0, float u1, float v1, ImU32 ctl, ImU32 ctr, ImU32 cbr, ImU32 cbl) {
    ImDrawList* dl = x.dl;
    const ImVec2 uvw = dl->_Data->TexUvWhitePixel;
    dl->PrimReserve(6, 4);
    const ImDrawIdx i = (ImDrawIdx)(dl->_VtxCurrentIdx);
    dl->PrimWriteVtx(f.at(u0, v0), uvw, x.c(ctl));
    dl->PrimWriteVtx(f.at(u1, v0), uvw, x.c(ctr));
    dl->PrimWriteVtx(f.at(u1, v1), uvw, x.c(cbr));
    dl->PrimWriteVtx(f.at(u0, v1), uvw, x.c(cbl));
    ImDrawIdx* ix = dl->_IdxWritePtr;
    ix[0] = (ImDrawIdx)(i); ix[1] = (ImDrawIdx)(i + 1); ix[2] = (ImDrawIdx)(i + 2);
    ix[3] = (ImDrawIdx)(i); ix[4] = (ImDrawIdx)(i + 2); ix[5] = (ImDrawIdx)(i + 3);
    dl->_IdxWritePtr += 6;
}
void vgrad(Ctx& x, const Face& f, float u0, float v0, float u1, float v1, ImU32 top, ImU32 bot) { grad(x, f, u0, v0, u1, v1, top, top, bot, bot); }
void hgrad(Ctx& x, const Face& f, float u0, float v0, float u1, float v1, ImU32 l, ImU32 r) { grad(x, f, u0, v0, u1, v1, l, r, r, l); }

/* Centre-crops `tex` to fill a target of aspect `ta` (w/h); anchor_y 0 = keep the top, 0.5 = centre. */
void crop_uv(int tw, int th, float ta, float anchor_y, ImVec2* a, ImVec2* b) {
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (tw > 0 && th > 0) {
        const float aspect = (float)tw / (float)th;
        if (aspect > ta) { const float k = ta / aspect; u0 = (1.f - k) * 0.5f; u1 = u0 + k; }
        else { const float k = aspect / ta; v0 = (1.f - k) * anchor_y; v1 = v0 + k; }
    }
    *a = ImVec2(u0, v0);
    *b = ImVec2(u1, v1);
}
void image(Ctx& x, const Face& f, float u0, float v0, float u1, float v1, SDL_Texture* tex, int tw, int th, float anchor_y, float bright = 1.f) {
    const float ta = ((u1 - u0) * f.W) / std::max(1.f, (v1 - v0) * f.H);
    ImVec2 a, b;
    crop_uv(tw, th, ta, anchor_y, &a, &b);
    const int k = cl8(255 * bright);
    ImU32 col = IM_COL32(k, k, k, 255);
    if (x.ghost > 0.f) col = lerpc(col, IM_COL32(150, 152, 160, 255), x.ghost * 0.6f);
    if (x.a < 1.f) col = alpha(col, x.a);
    x.dl->AddImageQuad((ImTextureID)tex, f.at(u0, v0), f.at(u1, v0), f.at(u1, v1), f.at(u0, v1), a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y), col);
}

/* Text on a face. Local px coordinates (lx, ly): horizontal text starts at its top-left; vertical text (rotated 90 degrees clockwise,
 * reading top to bottom) starts at its top end with the glyph tops pointing to +x, so lx is the right edge of the line. */
void face_text(Ctx& x, const Face& f, ImFont* font, float size, const char* s, float lx, float ly, ImU32 col, bool vertical, float track = 0.f) {
    if (!font || !s || !*s || size < 3.f || !x.detail) return;
    {   // draw from the smallest same-weight atlas font that is still >= 95% of `size`: shrinking a glyph drops whole 1-px rows under
        // nearest sampling (the headless software renderer) and thins them under linear ("GENRE" read "GFNRF" on case backs)
        const ui::Fonts& F = ui::F;
        const bool heavy = font == F.bold || font == F.h2 || font == F.title || font == F.hero || font == F.big || font == F.tiny_bold;
        ImFont* const reg[] = {F.tiny, F.cap, F.body};
        ImFont* const bld[] = {F.tiny_bold, F.bold, F.h2, F.title, F.hero, F.big};
        ImFont* const* list = heavy ? bld : reg;
        const int count = heavy ? 6 : 3;
        ImFont* best = font;
        for (int i = 0; i < count; ++i) {
            ImFont* c = list[i];
            if (c && c->ContainerAtlas == font->ContainerAtlas && c->FontSize >= size * 0.95f && c->FontSize < best->FontSize) best = c;
        }
        font = best;
    }
    ImDrawList* dl = x.dl;
    const float sc = size / font->FontSize;
    const ImVec2 inv(1.f / f.W, 1.f / f.H);
    col = x.c(col);
    float pen = 0.f;
    dl->PushTextureID(font->ContainerAtlas->TexID);
    const char* p = s;
    while (*p) {
        unsigned int ch = 0;
        const int n = ImTextCharFromUtf8(&ch, p, nullptr);
        if (n <= 0) break;
        p += n;
        if (ch == '\n' || ch == '\r') continue;
        const ImFontGlyph* g = font->FindGlyph((ImWchar)ch);
        if (!g) continue;
        if (g->Visible) {
            const float a0 = pen + g->X0 * sc, a1 = pen + g->X1 * sc, b0 = g->Y0 * sc, b1 = g->Y1 * sc;
            ImVec2 c[4] = {ImVec2(a0, b0), ImVec2(a1, b0), ImVec2(a1, b1), ImVec2(a0, b1)};
            ImVec2 sp4[4];
            for (int i = 0; i < 4; ++i) {
                const float X = vertical ? lx - c[i].y : lx + c[i].x;
                const float Y = vertical ? ly + c[i].x : ly + c[i].y;
                sp4[i] = f.at(X * inv.x, Y * inv.y);
            }
            dl->PrimReserve(6, 4);
            dl->PrimQuadUV(sp4[0], sp4[1], sp4[2], sp4[3], ImVec2(g->U0, g->V0), ImVec2(g->U1, g->V0), ImVec2(g->U1, g->V1), ImVec2(g->U0, g->V1), col);
        }
        pen += g->AdvanceX * sc + track;
    }
    dl->PopTextureID();
}
float text_w(ImFont* font, float size, const std::string& s) { return font ? font->CalcTextSizeA(size, FLT_MAX, 0.f, s.c_str()).x : 0.f; }
