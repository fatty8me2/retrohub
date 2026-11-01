#include "app.h"
#include "party_logic.h"
#include "party_ui.h"
#include "surprise_logic.h"

/* Party mode drawing (Plan 4 P1-P3): the room page panel, the standings / podium / vote overlay, the tie-break reel and the couch
 * party setup. Flow, messages and rounds live in party_ui.cpp; every button here calls a partyui:: action, which runs the pure
 * reducer (party_logic.h) and broadcasts the State.
 *
 * Overlays obey the after-game rule (popup_guard.h): they open only when prompt_allowed() (no game running, hub focused), close
 * themselves (without being dismissed) when a game starts, and nothing here raises or focuses a window.
 * Pad: every control is a ui:: widget (button / chip / segmented / tile) with nav enabled, in reading order; B closes a modal. */

namespace pl = hubui::party;

namespace {

ImU32 gold() { return IM_COL32(244, 190, 60, 255); }
ImU32 silver() { return IM_COL32(190, 198, 212, 255); }
ImU32 bronze() { return IM_COL32(205, 127, 80, 255); }
ImU32 medal(int rank) { return rank == 1 ? gold() : rank == 2 ? silver() : rank == 3 ? bronze() : ui::PANEL3; }

/* A control the pad can reach: remembers which one had the focus for the headless pad-navigation check (HUB_SHOT_TRACE). */
bool pbutton(const char* label, ImVec2 size = ImVec2(0, 0), bool primary = false, bool enabled = true, ui::Icon ic = ui::Icon::None) {
    const bool pressed = ui::button(label, size, primary, enabled, ic);
    if (ImGui::IsItemFocused()) shot_trace_once("party-focus", label);
    return pressed;
}

/* ui::segmented that also reports (HUB_SHOT_TRACE) when the pad focus is on any of its segments: id "##rounds" traces as "rounds". */
int pseg(const char* id, const char* const* labels, int n, int cur) {
    const int r = ui::segmented(id, labels, n, cur);
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.NavId) {
        ImGui::PushID(id);
        for (int i = 0; i < n; ++i) {   // the same ids ui::segmented gave its segments
            ImGui::PushID(i);
            const ImGuiID sid = ImGui::GetID("seg");
            ImGui::PopID();
            if (sid == g.NavId) shot_trace_once("party-focus", std::string(id[0] == '#' && id[1] == '#' ? id + 2 : id));
        }
        ImGui::PopID();
    }
    return r;
}

void caption(const char* t) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
}

/* A game's cover in [p0,p1]; a coloured stand-in with the initials when the library has no art (or no copy). */
void cover(ImDrawList* dl, const std::string& id, ImVec2 p0, ImVec2 p1, float rad, float alpha = 1.f) {
    const hub::GameEntry* e = partyui::entry_of(id);
    TileSpec ts;
    ts.title = partyui::title_of(id);
    ArtRef art;
    if (e) { entry_colors(*e, &ts.c0, &ts.c1); art = art_for_entry(*e); }
    else game_colors(id, &ts.c0, &ts.c1);
    const bool tiny = p1.y - p0.y < sp(8.f);   // a playlist row thumbnail: no room for the initials
    if (!tiny) ui::shadow(dl, p0, p1, rad, 0.8f * alpha);
    if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, p0, p1, rad, 1.f, alpha);
    else if (tiny) gradient_round_rect(dl, p0, p1, ts.c0, ts.c1, rad);
    else draw_no_art(dl, ts, p0, p1, rad, 1.f);
    dl->AddRect(p0, p1, ui::scale_alpha(ui::HAIRLINE, alpha), rad, 0, 1.f);
}

std::string console_of(const std::string& id) {
    if (const hub::GameEntry* e = partyui::entry_of(id)) return hub::system_name(e->system);
    if (const GameInfo* g = hub::find_game(id)) return hub::system_name(hub::system_from_short(g->system.c_str()));
    return std::string();
}

/* The room's pickers live on the room page: from the overlay (which can be up over any page) go there first, and step the overlay aside. */
void to_room_page() {
    if (!ImGui::IsPopupOpen("##partyov")) return;
    A.page = Page::Online;
    partyui::overlay_close();
}

/* ------------------------------------------------------------------ standings / podium */

void standings_table(const pl::State& s, float w, bool big) {
    const std::vector<pl::Standing> rows = pl::standings(s);
    if (rows.empty()) { text_wrapped_disabled("No points yet."); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rh = big ? sp(5.25f) : sp(3.75f);
    ImFont* nf = big ? (ui::F.h2 ? ui::F.h2 : ui::F.bold) : ui::F.bold;
    const int me = partyui::my_slot();
    for (const pl::Standing& e : rows) {
        const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1 = p0 + ImVec2(w, rh);
        dl->AddRectFilled(p0, p1, e.rank == 1 ? mix(ui::PANEL2, gold(), 0.15f) : ui::PANEL2, sp(1.25f));
        if (e.slot == me && !e.ghost) dl->AddRect(p0, p1, with_alpha(ui::ACCENT, 0.9f), sp(1.25f), 0, 2.f);
        const float r = rh * 0.32f;
        const ImVec2 c(p0.x + sp(1.5f) + r, p0.y + rh * 0.5f);
        dl->AddCircleFilled(c, r, medal(e.rank), 28);
        char rk[8];
        std::snprintf(rk, sizeof rk, "%d", e.rank);
        const ImVec2 rs = ui::F.bold->CalcTextSizeA(ui::F.bold->FontSize, FLT_MAX, 0.f, rk);
        dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(c.x - rs.x * 0.5f, c.y - rs.y * 0.5f), e.rank <= 3 ? IM_COL32(26, 22, 12, 255) : ui::TEXT, rk);
        char pt[16];
        std::snprintf(pt, sizeof pt, "%d", e.points);
        ImFont* pf = ui::F.title ? ui::F.title : ui::F.h2;
        const ImVec2 ps = pf->CalcTextSizeA(pf->FontSize, FLT_MAX, 0.f, pt);
        const float px = p1.x - sp(2.f) - ps.x;
        dl->AddText(pf, pf->FontSize, ImVec2(px, p0.y + (rh - ps.y) * 0.5f), ui::TEXT, pt);
        float right = px - sp(1.25f);
        if (e.gain > 0) {
            char g[12];
            std::snprintf(g, sizeof g, "+%d", e.gain);
            const float gw = ImGui::CalcTextSize(g).x + sp(2.f);
            ui::pill_at(dl, ImVec2(right - gw, p0.y + (rh - ui::F.cap->FontSize - sp(0.75f)) * 0.5f), g, with_alpha(ui::GOOD, 0.9f), IM_COL32(8, 28, 16, 255));
            right -= gw + sp(1.f);
        }
        const float nx = c.x + r + sp(1.5f);
        std::string nm = e.name + (e.ghost ? "  (left)" : "");
        nm = ui::ellipsize(nf, nf->FontSize, nm, std::max(sp(4.f), right - nx));
        dl->AddText(nf, nf->FontSize, ImVec2(nx, p0.y + (rh - nf->FontSize) * 0.5f), e.ghost ? ui::TEXT2 : ui::TEXT, nm.c_str());
        ImGui::Dummy(ImVec2(w, rh + sp(0.5f)));
    }
}

/* The night's end: three blocks (2nd, 1st, 3rd) with name and points above each, then everyone else. */
void podium_view(const pl::State& s, float w) {
    const std::vector<pl::Standing> rows = pl::podium(s);
    if (rows.empty()) { text_wrapped_disabled("Nobody scored."); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = sp(23.f);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(o, o + ImVec2(w, h), mix(ui::PANEL, gold(), 0.06f), sp(1.5f));
    const int order[3] = {1, 0, 2};   // drawn left to right: 2nd, 1st, 3rd (a tie keeps the order and the height)
    const float cw = std::min(sp(20.f), (w - sp(6.f)) / 3.f), gap = sp(1.5f);
    const float total = cw * 3 + gap * 2, x0 = o.x + (w - total) * 0.5f, base = o.y + h - sp(1.25f);
    ImFont* nf = ui::F.h2 ? ui::F.h2 : ui::F.bold;
    ImFont* rf = ui::F.title ? ui::F.title : ui::F.h2;
    for (int k = 0; k < 3; ++k) {
        const int idx = order[k];
        if (idx >= (int)rows.size()) continue;
        const pl::Standing& e = rows[(size_t)idx];
        const float bh = e.rank == 1 ? sp(9.5f) : e.rank == 2 ? sp(7.f) : sp(5.f);
        const float x = x0 + k * (cw + gap);
        const ImVec2 b0(x, base - bh), b1(x + cw, base);
        dl->AddRectFilled(b0, b1, mix(ui::PANEL3, medal(e.rank), 0.6f), sp(1.f), ImDrawFlags_RoundCornersTop);
        char rk[8];
        std::snprintf(rk, sizeof rk, "%d", e.rank);
        const ImVec2 rs = rf->CalcTextSizeA(rf->FontSize, FLT_MAX, 0.f, rk);
        dl->AddText(rf, rf->FontSize, ImVec2(x + (cw - rs.x) * 0.5f, b0.y + (bh - rs.y) * 0.5f), IM_COL32(26, 22, 12, 235), rk);
        char pt[24];
        std::snprintf(pt, sizeof pt, "%d point%s", e.points, e.points == 1 ? "" : "s");
        const ImVec2 ps = ImGui::CalcTextSize(pt);
        dl->AddText(ImVec2(x + (cw - ps.x) * 0.5f, b0.y - sp(1.f) - ps.y), ui::TEXT2, pt);
        const std::string nm = ui::ellipsize(nf, nf->FontSize, e.name, cw);
        const ImVec2 ns = nf->CalcTextSizeA(nf->FontSize, FLT_MAX, 0.f, nm.c_str());
        const float ny = b0.y - sp(1.f) - ps.y - sp(0.5f) - ns.y;
        dl->AddText(nf, nf->FontSize, ImVec2(x + (cw - ns.x) * 0.5f, ny), ui::TEXT, nm.c_str());
        if (e.rank == 1) ui::icon(dl, ui::Icon::StarFilled, ImVec2(x + cw * 0.5f, ny - sp(2.25f)), sp(3.f), gold());
    }
    ImGui::Dummy(ImVec2(w, h + sp(0.75f)));
    for (size_t i = 3; i < rows.size(); ++i) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
        ImGui::Text("%d.  %s  -  %d point%s", rows[i].rank, rows[i].name.c_str(), rows[i].points, rows[i].points == 1 ? "" : "s");
        ImGui::PopStyleColor();
    }
}

/* ------------------------------------------------------------------ polls */

/* "Who won round N?": one big button per player; the host settles a tie. */
void winner_poll(const pl::State& s, float w) {
    const bool host = partyui::is_host();
    const bool couch = partyui::couch();
    const pl::Tally t = pl::tally(s);
    ImGui::PushFont(ui::F.h2);
    ImGui::Text("Who won round %d?", s.round);
    ImGui::PopFont();
    text_wrapped_disabled(couch ? "Press the winner's name. Nobody scores if you skip." : "Everyone votes. The most votes wins 3 points; the host breaks a tie.");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    const std::string mine = partyui::my_vote();
    const int cols = std::max(1, std::min((int)s.cand.size(), 4));
    const float gap = sp(1.f), bw = (w - gap * (cols - 1)) / cols;
    for (size_t i = 0; i < s.cand.size(); ++i) {
        const std::string& c = s.cand[i];
        const int slot = std::atoi(c.c_str());
        int votes = 0;
        for (const auto& r : t.rows) if (r.first == c) votes = r.second;
        if (i % (size_t)cols) ImGui::SameLine(0, gap);
        ImGui::PushID((int)i);
        char lab[128];
        std::snprintf(lab, sizeof lab, "%s###w%d", partyui::slot_name(slot).c_str(), (int)i);   // ### keeps the id (and the pad focus) when the name changes
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool voted = mine == c && !couch;
        if (pbutton(lab, ImVec2(bw, sp(6.f)), voted, true, voted ? ui::Icon::Check : ui::Icon::None)) {
            if (couch) partyui::decide_winner({slot});
            else partyui::cast_vote(c);
        }
        if (!couch && votes > 0) {   // tally pips under the name
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (int k = 0; k < votes; ++k) dl->AddCircleFilled(ImVec2(p.x + sp(1.5f) + k * sp(1.6f), p.y + sp(5.f)), sp(0.6f), voted ? IM_COL32_WHITE : ui::GOOD, 12);
        }
        ImGui::PopID();
    }
    if (!couch) {
        ImGui::Dummy(ImVec2(0, sp(0.25f)));
        ImGui::TextDisabled("%d of %d voted", t.voters, (int)partyui::seats().size());
    }
    if (host) {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        if (t.tied()) {   // a tie: the host decides
            ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
            ImGui::TextUnformatted("It's a tie - you decide:");
            ImGui::PopStyleColor();
            std::vector<int> all;
            for (const std::string& l : t.leaders) {
                const int slot = std::atoi(l.c_str());
                all.push_back(slot);
                ImGui::SameLine(0, sp(1.f));
                if (pbutton(("Give it to " + partyui::slot_name(slot) + "##tie" + l).c_str(), ImVec2(0, sp(4.5f)), true)) partyui::decide_winner({slot});
            }
            ImGui::SameLine(0, sp(1.f));
            if (pbutton("Share the win##tie", ImVec2(0, sp(4.5f)))) partyui::decide_winner(all);
            ImGui::SameLine(0, sp(1.f));
        }
        if (!couch && !t.tied() && t.voters > 0 && pbutton("Settle now##close", ImVec2(0, sp(4.5f)))) partyui::close_vote();
        if (!couch && !t.tied() && t.voters > 0) ImGui::SameLine(0, sp(1.f));
        if (pbutton("Skip - no points##skip", ImVec2(0, sp(4.5f)))) partyui::skip_round();
    }
}

/* "Vote for the next game": a tile per option with its tally. */
void next_poll(const pl::State& s, float w, bool big) {
    const bool host = partyui::is_host();
    const pl::Tally t = pl::tally(s);
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Vote for the next game");
    ImGui::PopFont();
    text_wrapped_disabled("Everyone picks one. Most votes wins; a tie is settled with a spin of the covers.");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    const std::string mine = partyui::my_vote();
    const float gap = sp(1.25f), tmin = big ? sp(17.f) : sp(14.f);
    const int cols = std::max(1, std::min((int)s.cand.size() + 0, (int)((w + gap) / (tmin + gap))));
    const float tw = (w - gap * (cols - 1)) / cols, th = tw * 1.22f;
    for (size_t i = 0; i < s.cand.size(); ++i) {
        const std::string& id = s.cand[i];
        int votes = 0;
        for (const auto& r : t.rows) if (r.first == id) votes = r.second;
        if (i % (size_t)cols) ImGui::SameLine(0, gap);
        ImGui::PushID((int)i);
        const hub::GameEntry* e = partyui::entry_of(id);
        bool clicked = false;
        if (e) {
            TileSpec ts = spec_for_entry(*e, "pv", false, true);
            ts.subtitle = std::to_string(votes) + (votes == 1 ? " vote" : " votes");
            ts.selected = mine == id;
            ts.show_star = false;
            ts.owned = true;
            if (mine == id) { ts.tag = "YOUR VOTE"; ts.tag_accent = true; }
            ts.id += "|pv" + std::to_string(i);
            TileResult tr = draw_tile(ts, ImVec2(tw, th));
            clicked = tr.clicked || tr.activated;
            if (tr.focused) shot_trace_once("party-focus", "vote tile " + id);
        } else {   // no copy here (or a catalog-only id): the stand-in cover with the title and the tally
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            cover(dl, id, p, p + ImVec2(tw, th), sp(1.25f));
            clicked = ImGui::InvisibleButton(("##pvb" + std::to_string(i)).c_str(), ImVec2(tw, th), ImGuiButtonFlags_EnableNav);
            const bool focus = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
            if (mine == id) dl->AddRect(p, p + ImVec2(tw, th), ui::ACCENT, sp(1.25f), 0, 3.f);
            if (focus) { ui::focus_ring(dl, p, p + ImVec2(tw, th), sp(1.25f)); shot_trace_once("party-focus", "vote tile " + id); }
            dl->AddRectFilled(ImVec2(p.x, p.y + th - sp(4.5f)), ImVec2(p.x + tw, p.y + th), IM_COL32(0, 0, 0, 150), sp(1.25f), ImDrawFlags_RoundCornersBottom);
            dl->AddText(ImVec2(p.x + sp(1.f), p.y + th - sp(4.f)), IM_COL32_WHITE, ui::ellipsize(ImGui::GetFont(), ImGui::GetFontSize(), partyui::title_of(id), tw - sp(2.f)).c_str());
            dl->AddText(ImVec2(p.x + sp(1.f), p.y + th - sp(2.25f)), ui::TEXT2, (std::to_string(votes) + (votes == 1 ? " vote" : " votes")).c_str());
        }
        if (clicked) partyui::cast_vote(id);
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, sp(0.25f)));
    ImGui::TextDisabled("%d of %d voted", t.voters, (int)partyui::seats().size());
    ImGui::Dummy(ImVec2(0, sp(0.25f)));
    if (pbutton("Another game...##more", ImVec2(0, sp(4.75f)))) { to_room_page(); room_picker_open(PickMode::Vote); }
    if (host) {
        ImGui::SameLine(0, sp(1.f));
        if (pbutton("Pick now##closevote", ImVec2(0, sp(4.75f)), true, t.voters > 0)) partyui::close_vote();
        ImGui::SameLine(0, sp(1.f));
        if (pbutton("Cancel the vote##cancel", ImVec2(0, sp(4.75f)))) { partyui::close_vote(); }
    }
}

/* ------------------------------------------------------------------ next-up card, seats, host buttons */

void seat_dots(float w) {
    const std::vector<partyui::Seat> seats = partyui::seats();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = ImGui::GetCursorScreenPos().x;
    const float y = ImGui::GetCursorScreenPos().y;
    const float lim = x + w;
    for (const partyui::Seat& sd : seats) {
        std::string nm = sd.me ? "You" : sd.name;
        if (sd.host) nm += " (host)";
        const ImVec2 ts = ImGui::CalcTextSize(nm.c_str());
        if (x + sp(2.5f) + ts.x > lim) break;
        const ImVec2 c(x + sp(0.875f), y + ts.y * 0.5f);
        if (sd.ready) { dl->AddCircleFilled(c, sp(0.875f), ui::GOOD, 16); ui::icon(dl, ui::Icon::Check, c, sp(1.4f), IM_COL32(15, 20, 18, 255)); }
        else dl->AddCircle(c, sp(0.8f), ui::TEXT3, 16, 2.f);
        dl->AddText(ImVec2(x + sp(2.25f), y), sd.ready ? ui::TEXT : ui::TEXT2, nm.c_str());
        x += sp(2.25f) + ts.x + sp(1.5f);
    }
    ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight() + sp(0.25f)));
}

/* What is lined up next, with the host's buttons. `compact` = the room panel; otherwise the overlay. */
void next_up(const pl::State& s, float w, bool compact) {
    const bool host = partyui::is_host();
    const bool last_done = s.round >= s.rounds;
    if (last_done || s.cur.empty()) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float ch = compact ? sp(9.f) : sp(12.f), cw = ch * 0.72f;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(o, o + ImVec2(w, ch + sp(2.f)), ui::PANEL2, sp(1.5f));
    cover(dl, s.cur, o + ImVec2(sp(1.f), sp(1.f)), o + ImVec2(sp(1.f) + cw, sp(1.f) + ch), sp(1.f));
    const float tx = o.x + cw + sp(2.5f);
    dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, o.y + sp(1.25f)), ui::TEXT3,
                (std::string("ROUND ") + std::to_string(s.round + 1) + " OF " + std::to_string(s.rounds) + " - UP NEXT").c_str());
    ImFont* hf = ui::F.title ? ui::F.title : ui::F.h2;
    dl->AddText(hf, hf->FontSize, ImVec2(tx, o.y + sp(1.25f) + ui::F.cap->FontSize + sp(0.5f)), ui::TEXT,
                ui::ellipsize(hf, hf->FontSize, partyui::title_of(s.cur), w - cw - sp(4.f)).c_str());
    const std::string con = console_of(s.cur);
    if (!con.empty()) pill_at(dl, ImVec2(tx, o.y + sp(1.25f) + ui::F.cap->FontSize + sp(0.5f) + hf->FontSize + sp(1.f)), con.c_str(), ui::PANEL3, ui::TEXT2);
    ImGui::Dummy(ImVec2(w, ch + sp(2.f)));
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (!host) {
        ImGui::TextDisabled("Waiting for the host to start the round.");
        seat_dots(w);
        return;
    }
    std::string why;
    const bool can = partyui::host_can_start_round(&why);
    char lab[64];
    std::snprintf(lab, sizeof lab, "Start round %d##start", s.round + 1);
    if (pbutton(lab, ImVec2(sp(24.f), sp(6.f)), true, can, ui::Icon::Play)) partyui::launch_next_round();
    if (compact) ImGui::SetItemDefaultFocus();
    ImGui::SameLine(0, sp(1.f));
    if (!partyui::couch()) {
        ImGui::SameLine(0, sp(1.f));
        if (pbutton("Change game##chg", ImVec2(0, sp(6.f)), false, s.poll.empty())) { to_room_page(); room_picker_open(PickMode::Choose); }
        ImGui::SameLine(0, sp(1.f));
        if (pbutton("Vote for it##vote", ImVec2(0, sp(6.f)), false, s.poll.empty())) partyui::open_next_vote();
    }
    if (!can && !why.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN); ImGui::TextWrapped("%s", why.c_str()); ImGui::PopStyleColor(); }
    if (!partyui::couch()) seat_dots(w);
}

/* The Surprise-me style reel for a tie: the covers of the tied options spin and land on the seeded pick. */
bool reel_card(float w) {
    const partyui::Reel& R = partyui::reel();
    if (!R.on || R.tied.empty()) return false;
    namespace su = hubui::surprise;
    constexpr double kSpin = 2.7, kHold = 1.4;
    constexpr int kLen = 22;
    const double now = ImGui::GetTime(), t = R.frozen >= 0 ? R.frozen : now - R.t0;
    if (R.frozen < 0 && t > kSpin + kHold) { partyui::reel_done(); return false; }
    int target = 0;
    for (size_t i = 0; i < R.tied.size(); ++i) if (R.tied[i] == R.pick) target = (int)i;
    uint32_t seed = 2166136261u;
    for (char ch : R.pick) seed = (seed ^ (uint8_t)ch) * 16777619u;
    const std::vector<int> reel = su::make_reel((int)R.tied.size(), target, kLen, seed);
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("It's a tie - spinning the covers...");
    ImGui::PopFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cw = std::min(sp(13.f), w / 4.2f), ch = cw * 1.25f, gap = sp(1.5f), vh = ch * 1.3f;
    const ImVec2 v0 = ImGui::GetCursorScreenPos(), v1(v0.x + w, v0.y + vh);
    const float cx = (v0.x + v1.x) * 0.5f, cy = (v0.y + v1.y) * 0.5f;
    dl->AddRectFilled(v0, v1, ui::BG, sp(1.5f));
    dl->PushClipRect(v0, v1, true);
    const float pos = t >= kSpin ? (float)(reel.size() - 1) : su::spin_pos(t, kSpin, (int)reel.size());
    const int first = std::max(0, (int)std::floor(pos - w / (cw + gap) * 0.5f) - 1);
    const int last = std::min((int)reel.size() - 1, (int)std::ceil(pos + w / (cw + gap) * 0.5f) + 1);
    for (int i = first; i <= last; ++i) {
        const float d = (float)i - pos, near_ = std::max(0.f, 1.f - std::fabs(d)), sc = 1.f + 0.28f * near_;
        const float x = cx + d * (cw + gap);
        const ImVec2 p0(x - cw * sc * 0.5f, cy - ch * sc * 0.5f), p1(x + cw * sc * 0.5f, cy + ch * sc * 0.5f);
        const int ci = reel[(size_t)i];
        if (ci < 0 || ci >= (int)R.tied.size()) continue;
        cover(dl, R.tied[(size_t)ci], p0, p1, sp(1.25f), 0.55f + 0.45f * near_);
    }
    dl->PopClipRect();
    dl->AddTriangleFilled(ImVec2(cx - sp(1.f), v1.y + sp(0.25f)), ImVec2(cx + sp(1.f), v1.y + sp(0.25f)), ImVec2(cx, v1.y - sp(0.75f)), ui::ACCENT);
    dl->AddRect(v0, v1, ui::HAIRLINE, sp(1.5f), 0, 1.f);
    ImGui::Dummy(ImVec2(w, vh + sp(0.75f)));
    if (t >= kSpin) {
        ImGui::PushFont(ui::F.h2);
        ImGui::Text("%s wins!", partyui::title_of(R.pick).c_str());
        ImGui::PopFont();
    }
    return true;
}

/* ------------------------------------------------------------------ host setup (Lobby) */

constexpr int kRoundChoices[] = {3, 5, 7, 10};
constexpr int kTimerChoices[] = {0, 300, 600, 900};

void setup_view(const pl::State& s, float w) {
    const bool host = partyui::is_host();
    const float lw = sp(12.f);
    if (!host) {   // a guest sees the plan
        char b[160];
        std::snprintf(b, sizeof b, "%d rounds  -  %s  -  %s", s.rounds, s.scoring == pl::Scoring::Auto ? "points from the game's result, else a vote" : s.scoring == pl::Scoring::Vote ? "points by vote" : "no points (co-op)",
                      s.round_secs ? (std::to_string(s.round_secs / 60) + " min rounds").c_str() : "no timer");
        text_wrapped_disabled(b);
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        int n = 0;
        for (const std::string& id : s.list) { ImGui::Text("%d.  %s", ++n, partyui::title_of(id).c_str()); }
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::TextDisabled("The host is getting the night ready.");
        return;
    }
    // rounds
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Rounds");
    ImGui::SameLine(lw);
    {
        static const char* labs[] = {"3", "5", "7", "10"};
        int cur = -1;
        for (int i = 0; i < 4; ++i) if (kRoundChoices[i] == s.rounds) cur = i;
        const int pick = pseg("##rounds", labs, 4, cur);
        if (ImGui::IsItemFocused()) shot_trace_once("party-focus", "rounds");
        if (pick != cur && pick >= 0) partyui::set_options(kRoundChoices[pick], s.scoring, s.round_secs, s.auto_ready);
    }
    // games: shuffle or hand-picked
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Games");
    ImGui::SameLine(lw);
    {
        static const char* labs[] = {"Shuffle", "I'll pick"};
        const int cur = s.shuffle ? 0 : 1;
        const int pick = pseg("##games", labs, 2, cur);
        if (pick != cur) partyui::set_shuffle(pick == 0);
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    // the playlist
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float rh = sp(3.75f);
        for (size_t i = 0; i < s.list.size(); ++i) {
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const bool removable = !s.shuffle;
            const float rw = removable ? w - sp(5.f) : w;
            dl->AddRectFilled(p0, p0 + ImVec2(rw, rh), ui::PANEL2, sp(1.f));
            char n[8];
            std::snprintf(n, sizeof n, "%d", (int)i + 1);
            dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(p0.x + sp(1.25f), p0.y + (rh - ui::F.bold->FontSize) * 0.5f), ui::TEXT3, n);
            const float cth = rh - sp(0.75f);
            cover(dl, s.list[i], ImVec2(p0.x + sp(3.5f), p0.y + sp(0.375f)), ImVec2(p0.x + sp(3.5f) + cth * 0.72f, p0.y + sp(0.375f) + cth), sp(0.5f));
            const float tx = p0.x + sp(3.5f) + cth * 0.72f + sp(1.25f);
            dl->AddText(ImVec2(tx, p0.y + (rh - ImGui::GetFontSize()) * 0.5f), ui::TEXT, ui::ellipsize(ImGui::GetFont(), ImGui::GetFontSize(), partyui::title_of(s.list[i]), rw - (tx - p0.x) - sp(8.f)).c_str());
            const std::string con = console_of(s.list[i]);
            if (!con.empty()) {
                const float cw2 = ImGui::CalcTextSize(con.c_str()).x + sp(2.f);
                pill_at(dl, ImVec2(p0.x + rw - cw2 - sp(1.f), p0.y + (rh - ui::F.cap->FontSize - sp(0.75f)) * 0.5f), con.c_str(), ui::PANEL3, ui::TEXT2);
            }
            ImGui::Dummy(ImVec2(rw, rh));
            if (removable) {
                ImGui::SameLine(0, sp(0.5f));
                ImGui::PushID((int)i);
                if (ui::icon_button("rm", ui::Icon::Close, rh, false, "Remove")) { partyui::remove_game(i); ImGui::PopID(); break; }
                if (ImGui::IsItemFocused()) shot_trace_once("party-focus", "remove game");
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(0, sp(0.25f)));
        }
        if (s.list.empty()) text_wrapped_disabled(s.shuffle ? "There are no games everyone in the room can play yet." : "No games yet - add some below.");
    }
    if (s.shuffle) {
        if (pbutton("Shuffle again##reshuffle", ImVec2(0, sp(4.5f)), false, true, ui::Icon::Refresh)) partyui::reshuffle();
    } else {
        const bool room_left = (int)s.list.size() < s.rounds;
        if (pbutton("Add a game##addgame", ImVec2(0, sp(4.5f)), s.list.empty(), room_left, ui::Icon::Plus)) room_picker_open(PickMode::Playlist);
        ImGui::SameLine(0, sp(1.f));
        if (pbutton("Clear##cleargames", ImVec2(0, sp(4.5f)), false, !s.list.empty())) partyui::clear_games();
        if ((int)s.list.size() < s.rounds && !s.list.empty()) { ImGui::SameLine(0, sp(1.f)); ImGui::TextDisabled("%d games for %d rounds: they take turns", (int)s.list.size(), s.rounds); }
    }
    ImGui::Dummy(ImVec2(0, sp(0.75f)));
    // scoring
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Points");
    ImGui::SameLine(lw);
    {
        static const char* labs[] = {"Auto", "Vote", "None"};
        const int cur = (int)s.scoring;
        const int pick = pseg("##scoring", labs, 3, cur);
        if (pick != cur) partyui::set_options(s.rounds, (pl::Scoring)pick, s.round_secs, s.auto_ready);
    }
    text_wrapped_disabled(s.scoring == pl::Scoring::Auto ? "Smash Classic reports who won. Other games end on the timer (or when you quit), then everyone votes on the winner."
                          : s.scoring == pl::Scoring::Vote ? "Every round ends with a vote on who won."
                                                            : "Co-op: no points. Rounds end on the timer or when you quit.");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    // timer
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Round timer");
    ImGui::SameLine(lw);
    {
        static const char* labs[] = {"Off", "5 min", "10 min", "15 min"};
        int cur = -1;
        for (int i = 0; i < 4; ++i) if (kTimerChoices[i] == s.round_secs) cur = i;
        const int pick = pseg("##timer", labs, 4, cur);
        if (pick != cur && pick >= 0) partyui::set_options(s.rounds, s.scoring, kTimerChoices[pick], s.auto_ready);
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    // auto-ready
    {
        bool ar = s.auto_ready;
        if (ImGui::Checkbox("Ready everyone up between rounds", &ar)) partyui::set_options(s.rounds, s.scoring, s.round_secs, ar);
        if (ImGui::IsItemFocused()) shot_trace_once("party-focus", "auto-ready");
        text_wrapped_disabled("Nobody can press Ready while their game is closed behind the hub, so the next game readies you automatically.");
    }
}

}  // namespace

/* ================================================================== the room page panel */

void party_room_panel(float width) {
    (void)width;
    if (!partyui::active() && !partyui::is_host()) return;   // nothing for a guest while Party mode is off
    if (partyui::couch()) {   // a couch party and a room do not mix
        begin_panel("party");
        section_title("Couch party");
        text_wrapped_disabled("A couch party is still running on this PC. Finish or end it before using Party mode with this room.");
        if (pbutton("End the couch party##endcouch", ImVec2(0, sp(4.5f)))) partyui::turn_off();
        end_panel();
        return;
    }
    const pl::State& s = partyui::state();
    const bool host = partyui::is_host();
    const bool on = partyui::active();
    const float w_in = ImGui::GetContentRegionAvail().x - sp(5.f);
    begin_panel("party");
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted(on && s.phase != pl::Phase::Off ? "Party mode" : (on ? "Vote for the next game" : "Party mode"));
    ImGui::PopFont();
    const std::vector<std::string> old = partyui::need_update();
    if (host && !old.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("%s", pl::needs_update_text(old).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, sp(0.25f)));
    const bool idle = A.proc == nullptr;
    switch (s.phase) {
        case pl::Phase::Off:
            if (!s.poll.empty()) {
                if (s.poll == "next") next_poll(s, w_in, false);
            } else if (host) {
                text_wrapped_disabled("Turn the room into a game night: a playlist of games, points every round, standings in between and a podium at the end. Or just let everyone vote on what to play next.");
                ImGui::Dummy(ImVec2(0, sp(0.5f)));
                if (pbutton("Turn on Party mode##on", ImVec2(0, sp(5.5f)), true, idle, ui::Icon::Star)) partyui::turn_on();
                ImGui::SameLine(0, sp(1.f));
                if (pbutton("Vote for the next game##v", ImVec2(0, sp(5.5f)), false, idle)) partyui::open_next_vote();
            }
            break;
        case pl::Phase::Lobby: {
            setup_view(s, w_in);
            if (host) {
                ImGui::Dummy(ImVec2(0, sp(0.75f)));
                if (pbutton("Start the party##start", ImVec2(sp(26.f), sp(6.f)), true, !s.list.empty() && idle, ui::Icon::Play)) partyui::start();
                ImGui::Dummy(ImVec2(0, sp(0.5f)));   // its own row: the pad's Down reaches Start before Turn off
                if (pbutton("Turn off##off", ImVec2(0, sp(6.f)))) partyui::turn_off();
            }
            break;
        }
        case pl::Phase::Playing: {
            char b[120];
            std::snprintf(b, sizeof b, "Round %d of %d  -  %s", s.round, s.rounds, partyui::title_of(s.cur).c_str());
            ImGui::PushFont(ui::F.bold);
            ImGui::TextUnformatted(b);
            ImGui::PopFont();
            const int left = partyui::secs_left();
            if (left >= 0) ImGui::TextDisabled("%s left in this round", pl::clock_text(left).c_str());
            else ImGui::TextDisabled("The round ends with the game's result, or when the host quits the game.");
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            if (!s.played.empty()) standings_table(s, w_in, false);
            if (host && pbutton("End the party##endparty", ImVec2(0, sp(4.5f)))) partyui::turn_off();
            break;
        }
        case pl::Phase::Intermission: {
            if (s.round > 0) {
                ImGui::PushFont(ui::F.bold);
                ImGui::Text("After round %d of %d", s.round, s.rounds);
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0, sp(0.25f)));
                standings_table(s, w_in, false);
            } else {
                text_wrapped_disabled(s.auto_ready ? "Everyone is readied up automatically. Press Start round 1 when you are all here." : "Everyone presses Ready, then the host starts round 1.");
            }
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            if (s.poll == "winner") winner_poll(s, w_in);
            else if (s.poll == "next") next_poll(s, w_in, false);
            else if (s.round >= s.rounds) {
                text_wrapped_disabled("That was the last round.");
                if (host && pbutton("Show the podium##podium", ImVec2(0, sp(5.f)), true)) partyui::finish_night();
            } else next_up(s, w_in, true);
            if (host && s.poll.empty()) {
                ImGui::Dummy(ImVec2(0, sp(0.5f)));
                if (pbutton("End the party##endparty", ImVec2(0, sp(4.5f)))) partyui::turn_off();
            }
            break;
        }
        case pl::Phase::Done: {
            podium_view(s, w_in);
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            if (pbutton("Show it again##again", ImVec2(0, sp(4.5f)))) partyui::overlay_open();
            if (host) {
                ImGui::SameLine(0, sp(1.f));
                if (pbutton("Close the party##close", ImVec2(0, sp(4.5f)), true)) partyui::turn_off();
            }
            break;
        }
    }
    end_panel();
}

/* ================================================================== overlays */

namespace {

void couch_setup_modal() {
    partyui::CouchSetup& c = partyui::couch_setup();
    if (!c.open) return;
    if (!ImGui::IsPopupOpen("##couchsetup")) ImGui::OpenPopup("##couchsetup");
    const ImGuiIO& io = ImGui::GetIO();
    const float w = std::min(sp(88.f), io.DisplaySize.x * 0.94f);
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, io.DisplaySize.y * 0.94f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(2.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    const bool open = ImGui::BeginPopupModal("##couchsetup", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar(2);
    if (!open) { c.open = false; return; }
    if (!g_headless && game_active()) { c.open = false; ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    if (ui::modal_back()) { c.open = false; ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    const float inner = ImGui::GetContentRegionAvail().x;
    const float lw = sp(13.5f);
    ImGui::PushFont(ui::F.title ? ui::F.title : ui::F.h2);
    ImGui::TextUnformatted("Couch party");
    ImGui::PopFont();
    text_wrapped_disabled("Everyone on this PC takes turns. Players are your controllers: Player 1 is the first one, as on the Controllers page.");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Players");
    ImGui::SameLine(lw);
    {
        static const char* labs[] = {"2", "3", "4"};
        const int pick = pseg("##cplayers", labs, 3, std::max(0, std::min(2, c.players - 2)));
        c.players = 2 + pick;
    }
    ImGui::SameLine(0, sp(1.5f));
    if (pbutton("Assign controllers...##assign", ImVec2(0, sp(5.f)))) { c.open = false; ImGui::CloseCurrentPopup(); ImGui::EndPopup(); open_port_assign(); return; }
    for (int i = 0; i < c.players; ++i) {   // two names per row
        ImGui::PushID(i);
        if (i % 2) ImGui::SameLine(0, sp(2.f));
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Player %d", i + 1);
        ImGui::SameLine(i % 2 ? 0.f : lw);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%s", c.names[i].c_str());
        ImGui::SetNextItemWidth(std::min(sp(20.f), (inner - lw) * 0.5f - sp(8.f)));
        const std::string hint = "Player " + std::to_string(i + 1);
        osk::input("##pname", hint.c_str(), buf, (int)sizeof buf, 0);
        if (ImGui::IsItemFocused()) shot_trace_once("party-focus", "couch name " + std::to_string(i + 1));
        c.names[i] = buf;
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Rounds");
    ImGui::SameLine(lw);
    {
        static const char* labs[] = {"3", "5", "7", "10"};
        int cur = 1;
        for (int i = 0; i < 4; ++i) if (kRoundChoices[i] == c.rounds) cur = i;
        c.rounds = kRoundChoices[pseg("##crounds", labs, 4, cur)];
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Games");
    ImGui::SameLine(lw);
    {
        static const char* labs[] = {"Shuffle", "I'll pick"};
        c.shuffle = pseg("##cgames", labs, 2, c.shuffle ? 0 : 1) == 0;
    }
    const std::vector<std::string> pool = partyui::couch_pool();
    if (c.shuffle) text_wrapped_disabled(std::to_string(pool.size()) + " games in your library have 2 or more players. They are shuffled when the night starts.");
    else {
        for (size_t i = 0; i < pool.size() && i < 24; ++i) {
            ImGui::PushID((int)i);
            bool on = std::find(c.list.begin(), c.list.end(), pool[i]) != c.list.end();
            const bool chip_hit = ui::chip(partyui::title_of(pool[i]).c_str(), on, ui::ACCENT);
            if (ImGui::IsItemFocused()) shot_trace_once("party-focus", "couch game chip");
            if (chip_hit) {
                if (on) c.list.erase(std::find(c.list.begin(), c.list.end(), pool[i]));
                else if ((int)c.list.size() < c.rounds) c.list.push_back(pool[i]);
            }
            if (i + 1 < pool.size()) ImGui::SameLine();
            ImGui::PopID();
        }
        ImGui::NewLine();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Points");
    ImGui::SameLine(lw);
    {
        static const char* labs[] = {"Auto", "Vote", "None"};
        c.scoring = (pl::Scoring)pseg("##cscoring", labs, 3, (int)c.scoring);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Round timer");
    ImGui::SameLine(lw);
    {
        static const char* labs[] = {"Off", "5 min", "10 min", "15 min"};
        int cur = 0;
        for (int i = 0; i < 4; ++i) if (kTimerChoices[i] == c.round_secs) cur = i;
        c.round_secs = kTimerChoices[pseg("##ctimer", labs, 4, cur)];
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    const bool can = c.shuffle ? !pool.empty() : !c.list.empty();
    if (pbutton("Start the night##cstart", ImVec2(sp(24.f), sp(6.f)), true, can, ui::Icon::Play)) { partyui::couch_begin(); ImGui::CloseCurrentPopup(); }
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine(0, sp(1.f));
    if (pbutton("Cancel##ccancel", ImVec2(sp(14.f), sp(6.f)))) { c.open = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

/* The standings / podium / vote card. Returns false when there is nothing to show (the caller closes it). */
bool overlay_body(const pl::State& s, float w) {
    const bool host = partyui::is_host();
    if (reel_card(w)) return true;
    if (s.phase == pl::Phase::Done) {
        ImGui::PushFont(ui::F.title ? ui::F.title : ui::F.h2);
        ImGui::TextUnformatted("The night is over");
        ImGui::PopFont();
        const std::vector<pl::Standing> pod = pl::podium(s);
        if (!pod.empty()) ImGui::TextDisabled("%s takes the night with %d point%s after %d round%s.", pod[0].name.c_str(), pod[0].points, pod[0].points == 1 ? "" : "s",
                                              (int)s.played.size(), s.played.size() == 1 ? "" : "s");
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        podium_view(s, w);
        return true;
    }
    if (s.phase == pl::Phase::Intermission && (s.round > 0 || s.couch)) {
        ImGui::PushFont(ui::F.title ? ui::F.title : ui::F.h2);
        if (s.round > 0) ImGui::Text("Round %d of %d is done", s.round, s.rounds);
        else ImGui::TextUnformatted("Couch party");
        ImGui::PopFont();
        if (s.round > 0) {
            std::string line;
            if (s.poll == "winner") line = std::string(s.end == "timer" ? "Time's up!" : "The game was closed.") + " Nobody reported a winner, so name one.";
            else { line = pl::osd_round_over(s); if (s.end == "timer") line = "Time's up! " + line; }
            ImGui::TextDisabled("%s", line.c_str());
            ImGui::Dummy(ImVec2(0, sp(0.75f)));
            standings_table(s, w, s.poll.empty());   // a poll needs the room: compact rows while one is open
        } else {
            ImGui::TextDisabled("%d players, %d rounds. Everybody ready?", (int)s.points.size(), s.rounds);
        }
        ImGui::Dummy(ImVec2(0, sp(0.75f)));
        if (s.poll == "winner") winner_poll(s, w);
        else if (s.poll == "next") next_poll(s, w, true);
        else if (s.round >= s.rounds) {
            text_wrapped_disabled("That was the last round - the podium comes next.");
            if (host && pbutton("Show the podium##podium", ImVec2(sp(24.f), sp(6.f)), true)) partyui::finish_night();
        } else next_up(s, w, false);
        return true;
    }
    if (s.phase == pl::Phase::Off && s.poll == "next") {
        next_poll(s, w, true);
        return true;
    }
    return false;
}

void overlay_modal() {
    if (!partyui::overlay_wanted()) return;
    const pl::State& s = partyui::state();
    const bool shows = (s.phase == pl::Phase::Done) || (s.phase == pl::Phase::Intermission && (s.round > 0 || s.couch)) || (s.phase == pl::Phase::Off && s.poll == "next") || partyui::reel().on;
    if (!shows) { if (!ImGui::IsPopupOpen("##partyov")) partyui::overlay_close(); }
    const bool already = ImGui::IsPopupOpen("##partyov");
    const bool other = ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) && !already;
    if (!already) {
        if (!shows || other || !prompt_allowed()) return;   // the after-game rule: no game running and the hub already in front
        ImGui::OpenPopup("##partyov");
    }
    const ImGuiIO& io = ImGui::GetIO();
    const float w = std::min(sp(86.f), io.DisplaySize.x * 0.94f);
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, io.DisplaySize.y * 0.94f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(2.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    const bool open = ImGui::BeginPopupModal("##partyov", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar(2);
    if (!open) return;
    if (!g_headless && game_active()) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }   // a game started: step aside (not dismissed - it comes back after the game)
    if (ui::modal_back()) { partyui::overlay_close(); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    const float inner = ImGui::GetContentRegionAvail().x;
    if (!shows || !overlay_body(s, inner)) { partyui::overlay_close(); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (pbutton("Close##closeov", ImVec2(sp(14.f), sp(5.5f)))) { partyui::overlay_close(); ImGui::CloseCurrentPopup(); }
    if (partyui::is_host() && s.phase == pl::Phase::Done) {
        ImGui::SameLine(0, sp(1.f));
        if (pbutton("Close the party##closeparty", ImVec2(0, sp(5.5f)), true)) { partyui::overlay_close(); partyui::turn_off(); ImGui::CloseCurrentPopup(); }
    }
    if (partyui::is_host() && s.phase == pl::Phase::Intermission && s.poll.empty()) {
        ImGui::SameLine(0, sp(1.f));
        if (pbutton("End the party##endov", ImVec2(0, sp(5.5f)))) { partyui::overlay_close(); partyui::turn_off(); ImGui::CloseCurrentPopup(); }
    }
    ImGui::EndPopup();
}

}  // namespace

void party_draw() {
    couch_setup_modal();
    overlay_modal();
}
