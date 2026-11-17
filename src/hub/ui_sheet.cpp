#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui_sheet.h"

#include "imgui_internal.h"
#include "ui_layout_logic.h"
#include "lib_compact_logic.h"   // kShortWindowPx
#include "app.h"   // A.pad_mode (hint glyphs)

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

/* UI kit page sheet + dialog (see ui_sheet.h). */

namespace ui {
namespace detail {
extern ImGuiID tabbar_window;   // ui_controls.cpp: the last tab bar drawn
extern int tabbar_frame;
extern float tabbar_bottom;
}  // namespace detail
namespace {

struct ModalMemo {
    bool was_open = false;     // the popup was on screen last time we asked
    float footer_h = -1.f;     // sheet: footer height measured last frame
    float buttons_h = -1.f;    // dialog: button row height measured last frame
    ImGuiID body_win = 0;      // dialog: the scrolling middle's window (last frame)
    float btn_top = -1.f;      // dialog: screen y of the button row's first line (last frame)
    bool up_search = false;    // dialog: an Up from the buttons is still looking for an item in the middle (moved on to the next frame)
    float up_from = -1.f;      // dialog: the middle's scroll when that search began (put back when it finds nothing)
};
std::unordered_map<ImGuiID, ModalMemo> g_memo;

/* A crisp 1 px hairline on a whole pixel row (a filled rect: anti-aliased lines come out 1 or 2 px depending on the renderer). */
void hline(ImDrawList* dl, float x0, float x1, float y) { dl->AddRectFilled(ImVec2(x0, std::floor(y)), ImVec2(x1, std::floor(y) + 1.f), HAIRLINE); }

/* The B / Esc that closed a dialog or sheet must not also close what is under it (the parent gets the focus back in the same frame)
 * or reach the hub's global "back": lock both keys for the rest of this frame. */
void consume_back() {
    const ImGuiID owner = ImGui::GetID("##ui_back");
    ImGui::SetKeyOwner(ImGuiKey_GamepadFaceRight, owner, ImGuiInputFlags_LockThisFrame);
    ImGui::SetKeyOwner(ImGuiKey_Escape, owner, ImGuiInputFlags_LockThisFrame);
}

/* Opens / closes the popup `name` from *open and begins it. True = the popup is current (call EndPopup). `prep` sets the next-window
 * data and runs only right before BeginPopupModal, so it never leaks onto another window. */
bool modal_gate(const char* name, bool* open, ImGuiWindowFlags flags, ModalMemo& m, const std::function<void()>& prep) {
    const ImGuiID pid = ImGui::GetID(name);
    const bool is_open = ImGui::IsPopupOpen(pid, ImGuiPopupFlags_None);
    if (m.was_open && !is_open && *open) *open = false;   // closed from outside (its parent popup went away): do not come back on our own
    if (!*open) {
        if (is_open) {
            prep();
            if (ImGui::BeginPopupModal(name, nullptr, flags)) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); }
        }
        m.was_open = false;
        return false;
    }
    if (!is_open) ImGui::OpenPopupEx(pid, ImGuiPopupFlags_None);
    prep();
    const bool b = ImGui::BeginPopupModal(name, nullptr, flags);
    m.was_open = b;
    return b;
}

/* Header, body and footer inside the current host window (popup or pane child). */
ScrollInfo sheet_contents(const SheetParts& parts, const SheetOpts& opts, float pad, ModalMemo& m) {
    ImGuiWindow* hw = ImGui::GetCurrentWindow();
    const float gap = opts.gap >= 0.f ? opts.gap : sp(1.5f);
    const ImVec2 wp = hw->Pos, ws = hw->Size;
    const float x0 = wp.x + pad, x1 = wp.x + ws.x - pad, top = wp.y + pad, bottom = wp.y + ws.y - pad;
    ImDrawList* dl = hw->DrawList;

    ImGui::SetCursorScreenPos(ImVec2(x0, top));
    float header_h = 0.f;
    if (parts.header) {
        ImGui::BeginGroup();
        parts.header();
        ImGui::EndGroup();
        header_h = std::max(0.f, ImGui::GetItemRectMax().y - top);
    }
    const float footer_est = parts.footer ? (m.footer_h >= 0.f ? m.footer_h : sp(6.f)) : 0.f;   // first frame: about one button row
    const hubui::kit::Split s = hubui::kit::sheet_split(top, bottom, header_h, footer_est, gap);
    const bool ends_with_tabs = detail::tabbar_frame == ImGui::GetFrameCount() && detail::tabbar_window == hw->ID &&
                                std::fabs(detail::tabbar_bottom - (top + header_h)) < sp(1.f);   // the tab bar draws its own baseline
    if (opts.separators && header_h > 0.f && !ends_with_tabs) hline(dl, x0, x1, s.body_y - gap * 0.5f);

    /* the body: a kit scroll area as wide as the content plus half the padding each side (room for focus rings, aligned text) */
    const float spx = pad * 0.5f;
    std::string bid = "##sheet_body";
    if (opts.body_key && *opts.body_key) { bid += "_"; bid += opts.body_key; }
    ImGui::SetCursorScreenPos(ImVec2(x0 - spx, s.body_y));
    if (scroll_begin(bid.c_str(), ImVec2(std::max(1.f, x1 - x0 + 2.f * spx), std::max(1.f, s.body_h)), ScrollFlags_PadOwner, ImVec2(spx, sp(1.f))))
        if (parts.body) parts.body();
    const ScrollInfo si = scroll_end();

    if (parts.footer) {
        if (opts.separators) hline(dl, x0, x1, s.footer_y - gap * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(x0, s.footer_y));
        ImGui::BeginGroup();
        parts.footer();
        ImGui::EndGroup();
        const float fh = std::max(0.f, ImGui::GetItemRectMax().y - s.footer_y);
        if (std::fabs(fh - m.footer_h) > 0.5f) m.footer_h = fh;   // used from the next frame (an Overlay's first frame is hidden while it measures)
    } else m.footer_h = 0.f;

    /* the wheel over the header or footer scrolls the body too */
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheel != 0.f && ImGui::IsWindowHovered() && si.overflows)
        scroll_set(si.window_id, si.scroll - io.MouseWheel * sp(10.f));
    return si;
}

struct DlgFrame {
    ImGuiID memo = 0;
    bool* open = nullptr;
    DialogOpts opts;
    bool buttons = false;
    float btn_y0 = 0.f;
    bool up = false;           // this frame's Up from the buttons into the middle (up_into_middle_*)
};
std::vector<DlgFrame> g_dlg;

/* Up from the first line of a dialog's pinned buttons goes to the nearest item ABOVE them in the middle, also when it is scrolled out of
 * view. ImGui scores only the visible part of a NavFlattened child when the focus enters it from its parent window (NavScoreItem), so an
 * item below the visible part (a friend's "Ask ... to host" row under a long description) could not be reached that way. Before the middle
 * is drawn: scroll it to its end, so the move scores what is there. After the buttons: when the move found nothing in the middle, scroll it
 * up a page and carry the move on to the next frame, until something shows or the middle is at its top; finding nothing, the middle goes
 * back to where it was. A middle with nothing focusable (only text) is never searched. True = this frame's move is one. */

/* The search stops. `found` = it reached an item of the middle (the move takes the focus there and the view follows it); otherwise the
 * middle goes back to where it was before the search scrolled it. */
void up_search_stop(ModalMemo& m, ImGuiWindow* mid, bool found) {
    if (m.up_search && !found && mid && m.up_from >= 0.f && std::fabs(mid->Scroll.y - m.up_from) >= 0.5f) {
        scroll_set(mid->ID, m.up_from);
        shot_trace("dlg-up-restore", std::to_string((int)m.up_from));
    }
    m.up_search = false;
    m.up_from = -1.f;
}

bool up_into_middle_begin(ModalMemo& m) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* dw = g.CurrentWindow;   // the dialog itself (its buttons are its own items)
    const bool up = g.NavMoveSubmitted && g.NavMoveDir == ImGuiDir_Up && g.NavWindow == dw && g.NavLayer == ImGuiNavLayer_Main &&
                    !(g.NavMoveFlags & (ImGuiNavMoveFlags_IsTabbing | ImGuiNavMoveFlags_FocusApi | ImGuiNavMoveFlags_IsPageMove | ImGuiNavMoveFlags_ScrollToEdgeY));
    ImGuiWindow* mid = m.body_win ? ImGui::FindWindowByID(m.body_win) : nullptr;
    if (!up || !mid || !mid->WasActive || !hubui::kit::overflows(mid->ScrollMax.y) || m.btn_top < 0.f) { up_search_stop(m, mid, false); return false; }
    if ((g.NavMoveFlags & ImGuiNavMoveFlags_Forwarded) && m.up_search) return true;   // our own move, carried on: the middle was paged already
    /* Nothing focusable in the middle last frame (only text): there is nothing to find, so no search (it scrolled the text to its end and
     * back and ate D-pad presses meanwhile). ImGui marks the layer for every item it scores, clipped ones too (ItemAdd, before the clip test),
     * and a NavFlattened child passes its items' layers up to its parent (EndChild). NavLayersActiveMaskNext holds last frame's whole tally
     * until the middle begins again below (Begin copies it to NavLayersActiveMask, which still holds the frame before). */
    if (!(mid->DC.NavLayersActiveMaskNext & (1 << ImGuiNavLayer_Main))) { m.up_search = false; return false; }
    const ImRect cur = ImGui::WindowRectRelToAbs(dw, dw->NavRectRel[ImGuiNavLayer_Main]);
    if (cur.IsInverted() || cur.Min.y < m.btn_top - sp(0.5f) || cur.Min.y > m.btn_top + sp(1.f)) { m.up_search = false; return false; }   // not the buttons' first line
    m.up_from = mid->Scroll.y;   // put back if the search reaches the top without finding anything
    if (mid->Scroll.y < mid->ScrollMax.y - 0.5f) scroll_set(mid->ID, mid->ScrollMax.y);   // applied when the middle begins, below
    m.up_search = true;
    shot_trace("dlg-up-search", std::to_string((int)mid->Scroll.y));   // QA (HUB_SHOT_TRACE): the middle is searched for an item above the buttons
    return true;
}

void up_into_middle_end(ModalMemo& m) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* mid = m.body_win ? ImGui::FindWindowByID(m.body_win) : nullptr;
    if (!mid || !g.NavMoveSubmitted || g.NavMoveForwardToNextFrame) { up_search_stop(m, mid, false); return; }
    /* the result ImGui is about to take (NavMoveRequestApplyResult's choice) */
    const ImGuiNavItemData* res = g.NavMoveResultLocal.ID ? &g.NavMoveResultLocal : g.NavMoveResultOther.ID ? &g.NavMoveResultOther : nullptr;
    const ImGuiNavItemData& oth = g.NavMoveResultOther;
    if (res && res != &oth && oth.ID && oth.Window && oth.Window->ParentWindow == g.NavWindow &&
        (oth.DistBox < res->DistBox || (oth.DistBox == res->DistBox && oth.DistCenter < res->DistCenter)))
        res = &oth;
    const bool found = res && res->Window && ImGui::IsWindowChildOf(res->Window, mid, false);
    if (found || mid->Scroll.y <= 0.5f) { up_search_stop(m, mid, found); return; }   // found, or the top reached with nothing there
    scroll_set(mid->ID, std::max(0.f, mid->Scroll.y - std::max(sp(4.f), mid->InnerRect.GetHeight() * 0.8f)));
    shot_trace("dlg-up-page", std::to_string((int)mid->Scroll.y));
    ImGui::NavMoveRequestForward(g.NavMoveDir, g.NavMoveClipDir, g.NavMoveFlags & ~ImGuiNavMoveFlags_Forwarded, g.NavMoveScrollFlags);
}

}  // namespace

float sheet_pad(ImVec2 size, const SheetOpts& opts) {
    return opts.pad >= 0.f ? opts.pad : ((size.x < sp(70.f) || size.y < sp(60.f)) ? sp(2.f) : sp(3.f));
}

SheetResult sheet(const char* id, SheetHost host, ImVec2 pos, ImVec2 size, bool* open, const SheetParts& parts, const SheetOpts& opts) {
    SheetResult r;
    const float pad = sheet_pad(size, opts);
    if (host == SheetHost::Pane) {
        if (open && !*open) return r;
        ImGui::PushID(id);
        ModalMemo& m = g_memo[ImGui::GetID("##pane")];
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, opts.bg ? opts.bg : PANEL);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, opts.rounding >= 0.f ? opts.rounding : sp(2.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        const ImGuiChildFlags cf = ImGuiChildFlags_AlwaysUseWindowPadding | (opts.nav_flatten ? ImGuiChildFlags_NavFlattened : 0);
        ImGui::SetNextWindowScroll(ImVec2(0.f, 0.f));   // only the body scrolls
        const bool vis = ImGui::BeginChild("##pane", ImVec2(std::max(1.f, size.x), std::max(1.f, size.y)), cf,
                                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        if (vis) {
            r.shown = true;
            r.body = sheet_contents(parts, opts, pad, m);
        }
        ImGui::EndChild();
        ImGui::PopID();
        return r;
    }

    bool always = true;
    bool* op = open ? open : &always;
    const std::string name = std::string(id) + "##uisheet";
    ModalMemo& m = g_memo[ImGui::GetID(name.c_str())];
    const bool was = m.was_open;
    auto prep = [&] {
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(std::max(1.f, size.x), std::max(1.f, size.y)), ImGuiCond_Always);
        ImGui::SetNextWindowScroll(ImVec2(0.f, 0.f));   // only the body scrolls; the host never drifts when a footer row runs long
    };
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, opts.rounding >= 0.f ? opts.rounding : 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, opts.bg ? opts.bg : BG);
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse;
    const bool b = modal_gate(name.c_str(), op, wf, m, prep);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (!b) {
        r.closed = was;
        return r;
    }
    r.shown = true;
    r.body = sheet_contents(parts, opts, pad, m);
    if (opts.back_closes && *op && modal_back()) { *op = false; consume_back(); }   // after the callbacks: an open dropdown takes B first
    if (!*op) {
        ImGui::CloseCurrentPopup();
        m.was_open = false;
        r.closed = true;
    }
    ImGui::EndPopup();
    return r;
}

bool dialog_begin(const char* id, const char* title, bool* open, DialogSize size, const DialogOpts& opts) {
    ImGuiIO& io = ImGui::GetIO();
    const float w = hubui::kit::dialog_width((int)size, sp(1.f), io.DisplaySize.x), max_h = hubui::kit::dialog_max_h(io.DisplaySize.y);
    const float pad = io.DisplaySize.y < hubui::lc::kShortWindowPx ? sp(2.f) : sp(3.f);   // a short window: less padding, more room for the middle
    const std::string name = opts.keep_id ? std::string(id) : std::string(id) + "##uidlg";
    const ImGuiID mk = ImGui::GetID(name.c_str());
    ModalMemo& m = g_memo[mk];
    bool always = true;
    bool* op = open ? open : &always;
    if (!open && m.was_open && !ImGui::IsPopupOpen(mk, ImGuiPopupFlags_None)) m.was_open = false;   // the caller closed it: show it again at once
    auto prep = [&] {
        ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, 0.f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.f), ImVec2(w, max_h));
    };
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    const bool b = modal_gate(name.c_str(), op, wf, m, prep);
    ImGui::PopStyleVar(2);
    if (!b) return false;

    DlgFrame f;
    f.memo = mk;
    f.open = open;
    f.opts = opts;
    /* pinned title row */
    const float y0 = ImGui::GetCursorPosY();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float xb = opts.close_x && open ? sp(4.f) : 0.f;
    const ImVec2 title_pos = ImGui::GetCursorPos();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail - (xb > 0.f ? xb + sp(1.f) : 0.f));
    ImGui::PushFont(F.h2);
    ImGui::TextWrapped("%s", title);
    ImGui::PopFont();
    if (opts.subtitle && *opts.subtitle) {
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT2);
        ImGui::TextWrapped("%s", opts.subtitle);
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    if (xb > 0.f) {
        const ImVec2 keep = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(title_pos.x + avail - xb, title_pos.y));
        ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);   // like ImGui's own title-bar X: it never takes the dialog's first focus
        if (icon_button("##dlg_close", Icon::Close, xb, false, "Close")) *open = false;
        ImGui::PopItemFlag();
        ImGui::SetCursorPos(keep);
    }
    const float title_h = ImGui::GetCursorPosY() - y0;
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float body_max = hubui::kit::dialog_body_max(max_h, pad, title_h, m.buttons_h >= 0.f ? m.buttons_h : sp(7.f), spacing * 2.f + sp(1.f), sp(6.f));
    /* scrolling middle, half the padding wider each side (focus rings), text aligned with the title */
    const float spx = pad * 0.5f;
    f.up = up_into_middle_begin(m);   // (before the middle begins: it may scroll it)
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - spx);
    scroll_begin("##dlg_body", ImVec2(avail + 2.f * spx, body_max), ScrollFlags_AutoHeight | ScrollFlags_PadOwner, ImVec2(spx, sp(0.5f)));
    g_dlg.push_back(f);
    return true;
}

void dialog_buttons() {
    if (g_dlg.empty() || g_dlg.back().buttons) return;
    DlgFrame& f = g_dlg.back();
    f.buttons = true;
    const ScrollInfo si = scroll_end();
    if (si.overflows) {   // a line between the scrolled text and the pinned buttons
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        const float y = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y * 0.5f;
        hline(w->DrawList, w->Pos.x + sp(1.5f), w->Pos.x + w->Size.x - sp(1.5f), y);
    }
    f.btn_y0 = ImGui::GetCursorPosY();
    ModalMemo& m = g_memo[f.memo];
    m.body_win = si.window_id;
    m.btn_top = ImGui::GetCursorScreenPos().y;
}

void dialog_end() {
    if (g_dlg.empty()) return;
    dialog_buttons();
    const DlgFrame f = g_dlg.back();
    g_dlg.pop_back();
    ModalMemo& m = g_memo[f.memo];
    m.buttons_h = std::max(0.f, ImGui::GetCursorPosY() - f.btn_y0);
    if (f.opts.back_closes && f.open && *f.open && modal_back()) { *f.open = false; consume_back(); }
    if (f.up) {
        if (f.open && !*f.open) {   // the dialog closes this frame: never carry the move on (it would land in the window underneath)
            ImGuiContext& g = *GImGui;
            if (m.up_search && (g.NavMoveFlags & ImGuiNavMoveFlags_Forwarded) && g.NavMoveSubmitted) ImGui::NavMoveRequestCancel();   // our own carried move
            up_search_stop(m, nullptr, false);
        } else {
            up_into_middle_end(m);   // (every button scored: was an item of the middle found?)
        }
    }
    if (f.open && !*f.open) {
        ImGui::CloseCurrentPopup();
        m.was_open = false;
    }
    ImGui::EndPopup();
}

void dialog_same_line(float next_w, float gap) {
    const float g = gap >= 0.f ? gap : ImGui::GetStyle().ItemSpacing.x;
    /* the cursor is at the start of the next line now: its x + the available width is the same right edge GetContentRegionAvail() sizes a
     * "fill the row" button with (WorkRect is rounded differently and would wrap such a row by a fraction of a pixel) */
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (hubui::kit::fits_on_line(ImGui::GetItemRectMax().x, g, next_w, right)) ImGui::SameLine(0.f, g);
}

float dialog_button_w(const char* label, bool icon) {   // mirrors ui::button(): text + icon + gap + sp(2.5) padding each side
    const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
    const bool text = ts.x > 0.f;
    return ts.x + (icon ? sp(2.25f) : 0.f) + (icon && text ? sp(1.f) : 0.f) + sp(5.f);
}

namespace {
bool hint_shown(const KeyHint& h, bool pad) { return pad || (h.key && h.key[0]); }
bool hint_paired(const KeyHint* hints, int n, int i, bool pad) {   // no label: it sits tight against the next one ("LB RB Tabs")
    return !(hints[i].label && hints[i].label[0]) && i + 1 < n && hint_shown(hints[i + 1], pad);
}
float one_hint_w(const KeyHint& h, bool pad) { return key_hint_width(pad, h.pad ? h.pad : "", h.key ? h.key : "", h.label ? h.label : ""); }
}  // namespace

float hint_bar_width(const KeyHint* hints, int n) {
    const bool pad = A.pad_mode;
    float x = 0.f;
    bool tight = false;
    int k = 0;
    for (int i = 0; i < n; ++i) {
        const KeyHint& h = hints[i];
        if (!hint_shown(h, pad)) continue;
        if (k) x += tight ? sp(0.5f) : sp(3.f);
        x += one_hint_w(h, pad);
        tight = hint_paired(hints, n, i, pad);
        ++k;
    }
    return x;
}

void hint_bar(const KeyHint* hints, int n) {
    const bool pad = A.pad_mode;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = F.cap->FontSize + sp(0.75f);
    float x = p.x;
    bool any = false;
    for (int i = 0; i < n; ++i) {
        const KeyHint& hn = hints[i];
        if (!hint_shown(hn, pad)) continue;
        const bool pair = hint_paired(hints, n, i, pad);
        float need = one_hint_w(hn, pad);
        if (pair) need += sp(0.5f) + one_hint_w(hints[i + 1], pad);   // a pair shows whole or not at all
        if (x + need > p.x + w + 0.5f) break;
        x += key_hint(dl, ImVec2(x, p.y), pad, hn.pad ? hn.pad : "", hn.key ? hn.key : "", hn.label ? hn.label : "") + (pair ? sp(0.5f) : sp(3.f));
        any = true;
    }
    ImGui::Dummy(ImVec2(any ? std::max(1.f, x - sp(3.f) - p.x) : 1.f, h));
}

}  // namespace ui
