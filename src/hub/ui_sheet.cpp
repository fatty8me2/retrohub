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
    const float gap = sp(1.5f);
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
};
std::vector<DlgFrame> g_dlg;

}  // namespace

SheetResult sheet(const char* id, SheetHost host, ImVec2 pos, ImVec2 size, bool* open, const SheetParts& parts, const SheetOpts& opts) {
    SheetResult r;
    const float pad = opts.pad >= 0.f ? opts.pad : ((size.x < sp(70.f) || size.y < sp(60.f)) ? sp(2.f) : sp(3.f));
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
    const float pad = io.DisplaySize.y < hubui::lc::kShortWindowPx ? sp(2.f) : sp(3.f);   // like the game page modal on a short window
    const std::string name = std::string(id) + "##uidlg";
    const ImGuiID mk = ImGui::GetID(name.c_str());
    ModalMemo& m = g_memo[mk];
    auto prep = [&] {
        ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, 0.f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.f), ImVec2(w, max_h));
    };
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    const bool b = modal_gate(name.c_str(), open, wf, m, prep);
    ImGui::PopStyleVar(2);
    if (!b) return false;

    DlgFrame f;
    f.memo = mk;
    f.open = open;
    f.opts = opts;
    /* pinned title row */
    const float y0 = ImGui::GetCursorPosY();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float xb = opts.close_x ? sp(4.f) : 0.f;
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
        if (icon_button("##dlg_close", Icon::Close, xb, false, "Close")) *open = false;
        ImGui::SetCursorPos(keep);
    }
    const float title_h = ImGui::GetCursorPosY() - y0;
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float body_max = hubui::kit::dialog_body_max(max_h, pad, title_h, m.buttons_h >= 0.f ? m.buttons_h : sp(7.f), spacing * 2.f + sp(1.f), sp(6.f));
    /* scrolling middle, half the padding wider each side (focus rings), text aligned with the title */
    const float spx = pad * 0.5f;
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
}

void dialog_end() {
    if (g_dlg.empty()) return;
    dialog_buttons();
    const DlgFrame f = g_dlg.back();
    g_dlg.pop_back();
    ModalMemo& m = g_memo[f.memo];
    m.buttons_h = std::max(0.f, ImGui::GetCursorPosY() - f.btn_y0);
    if (f.opts.back_closes && *f.open && modal_back()) { *f.open = false; consume_back(); }
    if (!*f.open) {
        ImGui::CloseCurrentPopup();
        m.was_open = false;
    }
    ImGui::EndPopup();
}

float hint_bar_width(const KeyHint* hints, int n) {
    const bool pad = A.pad_mode;
    float x = 0.f;
    int k = 0;
    for (int i = 0; i < n; ++i) {
        const KeyHint& h = hints[i];
        if (!pad && (!h.key || !h.key[0])) continue;
        if (k) x += sp(3.f);
        x += key_hint_width(pad, h.pad ? h.pad : "", h.key ? h.key : "", h.label ? h.label : "");
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
        const char* key = hn.key ? hn.key : "";
        if (!pad && !key[0]) continue;
        const float need = key_hint_width(pad, hn.pad ? hn.pad : "", key, hn.label ? hn.label : "");
        if (x + need > p.x + w + 0.5f) break;
        x += key_hint(dl, ImVec2(x, p.y), pad, hn.pad ? hn.pad : "", key, hn.label ? hn.label : "") + sp(3.f);
        any = true;
    }
    ImGui::Dummy(ImVec2(any ? std::max(1.f, x - sp(3.f) - p.x) : 1.f, h));
}

}  // namespace ui
