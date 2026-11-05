/* "Welcome back" card: the first time RetroHub opens on a new version, a short animated summary of what changed since the last version
 * the player saw. Rules and card building are pure (welcome_logic.h, hub_welcome_test); this file only draws and wires them in.
 *
 * Hard rules: never while a game runs (it waits until the game exits and the hub already has the focus; nothing here raises, focuses or
 * shows a window), never on the lock screen, in headless / QA / autotest runs or over the first-run wizard. A fresh install just records
 * the version. Closing (Let's go, B, Esc, See all changes, Show me) marks the version seen; a game starting while it is open closes it
 * WITHOUT marking, so it is offered again later.
 *
 * Shot aids (headless only): HUB_SHOT_WELCOME=1 forces the card, HUB_SHOT_WELCOME_FROM=<ver> fake last-seen version (default 1.31.9),
 * HUB_SHOT_WELCOME_T=<secs> freezes the animation clock (mid-animation frames), HUB_SHOT_WELCOME_OWNER=1 shows the owner-only "Show me". */
#include "app.h"
#include "welcome_logic.h"
#include "wizard_logic.h"

namespace hw = hubwelcome;

namespace {

struct WelcomeState {
    bool open = false, request = false;
    bool done = false;                 // nothing more to decide this run
    bool computed = false;
    std::vector<hw::Card> cards;
    std::string from, to, title;       // last seen, current, title of the newest release
    int versions = 0;                  // releases collapsed into the card
    double t0 = 0;
    bool forced = false;
    bool reduced = false;
    double freeze = -1;
} WS;

bool os_reduced_motion() {
    return ui::reduce_motion;   // Settings > Reduce motion (its first-run default follows Windows "Show animations")
}

bool env_on(const char* n) { const char* v = std::getenv(n); return v && *v && *v != '0'; }

ui::Icon topic_icon(hw::Topic t) {
    switch (t) {
        case hw::Topic::Discover: return ui::Icon::Star;
        case hw::Topic::Admin: return ui::Icon::Info;
        case hw::Topic::Saves: return ui::Icon::Cloud;
        case hw::Topic::Controls: return ui::Icon::Gamepad;
        case hw::Topic::Online: return ui::Icon::Globe;
        case hw::Topic::Library: return ui::Icon::Home;
        case hw::Topic::Look: return ui::Icon::Star;
        case hw::Topic::Settings: return ui::Icon::Gear;
        case hw::Topic::Fix: return ui::Icon::Refresh;
        default: return ui::Icon::Bolt;
    }
}

bool target_page(hw::Target t, Page* p) {
    switch (t) {
        case hw::Target::Library: *p = Page::Library; return true;
        case hw::Target::Discover: *p = Page::Discover; return true;
        case hw::Target::Online: *p = Page::Online; return true;
        case hw::Target::Saves: *p = Page::Saves; return true;
        case hw::Target::Controllers: *p = Page::Controllers; return true;
        case hw::Target::Settings: *p = Page::Settings; return true;
        case hw::Target::Updates: *p = Page::About; return true;
        case hw::Target::Admin: *p = Page::Admin; return true;
        default: return false;
    }
}

void build(const std::string& last_seen, const std::string& current, bool owner) {
    WS.computed = true;
    WS.cards.clear();
    WS.from = last_seen;
    WS.to = current;
    const auto vs = hw::versions_since(changelog_entries(), last_seen, current);
    WS.versions = (int)vs.size();
    WS.title = vs.empty() ? "" : vs.front()->title;
    WS.cards = hw::collect_cards(vs, owner);
}

void close_card(bool mark, Page go, bool navigate) {
    WS.open = false;
    if (mark) { A.st.changelog_seen = WS.to; mark_dirty(); }
    if (navigate) A.page = go;
}

/* Confetti + sparkles from the logo: ~1.3 s, deterministic (no RNG state), foreground list so it can leave the card. */
void draw_burst(ImVec2 c, float t, float S_) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 pal[] = {ui::ACCENT, ui::GOOD, ui::WARN, IM_COL32(255, 105, 180, 255), IM_COL32(120, 200, 255, 255), IM_COL32(255, 255, 255, 255)};
    const float life = 1.5f;
    if (t < 0.f || t > life) return;
    for (int i = 0; i < 34; ++i) {
        const float h1 = std::fmod(std::sin((float)i * 12.9898f) * 43758.5453f, 1.f), h2 = std::fmod(std::sin((float)i * 78.233f) * 12345.678f, 1.f);
        const float a = (std::fabs(h1) * 6.2831853f);
        const float speed = (150.f + std::fabs(h2) * 260.f) * S_;
        const float x = c.x + std::cos(a) * speed * t * (1.f - 0.35f * t);
        const float y = c.y + std::sin(a) * speed * t * (1.f - 0.35f * t) + 260.f * S_ * t * t;   // gravity
        const float k = 1.f - t / life;
        const ImU32 col = ui::with_alpha(pal[i % 6], std::min(1.f, k * 1.6f));
        const float sz = (3.f + (i % 4)) * S_;
        if (i % 3 == 0) {   // sparkle: 4-point star
            const float r = sz * 1.8f * (0.6f + 0.4f * std::sin(t * 14.f + i));
            dl->AddQuadFilled(ImVec2(x, y - r), ImVec2(x + r * 0.28f, y), ImVec2(x, y + r), ImVec2(x - r * 0.28f, y), col);
            dl->AddQuadFilled(ImVec2(x - r, y), ImVec2(x, y - r * 0.28f), ImVec2(x + r, y), ImVec2(x, y + r * 0.28f), col);
        } else {   // confetti chip, spinning
            const float rot = t * (6.f + (i % 5)) + i;
            const float cx = std::cos(rot), sx = std::sin(rot);
            const ImVec2 p[4] = {{-sz, -sz * 0.5f}, {sz, -sz * 0.5f}, {sz, sz * 0.5f}, {-sz, sz * 0.5f}};
            ImVec2 q[4];
            for (int j = 0; j < 4; ++j) q[j] = ImVec2(x + p[j].x * cx - p[j].y * sx, y + p[j].x * sx + p[j].y * cx);
            dl->AddConvexPolyFilled(q, 4, col);
        }
    }
}

void wc_text(ImDrawList* dl, ImFont* f, float cx, float y, ImU32 col, const char* s) {
    ImGui::PushFont(f);
    const ImVec2 sz = ImGui::CalcTextSize(s);
    dl->AddText(f, f->FontSize, ImVec2(cx - sz.x * 0.5f, y), col, s);
    ImGui::PopFont();
}

}  // namespace

void draw_welcome() {
    const bool force = g_headless && env_on("HUB_SHOT_WELCOME");
    if (!WS.done && !WS.open && !WS.request) {
        hw::Context c;
        c.last_seen = force ? (std::getenv("HUB_SHOT_WELCOME_FROM") ? std::getenv("HUB_SHOT_WELCOME_FROM") : "1.31.9") : A.st.changelog_seen;
        c.current = HUB_VERSION_STR;
        c.enabled = A.st.show_whats_new;
        c.fresh_install = !force && hubui::wiz::should_autorun(A.st.setup_done, A.st.find_games_done, !A.st.rom_folders.empty());
        c.game_active = !g_headless && game_active();
        c.hub_focused = g_headless || hub_has_focus();
        c.locked = app_locked();
        c.wizard_open = wizard_state() != -1;
        c.other_popup = A.popup_last_frame;
        c.not_ready = !A.scanned_once;
        c.headless = g_headless && !force;
        c.qa_env = !force && (env_on("RETROHUB_QA") || env_on("RETROHUB_TEST"));
        c.autotest = autotest_active();
        if (force) { c.current = std::getenv("HUB_SHOT_WELCOME_TO") ? std::getenv("HUB_SHOT_WELCOME_TO") : "1.31.13"; c.fresh_install = false; c.enabled = true; }
        if (c.not_ready) return;   // library scan not finished: the hub is still booting
        if (!WS.computed && !c.not_ready) build(c.last_seen, c.current, force ? env_on("HUB_SHOT_WELCOME_OWNER") : fr_is_admin());
        c.has_cards = !WS.cards.empty();
        switch (hw::decide(c)) {
            case hw::Action::MarkSeen:
                if (!force) { A.st.changelog_seen = c.current; mark_dirty(); }
                WS.done = true;
                break;
            case hw::Action::Show:
                WS.request = true;
                WS.forced = force;
                break;
            case hw::Action::None:
                if (hw::cmp(c.last_seen, c.current) >= 0 || hw::cmp(c.current, "0") == 2) WS.done = true;   // nothing will ever change this run
                break;
        }
    }
    if (WS.request) {
        WS.request = false;
        WS.open = true;
        WS.t0 = ImGui::GetTime();
        WS.reduced = os_reduced_motion();
        WS.freeze = std::getenv("HUB_SHOT_WELCOME_T") && g_headless ? std::atof(std::getenv("HUB_SHOT_WELCOME_T")) : -1.0;
        WS.done = true;
        ImGui::OpenPopup("Welcome back##welcome");
    }
    if (!WS.open) return;

    const float t = WS.freeze >= 0 ? (float)WS.freeze : (float)(ImGui::GetTime() - WS.t0);
    const float p = hw::anim_progress(t, WS.reduced);
    const float S_ = ui::S;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float win_w = std::min(sp(78.f), vp->Size.x * 0.94f);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(win_w, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(2.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::min(1.f, 0.15f + p * 3.f));
    const bool open = ImGui::BeginPopupModal("Welcome back##welcome", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                                                                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(3);
    if (!open) { if (!ImGui::IsPopupOpen("Welcome back##welcome")) WS.open = false; return; }
    if (!g_headless && game_active()) {   // a game started while it was open: close it, NOT marked seen, so it is offered again later
        close_card(false, Page::Library, false);
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    if (ui::modal_back()) { close_card(true, Page::Library, false); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float inner = ImGui::GetContentRegionAvail().x;
    const float cx = wp.x + sp(3.f) + inner * 0.5f;

    /* Logo with a soft halo, bounce-in */
    const float logo_box = 72.f * S_;
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const ImVec2 lc(cx, top.y + logo_box * 0.5f);
    for (int i = 0; i < 5; ++i) dl->AddCircleFilled(lc, logo_box * (0.66f - i * 0.1f), ui::with_alpha(ui::ACCENT, 0.05f + 0.01f * i), 48);
    const float sc = hw::bounce_scale(std::min(1.f, p * 1.6f));
    const float ls = logo_box * 0.86f * sc;
    ui_draw_brand_logo(dl, ImVec2(lc.x - ls * 0.5f, lc.y - ls * 0.5f), ls);
    ImGui::Dummy(ImVec2(inner, logo_box + sp(0.5f)));

    const float tx = std::min(1.f, std::max(0.f, (t - 0.25f) / 0.4f));
    const float ty = ImGui::GetCursorScreenPos().y + (1.f - tx) * 8.f * S_;
    wc_text(dl, ui::F.title, cx, ty, ui::scale_alpha(ui::TEXT, tx), "Welcome back!");
    ImGui::Dummy(ImVec2(inner, ui::F.title->FontSize + sp(0.5f)));
    {
        std::string sub = "RetroHub " + hubui::app_version_label(WS.to) + (WS.title.empty() ? "" : ": " + WS.title);
        const float y = ImGui::GetCursorScreenPos().y;
        wc_text(dl, ui::F.h2, cx, y, ui::scale_alpha(ui::TEXT2, tx), ui::ellipsize(ui::F.h2, ui::F.h2->FontSize, sub, inner).c_str());
        ImGui::Dummy(ImVec2(inner, ui::F.h2->FontSize + sp(0.25f)));
        if (WS.versions > 1) {
            const std::string catch_up = "Catching you up on " + std::to_string(WS.versions) + " updates since " + hubui::app_version_label(WS.from);
            const float y2 = ImGui::GetCursorScreenPos().y;
            wc_text(dl, ui::F.body, cx, y2, ui::scale_alpha(ui::TEXT3, tx), catch_up.c_str());
            ImGui::Dummy(ImVec2(inner, ui::F.body->FontSize));
        }
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    /* Highlight cards */
    const float pad = sp(1.f), badge = 36.f * S_, gap = sp(0.25f), btn_w = sp(10.5f);
    int idx = 0;
    const float per_card = std::max(badge, ui::F.bold->FontSize + 3.f * S_ + 2.f * (ui::F.body->FontSize + 2.f * S_)) + pad * 2.f + gap + 8.f * S_;
    const float overhead = sp(5.f) + logo_box + ui::F.title->FontSize + ui::F.h2->FontSize + ui::F.body->FontSize + 44.f * S_ + sp(3.f) + 96.f * S_;
    const int limit = std::max(2, std::min(hw::kMaxCards, (int)((vp->Size.y * 0.95f - overhead) / per_card)));   // short windows show fewer cards
    for (const hw::Card& c : WS.cards) {
        if (idx >= limit) break;
        Page pg;
        const bool show_me = target_page(c.target, &pg);
        const float ca = std::min(1.f, std::max(0.f, (t - 0.45f - 0.09f * idx) / 0.3f));
        const float slide = (1.f - ca) * 12.f * S_;
        const float text_x0 = pad + badge + sp(1.25f);
        const float text_w = inner - text_x0 - pad - (show_me ? btn_w + sp(1.f) : 0.f);
        ImFont* bf = ui::F.bold;
        ImFont* nf = ui::F.body;
        const auto lines = ui::wrap_lines(nf, nf->FontSize, c.body, text_w, 2);
        float th = lines.size() * (nf->FontSize + 2.f * S_);
        if (!c.title.empty()) th += bf->FontSize + 3.f * S_;
        const float h = std::max(badge, th) + pad * 2.f;
        const ImVec2 p0(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + slide);
        const ImVec2 p1(p0.x + inner, p0.y + h);
        const float A_ = ca;
        dl->AddRectFilled(p0, p1, ui::scale_alpha(ui::PANEL2, A_), sp(1.25f));
        dl->AddRect(p0, p1, ui::scale_alpha(ui::HAIRLINE, A_), sp(1.25f));
        const ImVec2 bc(p0.x + pad + badge * 0.5f, p0.y + h * 0.5f);
        dl->AddCircleFilled(bc, badge * 0.5f, ui::scale_alpha(ui::mix(ui::PANEL2, ui::ACCENT, 0.25f), A_), 32);
        ui::icon(dl, topic_icon(c.topic), bc, badge * 0.5f, ui::scale_alpha(ui::ACCENT, A_));
        float ty2 = p0.y + (h - th) * 0.5f;
        if (!c.title.empty()) {
            dl->AddText(bf, bf->FontSize, ImVec2(p0.x + text_x0, ty2), ui::scale_alpha(ui::TEXT, A_), c.title.c_str());
            ty2 += bf->FontSize + 3.f * S_;
        }
        for (const std::string& ln : lines) {
            dl->AddText(nf, nf->FontSize, ImVec2(p0.x + text_x0, ty2), ui::scale_alpha(c.title.empty() ? ui::TEXT : ui::TEXT2, A_), ln.c_str());
            ty2 += nf->FontSize + 2.f * S_;
        }
        const ImVec2 row = ImGui::GetCursorScreenPos();
        if (show_me && ca >= 1.f) {
            ImGui::SetCursorScreenPos(ImVec2(p1.x - pad - btn_w, p0.y + (h - 36.f * S_) * 0.5f));
            ImGui::PushID(idx);
            if (ui::button("Show me", ImVec2(btn_w, 36.f * S_))) { close_card(true, pg, true); ImGui::PopID(); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(row);
        ImGui::Dummy(ImVec2(inner, h + gap));
        ++idx;
    }

    ImGui::Dummy(ImVec2(0, sp(1.f)));
    const float bw1 = sp(15.f), bw2 = sp(17.f), bh = 44.f * S_;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - bw1 - bw2 - sp(1.f)) * 0.5f);
    if (ui::button("Let's go", ImVec2(bw1, bh), true)) { close_card(true, Page::Library, false); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine(0, sp(1.f));
    if (ui::button("See all changes", ImVec2(bw2, bh))) { close_card(true, Page::About, true); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    ImGui::EndPopup();

    draw_burst(lc, t - 0.05f, S_);
}
