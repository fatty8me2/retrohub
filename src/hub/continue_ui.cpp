/* Home "Continue" hero (Plan 5): the newest save-state thumbnail (else newest screenshot, else the cover) fills the hero with a soft scrim,
 * "Last played 2 hours ago . 14 h total", a big Continue / Play button, Start fresh, Play Online and the usual star / details. Crossfades
 * when the hero game changes and pans slowly (Ken Burns, attract_logic.h). Plus the "Jump back in" row upgrade of the Continue Playing shelf.
 * Never launches by itself and never raises anything: it only draws and answers clicks inside the hub window.
 * Screenshot aid: HUB_SHOT_CONTINUE=1 seeds play history for the fake library, =playing also shows the hero as running. */
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"
#include "continue_ui.h"
#include "hack_ui.h"
#include "continue_logic.h"
#include "attract_logic.h"
#include "state_slots.h"
#include "../saves/savestore.h"

namespace {

namespace ct = hubui::cont;
namespace at = hubui::attract;
namespace fs = std::filesystem;

bool g_shot_playing = false;   // HUB_SHOT_CONTINUE=playing

/* ---- picture textures ---- */
struct Tex {
    SDL_Texture* t = nullptr;
    int w = 0, h = 0;
};
std::map<std::string, Tex> g_tex;
int g_loaded_frame = -1;

Tex* tex_get(const std::string& cache_key, const std::string& path) {
    auto it = g_tex.find(cache_key);
    if (it != g_tex.end()) return &it->second;
    const int fr = ImGui::GetFrameCount();
    if (g_loaded_frame == fr) return nullptr;   // at most one decode per frame (no hitch when two pictures are wanted)
    g_loaded_frame = fr;
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
            SDL_SetTextureScaleMode(t.t, SDL_ScaleModeLinear);
            t.w = iw;
            t.h = ih;
        }
    }
    if (px) stbi_image_free(px);
    return &(g_tex[cache_key] = t);
}

void evict_except(const std::string& a, const std::string& b) {
    if (g_tex.size() <= 10) return;
    for (auto it = g_tex.begin(); it != g_tex.end();) {
        if (it->first == a || it->first == b) { ++it; continue; }
        if (it->second.t) SDL_DestroyTexture(it->second.t);
        it = g_tex.erase(it);
    }
}

/* ---- where a game's backdrop comes from ---- */
struct Source {
    ct::Backdrop kind = ct::Backdrop::None;
    std::string path;   // the picture file (not for Cover)
    std::string ckey;   // texture cache key (path + file time, so a new thumbnail replaces the old one)
};

int64_t mtime_unix(const fs::path& p) {
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec) return 0;
    auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return (int64_t)std::chrono::system_clock::to_time_t(sys);
}

/* Newest PNG in `dir` (a state folder holds slotN.png / auto.png; Wrapped cards are not game pictures). */
bool newest_png(const std::string& dir, ct::Pic* out) {
    std::vector<ct::Pic> v;
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        std::string ext = it->path().extension().string();
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext != ".png") continue;
        const std::string name = it->path().filename().u8string();
        if (name.compare(0, 8, "wrapped-") == 0) continue;
        v.push_back({it->path().u8string(), mtime_unix(it->path())});
    }
    const int i = ct::newest_pic(v);
    if (i < 0) return false;
    *out = v[(size_t)i];
    return true;
}

Source find_source(const hub::GameEntry& e) {
    struct C { double at = -100; Source s; };
    static std::map<std::string, C> cache;
    C& c = cache[e.key];
    const double now = ImGui::GetTime();
    if (now - c.at > 4.0 || now < c.at) {   // directory scans at most every few seconds per game
        c.at = now;
        ct::Pic st, sh;
        const bool has_st = !is_native(e) && newest_png(hub::saves::states_dir(sys_short(e), e.key), &st);
        const bool has_sh = newest_png(no_slash(hub::saves::game_dir(sys_short(e), e.key)) + "\\shots", &sh);
        Source s;
        s.kind = ct::pick_backdrop(has_st, has_sh, true);   // the cover is the last resort and is looked up when drawn
        if (s.kind == ct::Backdrop::StateThumb) { s.path = st.path; s.ckey = st.path + "|" + std::to_string(st.time); }
        else if (s.kind == ct::Backdrop::Screenshot) { s.path = sh.path; s.ckey = sh.path + "|" + std::to_string(sh.time); }
        c.s = s;
    }
    return c.s;
}

/* ---- hero layers (crossfade) ---- */
struct Layer {
    std::string key;
    Source src;
    bool same(const Layer& o) const { return key == o.key && src.kind == o.src.kind && src.ckey == o.src.ckey; }
};
Layer g_cur, g_prev;
float g_fade = 1.f;
double g_layer_t0 = 0;

struct Resolved { SDL_Texture* tex = nullptr; int w = 0, h = 0; bool photo = false; };
Resolved resolve(const Layer& l) {
    Resolved r;
    if (l.src.kind == ct::Backdrop::StateThumb || l.src.kind == ct::Backdrop::Screenshot) {
        if (Tex* t = tex_get(l.src.ckey, l.src.path); t && t->t) { r.tex = t->t; r.w = t->w; r.h = t->h; r.photo = true; return r; }
    }
    if (const hub::GameEntry* e = find_entry(l.key)) {   // cover art (also the fallback while a picture is still decoding or failed)
        ArtRef a = art_for_entry(*e);
        r.tex = a.tex; r.w = a.w; r.h = a.h;
    }
    return r;
}

int key_idx(const std::string& k) {
    uint32_t h = 2166136261u;
    for (unsigned char c : k) h = (h ^ c) * 16777619u;
    return (int)(h & 3u);
}

void draw_layer(ImDrawList* dl, const Layer& l, ImVec2 p0, ImVec2 p1, float rad, float alpha) {
    if (alpha <= 0.01f) return;
    Resolved r = resolve(l);
    if (!r.tex || r.w <= 0 || r.h <= 0) return;
    const float w = p1.x - p0.x, h = p1.y - p0.y;
    const at::UV cov = at::cover_uv((float)r.w, (float)r.h, w, h);
    const float u = ui::reduce_motion ? 0.f : ct::pingpong01(ImGui::GetTime() - g_layer_t0 + (double)key_idx(l.key) * 5.0);
    const at::UV kb = at::ken_burns(key_idx(l.key), u, cov);
    const float br = r.photo ? 0.92f : 0.34f;   // a cover is only a tinted backdrop; a game picture is the star
    const int b = (int)(255 * br);
    dl->AddImageRounded((ImTextureID)r.tex, p0, p1, ImVec2(kb.u0, kb.v0), ImVec2(kb.u1, kb.v1),
                        IM_COL32(b, b, b, (int)(255 * std::max(0.f, std::min(1.f, alpha)))), rad);
}

bool busy_now() { return A.proc != nullptr || g_shot_playing || game_active(); }
bool running_this(const hub::GameEntry& e) { return (A.proc != nullptr && A.run.key == e.key) || (g_shot_playing && g_headless && A.sel_game == e.key); }

ct::Action action_for(const hub::GameEntry& e) {
    const bool supp = hubui::system_has_states(sys_short(e), is_native(e));
    const bool autop = supp && has_resume(e);
    const bool rt = running_this(e);
    return ct::pick_action(supp, autop, rt, !rt && busy_now(), app_locked());
}

}  // namespace

bool continue_hero_ok(const hub::GameEntry& e) {
    return ct::is_recent(unix_now(), gstate(e.key).last_played);
}

void continue_launch(const hub::GameEntry& e, bool resume, bool fresh) {
    if (launch_blocked("continue_launch") || A.proc) return;
    hub::GameEntry c = e;
    if (fresh) A.fresh_key = e.key;
    if (resume) play_entry_anim(c, true, [](const hub::GameEntry& g) { resume_entry(g, -1); });
    else play_entry_anim(c, true);
}

void continue_textures_clear() {
    for (auto& kv : g_tex) if (kv.second.t) SDL_DestroyTexture(kv.second.t);
    g_tex.clear();
}

float continue_hero_min_h(float h) {
    const bool compact = h < sp(29.f);
    const float pad = compact ? sp(2.5f) : sp(3.5f), bh = compact ? sp(5.f) : sp(6.5f);
    ImFont* tf = compact ? ui::F.title : ui::F.hero;
    const float need = pad * 2 + ui::F.cap->FontSize + sp(2.25f) + tf->FontSize * 1.08f + sp(0.75f) + ui::F.body->FontSize * 1.3f + sp(2.5f) + bh;
    return std::max(h, need);
}

void draw_continue_hero(const hub::GameEntry& e, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + h);
    const float rad = sp(2.5f), ha = A.hero_t;
    const hub::GameState& gs = gstate(e.key);
    const GameInfo* cat = catalog_for(e);
    const uint64_t now = unix_now();
    ImU32 c0, c1;
    entry_colors(e, &c0, &c1);
    const bool compact = h < sp(29.f);
    const ct::Action act = action_for(e);
    const bool playing = running_this(e);

    /* backdrop layers: a changed hero game (or a fresher thumbnail) fades in over the old one */
    Layer want;
    want.key = e.key;
    want.src = find_source(e);
    if (!g_cur.same(want)) {
        g_prev = g_cur;
        g_cur = want;
        g_fade = g_prev.key.empty() ? 1.f : 0.f;
        if (g_prev.key != g_cur.key) g_layer_t0 = ImGui::GetTime();
    }
    g_fade = ct::fade_step(g_fade, ImGui::GetIO().DeltaTime);
    evict_except(g_cur.src.ckey, g_prev.src.ckey);

    ui::shadow(dl, p0, p1, rad, 1.3f);
    gradient_round_rect(dl, p0, p1, mix(c0, ui::HERO_BASE, 0.68f), mix(c1, ui::HERO_BASE, 0.78f), rad);
    if (g_fade < 1.f && !g_prev.key.empty()) draw_layer(dl, g_prev, p0, p1, rad, 1.f);
    draw_layer(dl, g_cur, p0, p1, rad, g_fade);
    const bool photo = resolve(g_cur).photo;
    /* legibility: a soft ramp from the left and a darker foot under the buttons */
    ui::hfade_round_rect(dl, p0, p1, ui::SCRIM, rad, 0, photo ? 0.90f : 0.96f, photo ? 0.10f : 0.55f);
    ui::vfade_round_rect(dl, ImVec2(p0.x, p0.y + h * 0.35f), p1, ui::SCRIM, rad, ImDrawFlags_RoundCornersBottom, 0.f, 0.72f);
    dl->AddRect(p0, p1, ui::HAIRLINE, rad, 0, 1.f);

    const float pad = compact ? sp(2.5f) : sp(3.5f), bh = compact ? sp(5.f) : sp(6.5f);
    float tx = p0.x + pad;
    if (!photo) {   // cover-only: the box art stands at the left like today's hero
        const float inner_h = h - pad * 2;
        float art_h = inner_h, art_w = art_h * kTileAspect;
        if (art_w > w * 0.28f) { art_w = w * 0.28f; art_h = art_w / kTileAspect; }
        const ImVec2 a0(p0.x + pad, p0.y + pad + (inner_h - art_h) * 0.5f), a1(a0.x + art_w, a0.y + art_h);
        const float arad = sp(1.75f);
        ArtRef art = art_for_entry(e);
        TileSpec ts;
        ts.title = e.title; ts.c0 = c0; ts.c1 = c1;
        ui::shadow(dl, a0, a1, arad, 1.4f);
        if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, a0, a1, arad, 1.f, ha);
        else draw_no_art(dl, ts, a0, a1, arad, 1.f);
        if (art.loading) ui::shimmer(dl, a0, a1, arad, IM_COL32(0, 0, 0, 0));
        dl->AddRect(a0, a1, ui::HAIRLINE, arad, 0, 1.f);
        tx = a1.x + pad;
    }
    const float tw = std::min(p1.x - pad - tx, photo ? w * 0.58f : w);
    ImFont* cf = ui::F.cap;
    float ty = p0.y + pad;

    /* eyebrow: CONTINUE / PLAYING NOW + system badge */
    {
        float x = tx;
        const char* eb = playing ? "PLAYING NOW" : act.resume ? "CONTINUE WHERE YOU LEFT OFF" : "PICK UP WHERE YOU LEFT OFF";
        const float ew = ui::pill_at(dl, ImVec2(x, ty), eb, ui::scale_alpha(playing ? ui::GOOD : ui::ACCENT, ha), ui::scale_alpha(IM_COL32_WHITE, ha));
        x += ew + sp(0.75f);
        auto put = [&](const std::string& s, ImU32 bg) {
            const float pw = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, s.c_str()).x + sp(2.5f);
            if (x + pw > tx + tw) return;
            pill_at(dl, ImVec2(x, ty), s.c_str(), ui::scale_alpha(bg, ha), ui::scale_alpha(IM_COL32_WHITE, ha));
            x += pw + sp(0.75f);
        };
        put(hub::system_name(e.system), mix(c0, IM_COL32_BLACK, 0.3f));
        if (is_hack(e)) put(hack_ribbon(e), ui::ACCENT);
        if (is_native(e)) put("Native port", IM_COL32(255, 255, 255, 40));
        ty += cf->FontSize + sp(1.f) + sp(1.25f);
    }

    ImFont* tf = compact ? ui::F.title : ui::F.hero;
    std::vector<std::string> lines = ui::wrap_lines(tf, tf->FontSize, e.title, tw, compact ? 1 : 2);
    if (lines.size() == 1 && tf == ui::F.hero && ui::F.title != ui::F.hero &&
        tf->CalcTextSizeA(tf->FontSize, FLT_MAX, 0.f, e.title.c_str()).x > tw) {
        tf = ui::F.title;
        lines = ui::wrap_lines(tf, tf->FontSize, e.title, tw, 2);
    }
    for (auto& ln : lines) {
        ui::text_shadowed(dl, tf, tf->FontSize, ImVec2(tx, ty), ui::scale_alpha(IM_COL32_WHITE, ha), ln.c_str());
        ty += tf->FontSize * 1.08f;
    }
    ty += sp(0.75f);
    {
        std::string line = playing ? std::string("Running now") : ct::last_played_line(now, gs.last_played, gs.play_seconds, "  \xC2\xB7  ");
        if (playing && gs.play_seconds) line += "  \xC2\xB7  " + ct::total_time(gs.play_seconds);
        line = ui::ellipsize(ui::F.body, ui::F.body->FontSize, line, tw);
        ui::text_shadowed(dl, ui::F.body, ui::F.body->FontSize, ImVec2(tx, ty), ui::scale_alpha(IM_COL32(222, 226, 240, 255), ha), line.c_str());
        shot_trace_once("hero", e.key + " continue " + line + " [" + act.label + "]" + (photo ? " [photo]" : " [cover]"));
        ty += ui::F.body->FontSize * 1.3f;
    }
    if (!compact && cat && !cat->blurb.empty()) {   // a line of blurb when there is room above the buttons
        const float room = (p1.y - pad - bh) - ty - sp(1.5f);
        const int nl = (int)(room / (ui::F.body->FontSize * 1.25f));
        if (nl > 0) {
            auto bl = ui::wrap_lines(ui::F.body, ui::F.body->FontSize, cat->blurb, tw, 1);
            for (auto& ln : bl) ui::text_shadowed(dl, ui::F.body, ui::F.body->FontSize, ImVec2(tx, ty), ui::scale_alpha(IM_COL32(170, 175, 195, 255), ha), ln.c_str());
        }
    }

    /* buttons */
    const float by = p1.y - pad - bh;
    const float gap = sp(1.5f);
    const bool narrow = compact || tw < sp(70.f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.3f + 0.7f * ha);
    ImGui::SetCursorScreenPos(ImVec2(tx, by));
    const float main_w = sp(narrow ? 18.f : 24.f);
    if (button(act.label.c_str(), ImVec2(main_w, bh), true, act.enabled, playing ? ui::Icon::Gamepad : ui::Icon::Play))
        continue_launch(e, act.resume);
    if (act.resume && ImGui::IsItemHovered()) ImGui::SetTooltip("Pick up from your last save state");
    seed_nav_here();   // the Continue button is the default focus on Home
    if (act.show_fresh) {
        ImGui::SameLine(0, gap);
        if (button(narrow ? "Restart" : "Start fresh", ImVec2(sp(narrow ? 11.f : 16.f), bh), false, act.enabled)) continue_launch(e, false, true);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Start from your battery save, not the resume point");
    }
    const bool online = supports_online(e);
    if (online) {
        ImGui::SameLine(0, gap);
        if (button(narrow ? "##conline" : "Play Online", ImVec2(narrow ? bh : sp(21.f), bh), false, !busy_now(), ui::Icon::Globe)) {
            A.pending_room_game = room_id(e);
            enter_room(true, hubnet::new_room_code());
        }
        if (narrow && ImGui::IsItemHovered()) ImGui::SetTooltip("Play online with friends");
    }
    ImGui::SameLine(0, gap);
    if (ui::icon_button("cfav", gs.favorite ? ui::Icon::StarFilled : ui::Icon::Star, bh, gs.favorite, gs.favorite ? "Remove from favorites" : "Add to favorites", IM_COL32(236, 180, 40, 255)))
        toggle_favorite(e.key);
    ImGui::SameLine(0, gap);
    if (ui::icon_button("cmore", ui::Icon::Dots, bh, false, "Details")) A.show_details = true;
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(w, sp(2.f)));
}

bool continue_row_items(std::vector<int>* out) {
    std::vector<ct::Cand> c;
    c.reserve(A.lib.size());
    for (const hub::GameEntry& e : A.lib) {
        const hub::GameState& gs = gstate(e.key);
        c.push_back({e.key, gs.last_played, gs.play_seconds, false});
    }
    std::vector<int> v = ct::pick_row(c, unix_now());
    if (v.empty()) return false;
    *out = std::move(v);
    return true;
}

bool continue_row_result(const hub::GameEntry& e, const TileSpec& spec, const TileResult& r) {
    if (r.focused && A.sel_game != e.key) A.sel_game = e.key;
    if (!A.sel_ghost.empty() && (r.focused || r.clicked || r.activated)) A.sel_ghost.clear();
    const bool go = r.activated || (r.clicked && spec.selected);   // a second click, double-click, Enter or A continues
    if (r.clicked) A.sel_game = e.key;
    if (r.star) { A.sel_game = e.key; toggle_favorite(e.key); return true; }
    if (r.details) { A.sel_game = e.key; A.show_details = true; return true; }
    if (!go) return true;
    A.sel_game = e.key;
    const ct::Action act = action_for(e);
    if (!act.enabled) { A.show_details = true; return true; }   // a game is running or the app is locked: open the page instead, launch nothing
    shot_trace("continue-row", e.key + " " + act.label);
    continue_launch(e, act.resume);
    return true;
}

void continue_shot_seed() {
    const char* cs = std::getenv("HUB_SHOT_CONTINUE");
    if (!g_headless || !cs || !*cs) return;
    g_shot_playing = std::string(cs) == "playing";
    const uint64_t now = unix_now();
    /* made-up history for the fake library: fake:2 is the newest (3 h), the rest follow; fake:1 / fake:4 come from HUB_SHOT_STATES */
    struct H { const char* key; uint64_t ago, secs; };
    for (H hh : {H{"fake:2", 3 * 3600ull, 14 * 3600ull + 900}, H{"fake:0", 26 * 3600ull, 5400}, H{"fake:3", 3 * 86400ull, 2 * 3600ull},
                 H{"fake:5", 6 * 86400ull, 40 * 60ull}, H{"fake:6", 12 * 86400ull, 9 * 3600ull}, H{"fake:7", 20 * 86400ull, 7200}, H{"fake:8", 25 * 86400ull, 600}})
        hub::record_play(hh.key, now - hh.ago, hh.secs);
}
