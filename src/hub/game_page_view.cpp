/* The game page layout (see game_page.h): ONE arrangement of the gamepage:: pieces inside a ui::sheet, for both hosts (the full-window
 * page over the hub and the Library desk's pane). Sizing rules: game_page_layout.h. */
#include "app.h"
#include "game_page.h"
#include "game_page_layout.h"
#include "hack_ui.h"
#include "page_cache.h"
#include "qa_diskio.h"
#include "ui_controls.h"
#include "ui_sheet.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace gamepage {
namespace {

const char* const kTabKeys[4] = {"about", "saves", "shots", "settings"};   // each tab's own body scroll window

/* The item just submitted takes the nav focus (the ring shows only for pad / keyboard users). */
void focus_last_item(bool visible) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* w = g.CurrentWindow;
    if (g.NavWindow != w) ImGui::FocusWindow(w);
    const ImVec2 off = w->DC.CursorStartPos;
    const ImRect rel(ImGui::GetItemRectMin() - off, ImGui::GetItemRectMax() - off);
    ImGui::SetNavID(ImGui::GetItemID(), ImGuiNavLayer_Main, g.CurrentFocusScopeId, rel);
    if (visible) g.NavCursorVisible = true;
}

const Action* find_action(const std::vector<Action>& v, const char* id) {
    for (const Action& a : v) if (std::strcmp(a.id, id) == 0) return &a;
    return nullptr;
}

void tooltip_for(const Action& a) {
    if (!a.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", a.tooltip.c_str());
}

/* One row of the More menu: icon and label; a disabled row shows why under its label (pad users never see tooltips). */
float menu_item_h(const Action& a) {
    const float h = sp(4.75f);
    return (!a.enabled && !a.tooltip.empty()) ? h + ui::F.cap->FontSize + sp(0.25f) : h;
}
float menu_item_w(const Action& a) {
    const float lw = ImGui::CalcTextSize(a.label.c_str()).x;
    const float rw = (!a.enabled && !a.tooltip.empty()) ? ui::F.cap->CalcTextSizeA(ui::F.cap->FontSize, FLT_MAX, 0.f, a.tooltip.c_str()).x : 0.f;
    return sp(1.5f) + sp(2.25f) + sp(1.25f) + std::max(lw, std::min(rw, sp(44.f))) + sp(2.f);
}
bool menu_item(const Action& a, float w) {
    const float h = menu_item_h(a);
    ImGui::PushID(a.id);
    ImGui::BeginDisabled(!a.enabled);
    const bool pressed = ImGui::InvisibleButton("##mi", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    const bool hov = ImGui::IsItemHovered();
    const bool foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    ui::item_sound(foc, pressed);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    const float al = ImGui::GetStyle().Alpha, rd = sp(1.f);
    if (hov || foc) dl->AddRectFilled(p0, p1, ui::veil(foc ? 0.09f : 0.06f), rd);
    const float isz = sp(2.25f), fs = ImGui::GetFontSize();
    const bool why = !a.enabled && !a.tooltip.empty();
    const float ly = why ? p0.y + sp(1.f) : (p0.y + p1.y - fs) * 0.5f;
    if (a.icon != ui::Icon::None) ui::icon(dl, a.icon, ImVec2(p0.x + sp(1.5f) + isz * 0.5f, ly + fs * 0.5f), isz, ui::scale_alpha(a.primary ? ui::ACCENT : ui::TEXT2, al));
    const float tx = p0.x + sp(1.5f) + isz + sp(1.25f);
    dl->AddText(ImVec2(tx, ly), ui::scale_alpha(ui::TEXT, al), a.label.c_str());
    if (why) {
        ImFont* cf = ui::F.cap;
        const std::string t = ui::ellipsize(cf, cf->FontSize, a.tooltip, p1.x - tx - sp(1.f));
        dl->AddText(cf, cf->FontSize, ImVec2(tx, ly + fs + sp(0.25f)), ui::TEXT2, t.c_str());   // the reason stays readable on the dimmed row
    }
    if (foc) ui::focus_ring(dl, p0, p1, rd);
    ImGui::EndDisabled();
    if (why && hov) ImGui::SetTooltip("%s", a.tooltip.c_str());
    ImGui::PopID();
    return pressed && a.enabled;
}

/* Text drawn at p, cut with "..." to w; returns true when the mouse is over a cut text (the caller shows the whole text). */
bool text_cut(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 col, const std::string& s, float w) {
    const std::string t = ui::ellipsize(f, f->FontSize, s, w);
    dl->AddText(f, f->FontSize, p, col, t.c_str());
    return t != s && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p, ImVec2(p.x + w, p.y + f->FontSize));
}

struct Frame {
    View& v;
    HostKind host;
    hubui::gp::HostMetrics hm;
    ImVec2 size;
    bool* open = nullptr;
    std::optional<Ctx> ctx;
    bool gone = false;
    ui::SheetOpts so;
    float top_inset = 0.f;   // the desk pane: room above the header for the focused Play button's glow (game_page_layout.h)
    Frame(View& v_, HostKind h, ImVec2 s, bool* o) : v(v_), host(h), hm(hubui::gp::host_metrics(h == HostKind::Pane)), size(s), open(o) {}
};

/* Header: cover, title, facts, play time, the buttons and the More menu, the notices, the tab bar. False when an action ended the page. */
bool draw_page_header(Frame& F) {
    Ctx& c = *F.ctx;
    View& v = F.v;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* hw = ImGui::GetCurrentWindow();
    if (F.top_inset > 0.f) ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(0.f, F.top_inset));
    const float W = std::max(1.f, ImGui::GetContentRegionAvail().x);

    std::vector<Action> prim = primary_actions(c);
    if (!F.hm.close_button) prim.erase(std::remove_if(prim.begin(), prim.end(), [](const Action& a) { return std::strcmp(a.id, "close") == 0; }), prim.end());
    const std::vector<Action> sec = secondary_actions(c);
    trace_actions(c, prim);   // every action reports whether it is offered (QA reachability)
    trace_actions(c, sec);
    const Action* play = find_action(prim, "play");
    const Action* online = find_action(prim, "online");
    const Action* closea = find_action(prim, "close");

    hubui::gp::HeaderIn in;
    in.avail_w = W;
    in.sheet_h = F.size.y;
    in.u = sp(1.f);
    in.play_w = play ? std::max(sp(12.f), ui::button_width(play->label.c_str(), ui::Icon::Play)) : 0.f;
    in.online_w = online ? ui::button_width(online->label.c_str(), ui::Icon::Globe) : 0.f;
    in.more_w = ui::button_width("More", ui::Icon::Dots);
    in.close_w = closea ? ui::button_width("Close", ui::Icon::Close) : 0.f;
    in.aspect = kTileAspect;
    in.title_w = ui::F.h2->CalcTextSizeA(ui::F.h2->FontSize, FLT_MAX, 0.f, c.e.title.c_str()).x;   // the compact title font
    const hubui::gp::HeaderPlan P = hubui::gp::plan_header(in);
    const bool full = P.tier == hubui::gp::Tier::Full;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    if (P.cover_h > 0.f) draw_cover(c, dl, o, ImVec2(o.x + P.cover_w, o.y + P.cover_h));

    /* the text block: title (1-2 lines), facts, play time (Mini: the title alone, its tooltip has the rest) */
    const float tx = o.x + P.text_x, tw = W - P.text_x;
    const float text_w = P.beside ? std::max(sp(10.f), tw - P.buttons_w - sp(2.f)) : tw;
    ImFont* tf = full ? ui::F.title : P.small_title ? ui::F.bold : ui::F.h2;
    ImFont* bf = ui::F.body;
    ImFont* cf = ui::F.cap;
    const std::vector<std::string> tl = ui::wrap_lines(tf, tf->FontSize, c.e.title, text_w, P.title_lines);
    const float title_h = (float)tl.size() * tf->FontSize * 1.08f;
    const std::string time = play_time_line(c);
    std::string facts = facts_line(c);
    if (!P.time_line) facts += "  \xC2\xB7  " + time;
    const float line_gap = sp(0.5f);
    float block_h = title_h + (P.facts ? line_gap + bf->FontSize : 0.f) + (P.time_line ? line_gap + cf->FontSize : 0.f);
    const float btn_row = P.beside ? 0.f : sp(1.25f) + P.btn_h;
    const float content_h = std::max(P.beside ? P.btn_h : 0.f, block_h + btn_row);
    const float head_h = std::max(P.cover_h, content_h);
    float y = o.y + std::max(0.f, (head_h - (block_h + btn_row)) * 0.5f);   // centred on the cover
    const float title_y0 = y;
    for (const std::string& ln : tl) { dl->AddText(tf, tf->FontSize, ImVec2(tx, y), ui::TEXT, ln.c_str()); y += tf->FontSize * 1.08f; }
    const std::string& title = c.e.title;
    const bool title_cut = !tl.empty() && tl.back().size() >= 3 && tl.back().compare(tl.back().size() - 3, 3, "...") == 0 &&
                           !(title.size() >= 3 && title.compare(title.size() - 3, 3, "...") == 0);   // wrap_lines cut the title short
    if ((title_cut || !P.facts) && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(tx, title_y0), ImVec2(tx + text_w, y)))
        ImGui::SetTooltip("%s", P.facts ? c.e.title.c_str() : (c.e.title + "\n" + facts).c_str());
    if (P.facts) {
        y += line_gap;
        if (text_cut(dl, bf, ImVec2(tx, y), ui::TEXT2, facts, text_w)) ImGui::SetTooltip("%s", facts.c_str());
        y += bf->FontSize;
    }
    if (P.time_line) {
        y += line_gap;
        if (text_cut(dl, cf, ImVec2(tx, y), ui::TEXT2, time, text_w)) ImGui::SetTooltip("%s", time.c_str());
        y += cf->FontSize;
    }

    /* the buttons: at the right end of the title line, or on a row under the text */
    const float by = P.beside ? o.y + (head_h - P.btn_h) * 0.5f : y + sp(1.25f);
    const float bx = P.beside ? o.x + W - P.buttons_w : tx;
    ImGui::SetCursorScreenPos(ImVec2(bx, by));
    const bool nav_inside = g.NavWindow && ImGui::IsWindowChildOf(g.NavWindow, hw, true);
    if (play) {
        bool pressed = false;
        if (P.play_icon) {   // Mini: a round accent button, its name in the tooltip
            ImGui::BeginDisabled(!play->enabled);
            pressed = ui::icon_button("gp_play", ui::Icon::Play, P.btn_h, play->enabled, play->enabled ? play->label.c_str() : nullptr) && play->enabled;
            ImGui::EndDisabled();
            if (!play->enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", (play->label + (play->tooltip.empty() ? std::string() : ": " + play->tooltip)).c_str());
        } else {
            pressed = ui::button(play->label.c_str(), ImVec2(in.play_w, P.btn_h), true, play->enabled, ui::Icon::Play);
            tooltip_for(*play);
        }
        ImGui::SetItemDefaultFocus();
        /* The pane waits while a popup is up (a dialog the page opened, the friends panel's menus ...): focusing the pane would close it */
        const bool popup_up = F.host == HostKind::Pane && ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if ((v.focus_play && !popup_up) || (ImGui::GetFrameCount() <= v.focus_until && !nav_inside)) {
            focus_last_item(v.focus_play ? v.focus_visible : A.pad_mode);
            if (v.focus_play) shot_trace("gamepage-focus", "play " + c.e.key);
            v.focus_play = false;
        }
        if (play->run && (pressed || (play->act && shot_forced(c, play->act)))) play->run();
    }
    if (online) {
        ImGui::SameLine(0, P.gap);
        bool pressed = false;
        if (P.icons) {
            ImGui::BeginDisabled(!online->enabled);
            pressed = ui::icon_button("gp_online", ui::Icon::Globe, P.btn_h, false, online->enabled ? online->label.c_str() : nullptr);
            ImGui::EndDisabled();
            if (!online->enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", (online->label + (online->tooltip.empty() ? std::string() : ": " + online->tooltip)).c_str());
        } else {
            pressed = ui::button(online->label.c_str(), ImVec2(0, P.btn_h), false, online->enabled, ui::Icon::Globe);
            tooltip_for(*online);
        }
        if (online->run && ((pressed && online->enabled) || (online->act && shot_forced(c, online->act)))) online->run();
    }
    ImGui::SameLine(0, P.gap);
    const bool menu_was_open = ImGui::IsPopupOpen("##gp_more");
    const bool more = P.icons ? ui::icon_button("gp_more_btn", ui::Icon::Dots, P.btn_h, menu_was_open, "More")
                              : ui::button("More", ImVec2(0, P.btn_h), false, true, ui::Icon::Dots);
    if (!P.icons && ImGui::IsItemHovered() && !menu_was_open) ImGui::SetTooltip("Everything else for this game");
    const ImVec2 more1 = ImGui::GetItemRectMax();
    if (more) { ImGui::OpenPopup("##gp_more"); shot_trace("gamepage-more", c.e.key); }
    if (closea) {
        ImGui::SameLine(0, P.gap);
        const bool pressed = P.icons ? ui::icon_button("gp_close", ui::Icon::Close, P.btn_h, false, "Close")
                                     : ui::button("Close", ImVec2(0, P.btn_h), false, true, ui::Icon::Close);
        if (pressed && closea->run) closea->run();
    }
    const float buttons_bottom = ImGui::GetItemRectMax().y;

    /* the More menu: every secondary action this game offers */
    int chosen = -1;
    {
        float mw = sp(28.f);
        for (const Action& a : sec) mw = std::max(mw, menu_item_w(a));
        mw += sp(2.f);   // the window padding
        const ImVec2 wmin = hw->Pos, wmax(hw->Pos.x + hw->Size.x, hw->Pos.y + hw->Size.y);
        const float disp_h = ImGui::GetIO().DisplaySize.y;
        const float x = std::max(wmin.x + sp(1.f), std::min(more1.x - mw, wmax.x - mw - sp(1.f)));
        const float top = more1.y + sp(0.75f);
        const float max_h = std::max(sp(12.f), disp_h - top - sp(2.f));
        ImGui::SetNextWindowPos(ImVec2(x, top), ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints(ImVec2(mw, 0.f), ImVec2(mw, max_h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(1.f), sp(1.f)));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, sp(1.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, sp(0.25f)));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ui::PANEL2);
        ImGui::PushStyleColor(ImGuiCol_Border, ui::HAIRLINE);
        if (ImGui::BeginPopup("##gp_more", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
            const float iw = mw - sp(2.f);
            for (size_t i = 0; i < sec.size(); ++i)
                if (menu_item(sec[i], iw)) chosen = (int)i;
            if (chosen >= 0) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(4);
    }
    /* run the chosen action; pickers see every frame; HUB_SHOT_ACT presses each named action once */
    for (size_t i = 0; i < sec.size(); ++i) {
        const Action& a = sec[i];
        const bool pressed = (int)i == chosen;
        if (a.poll) { a.poll(pressed); continue; }
        if (a.run && (pressed || (a.act && shot_forced(c, a.act)))) {
            if (pressed) shot_trace("gamepage-action", std::string(a.id) + " " + c.e.key);
            a.run();
            if (a.ends_page) return false;   // the entry is gone from the library: nothing may touch it now
        }
    }

    /* below the cover / text / buttons: the notices, then the tabs */
    const float bottom = std::max({o.y + P.cover_h, y, buttons_bottom});
    {   /* the notices (warnings, Kanto how-to) stay in view under the header; nothing at all for most games */
        const float ny = bottom + (full ? sp(1.5f) : P.tier == hubui::gp::Tier::Mini ? sp(0.5f) : sp(1.f));
        ImGui::SetCursorScreenPos(ImVec2(o.x, ny));
        ImGui::BeginGroup();
        draw_notices(c);
        ImGui::EndGroup();
        const float nb = ImGui::GetItemRectMax().y;
        ImGui::SetCursorScreenPos(ImVec2(o.x, nb > ny + 1.f ? nb + sp(1.f) : ny));
    }
    const std::vector<TabInfo>& tabl = tabs(c);
    std::vector<const char*> labels;
    int cur = 0;
    for (size_t i = 0; i < tabl.size(); ++i) {
        labels.push_back(tabl[i].label.c_str());
        if (tabl[i].id == shown_tab(c)) cur = (int)i;
    }
    ui::TabBarOpts to;
    to.height = P.tabs_h;
    if (!labels.empty() && ui::tab_bar("gp_tabs", labels.data(), (int)labels.size(), &cur, to)) {
        select_tab(c, tabl[(size_t)cur].id);
        shot_trace("gamepage-tab", std::string(kTabKeys[std::max(0, std::min(3, shown_tab(c)))]) + " " + c.e.key);
    }
    F.so.body_key = kTabKeys[std::max(0, std::min(3, shown_tab(c)))];   // read by the sheet after this callback: each tab keeps its scroll
    return true;
}

}  // namespace

void view_reset(View& v) {
    v.fresh = true;
    v.popup_above = false;
}

Drawn draw_view(View& v, const hub::GameEntry* ep, HostKind host, ImVec2 pos, ImVec2 size, bool* open) {
    Drawn out;
    const bool pane = host == HostKind::Pane;
    Frame F(v, host, size, open);
    v.shown.clear();
    if (!pane && open && *open && !ep) *open = false;   // the game left the library while its page was open
    const bool want = ep && (pane || (open && *open));
    qadisk::Scope disk_scope(pane ? "pane" : "page", want);   // QA (HUB_SHOT_DISKIO): the page's own file-system calls
    std::optional<pagecache::PageScope> page_scope;
    if (want) {
        if (ep->key != v.key) {
            if (F.hm.keep_tab && !v.key.empty() && !v.fresh) v.st.tab_key = ep->key;   // the pane follows the list: keep the tab (About when this game lacks it)
            v.key_at = v.key.empty() ? -1e9 : ImGui::GetTime();   // the pane's heavy facts wait until the cursor has stayed here a moment (the first game: at once)
            v.key = ep->key;
            v.to_top = true;
        }
        if (v.fresh) {
            v.to_top = true;
            if (!pane) { v.focus_until = ImGui::GetFrameCount() + 4; v.popup_above = false; }
        }
        if (v.to_top) {
            for (unsigned id : v.body_win) if (id) ui::scroll_set((ImGuiID)id, 0.f);
            v.to_top = false;
        }
        Host h;
        if (pane) h.close = [] {};   // the pane stays; the action itself leads wherever it goes
        else h.close = [open] { if (open) *open = false; };
        const bool settled = !pane || ImGui::GetTime() - v.key_at >= pagecache::kSettleSecs;
        if (pane && settled && v.settled_key != v.key) { v.settled_key = v.key; shot_trace("gamepage-settled", v.key + " t=" + std::to_string(ImGui::GetTime())); }
        page_scope.emplace(pane ? pagecache::Host::Pane : pagecache::Host::Full, settled);   // what the page's disk facts may read now (page_cache.h)
        F.ctx.emplace(begin(v.st, *ep, h, v.fresh));
        v.fresh = false;
    }

    hubui::gp::sheet_spacing(hubui::gp::tier_for(size.y, sp(1.f)), sp(1.f), &F.so.pad, &F.so.gap);   // Mini: less padding and gaps
    if (pane) F.top_inset = hubui::gp::pane_header_inset(ui::sheet_pad(size, F.so), sp(1.f));
    ui::SheetParts parts;
    parts.header = [&F] {
        if (!F.ctx || F.gone) return;
        if (!draw_page_header(F)) {
            F.gone = true;
            if (F.open) *F.open = false;
        }
    };
    parts.body = [&F] {
        if (!F.ctx || F.gone) return;
        draw_tab(*F.ctx, shown_tab(*F.ctx));
    };
    const bool settings_tab = F.ctx && shown_tab(*F.ctx) == TabSettings;
    if (F.hm.footer_hints) {
        parts.footer = [&F, &v] {
            /* the hints; on the Settings tab the help strip of the focused row, with the hints at the right end of its heading line */
            static const ui::KeyHint hints[] = {{"A", "Enter", "Select"}, {"B", "Esc", "Close"}, {"LB", "[", ""}, {"RB", "]", "Tabs"},
                                                {"LT", "PgUp", "Page up"}, {"RT", "PgDn", "Page down"}};
            const int nh = (int)(sizeof hints / sizeof hints[0]);
            ui::OptionHelp h;
            if (F.ctx && !F.gone && shown_tab(*F.ctx) == TabSettings && ui::option_help(&h)) {
                const float w = std::max(1.f, ImGui::GetContentRegionAvail().x);
                const float hw = ui::hint_bar_width(hints, nh), gap = sp(3.f);
                const float heading_w = std::max(w * 0.4f, w - hw - gap);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const int mx = hubui::gp::help_lines_max(hubui::gp::tier_for(F.size.y, sp(1.f)));
                ui::option_help_strip(h, w, ui::option_help_lines(w, ui::StripStyle::Heading, mx), ui::StripStyle::Heading, heading_w);
                const ImVec2 after = ImGui::GetCursorScreenPos();
                const float line_h = ui::F.cap->FontSize + sp(0.75f);
                const float head_h = std::max(ui::F.bold->FontSize, line_h);
                ImGui::SetCursorScreenPos(ImVec2(p.x + std::max(heading_w + gap, w - hw), p.y + (head_h - line_h) * 0.5f));
                ui::hint_bar(hints, nh);   // (drops the hints that do not fit, from the end)
                ImGui::SetCursorScreenPos(after);
                ImGui::Dummy(ImVec2(0.f, 0.f));
            } else ui::hint_bar(hints, nh);
            if (F.ctx && !F.gone) custom_draw(1);   // Customize / Add to shelf, opened from More: inside the sheet so they stack on top of it
            if (F.ctx && !F.gone) hack_page_dialogs();   // Remove hack / Add hack or translation: the same, so the page is still there after them
            /* B / Esc over a popup above the page (the More menu, a confirm, a picker) closes that popup only, also on the frame it closed */
            ImGuiContext& g = *ImGui::GetCurrentContext();
            ImGuiWindow* me = g.CurrentWindow;
            int at = -1;
            for (int i = 0; i < g.OpenPopupStack.Size; ++i) if (g.OpenPopupStack[i].Window == me) at = i;
            const bool above = at >= 0 && at + 1 < g.OpenPopupStack.Size;
            F.so.back_closes = !(above || v.popup_above);
            v.popup_above = above;
        };
    } else if (settings_tab) {
        parts.footer = [&F] {   // the desk pane: a slim help strip at the bottom of the pane (its hints are in the hub's footer)
            ui::OptionHelp h;
            if (!F.ctx || F.gone || shown_tab(*F.ctx) != TabSettings || !ui::option_help(&h)) return;
            const float w = std::max(1.f, ImGui::GetContentRegionAvail().x);
            const int mx = hubui::gp::help_lines_max(hubui::gp::tier_for(F.size.y, sp(1.f)));
            ui::option_help_strip(h, w, ui::option_help_lines(w, ui::StripStyle::RunIn, mx), ui::StripStyle::RunIn);
        };
    }
    if (F.ctx) F.so.body_key = kTabKeys[std::max(0, std::min(3, shown_tab(*F.ctx)))];
    const ui::SheetResult r = ui::sheet(pane ? "gamepage" : "details", pane ? ui::SheetHost::Pane : ui::SheetHost::Overlay, pos, size, open, parts, F.so);
    out.shown = r.shown && F.ctx && !F.gone;
    out.closed = r.closed;
    out.gone = F.gone;
    if (out.shown) {
        const int t = std::max(0, std::min(3, shown_tab(*F.ctx)));
        v.body_win[t] = (unsigned)r.body.window_id;
        v.shown = v.key;
    }
    if (pane && F.ctx && !F.gone) custom_draw(1);   // the pane is not a popup: its dialogs open at the hub's level
    if (r.closed || (!pane && open && !*open)) view_reset(v);
    return out;
}

}  // namespace gamepage
