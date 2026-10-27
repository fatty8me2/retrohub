/* Attract mode (Wave 3 L6). After about a minute of no input on Home (window focused, nothing else open) the hero turns into
 * a slow Ken Burns slideshow of the player's OWN pictures: save-state thumbnails and screenshots of recently played games
 * (vault <system>/<key>/states and /shots). Crossfades between games, shows the title and a "Press any button" hint. Any input
 * stops it at once and puts the focus back on the hero. Local files only: nothing is downloaded. Settings > General toggle
 * (hub.json attract_mode, default on). The idle timer and slideshow maths are pure (attract_logic.h, unit tested).
 * Screenshot aid: HUB_ATTRACT_SECS=<n> (headless only) shortens the idle time. */
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"
#include "app.h"
#include "homefun.h"
#include "attract_logic.h"
#include "../saves/savestore.h"

namespace {

namespace at = hubui::attract;
namespace fs = std::filesystem;

at::Idle IDLE;
bool g_wake = false;   // an event was swallowed (attract_swallow_event): count it as input
std::vector<at::Slide> SLIDES;

struct Tex {
    SDL_Texture* t = nullptr;
    int w = 0, h = 0;
    bool failed = false;
};
std::map<std::string, Tex> TEX;

double idle_threshold() {
    static const double v = [] {
        const char* s = g_headless ? std::getenv("HUB_ATTRACT_SECS") : nullptr;
        double x = s ? std::atof(s) : 0.0;
        return x > 0.0 ? x : at::kIdleSecs;
    }();
    return v;
}

bool any_input(const ImGuiIO& io) {
    if (g_wake) return true;
    if (io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f || io.MouseWheel != 0.f || io.MouseWheelH != 0.f) return true;
    for (int b = 0; b < 5; ++b) if (io.MouseDown[b]) return true;
    if (!io.InputQueueCharacters.empty()) return true;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        if (k >= ImGuiKey_MouseLeft && k <= ImGuiKey_MouseWheelY) continue;   // handled above
        if (ImGui::IsKeyDown((ImGuiKey)k)) return true;
    }
    return false;
}

bool is_png(const fs::path& p) {
    std::string e = p.extension().string();
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e == ".png";
}

long long mtime_unix(const fs::path& p) {
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec) return 0;
    auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return (long long)std::chrono::system_clock::to_time_t(sys);
}

void collect(const std::string& dir, bool is_state, const hub::GameEntry& e, std::vector<at::Slide>* out) {
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || !is_png(it->path())) continue;
        const std::string name = it->path().filename().u8string();
        if (name.compare(0, 8, "wrapped-") == 0) continue;   // RetroHub Wrapped cards are not game pictures
        at::Slide s;
        s.key = e.key;
        s.title = e.title;
        s.path = it->path().u8string();
        s.when = mtime_unix(it->path());
        s.is_state = is_state;
        out->push_back(s);
    }
}

/* The slideshow: up to kMaxGames recently played games, kPerGame newest pictures each, interleaved so neighbours differ. */
std::vector<at::Slide> build_slides() {
    std::vector<const hub::GameEntry*> games;
    for (auto& e : A.lib) {
        if (is_native(e)) continue;
        if (gstate(e.key).last_played == 0) continue;
        games.push_back(&e);
    }
    std::stable_sort(games.begin(), games.end(), [](const hub::GameEntry* a, const hub::GameEntry* b) { return gstate(a->key).last_played > gstate(b->key).last_played; });
    if (games.size() > at::kMaxGames) games.resize(at::kMaxGames);
    std::vector<std::vector<at::Slide>> per;
    for (auto* e : games) {
        std::vector<at::Slide> pics;
        collect(hub::saves::states_dir(sys_short(*e), e->key), true, *e, &pics);
        collect(no_slash(hub::saves::game_dir(sys_short(*e), e->key)) + "\\shots", false, *e, &pics);
        std::stable_sort(pics.begin(), pics.end(), [](const at::Slide& a, const at::Slide& b) { return a.when > b.when; });
        if (pics.size() > at::kPerGame) pics.resize(at::kPerGame);
        if (!pics.empty()) per.push_back(std::move(pics));
    }
    return at::interleave(per);
}

void free_textures() {
    for (auto& kv : TEX) if (kv.second.t) SDL_DestroyTexture(kv.second.t);
    TEX.clear();
}

/* Texture for a picture; decodes at most one new picture per call (no hitch when two are needed). */
Tex* tex_for(const std::string& path) {
    auto it = TEX.find(path);
    if (it != TEX.end()) return &it->second;
    Tex t;
    std::ifstream f(fs::u8path(path), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    int iw = 0, ih = 0, comp = 0;
    unsigned char* px = bytes.empty() ? nullptr : stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &iw, &ih, &comp, 4);
    SDL_Renderer* ren = A.win ? SDL_GetRenderer(A.win) : nullptr;
    if (px && ren && iw > 0 && ih > 0) {
        t.t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, iw, ih);
        if (t.t) {
            SDL_UpdateTexture(t.t, nullptr, px, iw * 4);
            SDL_SetTextureBlendMode(t.t, SDL_BLENDMODE_BLEND);
            t.w = iw;
            t.h = ih;
        }
    }
    if (px) stbi_image_free(px);
    t.failed = t.t == nullptr;
    return &(TEX[path] = t);
}

void evict_except(const std::set<std::string>& keep) {
    for (auto it = TEX.begin(); it != TEX.end();) {
        if (keep.count(it->first)) { ++it; continue; }
        if (it->second.t) SDL_DestroyTexture(it->second.t);
        it = TEX.erase(it);
    }
}

void draw_slide_image(ImDrawList* dl, const at::Slide& s, Tex* t, int idx, float progress, ImVec2 p0, ImVec2 p1, float rad, float alpha) {
    if (alpha <= 0.f) return;
    if (!t || !t->t) {   // not loaded yet (or broken): the game's own colours
        ImU32 c0 = ui::PANEL2, c1 = ui::PANEL;
        if (const hub::GameEntry* e = find_entry(s.key)) entry_colors(*e, &c0, &c1);
        gradient_round_rect(dl, p0, p1, ui::scale_alpha(mix(c0, ui::BG, 0.5f), alpha), ui::scale_alpha(mix(c1, ui::BG, 0.6f), alpha), rad);
        return;
    }
    at::UV cover = at::cover_uv((float)t->w, (float)t->h, p1.x - p0.x, p1.y - p0.y);
    at::UV uv = at::ken_burns(idx, progress, cover);
    dl->AddImageRounded((ImTextureID)t->t, p0, p1, ImVec2(uv.u0, uv.v0), ImVec2(uv.u1, uv.v1), IM_COL32(255, 255, 255, (int)(255 * std::max(0.f, std::min(1.f, alpha)))), rad);
}

}  // namespace

bool attract_active() { return IDLE.active; }

bool attract_swallow_event(const SDL_Event& e) {
    if (!IDLE.active) return false;
    switch (e.type) {
        case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT: case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL: case SDL_FINGERDOWN: case SDL_FINGERMOTION:
            g_wake = true;
            return true;
        default: return false;
    }
}

bool attract_update() {
    ImGuiIO& io = ImGui::GetIO();
    const bool popup = ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool eligible = A.st.attract_mode && A.page == Page::Library && A.view == LibView::Home && A.focused && !A.proc &&
                          !A.lib.empty() && !A.scanning && !popup && !osk::is_open() && !io.WantTextInput && !A.search_buf[0] && !A.in_room;
    const bool input = any_input(io);
    g_wake = false;
    const bool was_active = IDLE.active;
    const bool woke = IDLE.step(std::min((double)io.DeltaTime, 0.25), input, eligible, idle_threshold());
    if (IDLE.active && !was_active) {   // just started: gather the pictures; with none there is nothing to show, try again later
        SLIDES = build_slides();
        if (SLIDES.empty()) { IDLE.active = false; IDLE.idle = 0; shot_trace("attract", "no-pictures"); }
        else shot_trace("attract", "start slides=" + std::to_string(SLIDES.size()));
    }
    if (woke || (!IDLE.active && !TEX.empty())) {
        if (woke) {
            A.nav_seed = true;       // focus goes back to the hero's Play button
            {   // a D-pad / arrow press that woke us must not also move the focus somewhere else
                ImGuiContext& g = *ImGui::GetCurrentContext();
                ImGui::NavMoveRequestCancel();
                g.NavInitRequest = g.NavInitRequestFromMove = false;
                g.NavInitResult.ID = 0;
            }
            A.hero_key.clear();      // and the hero fades back in
            shot_trace("attract", "stop");
        }
        free_textures();
        SLIDES.clear();
    }
    return IDLE.active;
}

void attract_draw_hero(float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + h);
    const float rad = sp(2.5f);
    const int n = (int)SLIDES.size();
    const at::SlideFrame fr = at::slide_at(IDLE.shown, n);

    ui::shadow(dl, p0, p1, rad, 1.3f);
    dl->AddRectFilled(p0, p1, ui::BG, rad);
    if (fr.cur >= 0) {
        const double local = IDLE.shown - std::floor(IDLE.shown / at::kSlideSecs) * at::kSlideSecs;
        const float u_cur = (float)(local / at::kSlideSecs);
        const float u_next = std::max(0.f, (float)((local - (at::kSlideSecs - at::kFadeSecs)) / at::kSlideSecs));
        // keep the current and the next two pictures; load at most one missing one per frame (current first)
        std::set<std::string> keep;
        for (int k = 0; k < 3 && k < n; ++k) keep.insert(SLIDES[(fr.cur + k) % n].path);
        evict_except(keep);
        bool loaded_one = false;
        auto get = [&](int k) -> Tex* {
            const std::string& pth = SLIDES[(fr.cur + k) % n].path;
            auto itx = TEX.find(pth);
            if (itx != TEX.end()) return &itx->second;
            if (loaded_one) return nullptr;
            loaded_one = true;
            return tex_for(pth);
        };
        Tex* tc = get(0);
        Tex* tn = n > 1 ? get(1) : nullptr;
        if (n > 2) get(2);
        draw_slide_image(dl, SLIDES[fr.cur], tc, fr.cur, u_cur, p0, p1, rad, 1.f);
        if (fr.mix > 0.f) draw_slide_image(dl, SLIDES[fr.next], tn, fr.next, u_next, p0, p1, rad, fr.mix);

        // readability: dark ramps on the left and at the bottom
        ui::hfade_round_rect(dl, p0, p1, IM_COL32(14, 15, 20, 255), rad, 0, 0.82f, 0.0f);
        ui::vfade_round_rect(dl, ImVec2(p0.x, p0.y + h * 0.45f), p1, IM_COL32(14, 15, 20, 255), rad, ImDrawFlags_RoundCornersBottom, 0.f, 0.7f);

        const float pad = sp(3.f);
        ImFont* cf = ui::F.cap;
        ImFont* tf = ui::F.title;
        // text of the dominant slide, crossfaded with the next
        auto text_for = [&](const at::Slide& s, float a) {
            if (a <= 0.01f) return;
            const float ty = p1.y - pad - tf->FontSize - cf->FontSize - sp(1.f);
            std::string cap = std::string(s.is_state ? "From a save state" : "From your screenshots");
            if (s.when > 0) cap += "  \xC2\xB7  " + fmt_ago((uint64_t)s.when);
            dl->AddText(cf, cf->FontSize, ImVec2(p0.x + pad, ty), ui::scale_alpha(mix(ui::ACCENT, IM_COL32_WHITE, 0.6f), a), ui::ellipsize(cf, cf->FontSize, cap, w * 0.6f).c_str());
            ui::text_shadowed(dl, tf, tf->FontSize, ImVec2(p0.x + pad, ty + cf->FontSize + sp(0.75f)), ui::scale_alpha(IM_COL32_WHITE, a),
                              ui::ellipsize(tf, tf->FontSize, s.title, w * 0.62f).c_str());
        };
        if (fr.mix > 0.f && SLIDES[fr.next].key == SLIDES[fr.cur].key) text_for(SLIDES[fr.cur], 1.f);   // same game: no text flicker
        else {
            text_for(SLIDES[fr.cur], 1.f - fr.mix);
            if (fr.mix > 0.f) text_for(SLIDES[fr.next], fr.mix);
        }
        // hint
        const float pulse = 0.55f + 0.45f * (0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 2.2f));
        const char* hint = "Press any button";
        const ImVec2 ts = ui::F.bold->CalcTextSizeA(ui::F.bold->FontSize, FLT_MAX, 0.f, hint);
        const ImVec2 hp(p1.x - pad - ts.x - sp(2.f), p1.y - pad - ts.y - sp(0.5f));
        dl->AddRectFilled(ImVec2(hp.x - sp(1.5f), hp.y - sp(0.75f)), ImVec2(hp.x + ts.x + sp(1.5f), hp.y + ts.y + sp(0.75f)), ui::scale_alpha(IM_COL32(10, 11, 15, 190), pulse), sp(1.5f));
        dl->AddText(ui::F.bold, ui::F.bold->FontSize, hp, ui::scale_alpha(ui::TEXT, pulse), hint);
    }
    dl->AddRect(p0, p1, ui::HAIRLINE, rad, 0, 1.f);

    // nothing in the hero is focusable now; make sure a stray A / Enter cannot activate a tile while the picture is showing
    ImGui::SetNavID(0, ImGuiNavLayer_Main, 0, ImRect());
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(w, sp(2.f)));
}
