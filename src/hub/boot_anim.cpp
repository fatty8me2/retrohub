#include "boot_anim.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "art.h"
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

/* Per-media geometry, in units of 100 px * console scale: where the media hovers above the console, where it ends up (the console
 * art is drawn over it, so it looks pushed in), and its size relative to the console. Retune here when the art changes. */
struct Geo { float hover_dy, end_dy, size; bool lid; };
Geo geo_for(Media m) {
    switch (m) {
        case Media::Cartridge: return {-2.7f, -1.05f, 0.80f, false};
        case Media::SmallCart: return {-2.5f, -0.90f, 0.70f, false};
        case Media::Card:      return {-2.2f, -0.60f, 0.55f, false};
        case Media::MiniDisc:  return {-2.4f, -0.25f, 0.65f, true};
        case Media::Disc:      return {-2.5f, -0.15f, 0.80f, true};
        default:               return {-2.4f, -0.50f, 0.70f, false};
    }
}

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

void render(ImDrawList* dl, ImVec2 disp, hub::System sys, const Timeline& tl, float t) {
    const float W = disp.x, H = disp.y;
    const Phase ph = phase_at(tl, t);
    const float p = phase_progress(tl, t);
    const Media media = media_for(sys);
    const Geo g = geo_for(media);

    const float cs = std::max(0.4f, std::min(W / 1280.f, H / 800.f) * 1.25f);   // console scale (1 = 300 px wide console)
    const float u = 100.f * cs;
    const ImVec2 console_c(W * 0.5f, H * 0.68f);
    const float ms = cs * g.size * 1.35f;

    /* background */
    dl->AddRectFilledMultiColor(ImVec2(0, 0), disp, IM_COL32(18, 19, 26, 255), IM_COL32(18, 19, 26, 255), IM_COL32(10, 11, 15, 255), IM_COL32(10, 11, 15, 255));

    const ImVec2 shelf_pos(W * 0.5f, H * 0.46f), out_pos(W * 0.5f, H * 0.30f);
    const ImVec2 hover(console_c.x, console_c.y + g.hover_dy * u);
    const ImVec2 end_pt(console_c.x, console_c.y + g.end_dy * u);

    float power = 0.f, lid = 0.f, console_dy = 0.f, shelf_dark = 0.f;
    bool console_visible = true, media_visible = true, media_in_front = true;
    ImVec2 mpos = shelf_pos;
    float mscale = ms, mrot = 0.f;
    const bool disc = media == Media::Disc || media == Media::MiniDisc;

    switch (ph) {
        case Phase::Slide: {
            console_visible = false;
            float e = ease_out_back(p);
            lerp_pt(shelf_pos, out_pos, e, &mpos);
            mscale = ms * (0.55f + 0.45f * ease_out(p));
            break;
        }
        case Phase::Fly: {
            float e = ease_in_out(p);
            lerp_pt(out_pos, hover, e, &mpos);
            mpos.x += std::sin(e * 3.14159f) * W * 0.06f;   // gentle arc
            mscale = ms;
            mrot = disc ? e * 6.2832f * 1.5f : std::sin(e * 6.2832f) * 0.12f;
            shelf_dark = ease_out(p);
            console_dy = (1.f - ease_out(p)) * H * 0.35f;
            if (g.lid) lid = ease_in_out(p);
            break;
        }
        case Phase::Insert: {
            float e = ease_in_out(std::min(1.f, p / 0.8f));
            lerp_pt(hover, end_pt, e, &mpos);
            media_in_front = false;
            shelf_dark = 1.f;
            if (disc) mrot = e * 1.2f;
            if (g.lid) lid = p < 0.55f ? 1.f : 1.f - ease_in_out((p - 0.55f) / 0.45f);
            if (g.lid && p > 0.97f) media_visible = false;
            break;
        }
        default: {
            shelf_dark = 1.f;
            media_visible = false;
            power = ph == Phase::Power ? ease_out(p) : 1.f;
            break;
        }
    }

    if (shelf_dark < 1.f && ph != Phase::Idle) {
        dl->PushClipRect(ImVec2(0, 0), disp, true);
        draw_shelf(dl, ImVec2(0, 0), disp, t);
        dl->PopClipRect();
        if (shelf_dark > 0.f) dl->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(12, 13, 18, (int)(255 * shelf_dark)));
    }

    if (console_visible) {   // soft glow under the console, brighter once powered
        float gl = 0.25f + 0.75f * power;
        for (int i = 0; i < 7; ++i)
            dl->AddCircleFilled(ImVec2(console_c.x, console_c.y + console_dy), u * (3.4f - 0.4f * i), IM_COL32(108, 122, 255, (int)(7 * gl)), 64);
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

    const ImVec2 cpos(console_c.x, console_c.y + console_dy);
    auto draw_m = [&]() { if (media_visible) draw_media(sys, dl, mpos, mscale, mrot, label, uv0, uv1); };
    auto draw_c = [&]() { if (console_visible) draw_console(sys, dl, cpos, cs, lid, power); };
    if (media_in_front) { draw_c(); draw_m(); }
    else { draw_m(); draw_c(); }

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

void start(const hub::GameEntry& e, std::function<void()> on_done, bool online, bool quick) {
    Mode m = choose_mode(g_opt, online, quick);
    if (g_frozen) m = Mode::Full;
    if (const char* tr = std::getenv("HUB_SHOT_TRACE"); tr && *tr) {
        if (FILE* f = std::fopen(tr, "a")) { std::fprintf(f, "bootanim start %s mode=%d\n", e.key.c_str(), (int)m); std::fclose(f); }
    }
    if (R.running) { skip(); }   // a second start never stacks: finish the first (launches it) -- callers guard against this anyway
    G = e;
    g_done = std::move(on_done);
    R.begin(m);
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
        if (!fire && g_opt.sounds && R.want_sound() && sound_available(G.system)) sound_play(G.system);
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
