/* All Games layouts: Grid (the classic cover grid), List (dense sortable rows), Compact (small covers, the whole library at once) and
 * Showcase (a cover-flow carousel over a soft art backdrop), plus the A-Z / console jump strip at the right edge.
 *
 * Every layout is virtual: rows are laid out from numbers (libview_logic.h VLayout), only rows near the viewport submit items, and the
 * ones outside the viewport are bare nav stubs (an InvisibleButton with the same id as the real tile, so focus survives scrolling). Cover
 * art is only requested for tiles that are really on screen. Pad: D-pad moves, LT/RT page (main.cpp turns them into PageUp/PageDown),
 * the right stick jumps to the next / previous letter (or console group). Mouse: the strip, the wheel, header clicks.
 * Colours all come from the ui:: palette so every theme works. */
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"
#include "app.h"
#include "cases_logic.h"
#include "cases_audit.h"
#include "cases_stage.h"
#include "continue_logic.h"
#include "playtime_logic.h"
#include "../saves/savestore.h"
#include "state_slots.h"
#include "libview_ui.h"
#include "online_caps.h"
#include "shelf_draw.h"
#include "shelf_logic.h"

#include <SDL.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <unordered_map>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using hubui::LibLayout;
using hubui::ListCol;

namespace {

/* ---------- the display model: what is shown, in which order ---------- */

struct Disp {
    uint64_t sig = ~0ull;
    std::vector<int> flat;                       // item index (lib index, or lib.size() + ghost index) in display order
    std::vector<int> sec_size, sec_of;           // items per section; section of each flat position
    std::vector<std::string> sec_title, sec_sys;
    std::vector<bool> sec_head;                  // section gets a header row
    std::vector<std::string> labels;             // jump label of each flat position
    std::vector<hubui::JumpRun> runs, marks;
    std::vector<int> targets;                    // letter mode: strip slot -> mark
    bool letter_mode = false;
    std::unordered_map<std::string, int> pos_of;   // item key -> flat position
};

struct State {
    Disp D;
    hubui::VLayout L;
    uint64_t lsig = ~0ull;
    int jump_pos = -1;            // flat position to bring into view (and focus, see focus_key) on the next draw
    bool jump_top = false;        // letter jumps put the target at the top; others only reveal it
    std::string focus_key;        // pad focus lands on this item when it is submitted
    bool focus_quiet = false;     // ... without showing the focus ring (Cases: the mouse / wheel moved the selection)
    int traced = -2;
    bool jump_center = false;     // reveal puts the target mid-viewport (entering the view / switching layout)
    bool reveal = false;          // keep the selected game in view while the library is still loading / re-sorting
    uint64_t reveal_sig = ~0ull;
    int reveal_quiet = 0;
    std::string reveal_key;
    int top_pos = -1;             // flat position at the top of the viewport (the strip highlights its letter)
    std::string top_title;        // its section title (List header shows it)
    float scroll_abs = -1.f;       // paging: the exact scroll to apply on the next draw
    LibLayout last_layout = LibLayout::Grid;
    bool layout_seen = false;
    int strip_last = -1;
    // showcase
    float sc_pos = 0;
    int sc_focus = -1;
    bool sc_init = false;
    uint64_t sc_sig = 0;   // the Disp the carousel index belongs to
    std::string bg_cur, bg_prev;
    float bg_t = 1.f;
    ImU32 bg_c0 = 0, bg_c1 = 0, bg_p0 = 0, bg_p1 = 0;
} G;

bool is_ghost(int idx) { return idx >= (int)A.lib.size(); }
const hubui::fg::Ghost& ghost_of(int idx) { return A.ghosts[idx - A.lib.size()]; }
std::string selected_key();
std::string item_key(int idx) { return is_ghost(idx) ? "ghost:" + ghost_of(idx).id : A.lib[idx].key; }
const std::string& item_title(int idx) { return is_ghost(idx) ? ghost_of(idx).title : A.lib[idx].title; }
/* The item's console id (library.h): a system short, or "switch2" for a Switch 2 game. Friends' ghosts are catalog games, never Switch 2. */
std::string item_sys(int idx) { return is_ghost(idx) ? ghost_of(idx).sys : hub::console_id(A.lib[idx]); }
bool item_pinned(int idx) { return !is_ghost(idx) && is_native(A.lib[idx]); }
/* Short console name for pills and the strip. */
std::string short_sys(const std::string& s) {
    static const char* m[][2] = {{"n64", "N64"}, {"gc", "GC"}, {"ps1", "PS1"}, {"wiiu", "Wii U"}, {"3ds", "3DS"}, {"nds", "DS"}, {"ps2", "PS2"},
                                 {"psp", "PSP"}, {"switch", "Switch"}, {"switch2", "Switch 2"}, {"nes", "NES"}, {"snes", "SNES"}, {"gba", "GBA"}, {"gbc", "GBC"}, {"gb", "GB"}};
    for (auto& e : m) if (s == e[0]) return e[1];
    std::string o = s;
    for (char& c : o) c = (char)std::toupper((unsigned char)c);
    return o;
}
std::string upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
int item_players(int idx) {
    if (!is_ghost(idx)) return local_players_for(A.lib[idx]);
    return hubcaps::default_local_max(hub::system_from_short(ghost_of(idx).sys));
}

uint64_t mixh(uint64_t h, uint64_t v) { return (h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2))) * 0x100000001b3ull; }

ListCol effective_col(bool* desc) {   // the header's sorted column: the saved one, else whatever the Sort chip says
    ListCol c = hubui::col_from_key(A.st.lib_list_col);
    *desc = A.st.lib_list_desc;
    if (c != ListCol::None) return c;
    switch (hubui::sort_from_key(A.st.library_sort)) {
        case hubui::SortMode::Title: *desc = false; return ListCol::Title;
        case hubui::SortMode::LastPlayed: *desc = true; return ListCol::Last;
        case hubui::SortMode::MostPlayed: *desc = true; return ListCol::Time;
        default: return ListCol::None;
    }
}

Disp& build_disp(LibLayout lay) {
    Disp& D = G.D;
    const ListCol col = lay == LibLayout::List ? hubui::col_from_key(A.st.lib_list_col) : ListCol::None;
    uint64_t sig = mixh(0, A.vis_gen);
    sig = mixh(sig, (uint64_t)col * 2 + (A.st.lib_list_desc ? 1 : 0));
    sig = mixh(sig, A.lib.size() * 31 + A.ghosts.size());
    sig = mixh(sig, A.vis.size());
    const bool shelf_mixed = (lay == LibLayout::Shelf && A.st.lib_shelf_mixed) || (lay == LibLayout::Cases && !A.st.lib_group_genre && A.sort_mode != hubui::SortMode::Console);   // Cases: one flow in the Sort chip's order (the scrubber's marks follow it); "Console" keeps the per-console groups   // Shelf "Mixed": one flow in the Sort chip's order
    sig = mixh(sig, (uint64_t)A.sort_mode + (A.sort_rev ? 64 : 0) + (A.sort_favs ? 128 : 0) + (A.st.lib_group_genre ? 8 : 0) + (shelf_mixed ? 16 : 0) + (lay == LibLayout::Shelf ? 32 : 0));
    if (sig == D.sig) return D;
    D = Disp();
    D.sig = sig;
    if (col != ListCol::None) {   // one flat list ordered by the clicked column
        std::vector<hubui::ListRow> rows;
        rows.reserve(A.vis.size());
        for (int i : A.vis) {
            hubui::ListRow r;
            r.key = item_key(i); r.title = item_title(i); r.system = item_sys(i);
            r.pinned = item_pinned(i); r.ghost = is_ghost(i); r.players = item_players(i);
            if (!r.ghost) { const hub::GameState& g = gstate(A.lib[i].key); r.last_played = g.last_played; r.play_seconds = g.play_seconds; }
            rows.push_back(r);
        }
        for (int k : hubui::sort_list_rows(rows, nullptr, col, A.st.lib_list_desc)) D.flat.push_back(A.vis[k]);
        D.sec_size = {(int)D.flat.size()};
        D.sec_title = {""}; D.sec_sys = {"other"}; D.sec_head = {false};
    } else if (shelf_mixed) {
        for (int i : A.vis) D.flat.push_back(i);
        D.sec_size = {(int)D.flat.size()};
        D.sec_title = {""}; D.sec_sys = {"other"}; D.sec_head = {false};
    } else {
        for (const hubui::ConsoleSection& s : A.secs) {
            if (s.items.empty()) continue;
            D.sec_size.push_back((int)s.items.size());
            D.sec_title.push_back(s.title);
            D.sec_sys.push_back(s.system);
            D.sec_head.push_back(true);
            for (int i : s.items) D.flat.push_back(i);
        }
    }
    D.sec_of.reserve(D.flat.size());
    for (size_t s = 0; s < D.sec_size.size(); ++s) for (int k = 0; k < D.sec_size[s]; ++k) D.sec_of.push_back((int)s);
    D.letter_mode = col == ListCol::Title || (col == ListCol::None && A.sort_mode == hubui::SortMode::Title);
    D.labels.resize(D.flat.size());
    for (size_t p = 0; p < D.flat.size(); ++p) {
        int i = D.flat[p];
        D.pos_of[item_key(i)] = (int)p;
        if (D.letter_mode) D.labels[p] = item_pinned(i) ? std::string() : hubui::letter_of(item_title(i));
        else if (col == ListCol::Console) D.labels[p] = short_sys(item_sys(i));
        else if (col == ListCol::None) {
            int s = D.sec_of[p];
            D.labels[p] = D.sec_sys[s] != "other" ? short_sys(D.sec_sys[s]) : D.sec_title[s].substr(0, 4);
        }   // Players / Last / Time: nothing to jump between
    }
    D.runs = hubui::build_runs(D.labels);
    D.marks = hubui::strip_marks(D.runs);
    if (D.letter_mode) D.targets = hubui::strip_targets(D.marks);
    return D;
}

/* The selected item: a friend's game when one is selected (ensure_selection keeps a hero in sel_game meanwhile), else sel_game. */
std::string selected_key() {
    if (!A.sel_ghost.empty() && find_ghost(A.sel_ghost)) return "ghost:" + A.sel_ghost;
    return A.sel_game;
}
int selected_pos(const Disp& D) {
    auto it = D.pos_of.find(selected_key());
    return it == D.pos_of.end() ? -1 : it->second;
}

void select_item(int idx) {
    if (is_ghost(idx)) { A.sel_ghost = ghost_of(idx).id; A.sel_game.clear(); }
    else { A.sel_game = A.lib[idx].key; A.sel_ghost.clear(); }
}

void jump(const Disp& D, int pos, bool top) {
    if (pos < 0 || pos >= (int)D.flat.size()) return;
    G.jump_pos = pos;
    G.jump_top = top;
    select_item(D.flat[pos]);
    if (A.pad_mode) G.focus_key = item_key(D.flat[pos]);
    shot_trace("libview-jump", std::string(hubui::layout_key(lib_layout())) + " pos=" + std::to_string(pos) + " label=" + D.labels[pos] + " key=" + item_key(D.flat[pos]));
}

/* The grid child's nav focus goes to the item just submitted (pad shortcuts and strip jumps). */
void focus_item_here(bool visible = true) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* w = g.CurrentWindow;
    if (g.NavWindow != w) ImGui::FocusWindow(w);
    ImVec2 off = w->DC.CursorStartPos;
    ImRect rel(ImGui::GetItemRectMin() - off, ImGui::GetItemRectMax() - off);
    ImGui::SetNavID(ImGui::GetItemID(), ImGuiNavLayer_Main, g.CurrentFocusScopeId, rel);
    if (visible) g.NavCursorVisible = true;
}

/* ---------- one interactive item (same behaviour as draw_tile's front half) ---------- */

TileResult interact(const std::string& id, ImVec2 sz, bool selected, float star_dx = -1.f, float star_dy = 0.f) {
    TileResult r;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::PushID(id.c_str());
    ImGui::InvisibleButton("tile", sz, ImGuiButtonFlags_EnableNav);
    if (selected && A.view == LibView::All) seed_nav_here();
    r.visible = ImGui::IsItemVisible();
    bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused();
    r.hovered = hov;
    r.focused = foc && io.NavVisible;
    bool star_hov = false;
    if (star_dx >= 0.f && hov) {
        ImVec2 r0 = ImGui::GetItemRectMin();
        float sr = sp(1.25f);
        star_hov = std::fabs(io.MousePos.x - (r0.x + star_dx)) < sr && std::fabs(io.MousePos.y - (r0.y + star_dy)) < sr;
    }
    if (ImGui::IsItemDeactivated() && hov && !g_suppress_click && ImGui::IsMouseReleased(0)) { if (star_hov) r.star = true; else r.clicked = true; }
    if (ImGui::IsItemClicked(1)) { r.clicked = true; r.details = true; }
    if (hov && !star_hov && ImGui::IsMouseDoubleClicked(0)) r.activated = true;
    if (foc && A.focused && !io.WantTextInput && !A.proc) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) r.activated = true;
    }
    ImGui::PopID();
    return r;
}

void deliver(int idx, TileResult r) {
    if (is_ghost(idx)) { r.details = r.details; handle_ghost_result(ghost_of(idx), r); return; }
    const hub::GameEntry& e = A.lib[idx];
    TileSpec ms;
    ms.selected = item_key(idx) == selected_key();
    handle_entry_result(e, ms, r);
}

void apply_pending_focus(int idx) {
    if (!G.focus_key.empty() && G.focus_key == item_key(idx)) { focus_item_here(!G.focus_quiet); G.focus_key.clear(); G.focus_quiet = false; }
}

/* ---------- Grid (draw_tile) and Compact ---------- */

std::string grid_id(int idx) { return is_ghost(idx) ? "grid|ghost|" + ghost_of(idx).id : "grid|" + A.lib[idx].key; }

void draw_grid_item(int idx, ImVec2 sz, bool in_view, bool show_sys) {
    if (!in_view) {   // outside the viewport: just the nav stub (same id as the tile)
        ImGui::PushID(grid_id(idx).c_str());
        ImGui::InvisibleButton("tile", sz, ImGuiButtonFlags_EnableNav);
        bool foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        if (item_key(idx) == selected_key()) seed_nav_here();
        ImGui::PopID();
        if (foc) { TileResult r; r.focused = true; deliver(idx, r); }
        apply_pending_focus(idx);
        return;
    }
    TileResult r;
    if (is_ghost(idx)) { TileSpec s = spec_for_ghost(ghost_of(idx), "grid", show_sys); r = draw_tile(s, sz); handle_ghost_result(ghost_of(idx), r); }
    else { const hub::GameEntry& e = A.lib[idx]; TileSpec s = spec_for_entry(e, "grid", false, show_sys); r = draw_tile(s, sz); handle_entry_result(e, s, r); }
    apply_pending_focus(idx);
}

ArtRef art_of(int idx) { return is_ghost(idx) ? art_for_ghost(ghost_of(idx)) : art_for_entry(A.lib[idx]); }
void colors_of(int idx, ImU32* c0, ImU32* c1) {
    if (!is_ghost(idx)) { entry_colors(A.lib[idx], c0, c1); return; }
    console_colors(ghost_of(idx).sys, c0, c1);
    *c0 = ui::mix(*c0, IM_COL32(92, 95, 104, 255), 0.72f);
    *c1 = ui::mix(*c1, IM_COL32(70, 73, 82, 255), 0.72f);
}

void draw_compact_item(int idx, ImVec2 sz, bool in_view) {
    const std::string id = "cp|" + item_key(idx);
    const bool sel = item_key(idx) == selected_key();
    TileResult r = interact(id, sz, sel);
    if (!in_view && !r.focused && !r.hovered) { deliver(idx, r); apply_pending_focus(idx); return; }
    ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    ImGuiIO& io = ImGui::GetIO();
    float* an = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID(id.c_str()), 0.f);
    ui::anim(*an, (r.hovered || r.focused) ? 1.f : 0.f, 14.f);
    float e = *an, grow = e * sp(0.375f);
    ImVec2 p0(r0.x - grow, r0.y - grow - e * sp(0.25f)), p1(r1.x + grow, r1.y + grow - e * sp(0.25f));
    float rad = sp(1.f), w = p1.x - p0.x, h = p1.y - p0.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 c0, c1;
    colors_of(idx, &c0, &c1);
    ArtRef art = art_of(idx);
    const bool ghost = is_ghost(idx);
    ui::shadow(dl, p0, p1, rad, 0.6f + 0.6f * e, e * sp(0.4f));
    if (e > 0.02f) ui::glow(dl, p0, p1, rad, c1, e * 0.8f);
    TileSpec ts; ts.title = item_title(idx); ts.c0 = c0; ts.c1 = c1;
    if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, p0, p1, rad, ghost ? 0.7f : 1.f);
    else draw_no_art(dl, ts, p0, p1, rad, ghost ? 0.85f : 1.f);
    if (art.loading) ui::shimmer(dl, p0, p1, rad, IM_COL32(0, 0, 0, 0));
    if (ghost) dl->AddRectFilled(p0, p1, IM_COL32(96, 99, 108, (int)(150.f - 70.f * e)), rad);
    // the title shows on hover / focus (always when there is no cover to tell the games apart)
    float ta = std::max(e, art.tex ? 0.f : 0.85f);
    if (ta > 0.02f) {
        ui::vfade_round_rect(dl, ImVec2(p0.x, p0.y + h * 0.35f), p1, IM_COL32(6, 7, 12, 255), rad, ImDrawFlags_RoundCornersBottom, 0.f, 0.92f * ta);
        ImFont* f = ui::F.cap;
        std::vector<std::string> ln = ui::wrap_lines(f, f->FontSize, item_title(idx), w - sp(1.5f), 2);
        float ty = p1.y - sp(0.75f) - (float)ln.size() * f->FontSize * 1.1f;
        for (auto& l : ln) { dl->AddText(f, f->FontSize, ImVec2(p0.x + sp(0.75f), ty), IM_COL32(255, 255, 255, (int)(255 * ta)), l.c_str()); ty += f->FontSize * 1.1f; }
    }
    if (!ghost) {
        bool fav = gstate(A.lib[idx].key).favorite;
        if (fav) ui::icon(dl, ui::Icon::StarFilled, ImVec2(p1.x - sp(1.4f), p0.y + sp(1.4f)), sp(1.5f), IM_COL32(255, 214, 70, 255));
        if (entry_online(A.lib[idx])) dl->AddCircleFilled(ImVec2(p0.x + sp(1.1f), p0.y + sp(1.1f)), sp(0.45f), ui::GOOD);
    }
    dl->AddRect(p0, p1, ui::HAIRLINE, rad, 0, 1.f);
    if (r.focused) ui::focus_ring(dl, p0, p1, rad);
    else if (sel) dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 120), rad, 0, sp(0.25f));
    if (r.hovered && !io.MouseDown[0] && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && ta < 0.5f) ImGui::SetTooltip("%s", item_title(idx).c_str());
    deliver(idx, r);
    apply_pending_focus(idx);
}

/* ---------- List ---------- */

void draw_list_row(int idx, float w, float h, const hubui::ListCols& C, bool odd, bool in_view) {
    const std::string id = "ls|" + item_key(idx);
    const bool ghost = is_ghost(idx);
    const std::string key = item_key(idx);
    const bool sel = key == selected_key();
    TileResult r = interact(id, ImVec2(w, h), sel, ghost ? -1.f : C.icons_x + sp(4.75f), h * 0.5f);
    if (!in_view && !r.focused && !r.hovered) { deliver(idx, r); apply_pending_focus(idx); return; }
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float* an = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID(id.c_str()), 0.f);
    ui::anim(*an, (r.hovered || r.focused) ? 1.f : 0.f, 18.f);
    float e = *an, rad = sp(0.75f);
    if (odd) dl->AddRectFilled(p0, p1, ui::veil(0.03f), rad);
    if (e > 0.01f) dl->AddRectFilled(p0, p1, ui::scale_alpha(ui::FRAME_HOV, e), rad);
    if (sel) { dl->AddRectFilled(ImVec2(p0.x, p0.y + sp(0.5f)), ImVec2(p0.x + sp(0.3f), p1.y - sp(0.5f)), ui::ACCENT, sp(0.15f)); }
    ImU32 c0, c1;
    colors_of(idx, &c0, &c1);
    ArtRef art = art_of(idx);
    float th = h - sp(0.75f), tw = th * kTileAspect;
    ImVec2 a0(p0.x + sp(1.f), p0.y + (h - th) * 0.5f), a1(a0.x + tw, a0.y + th);
    TileSpec ts; ts.title = item_title(idx); ts.c0 = c0; ts.c1 = c1;
    if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, a0, a1, sp(0.5f), ghost ? 0.7f : 1.f);
    else gradient_round_rect(dl, a0, a1, c0, c1, sp(0.5f));
    const float cy = (p0.y + p1.y) * 0.5f;
    ImFont* bf = ui::F.body; ImFont* cf = ui::F.cap;
    const ImU32 tcol = ghost ? ui::TEXT3 : ui::TEXT, scol = ghost ? ui::TEXT3 : ui::TEXT2;
    const std::string& title = item_title(idx);
    dl->AddText(bf, bf->FontSize, ImVec2(p0.x + C.title_x, cy - bf->FontSize * 0.5f), tcol, ui::ellipsize(bf, bf->FontSize, title, C.title_w - sp(0.5f)).c_str());
    if (C.console) {
        ImU32 a, b;
        console_colors(item_sys(idx), &a, &b);
        std::string badge = short_sys(item_sys(idx));
        ui::pill_at(dl, ImVec2(p0.x + C.console_x, cy - (cf->FontSize + sp(1.f)) * 0.5f), badge.c_str(), ui::mix(a, ui::PANEL, ghost ? 0.7f : 0.45f), IM_COL32_WHITE);
    }
    if (C.players) {
        int n = item_players(idx);
        char b[24];
        std::snprintf(b, sizeof b, n >= 2 ? "1-%d" : "1", n);
        dl->AddText(cf, cf->FontSize, ImVec2(p0.x + C.players_x, cy - cf->FontSize * 0.5f), scol, b);
    }
    if (!ghost) {
        const hub::GameState& gs = gstate(A.lib[idx].key);
        if (C.last) {
            std::string s = gs.last_played ? fmt_ago(gs.last_played) : std::string("-");
            dl->AddText(cf, cf->FontSize, ImVec2(p0.x + C.last_x, cy - cf->FontSize * 0.5f), gs.last_played ? scol : ui::TEXT3, s.c_str());
        }
        if (C.time) {
            std::string s = gs.play_seconds ? hubui::pt::hours_compact(gs.play_seconds) : std::string("-");
            dl->AddText(cf, cf->FontSize, ImVec2(p0.x + C.time_x, cy - cf->FontSize * 0.5f), gs.play_seconds ? scol : ui::TEXT3, s.c_str());
        }
        bool on = entry_online(A.lib[idx]);
        if (on) ui::icon(dl, ui::Icon::Globe, ImVec2(p0.x + C.icons_x + sp(1.5f), cy), sp(1.75f), ui::GOOD);
        bool fav = gs.favorite;
        float sa = fav ? 1.f : e * 0.8f;
        if (sa > 0.02f) ui::icon(dl, fav ? ui::Icon::StarFilled : ui::Icon::Star, ImVec2(p0.x + C.icons_x + sp(4.75f), cy), sp(1.9f),
                                 fav ? IM_COL32(255, 214, 70, 255) : ui::scale_alpha(ui::TEXT2, sa));
    } else if (C.last) {
        dl->AddText(cf, cf->FontSize, ImVec2(p0.x + C.last_x, cy - cf->FontSize * 0.5f), ui::TEXT3, "A friend's game");
    }
    if (r.focused) ui::focus_ring(dl, p0, p1, rad);
    if (r.hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && ui::ellipsize(bf, bf->FontSize, title, C.title_w - sp(0.5f)) != title) ImGui::SetTooltip("%s", title.c_str());
    deliver(idx, r);
    apply_pending_focus(idx);
}

/* Sortable column headers above the list (they sit outside the scrolling child, so they stay put). */
void draw_list_header(float x_left, float inner_w) {
    const hubui::ListCols C = hubui::list_columns(inner_w, sp(1.f));
    const std::string group_now = G.top_title;
    bool desc = false;
    const ListCol act = effective_col(&desc);
    const float h = sp(3.25f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float x0 = x_left;
    struct H { ListCol c; float x, w; bool show; };
    H hs[] = {{ListCol::Title, C.thumb_x, C.title_x + C.title_w, true}, {ListCol::Console, C.console_x, C.console_w, C.console},
              {ListCol::Players, C.players_x, C.players_w, C.players}, {ListCol::Last, C.last_x, C.last_w, C.last}, {ListCol::Time, C.time_x, C.time_w, C.time}};
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
    for (const H& hd : hs) {
        if (!hd.show) continue;
        ImGui::SetCursorScreenPos(ImVec2(x0 + hd.x, o.y));
        ImGui::PushID((int)hd.c);
        bool pressed = ImGui::InvisibleButton("hdr", ImVec2(hd.w - sp(0.5f), h), ImGuiButtonFlags_EnableNav);
        bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
        if (hov || foc) dl->AddRectFilled(p0, p1, ui::FRAME_HOV, sp(0.75f));
        const bool on = hd.c == act;
        ImFont* f = ui::F.bold;
        float tx = hd.c == ListCol::Title ? x0 + C.title_x : p0.x + sp(0.5f);
        const char* nm = hubui::col_name(hd.c);
        dl->AddText(f, f->FontSize * 0.85f, ImVec2(tx, o.y + (h - f->FontSize * 0.85f) * 0.5f), on ? ui::TEXT : ui::TEXT2, nm);
        if (on) {   // sort arrow
            float aw = f->CalcTextSizeA(f->FontSize * 0.85f, FLT_MAX, 0.f, nm).x;
            float ax = tx + aw + sp(1.f), ay = o.y + h * 0.5f, s = sp(0.5f);
            if (desc) dl->AddTriangleFilled(ImVec2(ax - s, ay - s * 0.6f), ImVec2(ax + s, ay - s * 0.6f), ImVec2(ax, ay + s * 0.8f), ui::ACCENT);
            else dl->AddTriangleFilled(ImVec2(ax - s, ay + s * 0.6f), ImVec2(ax + s, ay + s * 0.6f), ImVec2(ax, ay - s * 0.8f), ui::ACCENT);
        }
        if (hd.c == ListCol::Title && !group_now.empty()) {   // which console / genre group the top row belongs to
            float aw = f->CalcTextSizeA(f->FontSize * 0.85f, FLT_MAX, 0.f, nm).x;
            dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx + aw + sp(3.f), o.y + (h - ui::F.cap->FontSize) * 0.5f), ui::TEXT3, group_now.c_str());
        }
        if (foc) ui::focus_ring(dl, p0, p1, sp(0.75f));
        if (hov) ImGui::SetTooltip("Sort by %s", nm);
        if (pressed) {
            ListCol nc; bool nd;
            hubui::col_click(act, desc, hd.c, &nc, &nd);
            A.st.lib_list_col = hubui::col_key(nc);
            A.st.lib_list_desc = nd;
            mark_dirty();
            A.grid_scroll_top = true;
            shot_trace("libview-sort", std::string(hubui::col_key(nc)) + (nd ? " desc" : " asc"));
        }
        ImGui::PopID();
    }
    ImGui::PopItemFlag();
    ImGui::SetCursorScreenPos(o);
    dl->AddLine(ImVec2(x0, o.y + h), ImVec2(x0 + inner_w, o.y + h), ui::HAIRLINE, 1.f);
    ImGui::Dummy(ImVec2(inner_w, h + sp(0.5f)));
}

/* ---------- virtual rows (grid / compact / list) ---------- */

void draw_virtual(Disp& D, LibLayout lay) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const float avail = ImGui::GetContentRegionAvail().x;
    int cols = 1;
    float iw = avail, ih = sp(3.75f), cgap = 0, rgap = 0, sec_gap = sp(1.f);
    const float head_h = ui::F.h2->FontSize + sp(1.75f);
    if (lay == LibLayout::Grid) {
        float gap = sp(2.f);
        hubui::GridLayout gl = hubui::grid_layout(avail, sp(26.f), sp(37.5f), gap);
        cols = gl.cols; iw = gl.tile_w; ih = iw / kTileAspect; cgap = rgap = gap; sec_gap = sp(1.5f);
    } else if (lay == LibLayout::Compact) {
        float gap = sp(1.25f);
        hubui::GridLayout gl = hubui::grid_layout(avail, sp(11.f), sp(14.f), gap);
        cols = gl.cols; iw = gl.tile_w; ih = iw / kTileAspect; cgap = rgap = gap; sec_gap = sp(1.25f);
    }
    const uint64_t lsig = mixh(mixh(mixh(D.sig, (uint64_t)lay), (uint64_t)avail), (uint64_t)(ui::S * 100));
    if (lsig != G.lsig) {
        G.L = hubui::build_vlayout(D.sec_size, D.sec_head, cols, ih, head_h, rgap, sec_gap);
        G.lsig = lsig;
    }
    const hubui::VLayout& L = G.L;
    const float x0 = ImGui::GetCursorPosX();
    const float base_y = ImGui::GetCursorPosY();
    const float view_h = ImGui::GetWindowHeight();
    float scroll = ImGui::GetScrollY();
    if (G.scroll_abs >= 0.f) { scroll = G.scroll_abs; G.scroll_abs = -1.f; ImGui::SetScrollY(scroll); }
    else if (G.jump_pos >= 0 && G.jump_pos < (int)D.flat.size() && G.jump_pos < (int)L.item_row.size()) {
        int r = L.item_row[G.jump_pos];
        if (G.jump_top) {
            int rr = (r > 0 && L.rows[r - 1].kind == 0) ? r - 1 : r;
            scroll = std::max(0.f, base_y + L.rows[rr].y - sp(0.5f));
        } else if (G.jump_center) {
            scroll = std::max(0.f, base_y + L.rows[r].y - view_h * 0.4f);
        } else {
            scroll = hubui::scroll_to_row(L, r, scroll - base_y, view_h - base_y, sp(1.f), 1e9f) + base_y;
            scroll = std::max(0.f, scroll);
        }
        ImGui::SetScrollY(scroll);
        G.jump_pos = -1;
        G.jump_center = false;
    }
    {   // what is at the top of the viewport now (strip highlight, List header group name)
        int tf = -1, tl = -1;
        const float ty = std::max(0.f, scroll - base_y) + 1.f;
        hubui::rows_in_range(L, ty, ty, &tf, &tl);
        G.top_pos = -1;
        if (tf >= 0) {
            if (L.rows[tf].kind == 0 && tf + 1 < (int)L.rows.size()) ++tf;
            if (L.rows[tf].kind == 1 && L.rows[tf].first < (int)D.flat.size()) G.top_pos = L.rows[tf].first;
        }
        G.top_title.clear();
        if (G.top_pos >= 0 && D.sec_head[D.sec_of[G.top_pos]]) G.top_title = D.sec_title[D.sec_of[G.top_pos]];
    }
    int f = -1, l = -1;
    hubui::rows_in_range(L, scroll - base_y - view_h, scroll - base_y + view_h * 2.f, &f, &l);   // one page of nav stubs each way (PageUp/PageDown, fast scrolling)
    hubui::ListCols C;
    if (lay == LibLayout::List) C = hubui::list_columns(iw, sp(1.f));
    for (int r = f; r >= 0 && r <= l; ++r) {
        const hubui::VRow& row = L.rows[r];
        const float ry = base_y + row.y;
        const bool vis = ry + row.h > scroll - sp(1.f) && ry < scroll + view_h + sp(1.f);
        if (row.kind == 0) {
            if (!vis) continue;
            ImGui::SetCursorPos(ImVec2(x0, ry));
            shelf_header(D.sec_title[row.first].c_str(), "", 0.f, D.sec_size[row.first]);
            continue;
        }
        for (int k = 0; k < row.count; ++k) {
            int pos = row.first + k;
            int idx = D.flat[pos];
            ImGui::SetCursorPos(ImVec2(x0 + (float)k * (iw + cgap), ry));
            if (lay == LibLayout::Grid) draw_grid_item(idx, ImVec2(iw, ih), vis, D.sec_sys[D.sec_of[pos]] == "other");
            else if (lay == LibLayout::Compact) draw_compact_item(idx, ImVec2(iw, ih), vis);
            else draw_list_row(idx, iw, ih, C, (pos & 1) != 0, vis);
        }
    }
    ImGui::SetCursorPos(ImVec2(x0, base_y + L.total_h));
    ImGui::Dummy(ImVec2(1.f, sp(1.f)));
    (void)win;
}

/* ---------- Showcase: cover flow ---------- */

void backdrop(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float rad, float a_cur) {
    ui::gradient_round_rect(dl, p0, p1, ui::mix(G.bg_p0, G.bg_c0, a_cur) , ui::mix(G.bg_p1, G.bg_c1, a_cur), rad);
    auto layer = [&](const std::string& key, float alpha) {
        if (key.empty() || alpha < 0.01f) return;
        int idx = -1;
        if (key.rfind("ghost:", 0) == 0) { for (size_t g = 0; g < A.ghosts.size(); ++g) if (A.ghosts[g].id == key.substr(6)) { idx = (int)(A.lib.size() + g); break; } }
        else { auto it = A.lib_index.find(key); if (it != A.lib_index.end()) idx = it->second; }
        if (idx < 0) return;
        ArtRef art = art_of(idx);
        if (!art.tex) return;
        // no blur shader in ImGui: a few magnified, low-alpha copies of the cover melt into a soft wash
        for (int k = 0; k < 3; ++k) {
            float ex = sp(2.5f) * (float)k;
            ui::draw_art_bg(dl, art.tex, art.w, art.h, ImVec2(p0.x - ex, p0.y - ex), ImVec2(p1.x + ex, p1.y + ex), rad, 0.34f, alpha * (k == 0 ? 0.5f : 0.3f));
        }
    };
    layer(G.bg_prev, 1.f - a_cur);
    layer(G.bg_cur, a_cur);
    ui::hfade_round_rect(dl, p0, p1, ui::SCRIM, rad, 0, 0.55f, 0.15f);
    ui::vfade_round_rect(dl, ImVec2(p0.x, p0.y + (p1.y - p0.y) * 0.38f), p1, ui::SCRIM, rad, ImDrawFlags_RoundCornersBottom, 0.f, 0.94f);
    ui::vfade_round_rect(dl, p0, ImVec2(p1.x, p0.y + (p1.y - p0.y) * 0.2f), ui::SCRIM, rad, ImDrawFlags_RoundCornersTop, 0.5f, 0.f);
}

void draw_showcase(Disp& D) {
    ImGuiIO& io = ImGui::GetIO();
    const int n = (int)D.flat.size();
    ImVec2 org = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImGui::GetContentRegionAvail();
    const float W = sz.x, H = sz.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rad = sp(2.f);
    if (G.sc_sig != D.sig && G.sc_init) {   // the list changed under us (scan, filter): keep the same game in the middle
        int p = selected_pos(D);
        if (p >= 0) { G.sc_focus = p; G.sc_pos = (float)p; }
        else G.sc_init = false;
    }
    G.sc_sig = D.sig;
    if (!G.sc_init || G.sc_focus < 0 || G.sc_focus >= n) {
        int sp0 = selected_pos(D);
        G.sc_focus = sp0 >= 0 ? sp0 : 0;
        G.sc_pos = (float)G.sc_focus;
        G.sc_init = true;
    }
    int pending = -1;
    if (G.jump_pos >= 0 && G.jump_pos < n) {
        pending = G.jump_pos;
        G.sc_focus = pending;
        if (std::fabs((float)pending - G.sc_pos) > 7.f) G.sc_pos = (float)pending;
        G.jump_pos = -1;
    }
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0.f && !io.WantTextInput) {   // wheel steps through the covers
        int step = io.MouseWheel > 0 ? -1 : 1;
        int np = std::max(0, std::min(n - 1, G.sc_focus + step));
        if (np != G.sc_focus) { pending = np; G.sc_focus = np; select_item(D.flat[np]); if (A.pad_mode) G.focus_key = item_key(D.flat[np]); }
    }
    ui::anim(G.sc_pos, (float)G.sc_focus, 9.f);
    if (std::fabs(G.sc_pos - (float)G.sc_focus) < 0.002f) G.sc_pos = (float)G.sc_focus;
    const float pos = G.sc_pos;

    // backdrop follows the focused game (crossfade)
    const int fidx = D.flat[G.sc_focus];
    const std::string fkey = item_key(fidx);
    ImU32 fc0, fc1;
    colors_of(fidx, &fc0, &fc1);
    if (fkey != G.bg_cur) {
        G.bg_prev = G.bg_cur; G.bg_cur = fkey; G.bg_t = G.bg_prev.empty() ? 1.f : 0.f;
        G.bg_p0 = G.bg_c0 ? G.bg_c0 : ui::mix(fc0, ui::HERO_BASE, 0.7f); G.bg_p1 = G.bg_c1 ? G.bg_c1 : ui::mix(fc1, ui::HERO_BASE, 0.8f);
    }
    ui::anim(G.bg_t, 1.f, 5.f);
    G.bg_c0 = ui::mix(fc0, ui::HERO_BASE, 0.7f);
    G.bg_c1 = ui::mix(fc1, ui::HERO_BASE, 0.8f);
    backdrop(dl, org, org + sz, rad, G.bg_t);
    dl->AddRect(org, org + sz, ui::HAIRLINE, rad, 0, 1.f);

    // carousel geometry
    // bottom up: hints, pills, title; the cover gets what is left above them
    ImFont* tfont = ui::F.title ? ui::F.title : ui::F.h2;
    const float foot_y = org.y + H - sp(3.f);
    const float pills_y = foot_y - sp(0.75f) - (ui::F.cap->FontSize + sp(1.f)) - sp(0.5f);
    const float title_y = pills_y - tfont->FontSize * 1.15f - sp(0.5f);
    const float cover_room = std::max(sp(8.f), title_y - org.y - sp(3.f));
    const float fh = std::max(sp(8.f), std::min(cover_room * 0.96f, W * 0.30f)), fw = fh * 1.08f;
    const float cx = org.x + W * 0.5f, cy = org.y + sp(1.5f) + cover_room * 0.5f;
    struct Slot { int i; ImVec2 p0, p1; float d, alpha, dim; };
    std::vector<Slot> slots;
    const int lo = std::max(0, (int)std::floor(pos) - 7), hi = std::min(n - 1, (int)std::ceil(pos) + 7);
    int focused_now = -1;
    for (int i = lo; i <= hi; ++i) {
        float d = (float)i - pos, ad = std::fabs(d);
        float side = d < 0 ? -1.f : 1.f;
        float off = side * (std::min(ad, 1.f) * fw * 0.78f + std::max(ad - 1.f, 0.f) * fw * 0.30f);
        float s = (1.f - 0.30f * std::min(ad, 1.f)) * std::pow(0.93f, std::max(ad - 1.f, 0.f));
        float w = fw * s, h = fh * s;
        Slot sl;
        sl.i = i; sl.d = d;
        sl.p0 = ImVec2(cx + off - w * 0.5f, cy - h * 0.5f);
        sl.p1 = ImVec2(cx + off + w * 0.5f, cy + h * 0.5f);
        sl.alpha = std::max(0.f, 1.f - 0.2f * std::min(ad, 1.f) - 0.17f * std::max(ad - 1.f, 0.f));
        sl.dim = 1.f - 0.45f * std::min(ad, 1.f);
        slots.push_back(sl);
    }
    // nav items in index order (the hit boxes follow the animated covers)
    std::vector<TileResult> res(slots.size());
    for (size_t k = 0; k < slots.size(); ++k) {
        const Slot& sl = slots[k];
        int idx = D.flat[sl.i];
        ImGui::SetCursorScreenPos(sl.p0);
        res[k] = interact("sc|" + item_key(idx), sl.p1 - sl.p0, sl.i == G.sc_focus);
        if (pending < 0 && (res[k].focused || (ImGui::IsItemFocused() && io.NavVisible))) focused_now = sl.i;
        if (res[k].clicked && sl.i != G.sc_focus && !res[k].details) { pending = sl.i; G.sc_focus = sl.i; select_item(idx); if (A.pad_mode) G.focus_key = item_key(idx); res[k].clicked = false; }
        if (pending == sl.i) apply_pending_focus(idx);
    }
    if (focused_now >= 0 && focused_now != G.sc_focus) { G.sc_focus = focused_now; select_item(D.flat[focused_now]); }
    // draw far to near so the focused cover sits on top
    std::vector<size_t> order(slots.size());
    for (size_t k = 0; k < order.size(); ++k) order[k] = k;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return std::fabs(slots[a].d) > std::fabs(slots[b].d); });
    for (size_t k : order) {
        const Slot& sl = slots[k];
        int idx = D.flat[sl.i];
        const bool center = std::fabs(sl.d) < 0.5f;
        const bool ghost = is_ghost(idx);
        ImU32 c0, c1;
        colors_of(idx, &c0, &c1);
        float lift = center ? sp(0.5f) * (1.f - std::fabs(sl.d) * 2.f) : 0.f;
        ImVec2 p0(sl.p0.x, sl.p0.y - lift), p1(sl.p1.x, sl.p1.y - lift);
        float cr = sp(1.5f);
        ui::shadow(dl, p0, p1, cr, 1.f + (center ? 0.9f : 0.f), sp(1.f) * sl.alpha);
        if (center) ui::glow(dl, p0, p1, cr, c1, 0.55f);
        ArtRef art = art_of(idx);
        TileSpec ts; ts.title = item_title(idx); ts.c0 = c0; ts.c1 = c1;
        float dim = ghost ? sl.dim * 0.8f : sl.dim;
        if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, p0, p1, cr, dim, sl.alpha);
        else draw_no_art(dl, ts, p0, p1, cr, dim * 0.9f + 0.1f);
        if (art.loading) ui::shimmer(dl, p0, p1, cr, IM_COL32(0, 0, 0, 0));
        if (ghost) dl->AddRectFilled(p0, p1, IM_COL32(96, 99, 108, 120), cr);
        if (!center) dl->AddRectFilled(p0, p1, ui::with_alpha(ui::SCRIM, 0.25f + 0.2f * (1.f - sl.alpha)), cr);
        dl->AddRect(p0, p1, ui::scale_alpha(center ? ui::mix(ui::HAIRLINE, IM_COL32_WHITE, 0.35f) : ui::HAIRLINE, sl.alpha), cr, 0, center ? sp(0.2f) : 1.f);
        const TileResult& r = res[(size_t)(&sl - &slots[0])];
        if (center) {
            if (r.focused) ui::focus_ring(dl, p0, p1, cr);
            if (!ghost) {
                if (gstate(A.lib[idx].key).favorite) {
                    dl->AddCircleFilled(ImVec2(p1.x - sp(2.f), p0.y + sp(2.f)), sp(1.4f), IM_COL32(0, 0, 0, 140), 24);
                    ui::icon(dl, ui::Icon::StarFilled, ImVec2(p1.x - sp(2.f), p0.y + sp(2.f)), sp(1.9f), IM_COL32(255, 214, 70, 255));
                }
            }
        }
    }
    for (size_t k = 0; k < slots.size(); ++k) deliver(D.flat[slots[k].i], res[k]);

    // info under the carousel
    {
        const int idx = fidx;
        const bool ghost = is_ghost(idx);
        float y = title_y;
        ImFont* tf = tfont;
        std::string title = ui::ellipsize(tf, tf->FontSize, item_title(idx), W * 0.8f);
        ImVec2 ts = tf->CalcTextSizeA(tf->FontSize, FLT_MAX, 0.f, title.c_str());
        ui::text_shadowed(dl, tf, tf->FontSize, ImVec2(cx - ts.x * 0.5f, y), IM_COL32_WHITE, title.c_str());
        y = pills_y;
        // meta pills, centred
        std::vector<std::pair<std::string, ImU32>> pills;
        ImU32 a, b;
        console_colors(item_sys(idx), &a, &b);
        pills.push_back({hub::console_name(item_sys(idx)), ui::mix(a, IM_COL32_BLACK, 0.3f)});
        int np = item_players(idx);
        pills.push_back({np >= 2 ? "1-" + std::to_string(np) + " players" : std::string("1 player"), IM_COL32(255, 255, 255, 34)});
        if (!ghost) {
            const hub::GameEntry& e = A.lib[idx];
            const hub::GameState& gs = gstate(e.key);
            const GameInfo* cat = catalog_for(e);
            if (cat && cat->year) pills.push_back({std::to_string(cat->year), IM_COL32(255, 255, 255, 34)});
            pills.push_back({gs.last_played ? "Played " + fmt_ago(gs.last_played) : std::string("Not played yet"), IM_COL32(255, 255, 255, 34)});
            if (gs.play_seconds) pills.push_back({hubui::pt::hours_compact(gs.play_seconds), IM_COL32(255, 255, 255, 34)});
            if (entry_online(e)) pills.push_back({"Online", ui::mix(ui::GOOD, IM_COL32_BLACK, 0.35f)});
        } else pills.push_back({"A friend's game", IM_COL32(255, 255, 255, 34)});
        ImFont* cf = ui::F.cap;
        float tw = 0;
        for (auto& p : pills) tw += cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, p.first.c_str()).x + sp(2.5f) + sp(0.75f);
        tw -= sp(0.75f);
        float x = cx - std::min(tw, W - sp(4.f)) * 0.5f;
        for (auto& p : pills) {
            float pw = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, p.first.c_str()).x + sp(2.5f);
            if (x + pw > org.x + W - sp(2.f)) break;
            ui::pill_at(dl, ImVec2(x, y), p.first.c_str(), p.second, IM_COL32_WHITE);
            x += pw + sp(0.75f);
        }
    }
    // footer: hints and position
    {
        ImFont* cf = ui::F.cap;
        char cnt[48];
        std::snprintf(cnt, sizeof cnt, "%d / %d", G.sc_focus + 1, n);
        float cw = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, cnt).x;
        float fy = foot_y;
        dl->AddText(cf, cf->FontSize, ImVec2(org.x + W - sp(2.f) - cw, fy + sp(0.25f)), ui::TEXT2, cnt);
        const bool pad = A.pad_mode;
        float w1 = ui::key_hint_width(pad, "A", "Enter", "Open"), w2 = ui::key_hint_width(pad, "X", "F", "Favorite"), w3 = ui::key_hint_width(pad, "RS", "Ctrl+Dn", D.letter_mode ? "Next letter" : "Next group");
        float hx = cx - (w1 + w2 + w3 + sp(4.f)) * 0.5f;
        hx += ui::key_hint(dl, ImVec2(hx, fy), pad, "A", "Enter", "Open") + sp(2.f);
        hx += ui::key_hint(dl, ImVec2(hx, fy), pad, "X", "F", "Favorite") + sp(2.f);
        ui::key_hint(dl, ImVec2(hx, fy), pad, "RS", "Ctrl+Dn", D.letter_mode ? "Next letter" : "Next group");
        const std::string lab = G.sc_focus < (int)D.labels.size() ? D.labels[G.sc_focus] : std::string();
        if (!lab.empty()) dl->AddText(ui::F.h2, ui::F.h2->FontSize, ImVec2(org.x + sp(2.f), fy - sp(0.75f)), ui::scale_alpha(ui::TEXT, 0.7f), lab.c_str());
    }
    ImGui::SetCursorScreenPos(org);
    ImGui::Dummy(sz);
}

/* ---------- Shelf: a wooden bookcase of cartridges, discs and cases (shelf_logic.h / shelf_draw.h) ---------- */

struct ShelfAnim { hubui::shelf::Spring sp; int stamp = 0; };
struct ShelfState {
    hubui::shelf::Layout lay;
    std::vector<hubui::shelf::Media> media;
    std::vector<hubui::shelf::Dims> dims;
    uint64_t lsig = ~0ull;
    std::unordered_map<std::string, ShelfAnim> anim;
    int frame = 0;
    int shot_focus = -1;
    float maxh = 1.f;
    uint64_t maxh_sig = ~0ull;
} SH;

bool reduced_motion() {   // Settings > Reduce motion (first-run default follows Windows "Show animations", see display.h); HUB_REDUCE_MOTION forces it for shots
    static const bool forced = std::getenv("HUB_REDUCE_MOTION") != nullptr;
    return ui::reduce_motion || forced;
}

void draw_shelf(Disp& D) {
    using namespace hubui::shelf;
    ImGuiIO& io = ImGui::GetIO();
    ImGuiContext& gctx = *ImGui::GetCurrentContext();
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int n = (int)D.flat.size();
    if (n == 0) { ImGui::Indent(sp(1.5f)); add_games_banner(); ImGui::Unindent(sp(1.5f)); return; }
    const bool mixed = A.st.lib_shelf_mixed;
    const bool reduced = reduced_motion();
    const float view_h = ImGui::GetWindowHeight();
    const float cabinet_x0 = win->Pos.x;
    const float cabinet_x1 = win->Pos.x + win->Size.x - (win->ScrollbarY ? ImGui::GetStyle().ScrollbarSize : 0.f);
    const float width = cabinet_x1 - cabinet_x0;
    float B = std::max(sp(11.f), std::min(sp(22.f), view_h * 0.19f));
    {   // a library of only short things (DS cases ...) gets bigger items: the tallest item on the shelves is about B tall
        if (SH.maxh_sig != D.sig) {
            float mh = 0.4f;
            for (int p = 0; p < n; ++p) mh = std::max(mh, item_dims(media_for(item_sys(D.flat[(size_t)p])), 0).h);
            SH.maxh = std::max(0.68f, std::min(1.f, mh));
            SH.maxh_sig = D.sig;
        }
        B /= SH.maxh;
    }
    const shelfdraw::RowGeo geo = shelfdraw::row_geo(B, ui::F.cap->FontSize);

    const uint64_t lsig = mixh(mixh(mixh(mixh(D.sig, 77), (uint64_t)width), (uint64_t)(B * 4.f)), (uint64_t)(mixed ? 1 : 0));
    if (lsig != SH.lsig || SH.media.size() != D.flat.size()) {
        SH.media.resize((size_t)n);
        SH.dims.resize((size_t)n);
        std::vector<float> slot((size_t)n);
        for (int p = 0; p < n; ++p) {
            const int idx = D.flat[(size_t)p];
            SH.media[(size_t)p] = media_for(item_sys(idx));
            SH.dims[(size_t)p] = item_dims(SH.media[(size_t)p], hash32(item_key(idx)));
            slot[(size_t)p] = (rest_width(SH.dims[(size_t)p]) + slot_gap(SH.media[(size_t)p])) * B;
        }
        SH.lay = pack_rows(slot, D.sec_size, width, geo.side + B * 0.22f, geo.pitch, sp(0.5f), sp(1.f));
        SH.lsig = lsig;
        G.L = SH.lay.L;
        G.lsig = lsig;
    }
    const Layout& lay = SH.lay;
    const hubui::VLayout& L = lay.L;

    const float x0 = ImGui::GetCursorPosX();
    const float base_y = ImGui::GetCursorPosY();
    const float top_screen = ImGui::GetCursorScreenPos().y;
    float scroll = ImGui::GetScrollY();
    if (G.scroll_abs >= 0.f) { scroll = G.scroll_abs; G.scroll_abs = -1.f; ImGui::SetScrollY(scroll); }
    else if (G.jump_pos >= 0 && G.jump_pos < n && G.jump_pos < (int)L.item_row.size()) {
        const int r = L.item_row[(size_t)G.jump_pos];
        if (G.jump_top) scroll = std::max(0.f, base_y + L.rows[(size_t)r].y - sp(0.5f));
        else if (G.jump_center) scroll = std::max(0.f, base_y + L.rows[(size_t)r].y - view_h * 0.35f);
        else scroll = std::max(0.f, hubui::scroll_to_row(L, r, scroll - base_y, view_h - base_y, geo.head * 0.5f, 1e9f) + base_y);
        ImGui::SetScrollY(scroll);
        G.jump_pos = -1;
        G.jump_center = false;
    }
    {   // what is at the top of the viewport (the A-Z strip highlights its letter)
        int tf = -1, tl = -1;
        const float ty = std::max(0.f, scroll - base_y) + geo.pitch * 0.5f;
        hubui::rows_in_range(L, ty, ty, &tf, &tl);
        G.top_pos = (tf >= 0 && L.rows[(size_t)tf].first < n) ? L.rows[(size_t)tf].first : -1;
        G.top_title.clear();
    }
    // Left / Right at the end of a shelf continue on the next / previous one (ImGui's own move finds nothing there)
    const bool nav_mine = gctx.NavWindow == win;
    if (nav_mine && A.focused && !A.proc && !io.WantTextInput) {
        const int dk = (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true)) ? -1
                       : (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true)) ? 1 : 0;
        const int cur = selected_pos(D);
        if (dk && cur >= 0 && cur < n) {
            const hubui::VRow& cr = L.rows[(size_t)row_of(lay, cur)];
            const bool edge = dk < 0 ? cur == cr.first : cur == cr.first + cr.count - 1;
            const int t = move_focus(lay, cur, dk < 0 ? Dir::Left : Dir::Right);
            if (edge && t != cur) jump(D, t, false);
        }
    }

    int f = -1, l = -1;
    hubui::rows_in_range(L, scroll - base_y - view_h, scroll - base_y + view_h * 2.f, &f, &l);
    ++SH.frame;
    struct Front { int pos; ImVec2 base; SlidePose pose; shelfdraw::Item item; bool ring, active; float s; };
    std::vector<Front> fronts;
    const std::string sel_key = selected_key();
    const float dt = io.DeltaTime;
    for (int r = f; r >= 0 && r <= l; ++r) {
        const hubui::VRow& row = L.rows[(size_t)r];
        const float ry = top_screen + row.y - 0.f;   // screen y of the row top (the cursor already moved with the scroll)
        const float ry_local = base_y + row.y;
        const bool vis = ry_local + row.h > scroll - sp(1.f) && ry_local < scroll + view_h + sp(1.f);
        if (vis) {
            std::string label, count;
            ImU32 chip = ui::ACCENT;
            const int sec = lay.row_sec[(size_t)r];
            if (!mixed) {
                label = D.sec_title[(size_t)sec];
                if (lay.row_first[(size_t)r]) count = std::to_string(D.sec_size[(size_t)sec]);
                ImU32 a, b;
                console_colors(D.sec_sys[(size_t)sec], &a, &b);
                chip = a;
            } else if (r == 0) { label = "All games"; count = std::to_string(n); }
            shelfdraw::bay(dl, geo, cabinet_x0, cabinet_x1, ry, label, count, chip, lay.row_first[(size_t)r] != 0);
        }
        for (int k = 0; k < row.count; ++k) {
            const int pos = row.first + k;
            const int idx = D.flat[(size_t)pos];
            const Dims& dm = SH.dims[(size_t)pos];
            const Media md = SH.media[(size_t)pos];
            const float slot_w = lay.w[(size_t)pos];
            const float item_w = slot_w - slot_gap(md) * B;
            const ImVec2 base(cabinet_x0 + lay.x[(size_t)pos] + item_w * 0.5f, ry + geo.base_dy);
            const float hit_h = dm.h * B + geo.depth * 0.5f + sp(0.25f);
            ImGui::SetCursorScreenPos(ImVec2(cabinet_x0 + lay.x[(size_t)pos], base.y - hit_h + geo.depth * 0.5f));
            const std::string key = item_key(idx);
            const bool sel = key == sel_key;
            TileResult res = interact("sh|" + key, ImVec2(slot_w, hit_h), sel);
            auto it = SH.anim.find(key);
            const bool active = A.pad_mode ? res.focused : (res.hovered || res.focused);
            const float target = slide_target(active, sel);
            if (!vis && !res.focused && !res.hovered) { if (it != SH.anim.end()) SH.anim.erase(it); deliver(idx, res); apply_pending_focus(idx); continue; }
            if (it == SH.anim.end() && target > 0.f) it = SH.anim.emplace(key, ShelfAnim()).first;
            ShelfAnim* an = it == SH.anim.end() ? nullptr : &it->second;
            if (an) { an->stamp = SH.frame; spring_step(an->sp, target, dt, reduced); }
            const float s = an ? an->sp.x : 0.f, v = an ? an->sp.v : 0.f;
            const bool resting = !an || (an->sp.x == 0.f && target == 0.f && an->sp.v == 0.f);
            if (an && resting) { SH.anim.erase(it); an = nullptr; }
            shelfdraw::Item di;
            di.media = md; di.dims = dm; di.seed = hash32(key);
            di.title = item_title(idx);
            di.sys_name = short_sys(item_sys(idx));
            colors_of(idx, &di.c0, &di.c1);
            di.ghost = is_ghost(idx);
            di.fav = !di.ghost && gstate(A.lib[idx].key).favorite;
            if (is_cart(md) || s > 0.02f) { ArtRef art = art_of(idx); di.art = art.tex; di.art_w = art.w; di.art_h = art.h; }
            const SlidePose pose = slide_pose(md, dm, s, v, reduced);
            if (resting) { if (vis) shelfdraw::item(dl, di, B, base, pose, false); }
            else fronts.push_back({pos, base, pose, di, res.focused && io.NavVisible, active, s});
            deliver(idx, res);
            apply_pending_focus(idx);
        }
    }
    // pulled-out games are drawn last (strongest on top) so they overlap their neighbours and the shelf above
    std::stable_sort(fronts.begin(), fronts.end(), [](const Front& a, const Front& b) { return a.s < b.s; });
    for (const Front& fr : fronts) {
        shelfdraw::item(dl, fr.item, B, fr.base, fr.pose, fr.ring);
        if (fr.active && fr.s > 0.5f) {
            std::string sub = fr.item.sys_name;
            const int idx = D.flat[(size_t)fr.pos];
            if (!is_ghost(idx)) { const hub::GameState& gs = gstate(A.lib[idx].key); if (gs.last_played) sub += "  -  played " + fmt_ago(gs.last_played); }
            shelfdraw::caption(dl, ImVec2(fr.base.x, fr.base.y + B * (0.07f + fr.pose.lift)), fr.item.title, sub, cabinet_x0 + geo.side, cabinet_x1 - geo.side, std::min(1.f, (fr.s - 0.5f) * 3.f));
        }
    }
    for (auto it = SH.anim.begin(); it != SH.anim.end();) { if (it->second.stamp != SH.frame) it = SH.anim.erase(it); else ++it; }   // rows that left the page
    ImGui::SetCursorPos(ImVec2(x0, base_y + L.total_h));
    ImGui::Dummy(ImVec2(1.f, 1.f));
}

#include "cases_view.inc"

/* LT / RT: a page up / down that keeps the focused game at the same height (ImGui's own page move would land on the selected bar below). */
void page_move(Disp& D, int dir, float view_h) {
    const int n = (int)D.flat.size();
    int cur = selected_pos(D);
    if (n == 0) return;
    if (cur < 0) cur = 0;
    if (lib_layout() == LibLayout::Showcase || lib_layout() == LibLayout::Cases) { jump(D, std::max(0, std::min(n - 1, cur + dir * 8)), false); return; }
    const hubui::VLayout& L = G.L;
    if (cur >= (int)L.item_row.size() || L.rows.empty()) return;
    const hubui::VRow& cr = L.rows[L.item_row[cur]];
    const int col = cur - cr.first;
    const float yt = cr.y + (float)dir * view_h * 0.85f;
    int f = -1, l = -1;
    hubui::rows_in_range(L, yt, yt + cr.h, &f, &l);   // a point can fall in the gap between rows, so ask for a row's worth
    int r = f;
    if (r > 0 && L.rows[r - 1].kind == 1 && std::fabs(L.rows[r - 1].y - yt) < std::fabs(L.rows[r].y - yt)) --r;   // the nearer of the two rows
    if (r < 0) r = dir > 0 ? (int)L.rows.size() - 1 : 0;
    if (L.rows[r].kind == 0) r = std::min((int)L.rows.size() - 1, r + 1);
    if (L.rows[r].kind != 1) return;
    int t = L.rows[r].first + std::min(col, L.rows[r].count - 1);
    if (t == cur) return;
    const float cur_scroll = ImGui::GetScrollY();
    jump(D, t, false);
    G.jump_pos = -1;                                      // no reveal: the exact scroll below keeps the same relative height
    G.scroll_abs = std::max(0.f, cur_scroll + (L.rows[r].y - cr.y));
}

/* ---------- the jump strip ---------- */

void draw_strip(Disp& D, float h, float w, bool in_nav_window) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::InvisibleButton("##jumpstrip", ImVec2(w, h));
    ImGui::PopItemFlag();
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool letters = D.letter_mode;
    static const char* az[] = {"A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z","#"};
    const int n = letters ? 27 : (int)D.marks.size();
    const float sh = h / (float)n;
    dl->AddRectFilled(p0, p1, ui::with_alpha(ui::PANEL, 0.85f), w * 0.5f);
    dl->AddRect(p0, p1, ui::HAIRLINE, w * 0.5f, 0, 1.f);
    ImFont* f = ui::F.cap;
    const int cur_pos = lib_layout() == LibLayout::Showcase ? G.sc_focus : (G.top_pos >= 0 ? G.top_pos : selected_pos(D));
    const int cr = cur_pos >= 0 ? hubui::run_of(D.runs, cur_pos) : -1;
    const std::string cur_label = cr >= 0 ? D.runs[cr].label : std::string();
    int mouse_slot = -1;
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (hot) mouse_slot = hubui::strip_slot(io.MousePos.y - p0.y, h, n);
    if (ImGui::IsItemActive() && mouse_slot >= 0) {
        int mk = letters ? D.targets[mouse_slot] : mouse_slot;
        if (mk >= 0 && mk != G.strip_last) { G.strip_last = mk; jump(D, D.marks[mk].first, true); }
    }
    if (!ImGui::IsItemActive()) G.strip_last = -1;
    const int skip = std::max(1, (int)std::ceil((f->FontSize + sp(0.25f)) / sh));
    for (int s = 0; s < n; ++s) {
        const std::string lab = letters ? az[s] : D.marks[s].label;
        const bool present = !letters || (D.targets[s] >= 0 && D.marks[D.targets[s]].label == lab);
        const bool on = lab == cur_label;
        const float cy = p0.y + sh * ((float)s + 0.5f);
        if (s % skip != 0 && !on && s != mouse_slot) continue;
        if (on) dl->AddCircleFilled(ImVec2((p0.x + p1.x) * 0.5f, cy), std::min(w * 0.5f - 1.f, std::max(f->FontSize * 0.62f, 1.f)), ui::ACCENT);
        ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, lab.c_str());
        ImU32 col = on ? IM_COL32_WHITE : present ? (s == mouse_slot ? ui::TEXT : ui::TEXT2) : ui::with_alpha(ui::TEXT3, 0.45f);
        dl->AddText(f, f->FontSize, ImVec2((p0.x + p1.x - ts.x) * 0.5f, cy - ts.y * 0.5f), col, lab.c_str());
    }
    if (hot && mouse_slot >= 0) {   // bubble with the big letter while pointing / dragging
        const std::string lab = letters ? az[mouse_slot] : D.marks[mouse_slot].label;
        ImFont* bf = ui::F.hero ? ui::F.hero : ui::F.h2;
        ImVec2 ts = bf->CalcTextSizeA(bf->FontSize, FLT_MAX, 0.f, lab.c_str());
        float bw = std::max(ts.x + sp(3.f), sp(7.f)), bh = ts.y + sp(2.f);
        ImVec2 b0(p0.x - sp(1.5f) - bw, std::max(p0.y, std::min(p1.y - bh, io.MousePos.y - bh * 0.5f)));
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(b0, b0 + ImVec2(bw, bh), ui::PANEL3, sp(1.5f));
        fg->AddRect(b0, b0 + ImVec2(bw, bh), ui::ACCENT, sp(1.5f), 0, sp(0.2f));
        fg->AddText(bf, bf->FontSize, ImVec2(b0.x + (bw - ts.x) * 0.5f, b0.y + (bh - ts.y) * 0.5f), ui::TEXT, lab.c_str());
    }
    (void)in_nav_window;
}

}  // namespace

/* ======================= public ======================= */

LibLayout lib_layout() { return hubui::layout_from_key(A.st.lib_layout); }
bool lib_layout_has_bar() { return lib_layout() != LibLayout::Showcase && lib_layout() != LibLayout::Cases; }

/* Cases hooks (frame.cpp / palette_ui.cpp / tv_mode.cpp): the pad buttons it uses must not also do their global jobs. */
bool lib_cases_active() { return lib_layout() == LibLayout::Cases && A.page == Page::Library && A.view == LibView::All; }
bool lib_cases_back(bool from_esc) {   // B / Esc: close the menu, leave a flipped / inspected case; Esc on the plain carousel opens the menu
    if (!lib_cases_active()) return false;
    if (nav_close_modal()) return true;   // the sort picker / quick-jump overlay
    if (CS.menu_open) { CS.menu_open = false; A.nav_seed = true; shot_trace("cases-menu", "closed"); return true; }
    if (hubui::cases::on_back(&CS.mode)) { shot_trace("cases-back", "browse"); return true; }
    if (from_esc) { lib_cases_menu_toggle(); return true; }
    return false;
}
bool lib_cases_nav_blocking() { return nav_blocking(); }   // the sort picker / quick-jump overlay own the keys (frame.cpp: F / I / S must not reach the selected game)
void lib_cases_menu_toggle() {
    CS.menu_open = !CS.menu_open;
    if (CS.menu_open) { CS.menu_focus_req = true; CS.mode = hubui::cases::Mode::Browse; }
    else A.nav_seed = true;
    shot_trace("cases-menu", CS.menu_open ? "open" : "closed");
}
void lib_cases_closed() { CS.menu_open = false; CS.menu_t = 0.f; }   // the layout is not showing: forget an open menu
void lib_cases_tv_enter() {   // TV mode opened with "TV mode opens in Cases view" on: All Games, Cases layout
    if (!A.st.tv_cases) return;
    if (lib_layout() != LibLayout::Cases) lib_set_layout(LibLayout::Cases);
    A.view = LibView::All;
}

void lib_set_layout(LibLayout l) {
    if (l == lib_layout()) return;
    if (l == LibLayout::Cases) { CS.intro = 0.f; CS.mode = hubui::cases::Mode::Browse; CS.init = false; CS.land_pending = false; }   // the carousel fades in on the selected game
    A.st.lib_layout = hubui::layout_key(l);
    mark_dirty();
    G.lsig = ~0ull;
    int p = selected_pos(G.D);
    if (p >= 0) { G.jump_pos = p; G.jump_top = false; G.sc_init = false; }
    A.nav_seed = true;
    shot_trace("libview-layout", hubui::layout_key(l));
}

void lib_sort_chip_changed() {
    if (!A.st.lib_list_col.empty()) { A.st.lib_list_col.clear(); A.st.lib_list_desc = false; }
}

static float switcher_cell() { return ImGui::GetFontSize() + sp(1.75f); }
static float shelf_toggle_w() { return ImGui::CalcTextSize("Mixed").x + sp(3.5f); }
float lib_switcher_width() {
    if (A.view != LibView::All) return 0.f;
    return switcher_cell() * (float)hubui::kLibLayouts * 1.2f + sp(0.5f) + (lib_layout() == LibLayout::Shelf ? shelf_toggle_w() + sp(1.f) : 0.f);
}

void lib_switcher() {
    if (A.view != LibView::All) return;
    const float h = switcher_cell(), cw = h * 1.2f;
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float nc = (float)hubui::kLibLayouts;
    dl->AddRectFilled(o, o + ImVec2(cw * nc, h), ui::PANEL, h * 0.5f);
    dl->AddRect(o, o + ImVec2(cw * nc, h), ui::HAIRLINE, h * 0.5f, 0, 1.f);
    const LibLayout cur = lib_layout();
    ImGui::PushID("libswitch");
    for (int i = 0; i < hubui::kLibLayouts; ++i) {
        ImGui::SetCursorScreenPos(o + ImVec2(cw * (float)i, 0));
        ImGui::PushID(i);
        bool pressed = ImGui::InvisibleButton("l", ImVec2(cw, h), ImGuiButtonFlags_EnableNav);
        bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
        const bool on = cur == (LibLayout)i;
        ImVec2 q0 = p0 + ImVec2(sp(0.2f), sp(0.2f)), q1 = p1 - ImVec2(sp(0.2f), sp(0.2f));
        if (on) dl->AddRectFilled(q0, q1, ui::ACCENT, h * 0.5f);
        else if (hov || foc) dl->AddRectFilled(q0, q1, ui::FRAME_HOV, h * 0.5f);
        const ImU32 col = on ? IM_COL32_WHITE : (hov || foc) ? ui::TEXT : ui::TEXT2;
        const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
        const float u = h * 0.19f;   // glyph unit
        switch ((LibLayout)i) {
            case LibLayout::Grid:
                for (int a = 0; a < 2; ++a) for (int b = 0; b < 2; ++b) dl->AddRectFilled(ImVec2(c.x - u * 1.05f + a * u * 1.15f, c.y - u * 1.05f + b * u * 1.15f), ImVec2(c.x - u * 0.1f + a * u * 1.15f, c.y - u * 0.1f + b * u * 1.15f), col, 1.5f);
                break;
            case LibLayout::List:
                for (int b = 0; b < 3; ++b) {
                    float y = c.y + (b - 1) * u * 0.95f;
                    dl->AddRectFilled(ImVec2(c.x - u * 1.1f, y - u * 0.22f), ImVec2(c.x - u * 0.5f, y + u * 0.22f), col, 1.f);
                    dl->AddRectFilled(ImVec2(c.x - u * 0.2f, y - u * 0.22f), ImVec2(c.x + u * 1.15f, y + u * 0.22f), col, 1.f);
                }
                break;
            case LibLayout::Compact:
                for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) dl->AddRectFilled(ImVec2(c.x - u * 1.2f + a * u * 0.85f, c.y - u * 1.2f + b * u * 0.85f), ImVec2(c.x - u * 0.6f + a * u * 0.85f, c.y - u * 0.6f + b * u * 0.85f), col, 0.5f);
                break;
            case LibLayout::Shelf:   // two planks with a few standing games
                for (int b = 0; b < 2; ++b) {
                    const float y = c.y + (b ? 1.35f : -0.15f) * u;
                    dl->AddRectFilled(ImVec2(c.x - u * 1.5f, y), ImVec2(c.x + u * 1.5f, y + u * 0.34f), col, 1.f);
                    dl->AddRectFilled(ImVec2(c.x - u * 1.25f, y - u * 0.95f), ImVec2(c.x - u * 0.7f, y - 0.06f * u), col, 1.f);
                    dl->AddRectFilled(ImVec2(c.x - u * 0.45f, y - u * 0.7f), ImVec2(c.x + u * 0.2f, y - 0.06f * u), col, 1.f);
                    dl->AddRectFilled(ImVec2(c.x + u * 0.45f, y - u * 1.0f), ImVec2(c.x + u * 0.75f, y - 0.06f * u), col, 1.f);
                    dl->AddRectFilled(ImVec2(c.x + u * 0.9f, y - u * 0.8f), ImVec2(c.x + u * 1.2f, y - 0.06f * u), col, 1.f);
                }
                break;
            case LibLayout::Cases:   // a tall case between two turned ones, standing on a floor line
                dl->AddRectFilled(ImVec2(c.x - u * 0.62f, c.y - u * 1.35f), ImVec2(c.x + u * 0.62f, c.y + u * 0.95f), col, 1.5f);
                dl->AddQuadFilled(ImVec2(c.x - u * 1.5f, c.y - u * 0.9f), ImVec2(c.x - u * 0.95f, c.y - u * 0.62f), ImVec2(c.x - u * 0.95f, c.y + u * 0.62f), ImVec2(c.x - u * 1.5f, c.y + u * 0.9f), col);
                dl->AddQuadFilled(ImVec2(c.x + u * 0.95f, c.y - u * 0.62f), ImVec2(c.x + u * 1.5f, c.y - u * 0.9f), ImVec2(c.x + u * 1.5f, c.y + u * 0.9f), ImVec2(c.x + u * 0.95f, c.y + u * 0.62f), col);
                dl->AddRectFilled(ImVec2(c.x - u * 1.5f, c.y + u * 1.25f), ImVec2(c.x + u * 1.5f, c.y + u * 1.55f), col, 1.f);
                break;
            default:   // showcase: a big cover between two small ones
                dl->AddRectFilled(ImVec2(c.x - u * 0.55f, c.y - u * 1.1f), ImVec2(c.x + u * 0.55f, c.y + u * 1.1f), col, 1.5f);
                dl->AddRectFilled(ImVec2(c.x - u * 1.45f, c.y - u * 0.65f), ImVec2(c.x - u * 0.8f, c.y + u * 0.65f), col, 1.f);
                dl->AddRectFilled(ImVec2(c.x + u * 0.8f, c.y - u * 0.65f), ImVec2(c.x + u * 1.45f, c.y + u * 0.65f), col, 1.f);
                break;
        }
        if (foc) ui::focus_ring(dl, p0, p1, h * 0.5f);
        if (hov) ImGui::SetTooltip("%s view", hubui::layout_name((LibLayout)i));
        if (pressed) lib_set_layout((LibLayout)i);
        ImGui::PopID();
    }
    ImGui::PopID();
    float total_w = cw * nc;
    if (cur == LibLayout::Shelf) {   // Grouped (a shelf per console) / Mixed (one flow in the Sort chip's order)
        const float tw = shelf_toggle_w();
        ImGui::SetCursorScreenPos(o + ImVec2(total_w + sp(1.f), 0));
        ImGui::PushID("shelfmix");
        const bool mixed = A.st.lib_shelf_mixed;
        const bool pressed = ImGui::InvisibleButton("m", ImVec2(tw, h), ImGuiButtonFlags_EnableNav);
        const bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
        dl->AddRectFilled(p0, p1, mixed ? ui::ACCENT : (hov || foc) ? ui::FRAME_HOV : ui::PANEL, h * 0.5f);
        dl->AddRect(p0, p1, mixed ? ui::ACCENT : ui::HAIRLINE, h * 0.5f, 0, 1.f);
        const char* tx = mixed ? "Mixed" : "Mixed";
        const ImVec2 ts = ImGui::CalcTextSize(tx);
        dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f), mixed ? IM_COL32_WHITE : ui::TEXT2, tx);
        if (foc) ui::focus_ring(dl, p0, p1, h * 0.5f);
        if (hov) ImGui::SetTooltip(mixed ? "Mixed: every game on the shelves in the Sort order. Click for a shelf per console" : "A shelf per console. Click to mix them in the Sort order");
        if (pressed) { A.st.lib_shelf_mixed = !mixed; mark_dirty(); G.lsig = ~0ull; const int p = selected_pos(G.D); if (p >= 0) { G.jump_pos = p; G.jump_center = true; } shot_trace("libview-shelf-mixed", A.st.lib_shelf_mixed ? "1" : "0"); }
        ImGui::PopID();
        total_w += tw + sp(1.f);
    }
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(total_w, h));
}

void lib_body(float bar_h) {
    const LibLayout lay = lib_layout();
    Disp& D = build_disp(lay);
    if (!G.layout_seen || G.last_layout != lay) { G.layout_seen = true; G.last_layout = lay; G.lsig = ~0ull; }
    if (A.nav_seed) { G.reveal = true; G.reveal_sig = ~0ull; G.reveal_quiet = 0; G.reveal_key = selected_key(); }
    if (G.reveal) {   /* entering All Games / switching layout: bring the selected game into view (so the pad focus lands on it), and again whenever
                       * the list changes under us (scan, art, friends loading) until the user scrolls or moves the selection. */
        ImGuiIO& rio = ImGui::GetIO();
        if (rio.MouseWheel != 0.f || ImGui::IsMouseClicked(0) || A.grid_scroll_top) G.reveal = false;
        else if (D.sig != G.reveal_sig) {   // first frame, or the list changed: (re)reveal; the selection may have been settled by ensure_selection meanwhile
            int p = selected_pos(D);
            if (p >= 0) { G.jump_pos = p; G.jump_top = false; G.jump_center = true; G.sc_init = false; }
            G.reveal_sig = D.sig;
            G.reveal_quiet = 0;
            G.reveal_key = selected_key();
        } else if (selected_key() != G.reveal_key || ++G.reveal_quiet > 60) G.reveal = false;
    }
    const bool strip_on = D.marks.size() >= 2 && D.flat.size() >= 10 && lay != LibLayout::Cases;
    float strip_w = strip_on ? sp(3.f) : 0.f;
    if (strip_on && !D.letter_mode)   // console / genre labels are wider than a letter
        for (const hubui::JumpRun& m : D.marks) strip_w = std::max(strip_w, ui::F.cap->CalcTextSizeA(ui::F.cap->FontSize, FLT_MAX, 0.f, m.label.c_str()).x + sp(1.25f));
    const float strip_gap = strip_on ? sp(0.75f) : 0.f;
    ImGuiStyle& st = ImGui::GetStyle();
    const float pad = sp(1.5f);
    const float full_w = ImGui::GetContentRegionAvail().x;
    const float child_w = full_w + pad - strip_w - strip_gap;
    if (lay == LibLayout::List) {
        const float inner = full_w - strip_w - strip_gap - st.ScrollbarSize - pad;
        draw_list_header(ImGui::GetCursorScreenPos().x, inner);
    }
    const bool flush = lay == LibLayout::Showcase || lay == LibLayout::Shelf || lay == LibLayout::Cases;   // these fill the whole child
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, flush ? ImVec2(0, 0) : ImVec2(pad, pad));
    if (A.grid_scroll_top) { ImGui::SetNextWindowScroll(ImVec2(0.f, 0.f)); A.grid_scroll_top = false; G.sc_init = false; G.jump_pos = -1; }
    ImGuiWindowFlags wf = 0;
    if (lay == LibLayout::List) wf |= ImGuiWindowFlags_AlwaysVerticalScrollbar;
    if (lay == LibLayout::Showcase || lay == LibLayout::Cases) wf |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground;
    if (!flush) ImGui::SetCursorPosX(ImGui::GetCursorPosX() - pad);
    ImGui::BeginChild("grid", ImVec2(flush ? full_w - strip_w - strip_gap : child_w, -bar_h), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding, wf);
    ImGui::PopStyleVar();
    A.page_scrollable = true;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* me = ImGui::GetCurrentWindow();
    const bool mine = g.NavWindow == me;
    // pad shortcut: right stick = next / previous letter (or console group)
    if (mine && !ImGui::GetIO().WantTextInput && A.focused && !A.proc && D.runs.size() > 1 && lay != LibLayout::Cases) {
        const bool ctrl = ImGui::GetIO().KeyCtrl;   // keyboard: Ctrl+Down / Ctrl+Up
        int dir = (ImGui::IsKeyPressed(ImGuiKey_GamepadRStickDown, false) || (ctrl && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))) ? 1
                  : (ImGui::IsKeyPressed(ImGuiKey_GamepadRStickUp, false) || (ctrl && ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))) ? -1 : 0;
        if (dir) {
            if (!ctrl) A.pad_mode = true;
            int cur = lay == LibLayout::Showcase ? G.sc_focus : selected_pos(D);
            if (cur < 0) cur = 0;
            int t = hubui::jump_target(D.runs, cur, dir);
            if (t >= 0) jump(D, t, true);
        }
    }
    A.lib_pg = mine;
    if (mine && !ImGui::GetIO().WantTextInput && A.focused && !A.proc && A.pad_mode && lay != LibLayout::Cases) {   // Cases: LT / RT jump sections (cases_nav.inc)
        int pdir = ImGui::IsKeyPressed(ImGuiKey_GamepadR2, true) ? 1 : ImGui::IsKeyPressed(ImGuiKey_GamepadL2, true) ? -1 : 0;
        if (pdir) page_move(D, pdir, ImGui::GetWindowHeight());
    }
    if (!flush) add_games_banner();
    if (lay == LibLayout::Showcase) draw_showcase(D);
    else if (lay == LibLayout::Shelf) draw_shelf(D);
    else if (lay == LibLayout::Cases) {
        static const bool perf = g_headless && std::getenv("HUB_SHOT_CASES_PERF") != nullptr;   // QA: CPU time of the carousel per frame (shot trace "cases-perf")
        static double acc = 0; static int frames = 0;
        LARGE_INTEGER t0, t1, fq;
        if (perf) QueryPerformanceCounter(&t0);
        draw_cases(D);
        if (perf) {
            QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
            acc += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
            if (++frames == 20) { shot_trace("cases-perf", "avg_ms=" + std::to_string(acc / frames) + " games=" + std::to_string(D.flat.size())); acc = 0; frames = 0; }
        }
    }
    else draw_virtual(D, lay);
    ImGui::EndChild();
    const ImVec2 csz = ImGui::GetItemRectSize();
    if (strip_on) {
        ImGui::SameLine(0, strip_gap);
        draw_strip(D, csz.y, strip_w, mine);
    }
    const int sp_now = selected_pos(D);
    if (sp_now != G.traced) {
        G.traced = sp_now;
        if (sp_now >= 0)
            shot_trace("libview", std::string(hubui::layout_key(lay)) + " pos=" + std::to_string(sp_now) + "/" + std::to_string(D.flat.size()) + " label=" + D.labels[sp_now] + " key=" + item_key(D.flat[sp_now]));
    }
}

/* ======================= screenshot aids ======================= */

void lib_shot_seed() {
    if (const char* cs = std::getenv("HUB_SHOT_CASES"); cs && *cs) {   // Cases screenshots: browse | flip | inspect | cartridge[:<console>] | missingart
        A.st.lib_layout = hubui::layout_key(LibLayout::Cases);
        A.view = LibView::All;
        const std::string m = cs;
        nav_shot_seed(m);   // "sortpicker:release_year+rev", "quickjump:console", "bubble:last_played", "sort:title+rev"
        if (m.rfind("cartridge", 0) == 0) A.sys_filter = m.size() > 10 ? m.substr(10) : std::string("n64");
    }
    if (const char* so = std::getenv("HUB_SHOT_SORT"); so && *so) {   // "most_played" / "title+rev" / "last_played+favs" (same grammar as nav_shot_seed's "sort:...")
        const std::string spec = so;
        A.st.library_sort_rev = spec.find("+rev") != std::string::npos;
        A.st.library_favs_first = spec.find("+favs") != std::string::npos;
        const size_t pl = spec.find('+');
        A.st.library_sort = hubui::sort_key(hubui::sort_from_key(pl == std::string::npos ? spec : spec.substr(0, pl)));
        A.sort_mode = hubui::sort_from_key(A.st.library_sort);
        A.vis_dirty = true;
    }
    const char* v = std::getenv("HUB_SHOT_LIBVIEW");
    if (!v || !*v) return;
    A.st.lib_layout = hubui::layout_key(hubui::layout_from_key(v));
    A.view = LibView::All;
    if (const char* m = std::getenv("HUB_SHOT_SHELF_MIXED")) A.st.lib_shelf_mixed = std::string(m) != "0";   // Shelf: Mixed mode
    if (const char* c = std::getenv("HUB_SHOT_LIBCOL")) {   // "last:desc" / "title:asc"
        std::string s = c;
        auto cut = s.find(':');
        A.st.lib_list_col = s.substr(0, cut);
        A.st.lib_list_desc = cut != std::string::npos && s.substr(cut + 1) == "desc";
    }
}

static SDL_Texture* g_fake[12];
static int g_fake_w = 240, g_fake_h = 260;
static int g_fake_dw[12], g_fake_dh[12];   // per-texture size (HUB_SHOT_FAKE_ART=posters: mixed aspect ratios)
static void fake_register_color(SDL_Texture* tex, SDL_Surface* s) {   // the dominant colour of a fake cover (the stage takes its mood from it)
    if (!tex || !s) return;
    double sr = 0, sg = 0, sb = 0, sw = 0;
    for (int y = 0; y < s->h; y += 2)
        for (int x = 0; x < s->w; x += 2) {
            const uint32_t c = ((const uint32_t*)s->pixels)[y * s->pitch / 4 + x];
            const int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255, mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
            if (mx < 50 || mn > 215) continue;
            const double sat = (mx - mn) / (double)mx;
            if (sat < 0.25) continue;
            const double w = sat * sat;
            sr += r * w; sg += g * w; sb += b * w; sw += w;
        }
    if (sw > 0) CS.fake_cols[tex] = IM_COL32((int)(sr / sw + 0.5), (int)(sg / sw + 0.5), (int)(sb / sw + 0.5), 255);
}
void lib_fake_art_init(SDL_Renderer* ren) {
    if (!std::getenv("HUB_SHOT_FAKE_ART") || !ren) return;
    const bool posters = std::string(std::getenv("HUB_SHOT_FAKE_ART")) == "posters";   // box-art-like posters: gradients, a title banner, mixed aspect ratios
    static const float aspect[12] = {0.72f, 0.70f, 1.0f, 1.33f, 0.68f, 0.75f, 1.0f, 0.72f, 1.5f, 0.70f, 0.8f, 1.0f};
    for (int t = 0; t < 12; ++t) {
        g_fake_dw[t] = g_fake_w; g_fake_dh[t] = g_fake_h;
        if (posters) { g_fake_dh[t] = 300; g_fake_dw[t] = (int)(300.f * aspect[t]); }
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, g_fake_dw[t], g_fake_dh[t], 32, SDL_PIXELFORMAT_ARGB8888);
        if (!s) continue;
        const float hue = (float)t / 12.f;
        auto hsv = [](float h, float sat, float v) {
            float r = 0, g = 0, b = 0;
            int i = (int)(h * 6.f);
            float f = h * 6.f - i, p = v * (1 - sat), q = v * (1 - f * sat), u = v * (1 - (1 - f) * sat);
            switch (i % 6) { case 0: r = v; g = u; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = u; break;
                             case 3: r = p; g = q; b = v; break; case 4: r = u; g = p; b = v; break; default: r = v; g = p; b = q; }
            return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
        };
        const ImU32 top = hsv(hue, 0.65f, 0.95f), bot = hsv(std::fmod(hue + 0.12f, 1.f), 0.8f, 0.35f), acc = hsv(std::fmod(hue + 0.5f, 1.f), 0.7f, 1.f);
        uint32_t* px = (uint32_t*)s->pixels;
        const int FW = g_fake_dw[t], FH = g_fake_dh[t];
        if (posters) {
            uint32_t rs = 977u + (uint32_t)t * 7919u;
            auto rnd = [&]() { rs = rs * 1664525u + 1013904223u; return (rs >> 8) & 0xFFFFu; };
            std::vector<int> rowsw;   // title banner: two or three rows of "lettering" blocks
            for (int y = 0; y < FH; ++y)
                for (int x = 0; x < FW; ++x) {
                    const float k = (float)y / FH, dx = (float)x / FW - 0.5f, dy = k - 0.38f;
                    ImU32 c = ImGui::GetColorU32(ImLerp(ImGui::ColorConvertU32ToFloat4(top), ImGui::ColorConvertU32ToFloat4(bot), k));
                    const float rr = std::sqrt(dx * dx * 1.4f + dy * dy);
                    if (rr < 0.20f) c = acc; else if (rr < 0.23f) c = IM_COL32(255, 255, 255, 255);
                    if (((x - y / 2) / 18) % 4 == 0 && k < 0.55f && rr > 0.26f) c = ImGui::GetColorU32(ImLerp(ImGui::ColorConvertU32ToFloat4(c), ImVec4(1, 1, 1, 1), 0.16f));
                    if (x < 6 || y < 6 || x >= FW - 6 || y >= FH - 6) c = IM_COL32(246, 240, 222, 255);   // a printed border
                    px[y * s->pitch / 4 + x] = (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
                }
            for (int row = 0; row < 3; ++row) {   // lettering blocks
                const int y0 = (int)(FH * (0.68f + 0.085f * row)), hh = (int)(FH * 0.055f);
                int x0 = (int)(FW * (0.12f + 0.04f * (float)(rnd() % 3)));
                const int xe = FW - (int)(FW * 0.12f) - (row == 2 ? (int)(FW * 0.25f) : 0);
                while (x0 < xe) {
                    const int bw = std::min(xe - x0, (int)(FW * (0.035f + 0.02f * (float)(rnd() % 4))));
                    for (int y = y0; y < y0 + hh && y < FH; ++y) for (int x = x0; x < x0 + bw; ++x) px[y * s->pitch / 4 + x] = 0xFFF8F4E6u;
                    x0 += bw + (int)(FW * 0.012f) + (int)(rnd() % 3 == 0 ? FW * 0.025f : 0);
                }
            }
            g_fake[t] = SDL_CreateTextureFromSurface(ren, s);
        fake_register_color(g_fake[t], s);
            SDL_FreeSurface(s);
            continue;
        }
        for (int y = 0; y < g_fake_h; ++y)
            for (int x = 0; x < g_fake_w; ++x) {
                float k = (float)y / g_fake_h;
                ImU32 c = ImGui::GetColorU32(ImLerp(ImGui::ColorConvertU32ToFloat4(top), ImGui::ColorConvertU32ToFloat4(bot), k));
                float dx = x - g_fake_w * 0.5f, dy = y - g_fake_h * 0.42f, rr = std::sqrt(dx * dx + dy * dy);
                if (rr < g_fake_w * 0.26f) c = acc;                                  // a "logo" disc
                else if (((x + y) / 14) % 5 == 0 && y > g_fake_h * 0.72f) c = IM_COL32(20, 20, 30, 255);   // stripes along the bottom
                px[y * s->pitch / 4 + x] = (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);   // RGBA -> ARGB
            }
        g_fake[t] = SDL_CreateTextureFromSurface(ren, s);
        fake_register_color(g_fake[t], s);
        SDL_FreeSurface(s);
    }
}
bool lib_fake_art(const std::string& key, SDL_Texture** tex, int* w, int* h) {
    static const bool on = std::getenv("HUB_SHOT_FAKE_ART") != nullptr && g_headless;
    if (!on) return false;
    uint32_t hsh = 2166136261u;
    for (unsigned char c : key) hsh = (hsh ^ c) * 16777619u;
    SDL_Texture* t = g_fake[hsh % 12];
    if (!t) return false;
    *tex = t; *w = g_fake_dw[hsh % 12] ? g_fake_dw[hsh % 12] : g_fake_w; *h = g_fake_dh[hsh % 12] ? g_fake_dh[hsh % 12] : g_fake_h;
    return true;
}
