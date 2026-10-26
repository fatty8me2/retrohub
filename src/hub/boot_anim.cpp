#include "boot_anim.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "art.h"
#include "boot_art.h"
#include "ui_kit.h"

/* Game start animation engine: draws the timeline from boot_logic.h with the vector art from boot_art.cpp (or the stub), as a
 * full-screen modal popup (so nothing underneath reacts to the pad/mouse while it plays). Any input skips straight to the launch. */

namespace bootanim {

namespace {

Options g_opt;
Runner R;
hub::GameEntry G;
std::function<void()> g_done;
bool g_frozen = false;
hub::System g_frozen_sys = hub::System::N64;
float g_frozen_t = 0.f;

const char* kPopup = "##bootanim";

bool any_input() {
    for (int b = 0; b < 5; ++b) if (ImGui::IsMouseClicked(b)) return true;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const ImGuiKey key = (ImGuiKey)k;
        if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) continue;
        if (key >= ImGuiKey_ReservedForModCtrl && key <= ImGuiKey_ReservedForModSuper) continue;
        if (key >= ImGuiKey_GamepadLStickLeft && key <= ImGuiKey_GamepadRStickDown) continue;   // stick drift must not skip
        if (ImGui::IsKeyPressed(key, false)) return true;
    }
    return false;
}

void label_uv(int w, int h, ImVec2* a, ImVec2* b) {   // centre-crop the cover art to a square label
    *a = ImVec2(0, 0);
    *b = ImVec2(1, 1);
    if (w <= 0 || h <= 0) return;
    float asp = (float)w / (float)h;
    if (asp < 1.f) { float m = (1.f - asp) * 0.5f; a->y = m; b->y = 1.f - m; }
    else if (asp > 1.f) { float m = (1.f - 1.f / asp) * 0.5f; a->x = m; b->x = 1.f - m; }
}

void lerp_pt(ImVec2 a, ImVec2 b, float t, ImVec2* o) { *o = ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t); }

void text_center(ImDrawList* dl, ImFont* f, float size, ImVec2 c, ImU32 col, const char* s) {
    ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, 0.f, s);
    ui::text_shadowed(dl, f, size, ImVec2(c.x - sz.x * 0.5f, c.y), col, s);
}

/* Draws the media with an optional squash (disc seen at an angle) and rotation about its centre. */
void draw_media_xf(hub::System sys, ImDrawList* dl, ImVec2 c, float scale, float rot, ImTextureID label, ImVec2 uv0, ImVec2 uv1,
                   float xs, float ys, float ang) {
    const int v0 = dl->VtxBuffer.Size;
    draw_media(sys, dl, c, scale, rot, label, uv0, uv1);
    if (xs == 1.f && ys == 1.f && ang == 0.f) return;
    const float ca = std::cos(ang), sa = std::sin(ang);
    for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
        ImVec2& p = dl->VtxBuffer[i].pos;
        float x = (p.x - c.x) * xs, y = (p.y - c.y) * ys;
        p = ImVec2(c.x + x * ca - y * sa, c.y + x * sa + y * ca);
    }
}

float lerpf(float a, float b, float t) { return a + (b - a) * t; }

void render(ImDrawList* dl, ImVec2 disp, hub::System sys, const Timeline& tl, float t) {
    const float W = disp.x, H = disp.y;
    const Phase ph = phase_at(tl, t);
    float p = phase_progress(tl, t);
    const Geo g = geo_for(sys);
    const bool disc_kind = g.kind == Insert::Lid || g.kind == Insert::Tray || g.kind == Insert::Front;

    const float cs = std::max(0.4f, std::min(W / 1280.f, H / 800.f) * 1.55f) * g.cs;   // console scale (1 = ~300 px wide console)
    const ImVec2 console_c(W * 0.5f, H * g.cy);
    const float ms = cs * g.ms;                       // media scale when seated

    dl->AddRectFilledMultiColor(ImVec2(0, 0), disp, IM_COL32(18, 19, 26, 255), IM_COL32(18, 19, 26, 255), IM_COL32(10, 11, 15, 255), IM_COL32(10, 11, 15, 255));

    const ImVec2 shelf_pos(W * 0.5f, H * 0.46f), out_pos(W * 0.5f, H * 0.28f);

    float power = 0.f, lid = 0.f, console_dy = 0.f, shelf_dark = 0.f;
    bool console_visible = true, media_visible = true;
    float mscale = ms * 1.5f, mrot = 0.f, mxs = 1.f, mys = 1.f, mang = 0.f;
    float ex = g.ex, ey = g.ey;                       // media centre in console-local units
    float hy = g.ey - g.hover;                        // hover height (console-local)
    ImVec2 mpos = shelf_pos;
    bool tray_follow = false;                         // PS2: the disc rides the tray
    const float tray_y0 = 42.f;

    switch (ph) {
        case Phase::Slide: {
            console_visible = false;
            float e = ease_out_back(p);
            lerp_pt(shelf_pos, out_pos, e, &mpos);
            mscale = ms * 1.5f * (0.55f + 0.45f * ease_out(p));
            break;
        }
        case Phase::Fly: {
            float e = ease_in_out(p);
            const float cdy = (1.f - ease_out(p)) * H * 0.35f;
            console_dy = cdy;
            ImVec2 hover_pt(console_c.x + ex * cs, console_c.y + hy * cs);
            if (g.kind == Insert::Tray) hover_pt.y = console_c.y + (93.f - g.hover) * cs;
            lerp_pt(out_pos, hover_pt, e, &mpos);
            mpos.x += std::sin(e * 3.14159f) * W * 0.06f;   // gentle arc
            mscale = ms * lerpf(1.5f, 1.f, e);
            mrot = disc_kind ? e * 6.2832f * 1.5f : std::sin(e * 6.2832f) * 0.10f;
            if (disc_kind) mys = lerpf(1.f, 0.8f, e);
            shelf_dark = ease_out(p);
            if (g.kind == Insert::Lid || g.kind == Insert::Tray) lid = ease_in_out(p);
            break;
        }
        case Phase::Insert: {
            shelf_dark = 1.f;
            /* let it breathe: the media hangs lined up over the slot for a beat, slides in, then sits still a moment before power */
            p = std::min(1.f, std::max(0.f, (p - 0.20f) / 0.65f));
            const float hover_y = g.kind == Insert::Tray ? 93.f - g.hover : hy;
            float y_local = ey, x_local = ex;
            mscale = ms;
            switch (g.kind) {
                case Insert::Behind: {
                    y_local = lerpf(hover_y, ey, ease_in_out(p));
                    break;
                }
                case Insert::Slot: {
                    if (g.push > 0.f) {   // front loader: pushed in first (tilted), then pressed down flat
                        const float mid = ey - g.push, k1 = 0.70f;
                        if (p < k1) { float e = ease_in_out(p / k1); y_local = lerpf(hover_y, mid, e); mang = -0.14f * e; }
                        else { float e = ease_out((p - k1) / (1.f - k1)); y_local = lerpf(mid, ey, e); mang = -0.14f * (1.f - e); }
                    } else {
                        y_local = lerpf(hover_y, ey, ease_in_out(p));
                    }
                    break;
                }
                case Insert::Lid: {
                    const float d1 = 0.55f;
                    float e = ease_in_out(std::min(1.f, p / d1));
                    y_local = lerpf(hover_y, ey, e);
                    mys = lerpf(0.8f, g.ys_end, e);
                    mrot = (1.5f + e * 0.3f) * 6.2832f;
                    lid = p < d1 ? 1.f : 1.f - ease_in_out((p - d1) / (1.f - d1));
                    break;
                }
                case Insert::Tray: {
                    const float d1 = 0.5f;
                    float e = ease_in_out(std::min(1.f, p / d1));
                    mys = lerpf(0.8f, g.ys_end, e);
                    mrot = (1.5f + e * 0.3f) * 6.2832f;
                    lid = p < d1 ? 1.f : 1.f - ease_in_out((p - d1) / (1.f - d1));
                    if (p < d1) y_local = lerpf(hover_y, 93.f, e);
                    else tray_follow = true;
                    break;
                }
                case Insert::Front: {
                    float e = ease_in_out(p);
                    y_local = lerpf(hover_y, ey, e);
                    mys = lerpf(0.8f, g.ys_end, e);
                    mrot = (1.5f + e * 0.2f) * 6.2832f;
                    break;
                }
            }
            if (tray_follow) {   // same formula as the console's tray drawing
                const float tr = ease_in_out(lid);   // lid is the raw 0..1 amount; the art smooth-steps it again
                (void)tr;
                float tray = lid * lid * (3.f - 2.f * lid);
                float wy = 44.f + 96.f * tray;
                y_local = (tray_y0 + wy) * 0.5f + 2.f;   // centre of the tray ellipse
            }
            ey = y_local; ex = x_local;
            break;
        }
        default: {   // Power, Fade, Done: the media is seated
            shelf_dark = 1.f;
            power = ph == Phase::Power ? ease_out(p) : 1.f;
            mscale = ms;
            mys = disc_kind ? g.ys_end : 1.f;
            if (g.kind == Insert::Front) media_visible = false;   // swallowed by the slot
            if (g.kind == Insert::Tray) media_visible = false;    // inside the closed tray bay
            break;
        }
    }
    if (ph == Phase::Power || ph == Phase::Fade || ph == Phase::Done) { ex = g.ex; ey = g.ey; }
    if (ph == Phase::Slide) { /* mpos already set */ }
    else if (ph == Phase::Insert) mpos = ImVec2(console_c.x + ex * cs, console_c.y + console_dy + ey * cs);
    else if (ph != Phase::Fly) mpos = ImVec2(console_c.x + ex * cs, console_c.y + ey * cs);

    if (shelf_dark < 1.f && ph != Phase::Idle) {
        dl->PushClipRect(ImVec2(0, 0), disp, true);
        draw_shelf(dl, ImVec2(0, 0), disp, t);
        dl->PopClipRect();
        if (shelf_dark > 0.f) dl->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(12, 13, 18, (int)(255 * shelf_dark)));
    }

    const ImVec2 cpos(console_c.x, console_c.y + console_dy);
    if (console_visible) {   // soft glow under the console, brighter once powered
        float gl = 0.25f + 0.75f * power;
        for (int i = 0; i < 7; ++i)
            dl->AddCircleFilled(cpos, 100.f * cs * (3.4f - 0.4f * i), IM_COL32(108, 122, 255, (int)(7 * gl)), 64);
    }

    /* the cover art as the media's label */
    ImTextureID label = (ImTextureID)nullptr;
    ImVec2 uv0(0, 0), uv1(1, 1);
    {
        int tw = 0, th = 0;
        if (SDL_Texture* tex = hubart::get(G, hubart::Kind::Boxart, &tw, &th)) {
            label = (ImTextureID)tex;
            label_uv(tw, th, &uv0, &uv1);
        }
    }

    auto draw_m = [&]() { draw_media_xf(sys, dl, mpos, mscale, mrot, label, uv0, uv1, mxs, mys, mang); };
    const bool in_console = (g.kind == Insert::Lid || g.kind == Insert::Tray) && (ph == Phase::Insert || ph == Phase::Power || ph == Phase::Fade || ph == Phase::Done);
    const bool landing = g.kind == Insert::Tray && ph == Phase::Insert;

    if (!media_visible || ph == Phase::Slide || ph == Phase::Fly || g.kind == Insert::Slot || g.kind == Insert::Front || g.kind == Insert::Behind) {}

    if (g.kind == Insert::Behind) {
        if (media_visible) draw_m();
        if (console_visible) draw_console(sys, dl, cpos, cs, lid, power);
    } else if ((g.kind == Insert::Lid || g.kind == Insert::Tray) && console_visible && ph != Phase::Fly) {
        /* disc in the well / on the tray: drawn by the console between the well and the lid */
        set_console_inner([&](ImDrawList*) {
            if (!media_visible && g.kind == Insert::Tray) return;
            if (g.kind == Insert::Tray) {
                float tray = lid * lid * (3.f - 2.f * lid);
                float wy = 44.f + 96.f * tray;
                dl->PushClipRect(ImVec2(0, cpos.y + tray_y0 * cs), ImVec2(W, cpos.y + (wy + 6.f) * cs), true);
                draw_m();
                dl->PopClipRect();
            } else {
                draw_m();
            }
        });
        draw_console(sys, dl, cpos, cs, lid, power);
        set_console_inner(nullptr);
        (void)in_console; (void)landing;
    } else {
        if (console_visible) draw_console(sys, dl, cpos, cs, lid, power);
        if (media_visible) {
            if (g.kind == Insert::Slot || g.kind == Insert::Front) {
                dl->PushClipRect(ImVec2(0, 0), ImVec2(W, cpos.y + g.clip * cs), true);
                draw_m();
                dl->PopClipRect();
            } else draw_m();
        }
    }

    /* title + system */
    {
        float ta = ph == Phase::Slide ? ease_out(p) : 1.f;
        if (ph == Phase::Fade) ta = 1.f - p;
        if (ta > 0.01f) {
            std::string title = ui::ellipsize(ui::F.h2, ui::F.h2->FontSize * 1.4f, G.title, W * 0.8f);
            float ty = H * 0.88f;
            text_center(dl, ui::F.h2, ui::F.h2->FontSize * 1.4f, ImVec2(W * 0.5f, ty), ui::scale_alpha(ui::TEXT, ta), title.c_str());
            text_center(dl, ui::F.cap, ui::F.cap->FontSize * 1.2f, ImVec2(W * 0.5f, ty + ui::F.h2->FontSize * 1.7f), ui::scale_alpha(ui::TEXT2, ta), hub::system_name(sys));
        }
    }
    /* skip hint */
    if (t > 0.5f && ph != Phase::Fade && !g_frozen) {
        float a = std::min(1.f, (t - 0.5f) * 2.f) * 0.55f;
        text_center(dl, ui::F.cap, ui::F.cap->FontSize, ImVec2(W * 0.5f, H - ui::F.cap->FontSize * 2.6f), ui::scale_alpha(ui::TEXT3, a), "Press any button to skip");
    }
    /* fade to black */
    if (ph == Phase::Fade || ph == Phase::Done) {
        float a = ph == Phase::Done ? 1.f : ease_in_out(p);
        dl->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 0, 0, (int)(255 * a)));
    }
}

} // namespace

Options& options() { return g_opt; }

void shot_freeze(hub::System s, float t) { g_frozen = true; g_frozen_sys = s; g_frozen_t = t; }
bool shot_frozen() { return g_frozen; }

void start(const hub::GameEntry& e, std::function<void()> on_done, bool online, bool quick, bool real_boot) {
    Mode m = choose_mode(g_opt, online, quick);
    if (g_frozen) m = Mode::Full;
    if (const char* tr = std::getenv("HUB_SHOT_TRACE"); tr && *tr) {
        if (FILE* f = std::fopen(tr, "a")) { std::fprintf(f, "bootanim start %s mode=%d\n", e.key.c_str(), (int)m); std::fclose(f); }
    }
    if (R.running) { skip(); }   // a second start never stacks: finish the first (launches it) -- callers guard against this anyway
    G = e;
    g_done = std::move(on_done);
    R.begin(m, real_boot);
    if (g_frozen) { R.armed = true; R.t = g_frozen_t; G.system = g_frozen_sys; }
    if (m == Mode::Skip) {
        auto cb = std::move(g_done);
        g_done = nullptr;
        if (cb) cb();
    }
}

bool active() { return R.running; }

void skip() {
    if (g_frozen) return;
    if (R.skip()) {
        sound_stop();
        auto cb = std::move(g_done);
        g_done = nullptr;
        if (cb) cb();
    }
}

void draw() {
    if (!R.running) return;
    ImGuiIO& io = ImGui::GetIO();
    bool fire = false, stop_snd = false;
    if (!g_frozen) {
        if (any_input() && R.skip()) { fire = true; stop_snd = true; }
        else if (R.tick(io.DeltaTime)) fire = true;
        if (!fire && g_opt.sounds && R.want_sound() && sound_available(G.system)) sound_play(G.system, R.tl.total() < 2.f);
    }
    if (!fire) {
        if (!ImGui::IsPopupOpen(kPopup)) ImGui::OpenPopup(kPopup);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(10, 11, 15, 255));
        if (ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings)) {
            render(ImGui::GetWindowDrawList(), io.DisplaySize, G.system, R.tl, R.t);
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
        return;
    }
    if (ImGui::IsPopupOpen(kPopup)) {   // close it; the frame after this one has no popup
        if (ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_NoTitleBar)) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); }
    }
    if (stop_snd) sound_stop();
    auto cb = std::move(g_done);
    g_done = nullptr;
    if (cb) cb();
}

} // namespace bootanim
