/* Cases layout: a full-screen 3D carousel of game cases (included into libview_ui.cpp's anonymous namespace; see cases_logic.h for the pure
 * parts and cases_draw.cpp for the drawing). The centre case faces you, the neighbours turn away and pack together like cases on a shelf,
 * everything stands on a glossy floor. Tabs along the top switch console; X flips the centre case to its back, R3 / dragging inspects it. */

struct CasesState {
    hubui::shelf::Spring pos, flip, insp;
    hubui::cases::Mode mode = hubui::cases::Mode::Browse;
    hubui::cases::Spin spin;
    hubui::cases::TabMemory mem;
    std::vector<hubui::cases::Tab> tabs;
    uint32_t tabs_gen = ~0u;
    std::string tabs_filter;
    int focus = -1;
    bool init = false;
    uint64_t sig = ~0ull;
    std::string last_game, last_ghost;        // the selection we last saw (compared without allocating)
    std::string land_key;                     // a tab change: land on this game once the new list is built
    bool land_pending = false;
    float intro = 0.f;                        // fade-in after entering / switching tab
    float hold_t = 0.f; int hold_dir = 0;     // direction held (acceleration)
    float x_t = -1.f; bool x_fav = false;     // X held: tap = flip, hold = favourite
    bool drag_ok = false, dragged = false;
    float wheel_acc = 0.f;
    float fav_flash = 0.f;
    float tab_off = 0.f;
    float title_a = 1.f;
    ImU32 tint0 = 0, tint1 = 0;
    // per-list caches (rebuilt when the Disp changes)
    std::vector<hubui::shelf::Media> media;
    std::vector<unsigned char> sysv;
    // the text under the centre case, recomputed when the focus changes
    int text_focus = -2; uint64_t text_sig = 0; float text_w = 0;
    std::string title_txt, sub_txt, tint_sys;
    // back cover
    std::string back_key, b_desc, b_genre, b_maker, b_label;
    shelfdraw::CaseBack back;
    SDL_Texture* back_tex[3] = {nullptr, nullptr, nullptr};
    std::string shot_mode;
    bool shot_applied = false, no_art = false;
    float pending_shuffle = -1.f;
    ImU32 sc0[32] = {}, sc1[32] = {};
    bool sc_have[32] = {};
} CS;

bool cases_on() { return lib_layout() == LibLayout::Cases; }

const char* sys_short_c(hub::System s) {
    switch (s) {
        case hub::System::N64: return "N64"; case hub::System::GBA: return "GBA"; case hub::System::GB: return "GB"; case hub::System::GBC: return "GBC";
        case hub::System::GC: return "GC"; case hub::System::N3DS: return "3DS"; case hub::System::WiiU: return "Wii U"; case hub::System::NDS: return "DS";
        case hub::System::PS1: return "PS1"; case hub::System::PS2: return "PS2"; case hub::System::NES: return "NES"; case hub::System::SNES: return "SNES";
        case hub::System::PSP: return "PSP"; default: return "?";
    }
}
hub::System sys_of(int idx) { return is_ghost(idx) ? hub::system_from_short(ghost_of(idx).sys) : A.lib[(size_t)idx].system; }
const char* title_c(int idx) { return is_ghost(idx) ? ghost_of(idx).title.c_str() : A.lib[(size_t)idx].title.c_str(); }
uint32_t item_hash(int idx) { return is_ghost(idx) ? hubui::shelf::hash32(ghost_of(idx).id) ^ 0x9e3779b9u : hubui::shelf::hash32(A.lib[(size_t)idx].key); }
bool item_fav(int idx) { return !is_ghost(idx) && gstate(A.lib[(size_t)idx].key).favorite; }

void cases_remember(int idx) {
    const std::string sys = item_sys(idx);
    CS.mem.remember(A.sys_filter, sys, item_key(idx));
}

/* Makes `pos` the selected case. snap_far: a long way glides in from a few cases off instead of racing through them. */
void cases_goto(const Disp& D, int pos, bool glide_from_afar, bool set_nav) {
    const int n = (int)D.flat.size();
    if (n == 0) return;
    pos = hubui::cases::clamp_index(pos, n);
    const bool moved = pos != CS.focus;
    CS.focus = pos;
    if (glide_from_afar) CS.pos.x = hubui::cases::jump_start(CS.pos.x, pos, 9.f);
    select_item(D.flat[(size_t)pos]);
    CS.last_game = A.sel_game; CS.last_ghost = A.sel_ghost;
    cases_remember(D.flat[(size_t)pos]);
    if (moved) CS.mode = hubui::cases::on_move(CS.mode);
    if (set_nav) { G.focus_key = item_key(D.flat[(size_t)pos]); G.focus_quiet = !A.pad_mode; }
}

void cases_switch_tab(const std::string& sys) {
    if (sys == A.sys_filter) return;
    A.sys_filter = sys;
    A.vis_dirty = true;
    CS.land_key = CS.mem.recall(sys);
    CS.land_pending = true;
    CS.mode = hubui::cases::Mode::Browse;
    CS.intro = 0.f;
    shot_trace("cases-tab", sys.empty() ? std::string("all") : sys);
}

/* ---- the back cover: description, genre, year, players, the player's own pictures (collected once when the case flips) ---- */
SDL_Texture* cases_load_tex(const std::string& path, int* w, int* h) {
    SDL_Renderer* ren = A.win ? SDL_GetRenderer(A.win) : nullptr;
    if (!ren) return nullptr;
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return nullptr;
    SDL_Texture* t = nullptr;
    if (path.size() > 4 && _stricmp(path.c_str() + path.size() - 4, ".bmp") == 0) {
        SDL_RWops* rw = SDL_RWFromConstMem(bytes.data(), (int)bytes.size());
        SDL_Surface* s = rw ? SDL_LoadBMP_RW(rw, 1) : nullptr;
        if (s) { t = SDL_CreateTextureFromSurface(ren, s); *w = s->w; *h = s->h; SDL_FreeSurface(s); }
    } else {
        int iw = 0, ih = 0, comp = 0;
        unsigned char* px = stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &iw, &ih, &comp, 4);
        if (px) {
            t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, iw, ih);
            if (t) { SDL_UpdateTexture(t, nullptr, px, iw * 4); SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); *w = iw; *h = ih; }
            stbi_image_free(px);
        }
    }
    return t;
}
void cases_release_back() {
    for (SDL_Texture*& t : CS.back_tex) if (t) { SDL_DestroyTexture(t); t = nullptr; }
    CS.back = shelfdraw::CaseBack();
    CS.back_key.clear();
}
SDL_Texture* cases_fake_pic(int i, int* w, int* h) {   // HUB_SHOT_CASES=flip: three made-up pictures so the back cover shows its strip
    SDL_Renderer* ren = A.win ? SDL_GetRenderer(A.win) : nullptr;
    if (!ren) return nullptr;
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, 320, 200, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return nullptr;
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 320; ++x) {
            const float k = (float)y / 200.f;
            Uint8 r = (Uint8)(40 + i * 50 + 60 * k), g = (Uint8)(120 - i * 25 + 40 * k), b = (Uint8)(210 - i * 40 - 70 * k);
            if (x > 40 + i * 50 && x < 110 + i * 50 && y > 90 && y < 170) { r = 250; g = 210; b = 70; }
            ((uint32_t*)s->pixels)[y * s->pitch / 4 + x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    SDL_Texture* t = SDL_CreateTextureFromSurface(ren, s);
    *w = s->w; *h = s->h;
    SDL_FreeSurface(s);
    return t;
}
void cases_collect_back(int idx) {
    cases_release_back();
    CS.back_key = item_key(idx);
    CS.b_desc.clear(); CS.b_genre.clear(); CS.b_maker.clear(); CS.b_label.clear();
    int year = 0, players = item_players(idx);
    if (!is_ghost(idx)) {
        const hub::GameEntry& e = A.lib[(size_t)idx];
        const GameInfo* cat = catalog_for(e);
        CaseInfo ci;
        gameinfo_case_info(e, cat ? cat->title : std::string(), true, &ci);
        CS.b_desc = ci.desc;
        if (CS.b_desc.empty() && cat) CS.b_desc = cat->blurb;
        CS.b_genre = !ci.genre.empty() ? ci.genre : (cat ? cat->genre : std::string());
        CS.b_maker = ci.maker;
        year = ci.year > 0 ? ci.year : (cat ? cat->year : 0);
        if (ci.players > 0) players = ci.players;
        else if (cat && cat->max_players > 0) players = cat->max_players;
        // the player's own screenshots, newest first; else save-state thumbnails
        std::vector<std::string> pics;
        if (!is_native(e)) {
            std::string dir = hub::saves::game_dir(sys_short(e), e.key);
            while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
            std::error_code ec;
            std::vector<std::string> names;
            for (auto& it : std::filesystem::directory_iterator(std::filesystem::u8path(dir + "\\shots"), ec)) {
                if (!it.is_regular_file(ec)) continue;
                const std::string nm = it.path().filename().u8string();
                if (nm.size() > 4 && (_stricmp(nm.c_str() + nm.size() - 4, ".png") == 0 || _stricmp(nm.c_str() + nm.size() - 4, ".bmp") == 0)) names.push_back(nm);
            }
            std::sort(names.begin(), names.end(), std::greater<std::string>());
            for (size_t i = 0; i < names.size() && i < 3; ++i) pics.push_back(dir + "\\shots\\" + names[i]);
            CS.b_label = "YOUR SCREENSHOTS";
            if (pics.empty()) {
                for (const hubui::StateSlot& s : hubui::list_states(hub::saves::states_dir(sys_short(e), e.key)))
                    if (s.present && !s.thumb_path.empty() && pics.size() < 3) pics.push_back(s.thumb_path);
                CS.b_label = "YOUR SAVES";
            }
        }
        int k = 0;
        for (const std::string& p : pics) {
            int w = 0, h = 0;
            SDL_Texture* t = cases_load_tex(p, &w, &h);
            if (!t) continue;
            CS.back_tex[k] = t;
            CS.back.shots[k] = {t, w, h};
            if (++k >= 3) break;
        }
        CS.back.nshots = k;
        if (g_headless && CS.shot_mode == "flip") {   // screenshot aid: a description and three pictures for the fake library
            if (CS.b_desc.empty()) CS.b_desc = "Race, battle and trade across a colourful world. Collect every cup, unlock hidden tracks and challenge friends in split-screen for up to four players. A story mode adds a grand adventure with new characters to meet along the way.";
            if (CS.b_genre.empty()) CS.b_genre = "Racing";
            if (year == 0) year = 2001;
            if (CS.back.nshots == 0) {
                for (int i = 0; i < 3; ++i) { int w = 0, h = 0; CS.back_tex[i] = cases_fake_pic(i, &w, &h); CS.back.shots[i] = {CS.back_tex[i], w, h}; }
                CS.back.nshots = 3;
                CS.b_label = "YOUR SCREENSHOTS";
            }
        }
    }
    CS.back.desc = CS.b_desc.c_str();
    CS.back.genre = CS.b_genre.c_str();
    CS.back.maker = CS.b_maker.c_str();
    CS.back.year = year;
    CS.back.players = players;
    CS.back.shots_label = CS.b_label.c_str();
}

/* ---- hits ---- */
struct CHit { bool hovered = false, focused = false, clicked = false, activated = false, details = false, pressed = false; };
CHit cases_hit(uint32_t id, ImVec2 p0, ImVec2 p1, bool is_focus_item) {
    CHit h;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::PushID((int)id);
    ImGui::SetCursorScreenPos(p0);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, 0);   // the case itself shows the selection; no focus box over it
    ImGui::InvisibleButton("c", ImVec2(std::max(2.f, p1.x - p0.x), std::max(2.f, p1.y - p0.y)), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    if (is_focus_item && A.view == LibView::All) seed_nav_here();
    h.hovered = ImGui::IsItemHovered();
    h.focused = ImGui::IsItemFocused();
    h.pressed = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivated() && h.hovered && !g_suppress_click && ImGui::IsMouseReleased(0)) h.clicked = true;
    if (ImGui::IsItemClicked(1)) { h.clicked = true; h.details = true; }
    if (h.hovered && ImGui::IsMouseDoubleClicked(0)) h.activated = true;
    if (h.focused && A.focused && !io.WantTextInput && !A.proc && is_focus_item) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) h.activated = true;
    }
    ImGui::PopID();
    return h;
}

float key_analog(ImGuiKey k) { const ImGuiKeyData* d = ImGui::GetKeyData(k); return d ? d->AnalogValue : 0.f; }

/* ---- the tab bar ---- */
void cases_tabs(ImDrawList* dl, ImVec2 org, float W, float y, float h, int active, bool can_pad) {
    using hubui::cases::Tab;
    ImGuiIO& io = ImGui::GetIO();
    ImFont* f = ui::F.bold ? ui::F.bold : ui::F.body;
    const float fs = f->FontSize;
    const float pad_x = sp(1.5f), gap = sp(0.3f), ph = h - sp(0.7f);
    const size_t nt = CS.tabs.size();
    float total = 0.f;
    float wv[40];
    for (size_t i = 0; i < nt && i < 40; ++i) { wv[i] = f->CalcTextSizeA(fs, FLT_MAX, 0.f, CS.tabs[i].label.c_str()).x + pad_x * 2.f; total += wv[i] + (i ? gap : 0.f); }
    const char* lb_k = "[";
    const float hint_w = ui::key_hint_width(A.pad_mode, "LB", lb_k, "") + sp(1.f);
    const float max_w = W - sp(2.f) - hint_w * 2.f - sp(1.f);
    const float cw = std::min(max_w, total + sp(1.f));
    const float cx0 = org.x + (W - cw) * 0.5f;
    const ImVec2 c0(cx0, y), c1(cx0 + cw, y + h);
    dl->AddRectFilled(ImVec2(c0.x + 2.f, c0.y + 4.f), ImVec2(c1.x + 2.f, c1.y + 4.f), IM_COL32(0, 0, 0, 70), h * 0.5f);
    dl->AddRectFilled(c0, c1, ui::with_alpha(ui::PANEL, 0.84f), h * 0.5f);
    dl->AddRect(c0, c1, ui::HAIRLINE, h * 0.5f, 0, 1.f);
    const float inner = cw - sp(1.f);
    float off_t = 0.f;
    if (total > inner && active >= 0 && active < (int)nt) {
        float acc = 0.f;
        for (int i = 0; i < active; ++i) acc += wv[i] + gap;
        off_t = std::max(0.f, std::min(total - inner, acc + wv[active] * 0.5f - inner * 0.5f));
    }
    ui::anim(CS.tab_off, off_t, 12.f);
    if (reduced_motion()) CS.tab_off = off_t;
    // LB / RB hints at both ends
    const float hy = y + (h - (ui::F.cap->FontSize + sp(0.75f))) * 0.5f;
    ui::key_hint(dl, ImVec2(c0.x - hint_w + sp(0.3f), hy), A.pad_mode, "LB", "[", "");
    ui::key_hint(dl, ImVec2(c1.x + sp(0.7f), hy), A.pad_mode, "RB", "]", "");
    ImGui::PushClipRect(ImVec2(c0.x + sp(0.5f), c0.y), ImVec2(c1.x - sp(0.5f), c1.y), true);
    float x = c0.x + sp(0.5f) - CS.tab_off;
    ImGui::PushID("casetabs");
    for (size_t i = 0; i < nt && i < 40; ++i) {
        const Tab& t = CS.tabs[i];
        const ImVec2 p0(x, y + (h - ph) * 0.5f), p1(x + wv[i], y + (h + ph) * 0.5f);
        ImGui::PushID((int)i);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        ImGui::SetCursorScreenPos(p0);
        const bool pressed = ImGui::InvisibleButton("t", p1 - p0);
        ImGui::PopItemFlag();
        const bool hov = ImGui::IsItemHovered();
        const bool on = (int)i == active;
        ImU32 col = ui::ACCENT, b2;
        if (!t.sys.empty()) system_colors(hub::system_from_short(t.sys), &col, &b2);
        if (on) {
            dl->AddRectFilled(p0, p1, col, ph * 0.5f);
            dl->AddRectFilledMultiColor(ImVec2(p0.x + ph * 0.3f, p0.y), ImVec2(p1.x - ph * 0.3f, p0.y + ph * 0.5f), IM_COL32(255, 255, 255, 46), IM_COL32(255, 255, 255, 46), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
        } else if (hov) dl->AddRectFilled(p0, p1, ui::FRAME_HOV, ph * 0.5f);
        const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.f, t.label.c_str());
        const bool light = on && ((0.2126f * (col & 0xFF) + 0.7152f * ((col >> 8) & 0xFF) + 0.0722f * ((col >> 16) & 0xFF)) / 255.f) > 0.72f;
        dl->AddText(f, fs, ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f), on ? (light ? IM_COL32(20, 22, 30, 255) : IM_COL32_WHITE) : hov ? ui::TEXT : ui::TEXT2, t.label.c_str());
        if (hov) ImGui::SetTooltip("%s: %d game%s", t.label.c_str(), t.count, t.count == 1 ? "" : "s");
        if (pressed) cases_switch_tab(t.sys);
        ImGui::PopID();
        x += wv[i] + gap;
    }
    ImGui::PopID();
    ImGui::PopClipRect();
    if (total > inner) {   // fade the strip out at both ends
        const ImU32 pc = ui::with_alpha(ui::PANEL, 0.9f), pc0 = ui::with_alpha(ui::PANEL, 0.f);
        dl->AddRectFilledMultiColor(ImVec2(c0.x + sp(0.4f), c0.y + 1.f), ImVec2(c0.x + sp(2.8f), c1.y - 1.f), pc, pc0, pc0, pc);
        dl->AddRectFilledMultiColor(ImVec2(c1.x - sp(2.8f), c0.y + 1.f), ImVec2(c1.x - sp(0.4f), c1.y - 1.f), pc0, pc, pc, pc0);
    }
    (void)io; (void)can_pad;
}

/* ---- the hint bar ---- */
void cases_hints(ImDrawList* dl, ImVec2 org, float W, float y, float h) {
    struct H { const char* pad; const char* key; const char* label; bool tail; };   // tail: dropped first when the bar is too narrow
    const bool pad = A.pad_mode;
    H hs[12];
    int nh = 0;
    hs[nh++] = {"A", "Enter", "Play", false};
    hs[nh++] = {"Y", "I", "Details", false};
    hs[nh++] = {"X", "X", "Flip", false};
    hs[nh++] = {"X", "F", pad ? "Hold: Favorite" : "Favorite", true};
    hs[nh++] = {"R3", "R", "Inspect", false};
    hs[nh++] = {"L3", "Z", "Shuffle", true};
    hs[nh++] = {"LB", "[", "", false};
    hs[nh++] = {"RB", "]", "Console", false};
    if (!pad) { hs[nh++] = {"", "Wheel", "Scroll", true}; hs[nh++] = {"", "Drag", "Inspect", true}; }
    const float gap = sp(2.f);
    auto is_lb = [](const H& h) { return h.pad[0] == 'L' && h.pad[1] == 'B'; };
    auto width = [&]() {
        float w = 0;
        for (int i = 0; i < nh; ++i) {
            w += ui::key_hint_width(pad, hs[i].pad[0] ? hs[i].pad : "A", hs[i].key, hs[i].label);
            if (i + 1 < nh) w += is_lb(hs[i]) ? sp(0.5f) : gap;
        }
        return w;
    };
    float tw = width();
    while (tw > W - sp(3.f)) {   // drop the least important hints
        int di = -1;
        for (int i = nh - 1; i >= 0; --i) if (hs[i].tail) { di = i; break; }
        if (di < 0) break;
        for (int i = di; i + 1 < nh; ++i) hs[i] = hs[i + 1];
        --nh;
        tw = width();
    }
    const ImU32 sc = ui::with_alpha(ui::SCRIM, 0.f), sc1 = ui::with_alpha(ui::SCRIM, 0.78f);
    dl->AddRectFilledMultiColor(ImVec2(org.x, y - sp(1.f)), ImVec2(org.x + W, y + h), sc, sc, sc1, sc1);
    float x = org.x + (W - tw) * 0.5f;
    const float fh = ui::F.cap->FontSize + sp(0.75f);
    for (int i = 0; i < nh; ++i) {
        const bool wheel = !hs[i].pad[0];
        x += ui::key_hint(dl, ImVec2(x, y + (h - fh) * 0.5f), wheel ? false : pad, hs[i].pad[0] ? hs[i].pad : "A", hs[i].key, hs[i].label) + (is_lb(hs[i]) ? sp(0.5f) : gap);
    }
}

/* ---- the carousel ---- */
void draw_cases(Disp& D) {
    using namespace hubui::cases;
    ImGuiIO& io = ImGui::GetIO();
    ImGuiContext& gctx = *ImGui::GetCurrentContext();
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int n = (int)D.flat.size();
    const ImVec2 org = win->Pos, sz = win->Size;
    const float W = sz.x, H = sz.y;
    const float dt = std::min(io.DeltaTime, 0.1f);
    const bool reduced = reduced_motion();
    const bool nav_mine = gctx.NavWindow == win;

    // ---- the list changed under us / first frame ----
    if (CS.sig != D.sig) {
        CS.sig = D.sig;
        CS.media.assign((size_t)n, hubui::shelf::Media::Generic);
        CS.sysv.assign((size_t)n, 0);
        for (int p = 0; p < n; ++p) {
            const int idx = D.flat[(size_t)p];
            const hub::System sy = sys_of(idx);
            CS.media[(size_t)p] = hubui::shelf::media_for(hub::system_short(sy));
            CS.sysv[(size_t)p] = (unsigned char)std::min(31, (int)sy);
        }
        CS.text_sig = 0;
        if (CS.land_pending) {
            int p = 0;
            if (!CS.land_key.empty()) { auto it = D.pos_of.find(CS.land_key); if (it != D.pos_of.end()) p = it->second; }
            CS.land_pending = false;
            if (n > 0) { CS.focus = p; CS.pos.x = (float)p; CS.pos.v = 0; select_item(D.flat[(size_t)p]); CS.last_game = A.sel_game; CS.last_ghost = A.sel_ghost; cases_remember(D.flat[(size_t)p]); if (A.pad_mode) G.focus_key = item_key(D.flat[(size_t)p]); }
            CS.init = true;
        } else if (CS.init) {
            const int p = selected_pos(D);
            if (p >= 0) { CS.focus = p; CS.pos.x = (float)p; CS.pos.v = 0; } else CS.init = false;
        }
    }
    if (!CS.init || CS.focus < 0 || CS.focus >= n) {
        const int p = selected_pos(D);
        CS.focus = n ? (p >= 0 ? p : 0) : -1;
        CS.pos = hubui::shelf::Spring();
        CS.pos.x = (float)std::max(0, CS.focus);
        CS.init = true;
        CS.intro = reduced ? 1.f : 0.f;
        if (n && p < 0) { select_item(D.flat[(size_t)CS.focus]); CS.last_game = A.sel_game; CS.last_ghost = A.sel_ghost; }
    }
    if (CS.shot_mode.empty()) if (const char* sm = std::getenv("HUB_SHOT_CASES")) if (g_headless) { CS.shot_mode = sm; CS.no_art = CS.shot_mode == "missingart"; }
    // someone else moved the selection (details, search, a friend, pad focus elsewhere): follow it
    if (n && (A.sel_game != CS.last_game || A.sel_ghost != CS.last_ghost)) {
        const int p = selected_pos(D);
        CS.last_game = A.sel_game; CS.last_ghost = A.sel_ghost;
        if (p >= 0 && p != CS.focus) { CS.focus = p; CS.mode = on_move(CS.mode); }
    }
    ui::anim(CS.intro, 1.f, 5.f);
    if (reduced) CS.intro = 1.f;

    // ---- tabs ----
    if (CS.tabs_gen != A.vis_gen || CS.tabs_filter != A.sys_filter) {
        CS.tabs = build_tabs(A.chip_counts, A.chip_total, A.sys_filter);
        CS.tabs_gen = A.vis_gen; CS.tabs_filter = A.sys_filter;
    }
    const int active_tab = tab_index(CS.tabs, A.sys_filter);

    // ---- input ----
    const bool popup = ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool can = A.focused && !A.proc && !io.WantTextInput && !popup && !A.show_details && !A.show_ghost && A.page == Page::Library;
    const bool act = can && (nav_mine || ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) || !A.pad_mode);
    if (can && !CS.tabs.empty()) {
        int td = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false) || (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))) td = -1;
        else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_RightBracket, false) || (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))) td = 1;
        if (td) {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) A.pad_mode = true;
            cases_switch_tab(CS.tabs[(size_t)tab_step(active_tab, td, (int)CS.tabs.size())].sys);
        }
    }
    const int cur_idx = n ? D.flat[(size_t)CS.focus] : -1;
    if (act && n) {
        // X: a tap flips the centre case, holding it toggles the favourite
        const bool x_press = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) || ImGui::IsKeyPressed(ImGuiKey_X, false);
        const bool x_down = ImGui::IsKeyDown(ImGuiKey_GamepadFaceLeft) || ImGui::IsKeyDown(ImGuiKey_X);
        if (x_press) { CS.x_t = 0.f; CS.x_fav = false; if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)) A.pad_mode = true; }
        if (CS.x_t >= 0.f) {
            if (x_down) {
                CS.x_t += dt;
                if (CS.x_t >= kFavHold && !CS.x_fav && !is_ghost(cur_idx)) { CS.x_fav = true; toggle_favorite(A.lib[(size_t)cur_idx].key); CS.fav_flash = 1.f; }
            } else {
                if (!CS.x_fav) {
                    const Mode m = on_flip_tap(CS.mode);
                    if (m == Mode::Flip && !is_ghost(cur_idx)) cases_collect_back(cur_idx);
                    CS.mode = m;
                    shot_trace("cases-flip", m == Mode::Flip ? "front->back" : "back->front");
                }
                CS.x_t = -1.f;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR3, false) || ImGui::IsKeyPressed(ImGuiKey_R, false)) {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadR3, false)) A.pad_mode = true;
            CS.mode = on_inspect(CS.mode);
            shot_trace("cases-inspect", CS.mode == Mode::Inspect ? "on" : "off");
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL3, false) || ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL3, false)) A.pad_mode = true;
            std::vector<ShuffleCand> cands;
            std::vector<int> posmap;
            cands.reserve((size_t)n);
            for (int p = 0; p < n; ++p) {
                const int idx = D.flat[(size_t)p];
                if (is_ghost(idx)) continue;
                const hub::GameState& gs = gstate(A.lib[(size_t)idx].key);
                cands.push_back({A.lib[(size_t)idx].key, (int64_t)gs.last_played, gs.favorite});
                posmap.push_back(p);
            }
            int curc = -1;
            for (size_t k = 0; k < posmap.size(); ++k) if (posmap[k] == CS.focus) curc = (int)k;
            static uint32_t rng = 2463534242u;
            rng ^= (uint32_t)GetTickCount64() * 2654435761u; rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            const int pick = pick_shuffle(cands, curc, (int64_t)std::time(nullptr), (double)(rng % 100000u) / 100000.0);
            if (pick >= 0) { cases_goto(D, posmap[(size_t)pick], true, true); shot_trace("cases-shuffle", cands[(size_t)pick].key); }
        }
        // the wheel scrolls one case per notch
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && io.MouseWheel != 0.f) {
            CS.wheel_acc += io.MouseWheel;
            int steps = 0;
            while (CS.wheel_acc >= 1.f) { CS.wheel_acc -= 1.f; --steps; }
            while (CS.wheel_acc <= -1.f) { CS.wheel_acc += 1.f; ++steps; }
            if (steps) cases_goto(D, CS.focus + steps, false, true);
        }
    }
    if (!act) CS.x_t = -1.f;

    // held direction: boosts the nav repeat the longer it is held
    int held_dir = 0;
    if (act) {
        const bool l = ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickLeft);
        const bool r = ImGui::IsKeyDown(ImGuiKey_RightArrow) || ImGui::IsKeyDown(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickRight);
        held_dir = l && !r ? -1 : r && !l ? 1 : 0;
    }
    if (held_dir != 0 && held_dir == CS.hold_dir) CS.hold_t += dt; else CS.hold_t = 0.f;
    CS.hold_dir = held_dir;
    // page keys (LT / RT) and jumps from outside
    if (G.jump_pos >= 0 && G.jump_pos < n) { const int jp = G.jump_pos; G.jump_pos = -1; G.jump_center = false; G.jump_top = false; if (jp != CS.focus) { CS.focus = jp; CS.mode = on_move(CS.mode); } if (std::fabs((float)jp - CS.pos.x) > 14.f) CS.pos.x = jump_start(CS.pos.x, jp, 9.f); CS.last_game = A.sel_game; CS.last_ghost = A.sel_ghost; }

    // ---- inspect: spin with the right stick / arrows; a leaving case swings back ----
    if (CS.mode == Mode::Inspect && act) {
        const float sx = key_analog(ImGuiKey_GamepadRStickRight) - key_analog(ImGuiKey_GamepadRStickLeft);
        const float sy = key_analog(ImGuiKey_GamepadRStickDown) - key_analog(ImGuiKey_GamepadRStickUp);
        float kx = 0.f, ky = 0.f;
        if (ImGui::IsKeyDown(ImGuiKey_RightArrow) || ImGui::IsKeyDown(ImGuiKey_GamepadDpadRight)) kx += 1.f;
        if (ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_GamepadDpadLeft)) kx -= 1.f;
        if (ImGui::IsKeyDown(ImGuiKey_DownArrow) || ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown)) ky += 1.f;
        if (ImGui::IsKeyDown(ImGuiKey_UpArrow) || ImGui::IsKeyDown(ImGuiKey_GamepadDpadUp)) ky -= 1.f;
        const float ax = std::fabs(sx) > 0.18f ? sx : 0.f, ay = std::fabs(sy) > 0.18f ? sy : 0.f;
        CS.spin = spin_apply(CS.spin, (ax + kx) * dt * 3.4f, (ay + ky) * dt * 1.8f);
    } else if (CS.mode != Mode::Inspect) {
        const float k = reduced ? 1.f : 1.f - std::exp(-dt * 7.f);
        CS.spin.yaw += (spin_home(CS.spin.yaw) - CS.spin.yaw) * k;
        CS.spin.pitch += (0.f - CS.spin.pitch) * k;
    }
    pos_step(CS.pos, (float)std::max(0, CS.focus), dt, reduced);
    pos_step(CS.flip, ModeState{CS.mode}.flip_target(), dt, reduced);
    pos_step(CS.insp, CS.mode == Mode::Inspect ? 1.f : 0.f, dt, reduced);
    if (CS.mode != Mode::Flip && CS.flip.x < 0.02f && !CS.back_key.empty() && CS.flip.v == 0.f) cases_release_back();
    ui::anim(CS.fav_flash, 0.f, 2.2f);

    // ---- stage ----
    const bool tight = H < sp(44.f);   // a big UI size in a small window: a smaller title and hint bar so the cases keep their room
    ImFont* tfont = (ui::F.title && !tight) ? ui::F.title : ui::F.h2;
    ImFont* cf = ui::F.cap;
    const float tab_h = sp(3.6f), tab_y = org.y + sp(tight ? 0.8f : 1.2f);
    const float hint_h = sp(tight ? 3.4f : 4.f), hint_y = org.y + H - hint_h;
    const float title_h = sp(1.3f) + tfont->FontSize * 1.18f + cf->FontSize * 1.35f + sp(0.3f);
    const float avail_top = tab_y + tab_h + sp(1.2f), avail_bot = hint_y - sp(0.3f);
    const float avail = std::max(sp(20.f), avail_bot - avail_top);
    float Hc = std::min((avail - title_h) * 0.90f, W * 0.42f);
    Hc = std::max(sp(12.f), Hc);
    const float case_top = avail_top + std::max(0.f, (avail - Hc * 1.1f - title_h) * 0.22f) + Hc * 0.10f;
    const float f0 = case_top + Hc;
    shelfdraw::CaseScene sc;
    sc.dl = dl; sc.dist = 6.5f; sc.eye = 0.55f; sc.U = Hc; sc.cx = org.x + W * 0.5f; sc.hy = f0 - sc.eye * sc.U;
    // tint follows the centre case's console
    ImU32 t0 = ui::ACCENT, t1 = ui::ACCENT;
    if (n) system_colors(sys_of(D.flat[(size_t)CS.focus]), &t0, &t1);
    if (!CS.tint0) { CS.tint0 = t0; CS.tint1 = t1; }
    {
        const float k = reduced ? 1.f : 1.f - std::exp(-dt * 4.5f);
        CS.tint0 = ui::mix(CS.tint0, t0, k); CS.tint1 = ui::mix(CS.tint1, t1, k);
    }
    const ImVec2 p0 = org, p1(org.x + W, org.y + H);
    dl->PushClipRect(p0, p1, true);
    shelfdraw::cases_stage_back(sc, p0, p1, CS.tint0, CS.tint1);

    // ---- poses ----
    struct Slot { int i; float d; shelfdraw::CasePose ps; shelfdraw::CaseDraw cd; ImVec2 mn, mx; };
    static Slot slots[2 * kWindow + 4];
    int ns = 0;
    int first = 0, last = -1;
    if (n) window(CS.pos.x, n, &first, &last);
    const int ic = n ? std::max(0, std::min(n - 1, (int)std::lround(CS.pos.x))) : 0;
    const bool settled = n && std::fabs(CS.pos.x - (float)CS.focus) < 1.3f;
    const float fl = CS.flip.x, ins = std::max(0.f, std::min(1.f, CS.insp.x));
    const bool showing_back = fl > 0.02f && !CS.back_key.empty();
    for (int i = first; i <= last && ns < 2 * kWindow + 4; ++i) {
        Slot& s = slots[ns];
        s.i = i;
        s.d = (float)i - CS.pos.x;
        const SlotPose sp0 = slot_pose(s.d);
        const float ad = std::fabs(s.d);
        const float c = std::max(0.f, 1.f - ad);   // 1 at the centre, 0 one slot away
        shelfdraw::CasePose& ps = s.ps;
        ps = shelfdraw::CasePose();
        ps.x = sp0.x; ps.z = sp0.z; ps.yaw = sp0.yaw; ps.scale = sp0.scale;
        ps.alpha = sp0.alpha * (0.15f + 0.85f * CS.intro);
        // flip: the centre case turns over, rising a little toward the viewer so it never clips its neighbours
        ps.yaw += c * fl;
        const float sf = std::sin(std::min(fl, 3.14159f));
        ps.z -= c * (0.5f * sf + 0.0f);
        ps.scale *= 1.f + c * (0.05f * sf + 0.10f * smoothstep01(fl / 3.14159f));
        ps.z -= c * 0.4f * smoothstep01(fl / 3.14159f);
        // inspect: the case comes forward and turns freely
        if (c > 0.f && ins > 0.001f) {
            ps.z -= c * ins * 1.5f;
            ps.scale *= 1.f + c * ins * 0.12f;
            ps.lift = c * ins * 0.05f;
            ps.yaw = ps.yaw * (1.f - c * ins) + (CS.spin.yaw) * c * ins;
            ps.pitch = CS.spin.pitch * c * ins;
        }
        if (ins > 0.001f && ad > 0.5f) ps.alpha *= 1.f - 0.75f * ins;
        ps.refl = c > 0.5f ? 0.34f : 0.28f;
        ps.refl_detail = ad < 1.6f;
        const int idx = D.flat[(size_t)i];
        shelfdraw::CaseDraw& cd = s.cd;
        cd = shelfdraw::CaseDraw();
        cd.media = CS.media[(size_t)i];
        cd.seed = item_hash(idx);
        cd.title = title_c(idx);
        const hub::System sy = (hub::System)CS.sysv[(size_t)i];
        cd.sys_name = hub::system_name(sy);
        cd.sys_short = sys_short_c(sy);
        const int si = CS.sysv[(size_t)i];
        if (!CS.sc_have[si]) { system_colors(sy, &CS.sc0[si], &CS.sc1[si]); CS.sc_have[si] = true; }
        cd.c0 = CS.sc0[si]; cd.c1 = CS.sc1[si];
        cd.ghost = is_ghost(idx);
        cd.fav = item_fav(idx);
        if (!CS.no_art && (ad <= 2.5f || (settled && ad <= 5.2f))) {
            const ArtRef art = art_of(idx);
            cd.art = art.tex; cd.art_w = art.w; cd.art_h = art.h;
        }
        if (c > 0.5f && showing_back) cd.back = &CS.back;
        shelfdraw::cases_bounds(sc, cd.media, ps, &s.mn, &s.mx);
        {   // cases fade out toward the edges of the screen instead of being cut off by them
            const float ex = 0.5f * (s.mn.x + s.mx.x);
            ps.alpha *= smoothstep01(std::min(ex - p0.x, p1.x - ex) / (W * 0.13f));
        }
        ++ns;
    }

    // ---- reflections (far to near), the veil that fades them into the floor, then the floor details ----
    auto each_far_to_near = [&](auto&& fn) {
        for (int k = 0; k < ns; ++k) if (slots[k].i < ic) fn(slots[k]);
        for (int k = ns - 1; k >= 0; --k) if (slots[k].i > ic) fn(slots[k]);
        for (int k = 0; k < ns; ++k) if (slots[k].i == ic) fn(slots[k]);
    };
    each_far_to_near([&](Slot& s) { if (std::fabs(s.d) <= 7.f) shelfdraw::cases_case(sc, s.cd, s.ps, true); });
    shelfdraw::cases_veil(sc, p0, p1, CS.tint0, CS.tint1, 1.f);
    shelfdraw::cases_stage_front(sc, p0, p1, CS.tint0, CS.tint1);
    if (ins > 0.01f) dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, (int)(110.f * ins)));

    // ---- hit boxes (left to right; the centre case claims its whole box) ----
    if (n) {
        int a0 = -1;
        for (int k = 0; k < ns; ++k) if (slots[k].i == ic) a0 = k;
        float xs[2 * kWindow + 5];
        const float fy = f0 + Hc * 0.04f, ty = f0 - Hc * 1.08f;
        for (int k = 0; k < ns; ++k) xs[k] = 0.5f * (slots[k].mn.x + slots[k].mx.x);
        float bd[2 * kWindow + 5];   // boundary k sits between slot k-1 and slot k
        bd[0] = xs[0] - sc.U * 0.1f;
        for (int k = 1; k < ns; ++k) bd[k] = 0.5f * (xs[k - 1] + xs[k]);
        bd[ns] = xs[ns - 1] + sc.U * 0.1f;
        if (a0 >= 0) { bd[a0] = std::min(bd[a0], slots[a0].mn.x); bd[a0 + 1] = std::max(bd[a0 + 1], slots[a0].mx.x); }
        int nav_to = -1;
        bool centre_click = false, centre_act = false, centre_det = false, centre_press = false;
        bool centre_hover = false;
        for (int k = 0; k < ns; ++k) {
            const Slot& s = slots[k];
            const int idx = D.flat[(size_t)s.i];
            const ImVec2 q0(std::max(bd[k], p0.x), ty), q1(std::min(bd[k + 1], p1.x), fy);
            if (q1.x - q0.x < 2.f) continue;
            const CHit h = cases_hit(item_hash(idx), q0, q1, s.i == CS.focus);
            if (!G.focus_key.empty() && s.i == CS.focus && G.focus_key == item_key(idx)) { apply_pending_focus(idx); G.focus_quiet = false; }
            if (s.i == CS.focus) { centre_click = h.clicked; centre_act = h.activated; centre_det = h.details; centre_press = h.pressed; centre_hover = h.hovered; }
            else {
                if (h.focused && io.NavVisible && nav_to < 0 && CS.mode != Mode::Inspect) nav_to = s.i;
                if (h.clicked && !h.details && !CS.dragged) { cases_goto(D, s.i, false, true); }
                else if (h.details && !is_ghost(idx)) { A.sel_game = A.lib[(size_t)idx].key; A.show_details = true; }
            }
        }
        if (CS.mode == Mode::Inspect) {   // the arrows spin the case here, so keep the nav focus on it
            if (!G.focus_key.empty()) {} else { G.focus_key = item_key(D.flat[(size_t)CS.focus]); G.focus_quiet = false; }
        }
        if (nav_to >= 0 && nav_to != CS.focus) {
            int target = nav_to;
            if (held_dir != 0 && CS.hold_t > 0.f) {   // holding a direction accelerates
                const int extra = hold_boost(CS.hold_t) - 1;
                target = clamp_index(nav_to + held_dir * extra, n);
            }
            cases_goto(D, target, false, target != nav_to);
        }
        // a drag on the centre case inspects it; a click plays / opens details
        if (ImGui::IsMouseClicked(0)) { CS.drag_ok = centre_hover && CS.mode != Mode::Flip; CS.dragged = false; }
        if (CS.drag_ok && ImGui::IsMouseDown(0) && ImGui::IsMouseDragging(0, sp(0.6f))) {
            if (CS.mode == Mode::Browse) { CS.mode = Mode::Inspect; shot_trace("cases-inspect", "drag"); }
            CS.dragged = true;
            if (CS.mode == Mode::Inspect) CS.spin = spin_apply(CS.spin, io.MouseDelta.x * 0.011f, io.MouseDelta.y * 0.006f);
        }
        if (ImGui::IsMouseReleased(0)) CS.drag_ok = false;
        if (!CS.dragged) {
            if (centre_det && !is_ghost(cur_idx)) deliver(cur_idx, [] { TileResult r; r.details = true; r.clicked = true; return r; }());
            else if (centre_click && CS.mode == Mode::Flip) { CS.mode = Mode::Browse; }
            else if (centre_click && CS.mode == Mode::Browse) { TileResult r; r.clicked = true; r.hovered = true; deliver(cur_idx, r); }
            else if (centre_act && CS.mode == Mode::Browse) { TileResult r; r.activated = true; r.focused = true; deliver(cur_idx, r); }
        }
        (void)centre_press;
        if (!ImGui::IsMouseDown(0)) CS.dragged = false;
    }

    // ---- the cases, far to near, each with its floor shadow ----
    each_far_to_near([&](Slot& s) {
        shelfdraw::cases_shadow(sc, s.cd, s.ps);
        shelfdraw::cases_case(sc, s.cd, s.ps, false);
    });

    // ---- title and subtitle under the centre case ----
    if (n) {
        const int idx = cur_idx;
        const float maxw = W * 0.78f;
        if (CS.text_focus != CS.focus || CS.text_sig != D.sig || std::fabs(CS.text_w - maxw) > 2.f) {
            CS.text_focus = CS.focus; CS.text_sig = D.sig; CS.text_w = maxw;
            CS.title_txt = ui::ellipsize(tfont, tfont->FontSize, item_title(idx), maxw);
            const hub::System sy = sys_of(idx);
            int year = 0, players = item_players(idx);
            if (!is_ghost(idx)) {
                const hub::GameEntry& e = A.lib[(size_t)idx];
                const GameInfo* cat = catalog_for(e);
                CaseInfo ci;
                gameinfo_case_info(e, cat ? cat->title : std::string(), false, &ci);
                year = ci.year > 0 ? ci.year : (cat ? cat->year : 0);
                if (ci.players > 0) players = ci.players;
                else if (cat && cat->max_players > 0) players = cat->max_players;
            }
            CS.sub_txt = subtitle_text(hub::system_name(sy), year, players);
        }
        const float moving = std::min(1.f, std::fabs(CS.pos.x - (float)CS.focus) * 0.8f);
        const float ta = (1.f - moving * 0.85f) * (0.2f + 0.8f * CS.intro) * (1.f - 0.9f * ins);
        const ImVec2 ts = tfont->CalcTextSizeA(tfont->FontSize, FLT_MAX, 0.f, CS.title_txt.c_str());
        const float ty = f0 + sp(1.3f);
        const float tx = sc.cx - ts.x * 0.5f;
        shelfdraw::glow_text(dl, tfont, tfont->FontSize, ImVec2(tx, ty), ui::scale_alpha(IM_COL32(255, 255, 255, 255), ta), CS.tint1 ? ui::mix(CS.tint0, IM_COL32_WHITE, 0.35f) : ui::ACCENT, ta, CS.title_txt.c_str());
        const ImVec2 ss = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, CS.sub_txt.c_str());
        dl->AddText(cf, cf->FontSize, ImVec2(sc.cx - ss.x * 0.5f, ty + tfont->FontSize * 1.18f), ui::scale_alpha(ui::TEXT2, ta), CS.sub_txt.c_str());
        // favourite star beside the title; a ring fills while X is held down
        const bool fav = item_fav(idx);
        const float sr = tfont->FontSize * 0.62f;
        const ImVec2 sc_pos(tx + ts.x + sr * 1.4f, ty + tfont->FontSize * 0.55f);
        if (fav) ui::icon(dl, ui::Icon::StarFilled, sc_pos, sr * 2.f, ui::scale_alpha(IM_COL32(255, 214, 70, 255), ta));
        if (CS.x_t > 0.12f && !CS.x_fav) {
            const float pr = std::min(1.f, CS.x_t / kFavHold);
            ui::icon(dl, ui::Icon::Star, sc_pos, sr * 2.f, IM_COL32(255, 214, 70, 200));
            dl->PathArcTo(sc_pos, sr * 1.35f, -1.5708f, -1.5708f + 6.2831f * pr, 28);
            dl->PathStroke(IM_COL32(255, 214, 70, 230), 0, sp(0.25f));
        }
        if (CS.fav_flash > 0.01f) {
            const float k = 1.f - CS.fav_flash;
            dl->AddCircle(sc_pos, sr * (1.4f + 2.2f * k), IM_COL32(255, 214, 70, (int)(200 * CS.fav_flash)), 28, sp(0.25f));
        }
    } else {
        const char* m1 = A.search_buf[0] ? "No games match your search" : "No games in this console yet";
        const ImVec2 ms = tfont->CalcTextSizeA(tfont->FontSize, FLT_MAX, 0.f, m1);
        dl->AddText(tfont, tfont->FontSize, ImVec2(sc.cx - ms.x * 0.5f, f0 - Hc * 0.6f), ui::TEXT, m1);
        const char* m2 = "Pick another console with LB / RB, or change the search.";
        const ImVec2 m2s = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, m2);
        dl->AddText(cf, cf->FontSize, ImVec2(sc.cx - m2s.x * 0.5f, f0 - Hc * 0.6f + tfont->FontSize * 1.3f), ui::TEXT2, m2);
    }

    cases_tabs(dl, org, W, tab_y, tab_h, active_tab, can);
    cases_hints(dl, org, W, hint_y, hint_h);
    dl->PopClipRect();

    // ---- screenshot aids (HUB_SHOT_CASES=browse|flip|inspect|cartridge|missingart) ----
    if (!CS.shot_applied && n > 0 && !CS.shot_mode.empty() && A.scanned_once) {
        CS.shot_applied = true;
        const int target = std::min(n - 1, std::max(0, n / 5));
        cases_goto(D, CS.focus > 0 ? CS.focus : target, false, false);
        CS.pos.x = (float)CS.focus;
        if (CS.shot_mode == "flip") { CS.mode = Mode::Flip; if (!is_ghost(D.flat[(size_t)CS.focus])) cases_collect_back(D.flat[(size_t)CS.focus]); CS.flip.x = 3.14159f; CS.flip.v = 0; }
        else if (CS.shot_mode == "inspect") { CS.mode = Mode::Inspect; CS.spin.yaw = 0.62f; CS.spin.pitch = -0.2f; CS.insp.x = 1.f; }
        CS.intro = 1.f;
    }
    ImGui::SetCursorScreenPos(org);
    ImGui::Dummy(sz);
}
