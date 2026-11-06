/* Cases navigation (included by cases_view.inc, after CS and cases_goto): the sort chip + picker, the scrubber strip, section jumps,
 * the quick-jump overlay and the section bubble. The pure parts (labels, sections, scrubber maths, picker rows) are in cases_logic.h
 * ("nav" section) and tested in test_cases.cpp.
 *
 * Controls
 *   keyboard: O sort picker, J quick jump, PageUp / PageDown previous / next section, Home / End first / last, click or drag the strip
 *   pad:      LT / RT previous / next section, right stick up / down quick jump, right stick left / right sort picker
 *   mouse:    click the Sort chip, click or drag the scrubber, fast wheel spins travel further
 * While the picker or the overlay is open the carousel ignores its own keys (nav_blocking()). */

struct NavState {
    uint64_t sig = ~0ull; int n = -1;                       // the list the labels belong to
    hubui::cases::NavKind kind = hubui::cases::NavKind::Letters;
    std::vector<std::string> labels;                        // one per flat position
    std::vector<hubui::cases::NavSection> secs;
    bool sort_open = false; int sort_row = 0; float sort_t = 0.f;
    bool jump_open = false; int jump_sel = 0, jump_origin = 0; float jump_f = 0.f, jump_t = 0.f;
    std::string typed; float typed_age = 0.f;               // quick-jump typing (Title sort)
    bool closing = false;                                   // a modal closed this frame: its key must not also reach the cases
    bool glide = false;                                     // the list was re-sorted: the carousel glides to the selected game's new place
    bool scrubbing = false;
    float fast_t = 0.f, bub = 0.f; std::string bub_txt;     // section bubble
    float wheel_v = 0.f, wheel_gain = 1.f;
    bool force_bubble = false;                              // screenshot aid
} NV;

bool nav_blocking() { return NV.sort_open || NV.jump_open || NV.closing; }
hubui::SortSpec nav_spec() { return hubui::SortSpec(hubui::sort_from_key(A.st.library_sort), A.st.library_sort_rev, A.st.library_favs_first); }
float nav_strip_h(bool tight) { return sp(tight ? 3.4f : 4.2f); }
float nav_side_w() { return sp(21.f); }   // room kept clear on both sides of the console tabs: the menu button on the left, the Sort chip on the right

void nav_refresh(const Disp& D) {
    using namespace hubui::cases;
    const uint64_t key = D.sig * 1000003ull ^ ((uint64_t)A.sort_mode * 7 + (A.sort_rev ? 131 : 0) + (A.sort_favs ? 977 : 0));
    if (NV.sig == key && NV.n == (int)D.flat.size()) return;
    NV.sig = key; NV.n = (int)D.flat.size();
    NV.kind = nav_kind_for(A.sort_mode);
    const int64_t now = (int64_t)std::time(nullptr);
    NV.labels.assign(D.flat.size(), std::string());
    for (size_t p = 0; p < D.flat.size(); ++p) {
        const int idx = D.flat[p];
        std::string& l = NV.labels[p];
        if (is_ghost(idx)) { l = "Friends"; continue; }                  // friends' games you do not own sort after everything
        if (item_pinned(idx)) continue;                                  // the bundled game joins its neighbour's section
        const hub::GameEntry& e = A.lib[(size_t)idx];
        const hub::GameState& g = gstate(e.key);
        if (A.sort_favs && g.favorite) { l = kNavFavorites; continue; }
        switch (NV.kind) {
            case NavKind::Letters: l = nav_label_letter(e.title); break;
            case NavKind::Years: l = nav_label_year(lib_year_of(idx)); break;
            case NavKind::Consoles: l = short_sys(item_sys(idx)); break;
            case NavKind::Recency: l = nav_label_age(now, (int64_t)g.last_played); break;
            case NavKind::Plays: l = nav_label_plays((int64_t)g.play_seconds); break;
            case NavKind::Added: l = nav_label_age(now, (int64_t)lib_added_unix(e.path), false); break;
        }
    }
    NV.secs = build_sections(NV.labels);
}

const char* nav_kind_word(hubui::cases::NavKind k) {
    using hubui::cases::NavKind;
    switch (k) { case NavKind::Years: return "year"; case NavKind::Consoles: return "console"; case NavKind::Letters: return "letter"; default: return "section"; }
}

void nav_open_sort() {
    NV.sort_open = true; NV.jump_open = false;
    NV.sort_row = (int)nav_spec().mode;
    shot_trace("cases-nav", "sort-open");
}
void nav_close_all() { if (NV.sort_open || NV.jump_open) { NV.closing = true; A.nav_seed = true; G.focus_quiet = false; } NV.sort_open = false; NV.jump_open = false; NV.typed.clear(); }
bool nav_close_modal() {   // B / Esc (lib_cases_back): closes the picker / overlay and uses the press
    if (!NV.sort_open && !NV.jump_open) return false;
    if (NV.jump_open && NV.jump_origin >= 0) NV.fast_t = 0.f;
    nav_close_all();
    shot_trace("cases-nav", "closed");
    return true;
}
void nav_apply_sort_row(int row) {
    const hubui::SortSpec s = hubui::cases::sort_apply_row(nav_spec(), row);
    A.st.library_sort = hubui::sort_key(s.mode);
    A.st.library_sort_rev = s.reverse;
    A.st.library_favs_first = s.favs_first;
    NV.glide = true;
    sort_changed();
    if (row < hubui::kSortModes) nav_close_all();
}
void nav_open_jump(const Disp& D) {
    nav_refresh(D);
    if (NV.secs.empty()) return;
    NV.jump_open = true; NV.sort_open = false;
    NV.jump_origin = CS.focus;
    NV.jump_sel = hubui::cases::section_index(NV.secs, CS.focus);
    NV.jump_f = (float)NV.jump_sel;
    NV.typed.clear();
    shot_trace("cases-nav", "jump-open");
}
float nav_land_x(float old_x, int p) {   // the carousel position to start from after the list was rebuilt around the selected game
    if (!NV.glide) return (float)p;
    NV.glide = false;
    return hubui::cases::jump_start(old_x, p, 9.f);
}

/* Travel helpers: every long move goes through here so the bubble shows and the glide stays short. */
void nav_travel(const Disp& D, int pos, float bubble_secs) {
    cases_goto(D, pos, true, true);
    NV.fast_t = std::max(NV.fast_t, bubble_secs);
}

int nav_wheel_steps(int steps) {   // CS wheel block: a faster spin travels further per notch
    NV.wheel_v += (float)std::abs(steps);
    NV.wheel_gain = hubui::cases::nav_wheel_gain(NV.wheel_v * 3.f);
    if (NV.wheel_gain > 1.01f) { NV.fast_t = std::max(NV.fast_t, 0.5f); return (int)std::lround((float)steps * NV.wheel_gain); }
    return steps;
}

/* Keys and pad. `can` = the carousel may take input at all (not the menu / a popup). Called once per frame from draw_cases' input block. */
void nav_input(Disp& D, bool can, bool act, float dt) {
    using namespace hubui::cases;
    ImGuiIO& io = ImGui::GetIO();
    const int n = (int)D.flat.size();
    NV.closing = false;
    NV.wheel_v *= std::exp(-dt * 3.f);
    if (NV.wheel_v < 0.05f) NV.wheel_gain = 1.f;
    NV.fast_t = std::max(0.f, NV.fast_t - dt);
    NV.typed_age += dt;
    nav_refresh(D);
    if (CS.menu_open || !can || n == 0 || !(A.page == Page::Library)) { if (NV.sort_open || NV.jump_open) nav_close_all(); NV.scrubbing = false; return; }
    auto pr = [](ImGuiKey k, bool rep = false) { return ImGui::IsKeyPressed(k, rep); };
    auto padk = [&](ImGuiKey k, bool rep = false) { if (ImGui::IsKeyPressed(k, rep)) { A.pad_mode = true; return true; } return false; };
    const bool typing = io.WantTextInput;
    if (NV.sort_open) {   // ---- the sort picker ----
        const bool up = pr(ImGuiKey_UpArrow, true) || padk(ImGuiKey_GamepadDpadUp, true) || padk(ImGuiKey_GamepadLStickUp, true) || padk(ImGuiKey_GamepadRStickUp, false);
        const bool dn = pr(ImGuiKey_DownArrow, true) || padk(ImGuiKey_GamepadDpadDown, true) || padk(ImGuiKey_GamepadLStickDown, true) || padk(ImGuiKey_GamepadRStickDown, false);
        if (up) NV.sort_row = (NV.sort_row + kSortRows - 1) % kSortRows;
        if (dn) NV.sort_row = (NV.sort_row + 1) % kSortRows;
        if (pr(ImGuiKey_Enter) || pr(ImGuiKey_KeypadEnter) || padk(ImGuiKey_GamepadFaceDown)) nav_apply_sort_row(NV.sort_row);
        else if (pr(ImGuiKey_O) || padk(ImGuiKey_GamepadRStickLeft) || padk(ImGuiKey_GamepadRStickRight)) nav_close_all();
        return;
    }
    if (NV.jump_open) {   // ---- the quick-jump overlay: moves the carousel live; Enter / A keeps it, B / Esc goes back ----
        nav_refresh(D);   // the sections may have shrunk (rescan, search, tab) while the overlay is open
        if (NV.secs.empty()) { nav_close_all(); return; }
        NV.jump_sel = std::max(0, std::min((int)NV.secs.size() - 1, NV.jump_sel));
        int mv = 0;
        if (pr(ImGuiKey_LeftArrow, true) || padk(ImGuiKey_GamepadDpadLeft, true) || padk(ImGuiKey_GamepadLStickLeft, true) || padk(ImGuiKey_GamepadRStickLeft, true) || pr(ImGuiKey_UpArrow, true) || padk(ImGuiKey_GamepadRStickUp, false)) mv = -1;
        else if (pr(ImGuiKey_RightArrow, true) || padk(ImGuiKey_GamepadDpadRight, true) || padk(ImGuiKey_GamepadLStickRight, true) || padk(ImGuiKey_GamepadRStickRight, true) || pr(ImGuiKey_DownArrow, true) || padk(ImGuiKey_GamepadRStickDown, false)) mv = 1;
        int sel = NV.jump_sel;
        if (mv) sel = std::max(0, std::min((int)NV.secs.size() - 1, sel + mv));
        if (pr(ImGuiKey_PageUp, true) || padk(ImGuiKey_GamepadL1, true)) sel = std::max(0, sel - 5);
        if (pr(ImGuiKey_PageDown, true) || padk(ImGuiKey_GamepadR1, true)) sel = std::min((int)NV.secs.size() - 1, sel + 5);
        if (pr(ImGuiKey_Home)) sel = 0;
        if (pr(ImGuiKey_End)) sel = (int)NV.secs.size() - 1;
        if (NV.kind == NavKind::Letters && !io.KeyCtrl && !io.KeyAlt) {   // typing a title prefix
            if (NV.typed_age > 1.2f) NV.typed.clear();
            for (int c = 0; c < 26; ++c) if (pr((ImGuiKey)(ImGuiKey_A + c))) { NV.typed += (char)('a' + c); NV.typed_age = 0.f; }
            for (int c = 0; c < 10; ++c) if (pr((ImGuiKey)(ImGuiKey_0 + c))) { NV.typed += (char)('0' + c); NV.typed_age = 0.f; }
            if (pr(ImGuiKey_Backspace) && !NV.typed.empty()) { NV.typed.pop_back(); NV.typed_age = 0.f; }
            if (!NV.typed.empty() && NV.typed_age == 0.f) {
                const int hit = prefix_index(n, [&](int i) { return item_title(D.flat[(size_t)i]); }, NV.typed);
                if (hit >= 0) { nav_travel(D, hit, 1.f); NV.jump_sel = section_index(NV.secs, hit); }
                else if (NV.typed.size() > 1) NV.typed.pop_back();   // no such title: keep the last good prefix
            }
        } else if (pr(ImGuiKey_J) && !typing) { nav_close_all(); return; }
        if (sel != NV.jump_sel) {
            NV.jump_sel = sel;
            nav_travel(D, NV.secs[(size_t)sel].first, 1.f);
            shot_trace("cases-nav", "jump-to " + NV.secs[(size_t)sel].label);
        }
        NV.fast_t = std::max(NV.fast_t, 0.3f);
        if (pr(ImGuiKey_Enter) || pr(ImGuiKey_KeypadEnter) || padk(ImGuiKey_GamepadFaceDown)) nav_close_all();
        return;
    }
    if (typing || !act) return;
    // ---- browse mode shortcuts ----
    if (pr(ImGuiKey_O) && !io.KeyCtrl) nav_open_sort();
    else if (pr(ImGuiKey_J) && !io.KeyCtrl) nav_open_jump(D);
    else if (CS.mode == Mode::Browse && (padk(ImGuiKey_GamepadRStickLeft) || padk(ImGuiKey_GamepadRStickRight))) nav_open_sort();
    else if (CS.mode == Mode::Browse && (padk(ImGuiKey_GamepadRStickUp) || padk(ImGuiKey_GamepadRStickDown))) nav_open_jump(D);
    int sd = 0;
    if (pr(ImGuiKey_PageUp, true) || padk(ImGuiKey_GamepadL2, true)) sd = -1;
    else if (pr(ImGuiKey_PageDown, true) || padk(ImGuiKey_GamepadR2, true)) sd = 1;
    if (sd) { nav_travel(D, section_step(NV.secs, CS.focus, sd, n), 0.9f); shot_trace("cases-nav", sd > 0 ? "section-next" : "section-prev"); }
    if (pr(ImGuiKey_Home)) nav_travel(D, 0, 0.9f);
    if (pr(ImGuiKey_End)) nav_travel(D, n - 1, 0.9f);
}

/* ---- drawing ---- */
static void nav_text_c(ImDrawList* dl, ImFont* f, float fs, ImVec2 c, ImU32 col, const char* t) {
    const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.f, t);
    dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, t);
}

/* The Sort chip at the right end of the top bar. Returns its rect through p0 / p1 (for the picker's anchor). */
void nav_draw_chip(ImDrawList* dl, ImVec2 org, float W, float y, float h, ImVec2* p0, ImVec2* p1) {
    ImFont* f = ui::F.bold ? ui::F.bold : ui::F.body;
    const float fs = f->FontSize, pad = sp(1.3f), caret = sp(1.4f);
    const float maxw = nav_side_w() - sp(1.4f) - sp(0.8f);
    std::string full = "Sort: " + hubui::sort_label(nav_spec());
    if (f->CalcTextSizeA(fs, FLT_MAX, 0.f, full.c_str()).x > maxw - pad * 2.f - caret) full = hubui::sort_label(nav_spec());   // too long: drop "Sort: "
    std::string txt = ui::ellipsize(f, fs, full, maxw - pad * 2.f - caret);
    const float tw = f->CalcTextSizeA(fs, FLT_MAX, 0.f, txt.c_str()).x;
    const float w = std::min(maxw, tw + pad * 2.f + caret);
    *p1 = ImVec2(org.x + W - sp(1.4f), y + h);
    *p0 = ImVec2(p1->x - w, y);
    ImGui::PushID("casesortchip");
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);   // the pad reaches it with the right stick
    ImGui::SetCursorScreenPos(*p0);
    const bool pressed = ImGui::InvisibleButton("s", *p1 - *p0);
    ImGui::PopItemFlag();
    const bool hov = ImGui::IsItemHovered();
    const bool on = NV.sort_open;
    dl->AddRectFilled(ImVec2(p0->x + 2.f, p0->y + 4.f), ImVec2(p1->x + 2.f, p1->y + 4.f), IM_COL32(0, 0, 0, 70), h * 0.5f);
    dl->AddRectFilled(*p0, *p1, ui::with_alpha(ui::PANEL, hov || on ? 0.95f : 0.84f), h * 0.5f);
    dl->AddRect(*p0, *p1, on ? ui::ACCENT : ui::HAIRLINE, h * 0.5f, 0, 1.f);
    const float ty = (p0->y + p1->y - f->CalcTextSizeA(fs, FLT_MAX, 0.f, txt.c_str()).y) * 0.5f;
    dl->AddText(f, fs, ImVec2(p0->x + pad, ty), hov || on ? ui::TEXT : ui::TEXT2, txt.c_str());
    const ImVec2 cc(p1->x - pad - caret * 0.35f, (p0->y + p1->y) * 0.5f);
    const float cs = caret * 0.32f;
    dl->AddTriangleFilled(ImVec2(cc.x - cs, cc.y - cs * 0.5f), ImVec2(cc.x + cs, cc.y - cs * 0.5f), ImVec2(cc.x, cc.y + cs * 0.7f), hov || on ? ui::TEXT : ui::TEXT2);
    if (hov && !on) ImGui::SetTooltip("Sort order  (O, or the right stick left / right)");
    if (pressed) { if (NV.sort_open) nav_close_all(); else nav_open_sort(); }
    ImGui::PopID();
}

void nav_draw_sort_picker(ImDrawList* dl, ImVec2 org, ImVec2 sz, ImVec2 chip0, ImVec2 chip1) {
    using namespace hubui;
    using namespace hubui::cases;
    ui::anim(NV.sort_t, NV.sort_open ? 1.f : 0.f, 16.f);
    if (reduced_motion()) NV.sort_t = NV.sort_open ? 1.f : 0.f;
    if (NV.sort_t < 0.01f) return;
    const float t = NV.sort_t;
    ImFont* f = ui::F.body; ImFont* fb = ui::F.bold ? ui::F.bold : f; ImFont* cf = ui::F.cap;
    const float rh = sp(3.4f), pw = sp(28.f), pad = sp(1.2f), head = sp(3.f), foot = sp(2.6f), gap = sp(0.6f);
    const float ph = head + rh * (float)kSortRows + gap + foot + pad;
    ImVec2 q0(chip1.x - pw, chip1.y + sp(0.7f) - (1.f - t) * sp(1.f));
    q0.x = std::max(org.x + sp(1.f), q0.x);
    const ImVec2 q1(q0.x + pw, q0.y + ph);
    dl->AddRectFilled(ImVec2(q0.x + 3.f, q0.y + 6.f), ImVec2(q1.x + 3.f, q1.y + 6.f), IM_COL32(0, 0, 0, (int)(110.f * t)), sp(1.f));
    dl->AddRectFilled(q0, q1, ui::with_alpha(ui::PANEL, 0.97f * t), sp(1.f));
    dl->AddRect(q0, q1, ui::scale_alpha(ui::HAIRLINE, t), sp(1.f), 0, 1.f);
    dl->AddText(cf, cf->FontSize, ImVec2(q0.x + pad + sp(0.5f), q0.y + sp(1.f)), ui::scale_alpha(ui::TEXT2, t), "SORT BY");
    const SortSpec cur = nav_spec();
    ImGuiIO& io = ImGui::GetIO();
    bool inside = io.MousePos.x >= q0.x && io.MousePos.x <= q1.x && io.MousePos.y >= q0.y && io.MousePos.y <= q1.y;
    ImGui::PushID("casesortpick");
    for (int r = 0; r < kSortRows; ++r) {
        const float ry = q0.y + head + rh * (float)r + (r >= kSortModes ? gap : 0.f);
        const ImVec2 r0(q0.x + pad * 0.5f, ry), r1(q1.x - pad * 0.5f, ry + rh);
        ImGui::PushID(r);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        ImGui::SetCursorScreenPos(r0);
        const bool clicked = ImGui::InvisibleButton("r", r1 - r0);
        ImGui::PopItemFlag();
        if (ImGui::IsItemHovered() && io.MouseDelta.x * io.MouseDelta.x + io.MouseDelta.y * io.MouseDelta.y > 0.f) NV.sort_row = r;
        const bool sel = NV.sort_row == r, on = sort_row_on(cur, r);
        if (sel) { dl->AddRectFilled(r0, r1, ui::scale_alpha(ui::FRAME_HOV, t), rh * 0.3f); dl->AddRect(r0, r1, ui::scale_alpha(ui::ACCENT, t * 0.8f), rh * 0.3f, 0, 1.5f); }
        const ImVec2 mc(r0.x + sp(1.6f), (r0.y + r1.y) * 0.5f);
        if (r < kSortModes) {   // radio
            dl->AddCircle(mc, sp(0.6f), ui::scale_alpha(on ? ui::ACCENT : ui::TEXT2, t), 20, 1.5f);
            if (on) dl->AddCircleFilled(mc, sp(0.3f), ui::scale_alpha(ui::ACCENT, t), 16);
        } else {                // check box
            const float b = sp(0.62f);
            dl->AddRect(ImVec2(mc.x - b, mc.y - b), ImVec2(mc.x + b, mc.y + b), ui::scale_alpha(on ? ui::ACCENT : ui::TEXT2, t), sp(0.2f), 0, 1.5f);
            if (on) dl->AddRectFilled(ImVec2(mc.x - b + 3.f, mc.y - b + 3.f), ImVec2(mc.x + b - 3.f, mc.y + b - 3.f), ui::scale_alpha(ui::ACCENT, t), sp(0.1f));
        }
        const char* lab = r < kSortModes ? hubui::sort_name((SortMode)r) : r == kSortModes ? "Reverse order" : "Favorites first";
        ImFont* ff = on ? fb : f;
        dl->AddText(ff, ff->FontSize, ImVec2(r0.x + sp(3.2f), (r0.y + r1.y - ff->FontSize) * 0.5f), ui::scale_alpha(on || sel ? ui::TEXT : ui::TEXT2, t), lab);
        if (clicked) { NV.sort_row = r; nav_apply_sort_row(r); }
        ImGui::PopID();
    }
    ImGui::PopID();
    const char* fl = A.pad_mode ? "A apply    B close" : "Enter apply    Esc close";
    nav_text_c(dl, cf, cf->FontSize, ImVec2((q0.x + q1.x) * 0.5f, q1.y - foot * 0.5f - pad * 0.3f), ui::scale_alpha(ui::TEXT2, t), fl);
    // a click anywhere else closes it (the chip itself toggles)
    const bool on_chip = io.MousePos.x >= chip0.x && io.MousePos.x <= chip1.x && io.MousePos.y >= chip0.y && io.MousePos.y <= chip1.y;
    if (NV.sort_open && ImGui::IsMouseClicked(0) && !inside && !on_chip) nav_close_all();
    (void)sz;
}

/* The scrubber: a thin track for the whole list, section marks, the position marker and the count. Click or drag to scrub. */
void nav_draw_scrub(ImDrawList* dl, ImVec2 org, float W, float y, float h, Disp& D) {
    using namespace hubui::cases;
    const int n = (int)D.flat.size();
    if (n == 0) return;
    ImFont* cf = ui::F.cap; ImFont* bf = ui::F.bold ? ui::F.bold : ui::F.body;
    const std::string cnt = count_text(CS.focus, n);
    const float cnt_w = bf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, count_text(n - 1, n).c_str()).x;
    const float mx = sp(1.4f);
    const float x0 = org.x + mx + cnt_w + sp(1.6f), x1 = org.x + W - mx;
    if (x1 - x0 < sp(10.f)) return;
    const float ty = y + h * 0.74f, th = sp(0.5f);
    // backing plate
    dl->AddRectFilled(ImVec2(org.x + mx - sp(0.8f), y), ImVec2(x1 + sp(0.8f), y + h), ui::with_alpha(ui::PANEL, 0.62f), h * 0.3f);
    dl->AddText(bf, cf->FontSize, ImVec2(org.x + mx, y + (h - cf->FontSize) * 0.5f), ui::TEXT, cnt.c_str());
    dl->AddRectFilled(ImVec2(x0, ty - th * 0.5f), ImVec2(x1, ty + th * 0.5f), ui::with_alpha(ui::TEXT2, 0.28f), th);
    const int cur = CS.focus >= 0 ? CS.focus : 0;
    const int ks = section_index(NV.secs, cur);
    if (ks >= 0 && ks < (int)NV.secs.size()) {   // the current section's span
        const NavSection& s = NV.secs[(size_t)ks];
        const float a = scrub_x(s.first, n, x0, x1), b = std::max(a + 3.f, scrub_x(s.first + s.count - 1, n, x0, x1));
        dl->AddRectFilled(ImVec2(a, ty - th * 0.5f - 1.f), ImVec2(b, ty + th * 0.5f + 1.f), ui::with_alpha(CS.tint0 ? CS.tint0 : ui::ACCENT, 0.9f), th);
    }
    // marks: a tick for every section start, a label where there is room
    float next_free = x0;
    const float lfs = cf->FontSize;
    for (size_t k = 0; k < NV.secs.size(); ++k) {
        const NavSection& s = NV.secs[k];
        const float x = scrub_x(s.first, n, x0, x1);
        const bool here = (int)k == ks;
        dl->AddLine(ImVec2(x, ty - th * 0.5f - sp(0.3f)), ImVec2(x, ty + th * 0.5f + sp(0.3f)), ui::with_alpha(here ? ui::TEXT : ui::TEXT2, here ? 0.9f : 0.5f), 1.f);
        const ImVec2 ls = cf->CalcTextSizeA(lfs, FLT_MAX, 0.f, s.label.c_str());
        if (x + 2.f >= next_free && x + 2.f + ls.x <= x1) {
            dl->AddText(cf, lfs, ImVec2(x + 2.f, y + sp(0.35f)), here ? ui::TEXT : ui::TEXT2, s.label.c_str());
            next_free = x + 2.f + ls.x + sp(0.7f);
        } else if (here) {   // the current section is always named, even where it overlaps a neighbour
            dl->AddText(cf, lfs, ImVec2(std::min(x + 2.f, x1 - ls.x), y + sp(0.35f)), ui::TEXT, s.label.c_str());
        }
    }
    // marker
    const float fx = x0 + (n > 1 ? std::max(0.f, std::min((float)(n - 1), CS.pos.x)) / (float)(n - 1) : 0.f) * (x1 - x0);
    const float mw = sp(0.45f), mh = sp(1.15f);
    dl->AddRectFilled(ImVec2(fx - mw, ty - mh), ImVec2(fx + mw, ty + mh), IM_COL32_WHITE, mw);
    dl->AddRect(ImVec2(fx - mw, ty - mh), ImVec2(fx + mw, ty + mh), ui::with_alpha(CS.tint0 ? CS.tint0 : ui::ACCENT, 1.f), mw, 0, 1.5f);
    // mouse
    ImGui::PushID("casescrub");
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    const ImVec2 b0(x0 - sp(0.8f), y), b1(x1 + sp(0.8f), y + h);
    ImGui::SetCursorScreenPos(b0);
    ImGui::InvisibleButton("t", b1 - b0);
    ImGui::PopItemFlag();
    const bool active = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    const float fr = (io.MousePos.x - x0) / std::max(1.f, x1 - x0);
    if (active && !NV.sort_open && !NV.jump_open) {
        NV.scrubbing = true;
        const int idx = scrub_index(fr, n);
        if (idx != CS.focus) { cases_goto(D, idx, true, true); shot_trace("cases-nav", "scrub " + std::to_string(idx)); }
        NV.fast_t = std::max(NV.fast_t, 0.35f);
    } else NV.scrubbing = false;
    if (hov && !active && !nav_blocking()) {
        const int idx = scrub_index(fr, n);
        const int k = section_index(NV.secs, idx);
        ImGui::SetTooltip("%s  -  game %d of %d", k >= 0 ? NV.secs[(size_t)k].label.c_str() : "", idx + 1, n);
    }
    ImGui::PopID();
}

/* Quick-jump overlay: a rail of the sections around the chosen one. */
void nav_draw_jump(ImDrawList* dl, ImVec2 org, float W, float rail_bottom, Disp& D) {
    using namespace hubui::cases;
    ui::anim(NV.jump_t, NV.jump_open ? 1.f : 0.f, 16.f);
    if (reduced_motion()) NV.jump_t = NV.jump_open ? 1.f : 0.f;
    if (NV.jump_t < 0.01f || NV.secs.empty()) return;
    const float t = NV.jump_t;
    ui::anim(NV.jump_f, (float)NV.jump_sel, 14.f);
    if (reduced_motion()) NV.jump_f = (float)NV.jump_sel;
    ImFont* tf = ui::F.h2 ? ui::F.h2 : ui::F.body; ImFont* cf = ui::F.cap;
    const float ph = sp(9.4f), pw = std::min(W - sp(4.f), sp(86.f));
    const float cx = org.x + W * 0.5f;
    const ImVec2 q0(cx - pw * 0.5f, rail_bottom - ph - sp(0.5f) + (1.f - t) * sp(1.f)), q1(cx + pw * 0.5f, q0.y + ph);
    dl->AddRectFilled(ImVec2(q0.x + 3.f, q0.y + 6.f), ImVec2(q1.x + 3.f, q1.y + 6.f), IM_COL32(0, 0, 0, (int)(110.f * t)), sp(1.2f));
    dl->AddRectFilled(q0, q1, ui::with_alpha(ui::PANEL, 0.95f * t), sp(1.2f));
    dl->AddRect(q0, q1, ui::scale_alpha(ui::HAIRLINE, t), sp(1.2f), 0, 1.f);
    char head[96];
    std::snprintf(head, sizeof head, "JUMP TO %s", NV.kind == NavKind::Letters ? "LETTER" : NV.kind == NavKind::Years ? "YEAR" : NV.kind == NavKind::Consoles ? "CONSOLE" : "SECTION");
    dl->AddText(cf, cf->FontSize, ImVec2(q0.x + sp(1.4f), q0.y + sp(0.9f)), ui::scale_alpha(ui::TEXT2, t), head);
    if (!NV.typed.empty()) {
        std::string ty = "typing: " + NV.typed;
        const ImVec2 ts = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, ty.c_str());
        dl->AddText(cf, cf->FontSize, ImVec2(q1.x - sp(1.4f) - ts.x, q0.y + sp(0.9f)), ui::scale_alpha(ui::ACCENT, t), ty.c_str());
    }
    // the rail
    float maxl = 0.f;
    for (const NavSection& s : NV.secs) maxl = std::max(maxl, tf->CalcTextSizeA(tf->FontSize, FLT_MAX, 0.f, s.label.c_str()).x);
    const float step = std::max(sp(4.4f), maxl + sp(2.2f));
    const float cy = q0.y + sp(4.8f);
    ImU32 acc = CS.tint0 ? CS.tint0 : ui::ACCENT;
    dl->PushClipRect(ImVec2(q0.x + sp(0.6f), q0.y), ImVec2(q1.x - sp(0.6f), q1.y), true);
    const int a = std::max(0, (int)std::floor(NV.jump_f - pw / step * 0.5f) - 1), b = std::min((int)NV.secs.size() - 1, (int)std::ceil(NV.jump_f + pw / step * 0.5f) + 1);
    for (int k = a; k <= b; ++k) {
        const float d = (float)k - NV.jump_f, ad = std::fabs(d);
        const float x = cx + d * step + (d > 0.f ? 1.f : d < 0.f ? -1.f : 0.f) * std::min(ad, 1.f) * step * 0.25f;
        const float edge = 1.f - smoothstep01((std::fabs(x - cx) - pw * 0.30f) / (pw * 0.18f));
        if (edge <= 0.01f) continue;
        const float k1 = std::max(0.f, 1.f - ad);   // 1 on the chosen one
        const std::string& lab = NV.secs[(size_t)k].label;
        const float fs = tf->FontSize * (1.f + 0.35f * k1);
        const ImVec2 ts = tf->CalcTextSizeA(fs, FLT_MAX, 0.f, lab.c_str());
        if (k1 > 0.02f) dl->AddRectFilled(ImVec2(x - ts.x * 0.5f - sp(1.1f), cy - fs * 0.62f), ImVec2(x + ts.x * 0.5f + sp(1.1f), cy + fs * 0.62f), ui::scale_alpha(acc, t * k1), fs * 0.3f);
        dl->AddText(tf, fs, ImVec2(x - ts.x * 0.5f, cy - ts.y * 0.5f), ui::scale_alpha(k1 > 0.5f ? IM_COL32_WHITE : ui::TEXT2, t * edge * (0.45f + 0.55f * std::max(k1, 1.f - std::min(ad / 5.f, 1.f)))), lab.c_str());
        // click a name to go there
        if (ImGui::IsMouseClicked(0)) {
            const ImGuiIO& io = ImGui::GetIO();
            if (std::fabs(io.MousePos.x - x) < step * 0.5f && std::fabs(io.MousePos.y - cy) < fs) { NV.jump_sel = k; nav_travel(D, NV.secs[(size_t)k].first, 1.f); }
        }
    }
    dl->PopClipRect();
    const char* fl = A.pad_mode ? "Left / Right choose    A keep    B go back" : (NV.kind == NavKind::Letters ? "Left / Right, or type a title    Enter keep    Esc go back" : "Left / Right choose    Enter keep    Esc go back");
    nav_text_c(dl, cf, cf->FontSize, ImVec2(cx, q1.y - sp(1.4f)), ui::scale_alpha(ui::TEXT2, t), fl);
}

/* The big letter / year / console over the centre while travelling fast. */
void nav_draw_bubble(ImDrawList* dl, ImVec2 centre, float case_h, int n) {
    if (n == 0 || NV.secs.empty()) return;
    const bool want = NV.fast_t > 0.f || NV.scrubbing || NV.jump_open || NV.force_bubble;
    ui::anim(NV.bub, want ? 1.f : 0.f, want ? 20.f : 6.f);
    if (reduced_motion()) NV.bub = want ? 1.f : 0.f;
    if (NV.bub < 0.02f) return;
    const int p = std::max(0, std::min(n - 1, (int)std::lround(CS.pos.x)));
    const int k = hubui::cases::section_index(NV.secs, p);
    if (k < 0) return;
    const std::string& lab = NV.secs[(size_t)k].label;
    ImFont* f = ui::F.title ? ui::F.title : ui::F.h2;
    const float fs = std::max(f->FontSize * 1.9f, std::min(case_h * 0.42f, f->FontSize * 3.2f));
    const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.f, lab.c_str());
    const float padx = fs * 0.55f, pady = fs * 0.28f;
    const ImVec2 q0(centre.x - ts.x * 0.5f - padx, centre.y - ts.y * 0.5f - pady), q1(centre.x + ts.x * 0.5f + padx, centre.y + ts.y * 0.5f + pady);
    const float a = NV.bub;
    dl->AddRectFilled(q0, q1, IM_COL32(8, 10, 18, (int)(196.f * a)), fs * 0.3f);
    dl->AddRect(q0, q1, ui::scale_alpha(CS.tint0 ? CS.tint0 : ui::ACCENT, a), fs * 0.3f, 0, 2.f);
    dl->AddText(f, fs, ImVec2(centre.x - ts.x * 0.5f, centre.y - ts.y * 0.5f), ui::scale_alpha(IM_COL32_WHITE, a), lab.c_str());
}

/* One call from draw_cases, after the hint bar: the chip, the strip, the overlays, the bubble. */
void nav_draw(ImDrawList* dl, ImVec2 org, ImVec2 sz, float tab_y, float tab_h, float strip_y, float strip_h, ImVec2 stage_centre, float case_h, Disp& D) {
    ImVec2 c0, c1;
    nav_draw_chip(dl, org, sz.x, tab_y, tab_h, &c0, &c1);
    nav_draw_scrub(dl, org, sz.x, strip_y, strip_h, D);
    nav_draw_bubble(dl, stage_centre, case_h, (int)D.flat.size());
    nav_draw_jump(dl, org, sz.x, strip_y, D);
    nav_draw_sort_picker(dl, org, sz, c0, c1);
}

/* ---- screenshot aids: HUB_SHOT_CASES = <name>[:<sort key>[+rev][+favs]] with name sortpicker | quickjump | bubble | sort ---- */
void nav_shot_seed(const std::string& mode) {   // before the first frame: the sort to show
    const size_t c = mode.find(':');
    if (c == std::string::npos || mode.rfind("cartridge", 0) == 0) return;
    std::string spec = mode.substr(c + 1), key = spec;
    A.st.library_sort_rev = spec.find("+rev") != std::string::npos;
    A.st.library_favs_first = spec.find("+favs") != std::string::npos;
    const size_t pl = spec.find('+');
    if (pl != std::string::npos) key = spec.substr(0, pl);
    A.st.library_sort = hubui::sort_key(hubui::sort_from_key(key));
    A.sort_mode = hubui::sort_from_key(A.st.library_sort);
    A.vis_dirty = true;
}
void nav_shot_apply(const std::string& mode, Disp& D) {   // once the list is up
    nav_refresh(D);
    const std::string name = mode.substr(0, mode.find(':'));
    if (name == "sortpicker") { nav_open_sort(); NV.sort_row = 1; NV.sort_t = 1.f; }
    else if (name == "quickjump") { nav_open_jump(D); NV.jump_t = 1.f; NV.jump_f = (float)NV.jump_sel; }
    else if (name == "bubble") { NV.force_bubble = true; NV.bub = 1.f; }
}
