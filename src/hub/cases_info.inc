/* Coverflow state and the info panel under the carousel (included into libview_ui.cpp's anonymous namespace right before cases_nav.inc; see cases_view.inc).
 * Layout numbers adapted from PyroCade (MIT, Pyrodrifter). The panel sits bottom-left above the scrubber / hint bar: an accent rule, the eyebrow line
 * (console / position / media chip), the title (2 lines at most), the facts, a short description, the action buttons, and a 16:10 screenshot on the
 * right. Everything is cached when the selection (or the width) changes: nothing is built or allocated per frame in the draw path. */

struct CfState {
    hubui::cases::cf::EaseState pos_es;       // the glide of CS.pos.x (cubic-bezier .22 1 .36 1, 0.7 s)
    hubui::cases::cf::MediaReveal media;      // which cases have their disc / cartridge out
    double idle_t = 0.0; float idle_w = 0.f; bool idle_rest = false;  // time since the carousel settled; the idle loop's weight (it fades in / out, never snaps)
    // the background colour grid (darkened, saturated): the one shown, the one it came from, the one it goes to
    ImU32 bg_cur[64] = {}, bg_from[64] = {}, bg_to[64] = {};
    float bg_t = 1.f; bool bg_init = false, bg_art = false; int bg_idx = -2; uint64_t bg_sig = ~0ull;
    ImU32 bg_tint = 0;
    // the info panel
    int info_focus = -2; uint64_t info_sig = ~0ull, info_gen = ~0ull; int info_tick = -1; float info_w = 0.f, info_fs = 0.f; bool info_tight = false;
    float info_t = 100.f;                     // seconds since the selection changed (drives the fade / rise)
    std::string eyebrow, chip, meta, title;
    std::vector<std::string> title_lines, desc_lines;
    ImU32 accent = 0;
    SDL_Texture* fake_pic = nullptr; int fake_w = 0, fake_h = 0; bool fake_tried = false;
    bool have_desc = false;
} CFX;

bool cases_flow_on() {
    static const bool force_classic = g_headless && std::getenv("HUB_SHOT_CASES_STYLE") && std::string(std::getenv("HUB_SHOT_CASES_STYLE")) == "classic";
    return !A.st.lib_cases_classic && !force_classic;
}

/* ---- background ---- */
ImU32 cf_bg_cell(ImU32 c) {   // blur(40px) saturate(1.3) brightness(.45) of one average colour
    const float r = (float)(c & 255), g = (float)((c >> 8) & 255), b = (float)((c >> 16) & 255);
    const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    auto f = [&](float v) { return std::max(0, std::min(255, (int)((l + (v - l) * 1.3f) * 0.45f))); };
    return IM_COL32(f(r), f(g), f(b), 255);
}
/* Called every frame: starts a 1.1 s cross-fade when the selected cover (or its art arriving) changes, and mixes the grid that is shown. */
void cf_bg_update(int n, int cur_idx, uint64_t sig, ImU32 t0, ImU32 t1, bool no_art, float dt, bool reduced) {
    const uint32_t* bc = nullptr;
    if (n > 0 && !no_art) {
        const ArtRef a = art_of(cur_idx);
        SDL_Texture* tex = a.tex;
        if (sys_of(cur_idx) == hub::System::Switch) { int iw = 0, ih = 0; if (SDL_Texture* it = cases_shot_icon(&iw, &ih)) tex = it; }
        if (tex) bc = cases_art_blur(tex);
    }
    const bool have = bc != nullptr;
    if (!CFX.bg_init || CFX.bg_idx != cur_idx || CFX.bg_sig != sig || CFX.bg_art != have) {
        const bool first = !CFX.bg_init;
        std::memcpy(CFX.bg_from, CFX.bg_cur, sizeof CFX.bg_cur);
        for (int i = 0; i < 64; ++i) {
            if (bc) CFX.bg_to[i] = cf_bg_cell(bc[i]);
            else {   // no art: the console colours, top to bottom
                const ImU32 c = ui::mix(t0, t1, (float)(i / 8) / 7.f);
                CFX.bg_to[i] = cf_bg_cell(c);
            }
        }
        if (first) { std::memcpy(CFX.bg_cur, CFX.bg_to, sizeof CFX.bg_cur); std::memcpy(CFX.bg_from, CFX.bg_to, sizeof CFX.bg_to); }
        CFX.bg_t = first || reduced ? 1.f : 0.f;
        CFX.bg_init = true; CFX.bg_idx = cur_idx; CFX.bg_sig = sig; CFX.bg_art = have;
    }
    if (CFX.bg_t < 1.f) CFX.bg_t = reduced ? 1.f : std::min(1.f, CFX.bg_t + dt / 1.1f);
    const float k = hubui::cases::smoothstep01(CFX.bg_t);
    for (int i = 0; i < 64; ++i) CFX.bg_cur[i] = ui::mix(CFX.bg_from[i], CFX.bg_to[i], k);
}

/* ---- text with letter spacing (the eyebrow) ---- */
float cf_spaced_w(ImFont* f, float fs, const char* s, float spx) {
    float w = 0.f;
    const char* end = s + std::strlen(s);
    for (const char* p = s; p < end;) {
        unsigned int c = 0; const int len = ImTextCharFromUtf8(&c, p, end);
        char tmp[8] = {}; std::memcpy(tmp, p, (size_t)std::min(len, 4));
        w += f->CalcTextSizeA(fs, FLT_MAX, 0.f, tmp).x + spx;
        p += std::max(1, len);
    }
    return std::max(0.f, w - spx);
}
float cf_spaced(ImDrawList* dl, ImFont* f, float fs, ImVec2 pos, ImU32 col, const char* s, float spx) {
    const char* end = s + std::strlen(s);
    float x = pos.x;
    for (const char* p = s; p < end;) {
        unsigned int c = 0; const int len = ImTextCharFromUtf8(&c, p, end);
        char tmp[8] = {}; std::memcpy(tmp, p, (size_t)std::min(len, 4));
        dl->AddText(f, fs, ImVec2(x, pos.y), col, tmp);
        x += f->CalcTextSizeA(fs, FLT_MAX, 0.f, tmp).x + spx;
        p += std::max(1, len);
    }
    return x - pos.x - spx;
}
void cf_upper(std::string& s) { for (char& c : s) if ((unsigned char)c < 128) c = (char)std::toupper((unsigned char)c); }

/* A pill button: returns true when clicked. fill = the accent (Play), else a glassy dark pill. */
bool cf_button(ImDrawList* dl, const char* id, ImVec2 p0, ImVec2 p1, const char* label, ImU32 fill, bool filled, bool star, bool star_on, float a, bool enabled) {
    ImGui::PushID(id);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::SetCursorScreenPos(p0);
    const bool pressed = ImGui::InvisibleButton("b", p1 - p0) && enabled;
    ImGui::PopItemFlag();
    const bool hov = ImGui::IsItemHovered() && enabled;
    ImGui::PopID();
    const float h = p1.y - p0.y, r = std::min(h * 0.32f, sp(1.1f));
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (filled) {
        dl->AddRectFilled(ImVec2(p0.x, p0.y + 2.f), ImVec2(p1.x, p1.y + 2.f), ui::scale_alpha(fill, 0.35f * a), r + 2.f);
        dl->AddRectFilled(p0, p1, ui::scale_alpha(hov ? ui::mix(fill, IM_COL32_WHITE, 0.14f) : fill, a), r);
    } else {
        dl->AddRectFilled(p0, p1, ui::scale_alpha(hov ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 16), a), r);
        dl->AddRect(p0, p1, ui::scale_alpha(IM_COL32(255, 255, 255, hov ? 80 : 44), a), r, 0, 1.f);
    }
    const float luma = (0.2126f * (fill & 255) + 0.7152f * ((fill >> 8) & 255) + 0.0722f * ((fill >> 16) & 255)) / 255.f;
    ImU32 ink = filled ? (luma > 0.62f ? IM_COL32(18, 20, 28, 255) : IM_COL32_WHITE) : (enabled ? ui::TEXT : ui::TEXT3);
    ink = ui::scale_alpha(ink, a);
    ImFont* bf = ui::F.bold ? ui::F.bold : ui::F.body;
    const float fs = bf->FontSize;
    const ImVec2 ts = bf->CalcTextSizeA(fs, FLT_MAX, 0.f, label);
    float x = p0.x + (p1.x - p0.x - ts.x) * 0.5f;
    if (star) {
        const float ir = h * 0.30f, iw = ir * 2.f + sp(0.5f);
        x = p0.x + (p1.x - p0.x - ts.x - iw) * 0.5f;
        ui::icon(dl, star_on ? ui::Icon::StarFilled : ui::Icon::Star, ImVec2(x + ir, (p0.y + p1.y) * 0.5f), ir * 2.f, star_on ? ui::scale_alpha(IM_COL32(255, 214, 70, 255), a) : ink);
        x += iw;
    }
    dl->AddText(bf, fs, ImVec2(x, p0.y + (h - ts.y) * 0.5f), ink, label);
    return pressed;
}
float cf_button_w(const char* label, bool star, float h) {
    ImFont* bf = ui::F.bold ? ui::F.bold : ui::F.body;
    return bf->CalcTextSizeA(bf->FontSize, FLT_MAX, 0.f, label).x + sp(3.2f) + (star ? h * 0.6f + sp(0.5f) : 0.f);
}

/* The panel in [org.x, org.x + W] x [y0, y1]. idx = the selected game's list index. click_ok: the buttons are live. */
void cf_info_panel(ImDrawList* dl, ImVec2 org, float W, float y0, float y1, bool tight, int idx, bool click_ok, float ins, float dt, bool reduced, int n, uint64_t sig) {
    using namespace hubui::cases;
    namespace cf = hubui::cases::cf;
    ImFont* cap = ui::F.cap;
    ImFont* tfont = (ui::F.title && !tight) ? ui::F.title : (ui::F.h2 ? ui::F.h2 : ui::F.body);
    ImFont* bf = ui::F.bold ? ui::F.bold : ui::F.body;
    const float cfs = cap->FontSize, tsz = tfont->FontSize * (tight ? 1.0f : std::min(1.5f, 1.14f + (y1 - y0) / sp(100.f)));
    const float mx = std::max(sp(3.f), W * 0.047f);
    const float x0 = org.x + mx, x1 = org.x + W - mx;
    const float H = y1 - y0;
    const bool ghost = is_ghost(idx);
    const hub::System sy = sys_of(idx);
    // the screenshot (the game's own title screen / in-game picture; the player's own pictures are on the back of the case)
    SDL_Texture* pic = nullptr; int pw = 0, ph = 0;
    if (!ghost && !CS.no_art) {
        const hub::GameEntry& e = A.lib[(size_t)idx];
        pic = hubart::get(e, hubart::Kind::Snap, &pw, &ph);
        if (!pic) pic = hubart::get(e, hubart::Kind::Title, &pw, &ph);
    }
    if (!pic && g_headless && cases_fake_lib() && !CS.no_art) {   // headless screenshots of the synthetic shelves: a made-up picture
        if (!CFX.fake_tried) { CFX.fake_tried = true; CFX.fake_pic = cases_fake_pic(1, &CFX.fake_w, &CFX.fake_h); }
        pic = CFX.fake_pic; pw = CFX.fake_w; ph = CFX.fake_h;
    }
    float shot_w = 0.f;
    if (pic && pw > 0 && ph > 0) shot_w = std::min(std::min(W * 0.2f, sp(45.f)), std::max(0.f, (H - sp(1.5f)) * 1.6f));
    if (shot_w < sp(12.f)) shot_w = 0.f;
    const float gap = sp(3.5f);
    const float text_w = std::max(sp(12.f), (shot_w > 0.f ? x1 - shot_w - gap : x1) - x0);
    // ---- the strings (when the selection, the width or the description changes) ----
    const int tick = (int)(CS.clock / 5.f);
    if (CFX.info_focus != CS.focus || CFX.info_sig != sig || std::fabs(CFX.info_w - text_w) > 2.f || CFX.info_fs != tsz || CFX.info_gen != gameinfo_generation() || CFX.info_tick != tick || CFX.info_tight != tight) {
        if (CFX.info_focus != CS.focus) CFX.info_t = reduced ? 100.f : 0.f;
        CFX.info_focus = CS.focus; CFX.info_sig = sig; CFX.info_w = text_w; CFX.info_fs = tsz; CFX.info_gen = gameinfo_generation(); CFX.info_tick = tick; CFX.info_tight = tight;
        int year = 0, players = item_players(idx);
        std::string genre, maker, desc;
        if (!ghost) {
            const hub::GameEntry& e = A.lib[(size_t)idx];
            const GameInfo* cat = catalog_for(e);
            CaseInfo ci;
            gameinfo_case_info(e, cat ? cat->title : std::string(), true, &ci);
            year = ci.year > 0 ? ci.year : (cat ? cat->year : 0);
            if (ci.players > 0) players = ci.players; else if (cat && cat->max_players > 0) players = cat->max_players;
            genre = !ci.genre.empty() ? ci.genre : (cat ? cat->genre : std::string());
            maker = ci.maker;
            desc = ci.desc;
            if (desc.empty() && cat) desc = cat->blurb;
            if (g_headless && cases_fake_lib() && desc.empty()) { desc = "Race, battle and trade across a colourful world. Collect every cup, unlock hidden tracks and challenge friends in split-screen for up to four players."; if (genre.empty()) genre = "Racing"; if (year == 0) year = 2001; if (maker.empty()) maker = "Retro Studio"; }
        }
        const char* lab = tab_label(item_sys(idx));
        std::string con = lab ? lab : hub::system_name(sy);
        if (sy == hub::System::Switch && !ghost && A.lib[(size_t)idx].nx2) con = "Switch 2";
        CFX.eyebrow = con + "  \xC2\xB7  " + count_text(CS.focus, n);
        cf_upper(CFX.eyebrow);
        {
            const cf::MediaSpec ms = cf::media_spec(CS.media[(size_t)CS.focus]);
            CFX.chip = ms.kind == cf::MediaKind::Card ? "CARTRIDGE" : "DISC";
        }
        std::string m;
        const std::string dot = "  \xC2\xB7  ";
        auto add = [&](const std::string& s) { if (!s.empty()) { if (!m.empty()) m += dot; m += s; } };
        if (year > 0) add(std::to_string(year));
        add(maker);
        add(genre);
        if (players == 1) add("1 player"); else if (players > 1) add("1-" + std::to_string(players) + " players");
        if (ghost) add("From a friend's library");
        else {
            const hub::GameState& gs = gstate(A.lib[(size_t)idx].key);
            std::string pl = hubui::cont::last_played_line((uint64_t)std::time(nullptr), gs.last_played, gs.play_seconds, dot);
            if (!pl.empty()) { if (!m.empty()) m += dot; m += pl; }
        }
        CFX.meta = m.empty() ? m : ui::ellipsize(cap, cfs * 1.08f, m, text_w);
        CFX.title = item_title(idx);
        CFX.title_lines = ui::wrap_lines(tfont, tsz, CFX.title, text_w, tight ? 1 : 2);
        CFX.have_desc = !desc.empty();
        CFX.desc_lines.clear();
        if (!desc.empty()) CFX.desc_lines = ui::wrap_lines(cap, cfs * 1.06f, desc, text_w, 2);
        ImU32 c0 = ui::ACCENT, c1 = ui::ACCENT;
        system_colors(sy, &c0, &c1);
        if (sy == hub::System::Switch && !ghost && A.lib[(size_t)idx].nx2) c0 = IM_COL32(228, 0, 18, 255);
        {   // the accent also colours small text: a dark console colour is lifted until it reads on the dark room
            const float l = (0.2126f * (float)(c0 & 255) + 0.7152f * (float)((c0 >> 8) & 255) + 0.0722f * (float)((c0 >> 16) & 255)) / 255.f;
            CFX.accent = l < 0.55f ? ui::mix(c0, IM_COL32_WHITE, std::min(0.7f, (0.55f - l) * 1.6f)) : c0;
        }
    }
    if (!reduced) CFX.info_t += dt; else CFX.info_t = 100.f;
    const float ta = cf::info_alpha(CFX.info_t) * (1.f - 0.55f * ins);
    const float rise = cf::info_rise(CFX.info_t) * ui::S;
    const float moving = n ? std::min(1.f, std::fabs(CS.pos.x - (float)CS.focus) * 0.5f) : 0.f;
    const float a = ta * (1.f - 0.35f * moving);
    // ---- vertical budget: what fits ----
    const float bh = sp(tight ? 3.0f : 3.5f);
    const int tl = (int)CFX.title_lines.size();
    int dl_lines = std::min((int)CFX.desc_lines.size(), 2);
    int t_lines = std::min(tl, 2);
    const float eyebrow_h = cfs * 1.5f, title_lh = tsz * 1.1f, meta_h = cfs * 1.55f, desc_lh = cfs * 1.42f;
    auto need = [&](int tn, int dn, bool btn) { return eyebrow_h + title_lh * (float)tn + (CFX.meta.empty() ? 0.f : meta_h) + desc_lh * (float)dn + (dn > 0 ? sp(0.3f) : 0.f) + (btn ? bh + sp(0.8f) : 0.f); };
    const float room = H - sp(0.9f);
    bool btn = true;
    while (need(t_lines, dl_lines, btn) > room) {
        if (dl_lines > 1) --dl_lines; else if (dl_lines > 0) dl_lines = 0; else if (t_lines > 1) t_lines = 1; else if (btn) btn = false; else break;
    }
    float y = y0 + sp(0.5f) + rise;
    // the accent rule: accent, then white at 5%, then clear
    {
        const float ry = y0;
        const float xa = x0, xb = x0 + (x1 - x0) * 0.45f, xc = x1;
        dl->AddRectFilledMultiColor(ImVec2(xa, ry), ImVec2(xb, ry + 1.f), ui::scale_alpha(CFX.accent, 0.62f), IM_COL32(255, 255, 255, 14), IM_COL32(255, 255, 255, 14), ui::scale_alpha(CFX.accent, 0.62f));
        dl->AddRectFilledMultiColor(ImVec2(xb, ry), ImVec2(xc, ry + 1.f), IM_COL32(255, 255, 255, 14), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 14));
    }
    y += sp(0.35f);
    // eyebrow + media chip
    {
        const float spx = cfs * 0.2f;
        const float ew = cf_spaced(dl, bf, cfs, ImVec2(x0, y), ui::scale_alpha(CFX.accent, a), CFX.eyebrow.c_str(), spx);
        const float chs = cfs * 0.82f, cpad = cfs * 0.55f, cw = cf_spaced_w(bf, chs, CFX.chip.c_str(), chs * 0.14f) + cpad * 2.f, ch = chs * 1.5f;
        const ImVec2 c0(x0 + ew + cfs * 1.1f, y + (cfs - ch) * 0.5f + cfs * 0.12f);
        if (c0.x + cw < x0 + text_w) {
            dl->AddRect(c0, ImVec2(c0.x + cw, c0.y + ch), ui::scale_alpha(IM_COL32(255, 255, 255, 70), a), 3.f, 0, 1.f);
            cf_spaced(dl, bf, chs, ImVec2(c0.x + cpad, c0.y + (ch - chs) * 0.5f), ui::scale_alpha(ui::TEXT2, a), CFX.chip.c_str(), chs * 0.14f);
        }
        y += eyebrow_h;
    }
    // the title: big and bold with a soft shadow
    for (int i = 0; i < t_lines; ++i) {
        const char* s = CFX.title_lines[(size_t)i].c_str();
        const ImVec2 p(x0, y);
        dl->AddText(tfont, tsz, ImVec2(p.x, p.y + 4.f), IM_COL32(0, 0, 0, (int)(70 * a)), s);
        dl->AddText(tfont, tsz, ImVec2(p.x, p.y + 2.f), IM_COL32(0, 0, 0, (int)(120 * a)), s);
        dl->AddText(tfont, tsz, p, ui::scale_alpha(IM_COL32(255, 255, 255, 255), a), s);
        y += title_lh;
    }
    if (!CFX.meta.empty()) {
        y += cfs * 0.15f;
        dl->AddText(cap, cfs * 1.08f, ImVec2(x0, y), ui::scale_alpha(ui::TEXT, a * 0.82f), CFX.meta.c_str());
        y += meta_h - cfs * 0.15f;
    }
    if (dl_lines > 0) {
        y += sp(0.15f);
        for (int i = 0; i < dl_lines; ++i) {
            std::string& ln = CFX.desc_lines[(size_t)i];
            dl->AddText(cap, cfs * 1.06f, ImVec2(x0, y), ui::scale_alpha(ui::TEXT2, a * 0.92f), ln.c_str());
            y += desc_lh;
        }
        y += sp(0.15f);
    }
    // the action buttons
    if (btn) {
        y += sp(0.5f);
        const bool live = click_ok && a > 0.3f;
        float bx = x0;
        const ImU32 ac = CFX.accent;
        struct B { const char* label; int kind; };
        const B bs[4] = {{"Play", 0}, {"Details", 1}, {"Flip", 2}, {item_fav(idx) ? "Favorited" : "Favorite", 3}};   // the library's spelling (hint bar "F Favorite", "Favorites first")
        for (const B& b : bs) {
            if (ghost && b.kind == 2) continue;   // nothing to flip for a game you do not own
            const bool star = b.kind == 3;
            const float w = cf_button_w(b.label, star, bh);
            if (bx + w > x0 + text_w + sp(1.f) && b.kind != 0) break;
            const bool en = !(ghost && (b.kind == 1 || b.kind == 3));
            if (ghost && (b.kind == 1 || b.kind == 3)) continue;
            if (cf_button(dl, b.label, ImVec2(bx, y), ImVec2(bx + w, y + bh), b.label, ac, b.kind == 0, star, star && item_fav(idx), a, live && en)) {
                if (b.kind == 0) { TileResult r; r.activated = true; r.focused = true; deliver(idx, r); }
                else if (b.kind == 1 && !ghost) { TileResult r; r.details = true; r.clicked = true; deliver(idx, r); }
                else if (b.kind == 2 && !ghost) {
                    const Mode m = on_flip_tap(CS.mode);
                    if (m == Mode::Flip) cases_collect_back(idx);
                    CS.mode = m;
                    shot_trace("cases-flip", m == Mode::Flip ? "front->back" : "back->front");
                } else if (b.kind == 3 && !ghost) { toggle_favorite(A.lib[(size_t)idx].key); CS.fav_flash = 1.f; }
            }
            if (b.kind == 3 && CS.x_t > 0.12f && !CS.x_fav) {   // X held: a ring fills around the button
                const float pr = std::min(1.f, CS.x_t / kFavHold);
                dl->AddRect(ImVec2(bx - 2.f, y - 2.f), ImVec2(bx + w + 2.f, y + bh + 2.f), IM_COL32(255, 214, 70, (int)(60 + 170 * pr)), bh * 0.3f, 0, 2.f);
            }
            bx += w + sp(0.8f);
        }
    }
    // the screenshot on the right: 16:10, rounded, with a soft shadow
    if (shot_w > 0.f) {
        const float sh = shot_w * 0.625f;
        const float sx1 = x1, sx0 = sx1 - shot_w;
        const float sy1 = y1 - sp(0.7f) + rise * 0.5f, sy0 = std::max(y0 + sp(0.9f), sy1 - sh);
        const float rr = 14.f * ui::S;
        const ImVec2 q0(sx0, sy1 - sh), q1(sx1, sy1);
        (void)sy0;
        for (int i = 3; i >= 1; --i) dl->AddRectFilled(ImVec2(q0.x - 2.f * (float)i, q0.y + 6.f * (float)i), ImVec2(q1.x + 2.f * (float)i, q1.y + 8.f * (float)i), IM_COL32(0, 0, 0, (int)(26.f * a)), rr + 3.f * (float)i);
        // crop to 16:10 from the middle
        const float ta_ = 1.6f, asp = (float)pw / (float)ph;
        float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
        if (asp > ta_) { const float k = ta_ / asp; u0 = (1.f - k) * 0.5f; u1 = u0 + k; } else { const float k = asp / ta_; v0 = (1.f - k) * 0.5f; v1 = v0 + k; }
        dl->AddImageRounded((ImTextureID)pic, q0, q1, ImVec2(u0, v0), ImVec2(u1, v1), ui::scale_alpha(IM_COL32_WHITE, a), rr);
        dl->AddRect(q0, q1, ui::scale_alpha(IM_COL32(255, 255, 255, 46), a), rr, 0, 1.f);
    }
}
