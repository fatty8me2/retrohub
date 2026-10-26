/* PLACEHOLDER art for the start animation (boot_anim.h). Replaced by boot_art.cpp (agent/boot-art): CMake compiles this file only
 * while boot_art.cpp does not exist, so delete nothing -- just merge the real file. Simple shapes, same contract. */
#include "boot_anim.h"

namespace bootanim {

void draw_shelf(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float t) {
    (void)t;
    const float W = mx.x - mn.x, H = mx.y - mn.y;
    const float y = mn.y + H * 0.62f;
    dl->AddRectFilled(ImVec2(mn.x, y), ImVec2(mx.x, y + H * 0.03f), IM_COL32(60, 52, 46, 255));
    const int n = 9;
    for (int i = 0; i < n; ++i) {
        float bw = W / (n + 1.f) * 0.8f, bh = H * (0.18f + 0.04f * (i % 3));
        float x = mn.x + W * (i + 0.6f) / (n + 0.2f);
        ImU32 c = IM_COL32(60 + (i * 37) % 120, 70 + (i * 53) % 110, 110 + (i * 29) % 120, 255);
        dl->AddRectFilled(ImVec2(x, y - bh), ImVec2(x + bw, y), c, 4.f);
        dl->AddRect(ImVec2(x, y - bh), ImVec2(x + bw, y), IM_COL32(255, 255, 255, 40), 4.f);
    }
}

void draw_console(hub::System, ImDrawList* dl, ImVec2 c, float scale, float lid_open, float power) {
    const float w = 300.f * scale, h = 150.f * scale;
    ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y + h * 0.5f);
    dl->AddRectFilled(a, b, IM_COL32(70, 72, 82, 255), 14.f * scale);
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 50), 14.f * scale, 0, 2.f);
    ImVec2 la(a.x + w * 0.1f, a.y), lb(b.x - w * 0.1f, a.y + h * 0.18f * (1.f - 0.8f * lid_open));
    dl->AddRectFilled(la, lb, IM_COL32(50, 52, 60, 255), 6.f * scale);
    ImU32 led = power > 0.01f ? IM_COL32((int)(60 + 195 * power), (int)(60 + 160 * power), 60, 255) : IM_COL32(60, 40, 40, 255);
    dl->AddCircleFilled(ImVec2(b.x - w * 0.12f, b.y - h * 0.18f), 7.f * scale, led, 16);
}

void draw_media(hub::System, ImDrawList* dl, ImVec2 c, float scale, float rot, ImTextureID label, ImVec2 uv0, ImVec2 uv1) {
    (void)rot;
    const float w = 160.f * scale, h = 190.f * scale;
    ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y + h * 0.5f);
    dl->AddRectFilled(a, b, IM_COL32(90, 92, 100, 255), 8.f * scale);
    ImVec2 la(a.x + w * 0.1f, a.y + h * 0.12f), lb(b.x - w * 0.1f, b.y - h * 0.12f);
    if (label) dl->AddImageRounded(label, la, lb, uv0, uv1, IM_COL32_WHITE, 4.f * scale);
    else dl->AddRectFilled(la, lb, IM_COL32(150, 150, 170, 255), 4.f * scale);
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 60), 8.f * scale, 0, 2.f);
}

} // namespace bootanim
