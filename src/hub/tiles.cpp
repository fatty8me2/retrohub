#include "app.h"

#include <memory>
#include <mutex>
#include <thread>

bool g_suppress_click = false;   // set by shelf_strip while a drag-pan is in progress / just ended

/* ---------- Library: Home (hero + shelves) and All Games ---------- */


void toggle_favorite(const std::string& key) {
    hub::set_favorite(key, !gstate(key).favorite);
    hub::flush_library_state();
    refresh_state(key);
}

void section_title(const std::string& t) {
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopFont();
}

void refresh_save_info(const hub::GameEntry& e) {
    A.info_key = e.key;
    A.info_at = ImGui::GetTime();
    A.info_battery_files = A.info_state_files = 0;
    A.info_battery_bytes = 0;
    A.info_auto_state = false;
    if (A.info_states_key != e.key) { A.info_states_key = e.key; A.state_sel = -3; A.state_confirm = -2; }
    A.info_states.clear();
    if (is_native(e)) return;
    std::string sys = sys_short(e);
    uint64_t sb = 0;
    count_files(hub::saves::battery_dir(sys, e.key), &A.info_battery_files, &A.info_battery_bytes);
    std::string sd = hub::saves::states_dir(sys, e.key);
    count_files(sd, &A.info_state_files, &sb);
    A.info_auto_state = file_exists(with_slash(sd) + "auto.state");
    if (A.info_auto_state && A.info_state_files > 0) --A.info_state_files;
    if (hubui::system_has_states(sys, false)) {
        A.info_states = hubui::list_states(sd);
        A.info_state_files = hubui::count_states(A.info_states, false);   // slots only: thumbnails and sidecars are not states
    }
}

/* Periodic / first-open refresh of the details modal's save info: the directory walks run on a worker,
   the UI thread only swaps the result in (poll_save_info, every frame the modal is open). User actions
   that need the fresh numbers immediately (states_ui) still call refresh_save_info. */
namespace {
struct SaveInfoBg {
    std::mutex mu;
    bool running = false, have = false;
    std::string key, want_key;   // key of the finished result / key of the latest request
    int battery_files = 0, state_files = 0;
    uint64_t battery_bytes = 0;
    bool auto_state = false;
    std::vector<hubui::StateSlot> states;
};
SaveInfoBg& sib() { static SaveInfoBg* v = new SaveInfoBg(); return *v; }

void save_info_scan(std::string key, std::string sys, bool native) {
    SaveInfoBg r;
    if (!native) {
        uint64_t sb = 0;
        count_files(hub::saves::battery_dir(sys, key), &r.battery_files, &r.battery_bytes);
        std::string sd = hub::saves::states_dir(sys, key);
        count_files(sd, &r.state_files, &sb);
        r.auto_state = file_exists(with_slash(sd) + "auto.state");
        if (r.auto_state && r.state_files > 0) --r.state_files;
        if (hubui::system_has_states(sys, false)) {
            r.states = hubui::list_states(sd);
            r.state_files = hubui::count_states(r.states, false);
        }
    }
    SaveInfoBg& v = sib();
    std::lock_guard<std::mutex> l(v.mu);
    v.key = key;
    v.battery_files = r.battery_files; v.state_files = r.state_files; v.battery_bytes = r.battery_bytes;
    v.auto_state = r.auto_state; v.states = std::move(r.states);
    v.have = true;
    v.running = false;
}
}

void refresh_save_info_bg(const hub::GameEntry& e) {
    if (g_headless) { refresh_save_info(e); return; }   // screenshots stay deterministic
    A.info_at = ImGui::GetTime();   // throttle: next request in 4 s
    if (A.info_key != e.key) {      // different game: drop the old numbers now, fill in when the scan lands
        A.info_key = e.key;
        A.info_battery_files = A.info_state_files = 0;
        A.info_battery_bytes = 0;
        A.info_auto_state = false;
        A.info_states.clear();
        if (A.info_states_key != e.key) { A.info_states_key = e.key; A.state_sel = -3; A.state_confirm = -2; }
    }
    SaveInfoBg& v = sib();
    {
        std::lock_guard<std::mutex> l(v.mu);
        if (v.running) { A.info_at = -100; return; }   // retry next frame
        v.running = true;
    }
    std::thread(save_info_scan, e.key, sys_short(e), is_native(e)).detach();
}

void poll_save_info() {
    SaveInfoBg& v = sib();
    std::lock_guard<std::mutex> l(v.mu);
    if (!v.have) return;
    v.have = false;
    if (v.key != A.info_key) return;   // stale: the modal moved on to another game
    A.info_battery_files = v.battery_files; A.info_state_files = v.state_files;
    A.info_battery_bytes = v.battery_bytes; A.info_auto_state = v.auto_state;
    A.info_states = std::move(v.states);
}

std::string size_text(uint64_t b) {
    char s[32];
    if (b < 1024) std::snprintf(s, sizeof s, "%llu B", (unsigned long long)b);
    else if (b < 1024 * 1024) std::snprintf(s, sizeof s, "%.1f KB", b / 1024.0);
    else std::snprintf(s, sizeof s, "%.1f MB", b / 1048576.0);
    return s;
}

void open_in_explorer(const std::string& dir) {
    ShellExecuteW(nullptr, L"open", W(dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

/* ---------- cover art lookups ---------- */

ArtRef art_lookup(const std::string& seen_key, bool have_name, SDL_Texture* tex, int w, int h) {
    ArtRef a;
    a.tex = tex;
    a.w = w;
    a.h = h;
    if (!tex && have_name && hubart::enabled()) {
        double now = ImGui::GetTime();
        auto it = A.art_seen.find(seen_key);
        if (it == A.art_seen.end()) it = A.art_seen.emplace(seen_key, now).first;
        a.loading = now - it->second < 10.0;
    }
    return a;
}
ArtRef art_for_entry(const hub::GameEntry& e) {
    int w = 0, h = 0;
    SDL_Texture* t = nullptr;
    if (custom_cover(e.key, &t, &w, &h)) return art_lookup(e.key, false, t, w, h);   // the user's own cover (customize_ui.cpp)
    if (custom_is_renamed(e.key)) {   // art matching must see the scanned name, not the user's rename
        hub::GameEntry c = e;
        c.title = custom_original_title(e);
        t = hubart::get(c, hubart::Kind::Boxart, &w, &h);
    } else t = hubart::get(e, hubart::Kind::Boxart, &w, &h);
    return art_lookup(e.key, true, t, w, h);
}
ArtRef art_for_pick(const GameInfo& g) {
    if (g.nointro.empty()) return ArtRef();
    int w = 0, h = 0;
    SDL_Texture* t = hubart::get_by_name(g.system, g.nointro, hubart::Kind::Boxart, &w, &h);
    return art_lookup("pick:" + g.id, true, t, w, h);
}

std::string initials(const std::string& t) {
    std::string o;
    bool start = true;
    for (unsigned char c : t) {
        if (c == ' ' || c == '-' || c == ':' || c == '.') { start = true; continue; }
        if (start && std::isalnum(c)) {
            o += (char)std::toupper(c);
            if (o.size() >= 3) break;
        }
        start = false;
    }
    return o;
}

/* ---------- tiles ---------- */



void draw_no_art(ImDrawList* dl, const TileSpec& t, ImVec2 p0, ImVec2 p1, float rad, float dim) {
    ImU32 a = t.c0, b = t.c1;
    if (dim < 1.f) { a = mix(ui::PANEL, a, dim); b = mix(ui::PANEL, b, dim); }
    gradient_round_rect(dl, p0, p1, a, b, rad);
    std::string ini = initials(t.title);
    if (!ini.empty()) {
        ImFont* f = ui::F.hero;
        ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, ini.c_str());
        dl->AddText(f, f->FontSize, ImVec2((p0.x + p1.x - ts.x) * 0.5f, p0.y + (p1.y - p0.y) * 0.16f), IM_COL32(255, 255, 255, 52),
                    ini.c_str());
    }
}

TileResult draw_tile(const TileSpec& t, ImVec2 size) {
    TileResult r;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::PushID(t.id.c_str());
    ImGui::InvisibleButton("tile", size, ImGuiButtonFlags_EnableNav);
    if (t.selected && A.view == LibView::All) seed_nav_here();   // All Games: initial focus = the selected game
    r.visible = ImGui::IsItemVisible();
    bool item_hov = ImGui::IsItemHovered();
    bool focused = ImGui::IsItemFocused();
    r.hovered = item_hov;
    r.focused = focused && io.NavVisible;
    if (!r.visible && !focused && !item_hov) {
        ImGui::PopID();
        return r;
    }
    ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    ArtRef tart = t.art;
    if (t.ghost && !tart.tex) tart = art_for_ghost(*t.ghost);   // only visible ghost tiles ever fetch cover art (600+ friend games stay cheap)
    float sr = sp(1.5f);
    ImVec2 sc(r1.x - sp(2.25f), r0.y + sp(2.25f));
    bool star_hov = item_hov && t.show_star && t.owned && std::fabs(io.MousePos.x - sc.x) < sr + sp(0.75f) && std::fabs(io.MousePos.y - sc.y) < sr + sp(0.75f);
    /* A click fires on release (and not after a drag-pan), so dragging a shelf never selects or plays a tile. */
    if (ImGui::IsItemDeactivated() && item_hov && !g_suppress_click && ImGui::IsMouseReleased(0)) {
        if (star_hov) r.star = true;
        else r.clicked = true;
    }
    if (ImGui::IsItemClicked(1)) { r.clicked = true; r.details = t.owned; }
    if (item_hov && !star_hov && ImGui::IsMouseDoubleClicked(0)) r.activated = true;
    if (focused && A.focused && !io.WantTextInput && !A.proc) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false))
            r.activated = true;
    }

    float* an = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), 0.f);
    ui::anim(*an, (item_hov || r.focused) ? 1.f : 0.f, 14.f);
    float e = *an;
    float lift = e * sp(0.5f), grow = e * sp(0.375f);
    ImVec2 p0(r0.x - grow, r0.y - grow - lift), p1(r1.x + grow, r1.y + grow - lift);
    float rad = sp(1.75f), h = p1.y - p0.y, w = p1.x - p0.x;
    float dim = t.ghost ? 0.85f : t.owned ? 1.f : 0.52f + 0.3f * e;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ui::shadow(dl, p0, p1, rad, 0.8f + 0.7f * e, e * sp(0.5f));
    ui::glow(dl, p0, p1, rad, t.c1, e);
    if (tart.tex) ui::draw_art(dl, tart.tex, tart.w, tart.h, p0, p1, rad, dim);
    else draw_no_art(dl, t, p0, p1, rad, dim);
    if (tart.loading) ui::shimmer(dl, p0, p1, rad, IM_COL32(0, 0, 0, 0));
    std::string ghost_sub;
    if (t.ghost) ghost_tile_decor(dl, *t.ghost, p0, p1, e, &ghost_sub);   // grey wash + friend avatars + "Mickey, camer have this"
    // scrims keep the badge and title readable on any artwork
    ui::vfade_round_rect(dl, ImVec2(p0.x, p0.y + h * 0.30f), p1, IM_COL32(6, 7, 12, 255), rad, ImDrawFlags_RoundCornersBottom, 0.f, 0.96f);
    ui::vfade_round_rect(dl, ImVec2(p0.x, p0.y + h * 0.58f), p1, IM_COL32(6, 7, 12, 255), rad, ImDrawFlags_RoundCornersBottom, 0.f, 0.55f);
    if (tart.tex) ui::vfade_round_rect(dl, p0, ImVec2(p1.x, p0.y + h * 0.28f), IM_COL32(6, 7, 12, 255), rad, ImDrawFlags_RoundCornersTop, 0.5f, 0.f);
    if (e > 0.01f) dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, (int)(12 * e)), rad);

    float pad = sp(1.5f);
    {
        ImVec2 bp = p0 + ImVec2(sp(1.25f), sp(1.25f));
        if (!t.badge.empty()) bp.x += ui::pill_at(dl, bp, t.badge.c_str(), IM_COL32(0, 0, 0, 150)) + sp(0.5f);
        float tw2 = 0;
        if (!t.tag.empty()) {   // small corner tag: quiet unless it is a warning
            tw2 = t.tag_accent ? ui::pill_at(dl, bp, t.tag.c_str(), ui::ACCENT, IM_COL32_WHITE)
                                 : ui::pill_at(dl, bp, t.tag.c_str(), IM_COL32(0, 0, 0, t.tag_warn ? 170 : 120), t.tag_warn ? ui::WARN : IM_COL32(255, 255, 255, 190));
            ImVec2 mp = io.MousePos;
            if (item_hov && !t.tag_tip.empty() && mp.x >= bp.x && mp.x <= bp.x + tw2 && mp.y >= bp.y && mp.y <= bp.y + ui::F.cap->FontSize + sp(1.f))
                ImGui::SetTooltip("%s", t.tag_tip.c_str());
            bp.x += tw2 + sp(0.5f);
        }
        if (!t.cloud.empty()) ui::pill_at(dl, bp, t.cloud.c_str(), IM_COL32(0, 0, 0, 150), t.cloud_col);   // cloud saves status
    }
    if (t.show_star && t.owned && (t.favorite || e > 0.04f || star_hov)) {
        float sa = t.favorite ? 1.f : std::max(e, star_hov ? 1.f : 0.f);
        dl->AddCircleFilled(sc, sr + sp(0.5f), IM_COL32(0, 0, 0, (int)((star_hov ? 150 : 100) * sa)), 24);
        ui::icon(dl, t.favorite ? ui::Icon::StarFilled : ui::Icon::Star, sc, sr * 1.7f,
                 t.favorite ? IM_COL32(255, 214, 70, 255) : IM_COL32(255, 255, 255, (int)(235 * sa)));
    }

    // title (max 2 lines) + subtitle, anchored to the bottom
    ImFont* tf = ui::F.bold;
    float tfs = tf->FontSize, cap = ui::F.cap->FontSize;
    float maxw = w - pad * 2 - (t.online && t.owned ? sp(2.5f) : 0);
    // one line, ellipsized, so every card in a row has its title and year on the same lines (full title on hover)
    std::string title_line = ui::ellipsize(tf, tfs, t.title, w - pad * 2);
    float sub_y = p1.y - sp(1.25f) - cap;
    float ty = sub_y - sp(0.375f) - tfs * 1.12f;
    ui::text_shadowed(dl, tf, tfs, ImVec2(p0.x + pad, ty), IM_COL32_WHITE, title_line.c_str());
    if (title_line != t.title && item_hov && t.tag_tip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) &&
        !(io.MousePos.y < p0.y + sp(4.f))) ImGui::SetTooltip("%s", t.title.c_str());
    std::string sub = ui::ellipsize(ui::F.cap, cap, t.ghost ? ghost_sub : t.subtitle, maxw);
    dl->AddText(ui::F.cap, cap, ImVec2(p0.x + pad, sub_y), IM_COL32(255, 255, 255, 190), sub.c_str());
    if (t.online && t.owned) ui::icon(dl, ui::Icon::Globe, ImVec2(p1.x - pad - sp(1), sub_y + cap * 0.5f), sp(2), ui::GOOD);
    if (!t.owned && t.cta && e > 0.02f) {
        const char* c = t.cta;
        ImFont* f = ui::F.bold;
        float cw = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, c).x + sp(5.f);
        ImVec2 cp((p0.x + p1.x - cw) * 0.5f, p0.y + h * 0.40f);
        float ch = f->FontSize + sp(1.25f);
        dl->AddRectFilled(cp, cp + ImVec2(cw, ch), ui::scale_alpha(ui::ACCENT, e), ch * 0.5f);
        ui::icon(dl, ui::Icon::Plus, cp + ImVec2(sp(1.75f), ch * 0.5f), sp(1.75f), IM_COL32(255, 255, 255, (int)(255 * e)));
        dl->AddText(f, f->FontSize, cp + ImVec2(sp(3.f), sp(0.625f)), IM_COL32(255, 255, 255, (int)(255 * e)), c);
    }
    dl->AddRect(p0, p1, ui::HAIRLINE, rad, 0, 1.f);
    if (r.focused) ui::focus_ring(dl, p0, p1, rad);
    else if (t.selected) dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 120), rad, 0, sp(0.25f));
    ImGui::PopID();
    return r;
}

TileSpec spec_for_entry(const hub::GameEntry& e, const char* shelf, bool recent_sub, bool show_sys) {
    TileSpec t;
    t.id = std::string(shelf) + "|" + e.key;
    t.title = e.title;
    if (show_sys) t.badge = sys_badge(e.system);   // inside a console section the header already says it
    if (is_native(e)) t.tag = "NATIVE";
    if (is_kanto(e)) { t.tag = "CO-OP"; t.tag_accent = true; t.tag_tip = "Two-player online co-op FireRed (a friend's project). Host or join from a room."; }
    if (e.issue == "extract") {
        const bool sz = e.path.size() > 3 && e.path.compare(e.path.size() - 3, 3, ".7z") == 0;
        const bool rar = e.path.size() > 4 && e.path.compare(e.path.size() - 4, 4, ".rar") == 0;
        t.tag = rar ? "RAR" : sz ? "7Z" : "ZIP";
        t.tag_tip = "Archived - RetroHub unpacks it the first time you play";
    }
    else if (e.issue == "encrypted") { t.tag = "ENCRYPTED"; t.tag_warn = true; t.tag_tip = "Encrypted dump - it needs decrypting before it can run"; }
    else if (e.issue == "keys") { t.tag = "NEEDS KEYS"; t.tag_warn = true; t.tag_tip = "Needs decryption keys (see the 3DS setup dialog)"; }
    t.cloud = cloud_ui_pill(e, &t.cloud_col);
    entry_colors(e, &t.c0, &t.c1);
    t.art = art_for_entry(e);
    const hub::GameState& gs = gstate(e.key);
    t.favorite = gs.favorite;
    t.selected = A.sel_game == e.key;
    const GameInfo* cat = catalog_for(e);
    t.online = is_native(e) || supports_online(e);
    if (recent_sub && gs.last_played) t.subtitle = fmt_ago(gs.last_played);
    else if (gs.play_seconds) t.subtitle = hubui::format_play_time(gs.play_seconds) + " played";
    else t.subtitle = cat && cat->year ? std::to_string(cat->year) : std::string("Not played yet");
    return t;
}

void handle_entry_result(const hub::GameEntry& e, const TileSpec& spec, const TileResult& r) {
    if (r.focused && A.sel_game != e.key) A.sel_game = e.key;
    if (!A.sel_ghost.empty() && (r.focused || r.clicked || r.activated)) A.sel_ghost.clear();
    bool play = r.activated;
    if (r.clicked) {
        if (spec.selected) play = true;   // second click on the selected game plays it
        A.sel_game = e.key;
    }
    if (r.star) { A.sel_game = e.key; toggle_favorite(e.key); }
    if (r.details) { A.sel_game = e.key; A.show_details = true; }
    if (play) {   // selecting a game opens its details first; Play there runs the start animation and launches
        A.sel_game = e.key;
        A.show_details = true;
        shot_trace("select-details", e.key);
    }
}

