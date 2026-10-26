#include "app.h"
#include "online_caps.h"
#include "picker_logic.h"

/* ---------- Room game picker: hero card, search, console chips, virtualised cover-art grid ---------- */

namespace {

struct PickerState {
    char q[64] = "";
    std::string sys;                       // console chip
    std::vector<hubpick::Item> items;
    std::vector<std::string> keys;         // library key of each item (to find its entry again)
    std::vector<hubpick::Section> secs;
    std::string secs_q, secs_sys;
    double built_at = -10;
    size_t built_lib = 0, built_peers = 0;
    std::string built_sel;
    bool secs_dirty = true;
    bool scroll_top = false;
    std::string detail_id;
    bool detail_open = false;
    bool open = false;          // the big picker modal
    std::string auto_code;      // room the picker was already auto-opened for (host, no game yet)
    bool focus_first = false;   // pad: land on the selected tile when it opens
} PK;

const hub::GameEntry* entry_of(size_t item) {
    if (item >= PK.keys.size()) return nullptr;
    return find_entry(PK.keys[item]);
}

void build_items(const std::vector<hubui::PeerInfo>& peers) {
    PK.items.clear();
    PK.keys.clear();
    std::set<std::string> seen;
    for (const hub::GameEntry& e : A.lib) {
        std::string rid = room_id(e);
        if (!seen.insert(rid).second) continue;
        hubpick::Item it;
        it.id = rid;
        it.title = e.title;
        it.sys = sys_short(e);
        it.sys_name = hub::system_name(e.system);
        it.pinned = is_native(e);
        it.min_players = 1;
        it.max_players = local_players_for(e);
        it.supported = supports_online(e);
        if (!it.supported) {
            it.selectable = false;
            it.reason = hubcaps::online_unsupported_reason(e.system);
        } else {
            auto sel = hubui::game_selectable(rid, max_players_for(e), peers, 0);
            it.selectable = sel.ok;
            it.reason = sel.reason;
            it.untested = online_untested(e);
        }
        const hub::GameState& gs = gstate(e.key);
        it.favorite = gs.favorite;
        it.last_played = gs.last_played;
        PK.items.push_back(it);
        PK.keys.push_back(e.key);
    }
    PK.secs_dirty = true;
}

/* Rebuild when the library, the room, or the half-second clock says so (ownership changes as hellos arrive). */
void refresh_items(const std::vector<hubui::PeerInfo>& peers) {
    double now = ImGui::GetTime();
    if (now - PK.built_at > 0.5 || PK.built_lib != A.lib.size() || PK.built_peers != peers.size() || PK.built_sel != A.selected_game) {
        PK.built_at = now;
        PK.built_lib = A.lib.size();
        PK.built_peers = peers.size();
        PK.built_sel = A.selected_game;
        build_items(peers);
    }
}

void select_game(const hubpick::Item& it) {
    A.selected_game = it.id;
    A.st.last_game = it.id;
    mark_dirty();
    A.lobby.send_msg("{\"kind\":\"select\",\"game\":\"" + json::escape(it.id) + "\"}");
}

std::string status_text(const hubpick::Item& it) {
    if (!it.supported) return it.reason;
    if (!it.selectable) return it.reason;
    return it.untested ? "Untested online" : "Ready for online";
}

void draw_cover(ImDrawList* dl, const hub::GameEntry& e, ImVec2 p0, ImVec2 p1, float rad) {
    TileSpec t = spec_for_entry(e, "cover", false, false);
    if (t.art.tex) ui::draw_art(dl, t.art.tex, t.art.w, t.art.h, p0, p1, rad, 1.f);
    else draw_no_art(dl, t, p0, p1, rad, 1.f);
    if (t.art.loading) ui::shimmer(dl, p0, p1, rad, IM_COL32(0, 0, 0, 0));
    dl->AddRect(p0, p1, ui::HAIRLINE, rad, 0, 1.f);
}

void detail_popup(bool host) {
    if (!PK.detail_open) return;
    const hubpick::Item* it = nullptr;
    size_t idx = 0;
    for (size_t i = 0; i < PK.items.size(); ++i) if (PK.items[i].id == PK.detail_id) { it = &PK.items[i]; idx = i; break; }
    const hub::GameEntry* e = it ? entry_of(idx) : nullptr;
    if (!it || !e) { shot_trace("picker-detail", "missing " + PK.detail_id); PK.detail_open = false; return; }
    ImGui::OpenPopup("##pkdetail");
    ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(sp(64.f), ImGui::GetIO().DisplaySize.x * 0.92f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(2.5f), sp(2.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    bool open = ImGui::BeginPopupModal("##pkdetail", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!open) { PK.detail_open = false; return; }
    if (ui::modal_back()) { PK.detail_open = false; ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float cw = sp(18.f), ch = cw * 1.38f;
    draw_cover(dl, *e, o, o + ImVec2(cw, ch), sp(1.5f));
    float tx = o.x + cw + sp(2.5f), tw = ImGui::GetContentRegionAvail().x - cw - sp(2.5f);
    ImFont* hf = ui::F.title ? ui::F.title : ui::F.h2;
    dl->AddText(hf, hf->FontSize, ImVec2(tx, o.y), ui::TEXT, ui::ellipsize(hf, hf->FontSize, it->title, tw).c_str());
    float y = o.y + hf->FontSize + sp(1.f);
    ImU32 c0, c1;
    entry_colors(*e, &c0, &c1);
    float x = tx;
    x += ui::pill_at(dl, ImVec2(x, y), hub::system_name(e->system), mix(c0, IM_COL32_BLACK, 0.2f)) + sp(0.75f);
    x += ui::pill_at(dl, ImVec2(x, y), hubpick::players_text(it->min_players, it->max_players).c_str(), ui::PANEL2) + sp(0.75f);
    if (it->supported) ui::pill_at(dl, ImVec2(x, y), it->untested ? "Untested online" : "Verified online", it->untested ? IM_COL32(0, 0, 0, 170) : ui::GOOD, it->untested ? ui::WARN : IM_COL32(8, 28, 16, 255));
    else ui::pill_at(dl, ImVec2(x, y), "No online", ui::PANEL2, ui::TEXT2);
    y += ui::F.cap->FontSize + sp(2.5f);
    std::string body;
    if (!it->supported) body = it->reason + ". You can still play it offline from your library.";
    else if (!it->selectable) body = it->reason + ". Everyone in the room needs this exact game.";
    else if (it->untested) body = "This game isn't on the verified online list. It should start for everyone, but it may be single-player only or need accessories that don't work online.";
    else body = "Everyone in the room has this game. The host can pick it.";
    if (it->supported && e->system == hub::System::GC) body += " GameCube online needs the same disc image on every PC.";
    if (it->supported && e->system == hub::System::PS1) body += " PlayStation online needs the same BIOS setup on every PC.";
    for (auto& l : ui::wrap_lines(ImGui::GetFont(), ImGui::GetFontSize(), body, tw, 6)) { dl->AddText(ImVec2(tx, y), ui::TEXT2, l.c_str()); y += ImGui::GetFontSize() + 2 * S; }
    ImGui::Dummy(ImVec2(0, ch - sp(1.f)));
    float bh = sp(5.f);
    if (host && it->usable()) {
        if (ui::button("Choose this game", ImVec2(sp(20.f), bh), true, true, ui::Icon::Check)) { select_game(*it); PK.detail_open = false; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(0, sp(1.f));
        ImGui::SetItemDefaultFocus();
    }
    if (ui::button("Close", ImVec2(sp(12.f), bh))) { PK.detail_open = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

}  // namespace

void room_picker_set_query(const char* q) { std::snprintf(PK.q, sizeof PK.q, "%s", q ? q : ""); }

/* Big "selected game" card at the top of the room: cover, title, console, players and online status. Everyone sees it (guests: the host's pick). */
void room_game_hero(const hub::GameEntry* rg, bool host) {
    const float W = ImGui::GetContentRegionAvail().x, H = 132 * S;
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1 = p0 + ImVec2(W, H);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 c0 = ui::ACCENT, c1 = ui::ACCENT;
    if (rg) entry_colors(*rg, &c0, &c1);
    dl->AddRectFilled(p0, p1, mix(COL_PANEL, c1, 0.14f), 16 * S);
    dl->AddRect(p0, p1, with_alpha(c1, 0.5f), 16 * S, 0, 1.5f * S);
    float ph = H - 20 * S, pw = ph * 0.72f;
    ImVec2 a0 = p0 + ImVec2(10 * S, 10 * S), a1 = a0 + ImVec2(pw, ph);
    float tx = a1.x + 18 * S, tw = p1.x - tx - 16 * S;
    if (host) tw -= std::min(260 * S, W * 0.3f) + 16 * S;   // keep text clear of the Choose/Change game button
    ImFont* hf = ui::F.title ? ui::F.title : ui::F.h2;
    const char* cap = host ? "YOUR PICK" : "HOST'S PICK";
    if (rg) {
        draw_cover(dl, *rg, a0, a1, 10 * S);
        dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, p0.y + 12 * S), ui::TEXT3, cap);
        dl->AddText(hf, hf->FontSize, ImVec2(tx, p0.y + 12 * S + ui::F.cap->FontSize + 4 * S), ui::TEXT, ui::ellipsize(hf, hf->FontSize, rg->title, tw).c_str());
        float y = p0.y + 12 * S + ui::F.cap->FontSize + 4 * S + hf->FontSize + 10 * S, x = tx;
        x += pill_at(dl, ImVec2(x, y), hub::system_name(rg->system), mix(c0, IM_COL32_BLACK, 0.2f)) + 6 * S;
        x += pill_at(dl, ImVec2(x, y), hubpick::players_text(1, local_players_for(*rg)).c_str(), IM_COL32(0, 0, 0, 120)) + 6 * S;
        bool unt = online_untested(*rg);
        pill_at(dl, ImVec2(x, y), unt ? "Untested online" : "Online ready", unt ? IM_COL32(0, 0, 0, 170) : ui::GOOD, unt ? ui::WARN : IM_COL32(8, 28, 16, 255));
        if (!host) dl->AddText(ImVec2(tx, p1.y - 14 * S - ImGui::GetFontSize()), ui::TEXT2, "The host picks the game.");
    } else {
        dl->AddRectFilled(a0, a1, ui::PANEL2, 10 * S);
        dl->AddRect(a0, a1, ui::HAIRLINE, 10 * S, 0, 1.f);
        const char* q = "?";
        ImVec2 qs = hf->CalcTextSizeA(hf->FontSize, FLT_MAX, 0.f, q);
        dl->AddText(hf, hf->FontSize, (a0 + a1 - qs) * 0.5f, ui::TEXT3, q);
        std::string title = "No game chosen yet", sub = host ? "Pick a game below. It shows up here for everyone." : "Waiting for the host to pick a game.";
        if (!A.selected_game.empty()) {
            const GameInfo* g = hub::find_game(A.selected_game);
            title = g ? g->title : A.selected_game;
            sub = "You don't have this game, so you can't ready up for it.";
        }
        dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, p0.y + 12 * S), ui::TEXT3, cap);
        dl->AddText(hf, hf->FontSize, ImVec2(tx, p0.y + 12 * S + ui::F.cap->FontSize + 4 * S), ui::TEXT, ui::ellipsize(hf, hf->FontSize, title, tw).c_str());
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, p0.y + 12 * S + ui::F.cap->FontSize + 4 * S + hf->FontSize + 10 * S), ui::TEXT2, sub.c_str(), nullptr, tw);
    }
    if (host) {   // the host changes the pick in the big picker
        float bw = std::min(260 * S, W * 0.3f), bh = 52 * S;
        ImGui::SetCursorScreenPos(ImVec2(p1.x - bw - 16 * S, p0.y + (H - bh) * 0.5f));
        if (ui::button(rg ? "Change game" : "Choose a game", ImVec2(bw, bh), rg == nullptr, A.proc == nullptr, ui::Icon::Search)) { PK.open = true; PK.focus_first = true; }
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(W, 6 * S));
}

/* The host's picker is a big modal (the room column is too short for a grid); guests only trace their view of the pick. */
static void picker_modal(const std::vector<hubui::PeerInfo>& peers) {
    (void)peers;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::OpenPopup("##pkpick");
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(io.DisplaySize.x * 0.94f, 1500 * S), io.DisplaySize.y * 0.92f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(2.f), sp(1.75f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    bool open = ImGui::BeginPopupModal("##pkpick", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    if (!open) { PK.open = false; return; }
    if (!PK.detail_open && ui::modal_back()) { PK.open = false; ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    const float width = ImGui::GetContentRegionAvail().x;

    // ---- title + close
    ImGui::PushFont(ui::F.title ? ui::F.title : ui::F.h2);
    ImGui::TextUnformatted("Choose a game");
    ImGui::PopFont();
    ImGui::SameLine(width - sp(11.f));
    if (ui::button("Close", ImVec2(sp(11.f), sp(4.5f)))) { PK.open = false; ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    ImGui::Dummy(ImVec2(0, 2 * S));

    // ---- search + count
    ImGui::SetNextItemWidth(std::min(width * 0.5f, 460 * S));
    bool changed = false;
    {
        std::string before = PK.q;
        osk::input("##pksearch", "Search your games...", PK.q, sizeof PK.q);
        changed = before != PK.q;
    }
    int usable = 0, total = (int)PK.items.size();
    for (auto& it : PK.items) if (it.usable()) ++usable;
    ImGui::SameLine(0, 14 * S);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%d of %d games can be played with everyone in the room", usable, total);

    // ---- console chips (counts follow the search)
    std::vector<hubpick::Chip> chips = hubpick::chips(PK.items, PK.q);
    if (!PK.sys.empty() && std::none_of(chips.begin(), chips.end(), [](const hubpick::Chip& c) { return c.sys == PK.sys; })) PK.sys.clear();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp(0.75f), sp(0.4f)));
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
    {
        int all = 0;
        for (auto& c : chips) all += c.count;
        char lab[64];
        std::snprintf(lab, sizeof lab, "All  %d", all);
        const float lim = ImGui::GetCursorScreenPos().x + width;
        if (ui::chip(lab, PK.sys.empty(), COL_ACCENT)) { PK.sys.clear(); PK.scroll_top = true; }
        for (auto& c : chips) {
            std::snprintf(lab, sizeof lab, "%s  %d", c.name.c_str(), c.count);
            float w = ImGui::CalcTextSize(lab).x + sp(3.5f);
            if (ImGui::GetItemRectMax().x + sp(1.f) + w <= lim) ImGui::SameLine();   // otherwise the next chip starts a new line by itself
            ImU32 a = COL_ACCENT, b2;
            system_colors(hub::system_from_short(c.sys), &a, &b2);
            if (ui::chip(lab, PK.sys == c.sys, a)) { PK.sys = PK.sys == c.sys ? std::string() : c.sys; PK.scroll_top = true; }
        }
    }
    ImGui::PopItemFlag();
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, 2 * S));

    if (PK.secs_dirty || changed || PK.secs_q != PK.q || PK.secs_sys != PK.sys) {
        if (PK.secs_q != PK.q || PK.secs_sys != PK.sys) PK.scroll_top = true;
        PK.secs_q = PK.q;
        PK.secs_sys = PK.sys;
        hubpick::Filter f;
        f.query = PK.q;
        f.sys = PK.sys;
        PK.secs = hubpick::build_sections(PK.items, f);
        PK.secs_dirty = false;
    }

    // ---- the grid (virtualised: only the visible rows plus a margin are submitted)
    const float gh = std::max(120 * S, ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeight() - 14 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * S, 8 * S));
    static const float shot_scroll = std::getenv("HUB_SHOT_PICKER_SCROLL") ? (float)std::atof(std::getenv("HUB_SHOT_PICKER_SCROLL")) : -1.f;   // screenshot aid
    static bool shot_scrolled = false;
    if (shot_scroll >= 0 && !shot_scrolled && ImGui::GetFrameCount() > 12) { ImGui::SetNextWindowScroll(ImVec2(0.f, shot_scroll)); shot_scrolled = true; }
    else if (PK.scroll_top) { ImGui::SetNextWindowScroll(ImVec2(0.f, 0.f)); PK.scroll_top = false; }
    ImGui::BeginChild("pickgrid", ImVec2(width, gh), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 base = ImGui::GetCursorPos();
    if (PK.secs.empty()) {
        ImGui::Dummy(ImVec2(0, 30 * S));
        ImGui::TextDisabled(PK.items.empty() ? "No games in your library yet." : "No games match your search.");
    } else {
        const float gap = 12 * S, aw = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize - 4 * S;
        const float tmin = 150 * S;
        int cols = std::max(1, (int)((aw + gap) / (tmin + gap)));
        float tw = (aw - gap * (cols - 1)) / cols, th = tw * 1.22f;
        const float hh = 38 * S, rh = th + gap;
        hubpick::Layout L = hubpick::build_layout(PK.secs, cols, hh, rh);
        int ra, rb;
        hubpick::visible_rows(L, ImGui::GetScrollY(), gh, 2, &ra, &rb);
        bool pad_seed = PK.focus_first && A.pad_mode, sel_visible = false;
        if (pad_seed)
            for (int r = ra; r < rb && !sel_visible; ++r)
                if (!L.rows[r].header)
                    for (int k = 0; k < L.rows[r].count; ++k)
                        if (PK.items[PK.secs[L.rows[r].section].items[L.rows[r].first + k]].id == A.selected_game) sel_visible = true;
        for (int r = ra; r < rb; ++r) {
            const hubpick::Row& row = L.rows[r];
            const hubpick::Section& sec = PK.secs[row.section];
            if (row.header) {
                ImGui::SetCursorPos(ImVec2(base.x, base.y + L.y[r] + 8 * S));
                ImVec2 hp = ImGui::GetCursorScreenPos();
                dl->AddText(ui::F.h2, ui::F.h2->FontSize, hp, ui::TEXT, sec.title.c_str());
                float w = ui::F.h2->CalcTextSizeA(ui::F.h2->FontSize, FLT_MAX, 0.f, sec.title.c_str()).x;
                pill_at(dl, ImVec2(hp.x + w + 10 * S, hp.y + 3 * S), std::to_string(sec.items.size()).c_str(), ui::PANEL2, ui::TEXT2);
                continue;
            }
            for (int k = 0; k < row.count; ++k) {
                int ii = sec.items[row.first + k];
                const hubpick::Item& it = PK.items[ii];
                const hub::GameEntry* e = entry_of(ii);
                if (!e) continue;
                ImGui::SetCursorPos(ImVec2(base.x + k * (tw + gap), base.y + L.y[r]));
                TileSpec t = spec_for_entry(*e, ("pk" + sec.key).c_str(), false, true);
                t.subtitle = hubpick::players_text(it.min_players, it.max_players);
                t.owned = it.usable();
                t.show_star = false;
                t.selected = A.selected_game == it.id;
                t.online = it.supported && !it.untested;
                if (!it.supported) { t.tag = "NO ONLINE"; t.tag_warn = true; t.tag_tip = it.reason; }
                else if (!it.selectable) { t.tag = "MISSING"; t.tag_warn = true; t.tag_tip = it.reason; }
                else if (it.untested) { t.tag = "UNTESTED"; t.tag_warn = false; t.tag_tip = "Untested online: not on the verified list, but it should work. May be single-player only."; }
                t.id += "|" + std::to_string(row.section);
                if (pad_seed && (t.selected || !sel_visible)) { ImGui::SetKeyboardFocusHere(); pad_seed = false; PK.focus_first = false; }
                TileResult tr = draw_tile(t, ImVec2(tw, th));
                bool rc = ImGui::IsItemClicked(1);
                if (ImGui::IsItemHovered() && !it.usable() && !it.reason.empty() && t.tag_tip.empty()) ImGui::SetTooltip("%s", it.reason.c_str());
                bool y_pad = tr.focused && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false);
                if (rc || y_pad || tr.details) { PK.detail_id = it.id; PK.detail_open = true; }
                else if (tr.clicked || tr.activated) {
                    if (it.usable()) { select_game(it); PK.open = false; ImGui::CloseCurrentPopup(); }
                    else set_notice(it.title + ": " + it.reason, 5.0);
                }
            }
        }
        ImGui::SetCursorPos(ImVec2(base.x, base.y + L.total + 4 * S));
        ImGui::Dummy(ImVec2(1, 1));
    }
    ImGui::EndChild();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
    ImGui::TextUnformatted(A.pad_mode ? "A choose    Y details    B close    D-pad move" : "Click to choose    right-click for details    mouse wheel scrolls    Esc closes");
    ImGui::PopStyleColor();
    detail_popup(true);
    ImGui::EndPopup();
}

void room_picker(bool host, float width, const std::vector<hubui::PeerInfo>& peers) {
    (void)width;
    refresh_items(peers);
    for (size_t i = 0; i < PK.items.size(); ++i) {
        const hubpick::Item& it = PK.items[i];
        if (!it.supported) continue;
        bool chosen = A.selected_game == it.id;
        if (host || chosen) shot_trace_once("room-row", std::string(host ? "host " : "guest ") + it.id + (it.selectable ? " ok" : " blocked:" + it.reason) + (chosen ? " chosen" : ""));
    }
    static bool shot_open_done = false;
    if (host && !shot_open_done && std::getenv("HUB_SHOT_PICKER_OPEN") && ImGui::GetFrameCount() > 8) { shot_open_done = true; PK.open = true; PK.focus_first = true; }
    if (const char* dq = std::getenv("HUB_SHOT_PICKER_DETAIL"); host && dq && !PK.detail_open && PK.open && ImGui::GetFrameCount() > 14) { static bool d = false; if (!d) { d = true; PK.detail_id = dq; PK.detail_open = true; } }
    if (!host) { PK.open = false; PK.detail_open = false; return; }
    // A fresh room with no game: open the big picker straight away (once per room), so the room page stays clean.
    if (A.selected_game.empty() && A.proc == nullptr && !A.code.empty() && PK.auto_code != A.code && !PK.items.empty()) { PK.auto_code = A.code; PK.open = true; PK.focus_first = true; }
    if (PK.open) picker_modal(peers);
}
