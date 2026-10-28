#include "app.h"
#include "osk.h"
#include "ui_kit.h"

namespace osk {
namespace {

enum Cmd { C_UP, C_DOWN, C_LEFT, C_RIGHT, C_A, C_B, C_X, C_Y, C_START, C_BACK, C_LB, C_RB, C_N };

struct State {
    bool kb_seen = false;
    // text keyboard
    bool open = false, need_popup = false, root = false, accepted = false, finished = false;
    bool mask = false;   // ImGuiInputTextFlags_Password: the preview shows bullets, never the text
    ImGuiID owner = 0;
    Session ses;
    std::string hint;
    // folder browser
    bool fb_open = false, fb_need_popup = false;
    std::string fb_tag, fb_result, last_folder;
    FolderBrowser fb;
    int fb_top = 0, fb_last_sel = -1;
    // input
    int last_drawn = -100;
    bool armed = false;
    bool held[C_N] = {};
    float hold_t[C_N] = {};
    std::vector<int> shot_cmds;
    double shot_t = 0;
    std::vector<int> inject_q;
    bool saved = false;
    ImGuiConfigFlags saved_flags = 0;
} S;

constexpr const char* KB_POPUP = "##osk_keyboard";
constexpr const char* FB_POPUP = "##osk_folder";
constexpr ImGuiConfigFlags NAV_MASK = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

unsigned pad_mask() {
    unsigned m = 0;
    if (hub_input_blocked()) return 0;
    for (SDL_GameController* p : A.pads) {
        if (!p) continue;
        auto b = [&](SDL_GameControllerButton x) { return SDL_GameControllerGetButton(p, x) != 0; };
        auto ax = [&](SDL_GameControllerAxis x) { return (int)SDL_GameControllerGetAxis(p, x); };
        if (b(SDL_CONTROLLER_BUTTON_DPAD_UP) || ax(SDL_CONTROLLER_AXIS_LEFTY) < -16000) m |= 1u << C_UP;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ax(SDL_CONTROLLER_AXIS_LEFTY) > 16000) m |= 1u << C_DOWN;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax(SDL_CONTROLLER_AXIS_LEFTX) < -16000) m |= 1u << C_LEFT;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax(SDL_CONTROLLER_AXIS_LEFTX) > 16000) m |= 1u << C_RIGHT;
        if (b(SDL_CONTROLLER_BUTTON_A)) m |= 1u << C_A;
        if (b(SDL_CONTROLLER_BUTTON_B)) m |= 1u << C_B;
        if (b(SDL_CONTROLLER_BUTTON_X)) m |= 1u << C_X;
        if (b(SDL_CONTROLLER_BUTTON_Y)) m |= 1u << C_Y;
        if (b(SDL_CONTROLLER_BUTTON_START)) m |= 1u << C_START;
        if (b(SDL_CONTROLLER_BUTTON_BACK)) m |= 1u << C_BACK;
        if (b(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) m |= 1u << C_LB;
        if (b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) m |= 1u << C_RB;
    }
    return m;
}

/* Pad edges (+ auto-repeat for directions / shoulders / B) and scripted shot commands, once per frame. */
std::vector<int> gather_cmds() {
    std::vector<int> out;
    float dt = ImGui::GetIO().DeltaTime;
    unsigned m = pad_mask();
    for (int i = 0; i < C_N; ++i) {
        bool d = (m >> i) & 1u;
        bool rep = i <= C_RIGHT || i == C_B || i == C_LB || i == C_RB;
        if (d && !S.held[i]) { out.push_back(i); S.hold_t[i] = 0.4f; }
        else if (d && rep) {
            S.hold_t[i] -= dt;
            if (S.hold_t[i] <= 0.f) { out.push_back(i); S.hold_t[i] = 0.07f; }
        }
        S.held[i] = d;
    }
    if (!S.armed) {   // ignore the press that opened us until every button is up
        if (m == 0 && !ImGui::IsKeyDown(ImGuiKey_Enter)) S.armed = true;
        out.clear();
    }
    if (S.armed && !S.inject_q.empty()) {
        out.push_back(S.inject_q.front());
        S.inject_q.erase(S.inject_q.begin());
    }
    if (S.armed && !S.shot_cmds.empty() && ImGui::GetTime() > 1.5 && ImGui::GetTime() - S.shot_t > 0.3) {
        out.push_back(S.shot_cmds.front());
        S.shot_cmds.erase(S.shot_cmds.begin());
        S.shot_t = ImGui::GetTime();
    }
    return out;
}

void hint_row(ImDrawList* dl, ImVec2 p, float maxw, bool kb, bool filemode = false) {
    struct H { const char* btn; const char* label; };
    static const H kbh[] = {{"A", "Type"}, {"B", "Delete"}, {"X", "Space"}, {"Y", "Shift / symbols"}, {"LB/RB", "Move cursor"}, {"Start", "Done"}, {"Back", "Cancel"}};
    static const H fbh[] = {{"A", "Open / choose"}, {"B", "Up a level"}, {"Start", "Use this folder"}, {"Back", "Cancel"}};
    static const H fih[] = {{"A", "Open / choose"}, {"B", "Up a level"}, {"Back", "Cancel"}};
    const H* hs = kb ? kbh : filemode ? fih : fbh;
    int n = kb ? 7 : filemode ? 3 : 4;
    float x = p.x, gap = ui::sp(2.f);
    for (int i = 0; i < n; ++i) {
        float w = ui::key_hint_width(true, hs[i].btn, "", hs[i].label);
        if (x + w > p.x + maxw && x > p.x) { x = p.x; p.y += ui::sp(3.f); }
        ui::key_hint(dl, ImVec2(x, p.y), true, hs[i].btn, "", hs[i].label);
        x += w + gap;
    }
}

void finish_text(bool ok) {
    S.accepted = ok;
    S.finished = true;
    S.open = false;
    ImGui::CloseCurrentPopup();
}

void draw_keyboard() {
    ImGuiIO& io = ImGui::GetIO();
    if (S.need_popup) { ImGui::OpenPopup(KB_POPUP); S.need_popup = false; }
    const float dw = io.DisplaySize.x, dh = io.DisplaySize.y;
    Session& ss = S.ses;
    const auto& L = layout(ss.page);
    const float sp1 = ui::sp(1.f), g = ui::sp(0.5f), pad = ui::sp(2.f);
    float W = std::min(dw - ui::sp(2.f), ui::sp(112.f));
    float titleH = ui::sp(2.5f), prevH = ui::sp(6.f), hintsH = ui::sp(7.f);
    float kh = (dh - 2 * pad - titleH - prevH - hintsH - ui::sp(3.f)) / (float)L.size() - g;
    kh = std::max(ui::sp(3.5f), std::min(kh, ui::sp(6.5f)));
    float H = 2 * pad + titleH + prevH + sp1 + (float)L.size() * (kh + g) + sp1 + hintsH;
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::SetNextWindowPos(ImVec2(dw * 0.5f, dh - ui::sp(1.f)), ImGuiCond_Always, ImVec2(0.5f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui::sp(2.f));
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav;
    bool vis = ImGui::BeginPopupModal(KB_POPUP, nullptr, wf);
    ImGui::PopStyleVar(2);
    if (!vis) { S.open = false; return; }
    S.last_drawn = ImGui::GetFrameCount();

    // ---- input ----
    bool consumed_click = false;
    for (int c : gather_cmds()) {
        switch (c) {
        case C_UP: ss.dir(0, -1); break;
        case C_DOWN: ss.dir(0, 1); break;
        case C_LEFT: ss.dir(-1, 0); break;
        case C_RIGHT: ss.dir(1, 0); break;
        case C_A: ss.press_current(); break;
        case C_B: ss.ed.backspace(); break;
        case C_X: ss.type_chars(" "); break;
        case C_Y: ss.shift_cycle(); break;
        case C_START: ss.done = true; break;
        case C_BACK: ss.cancelled = true; break;
        case C_LB: ss.ed.move_cursor(-1); break;
        case C_RB: ss.ed.move_cursor(1); break;
        }
    }
    if (S.armed) {   // physical keyboard still works inside the keyboard
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) ss.ed.backspace();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) ss.ed.del();
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) ss.done = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ss.cancelled = true;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) ss.dir(0, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ss.dir(0, 1);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) ss.dir(-1, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) ss.dir(1, 0);
        for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
            ImWchar wc = io.InputQueueCharacters[i];
            if (wc < 32 || wc == 127) continue;
            char u[8] = {};
            ImTextCharToUtf8(u, wc);
            ss.type_chars(u);
        }
    }

    // ---- draw ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    dl->AddRect(wp, ImVec2(wp.x + W, wp.y + H), ui::with_alpha(ui::ACCENT, 0.5f), ui::sp(2.f), 0, 2.f);
    float x0 = wp.x + pad, iw = W - 2 * pad, y = wp.y + pad;
    dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(x0, y), ui::TEXT2, S.hint.c_str());
    y += titleH;
    {   // text preview with caret
        ImVec2 b0(x0, y), b1(x0 + iw, y + prevH);
        dl->AddRectFilled(b0, b1, ui::PANEL, ui::sp(1.f));
        dl->AddRect(b0, b1, ui::with_alpha(ui::ACCENT, 0.7f), ui::sp(1.f), 0, 1.5f);
        ImFont* f = ui::F.h2;
        float fs = f->FontSize;
        auto masked = [](const std::string& t) { std::string m; for (unsigned char c : t) if ((c & 0xC0) != 0x80) m += '*'; return m; };
        const std::string shown = S.mask ? masked(ss.ed.text) : ss.ed.text;
        const std::string shown_to_cursor = S.mask ? masked(ss.ed.text.substr(0, (size_t)ss.ed.cursor)) : ss.ed.text.substr(0, (size_t)ss.ed.cursor);
        ImVec2 tsz = f->CalcTextSizeA(fs, FLT_MAX, 0.f, shown.c_str());
        float cx = f->CalcTextSizeA(fs, FLT_MAX, 0.f, shown_to_cursor.c_str()).x;
        float room = iw - sp1 * 2;
        float off = cx > room ? cx - room : 0.f;
        float ty = y + (prevH - tsz.y) * 0.5f;
        dl->PushClipRect(ImVec2(b0.x + 2, b0.y), ImVec2(b1.x - 2, b1.y), true);
        if (ss.ed.text.empty()) dl->AddText(f, fs, ImVec2(b0.x + sp1, ty), ui::TEXT3, "...");
        else dl->AddText(f, fs, ImVec2(b0.x + sp1 - off, ty), ui::TEXT, shown.c_str());
        if (std::fmod(ImGui::GetTime(), 1.0) < 0.6)
            dl->AddRectFilled(ImVec2(b0.x + sp1 - off + cx, y + ui::sp(1.f)), ImVec2(b0.x + sp1 - off + cx + 2.f * ui::S, y + prevH - ui::sp(1.f)), ui::ACCENT);
        dl->PopClipRect();
        char cnt[32];
        std::snprintf(cnt, sizeof cnt, "%d/%d", (int)ss.ed.text.size(), (int)ss.ed.maxlen);
        ImVec2 cs = ui::F.cap->CalcTextSizeA(ui::F.cap->FontSize, FLT_MAX, 0.f, cnt);
        dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(b1.x - cs.x, y - titleH + 2), ui::TEXT3, cnt);
    }
    y += prevH + sp1;
    bool mouse_moved = io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f;
    for (size_t r = 0; r < L.size(); ++r) {
        float tw = 0;
        for (auto& k : L[r]) tw += k.w;
        float avail = iw - g * (float)(L[r].size() - 1), x = x0;
        for (size_t c = 0; c < L[r].size(); ++c) {
            const Key& k = L[r][c];
            float w = avail * k.w / tw;
            ImVec2 k0(x, y), k1(x + w, y + kh);
            bool sel = (int)r == ss.pos.r && (int)c == ss.pos.c;
            bool hov = ImGui::IsMouseHoveringRect(k0, k1) && ImGui::IsWindowHovered();
            if (hov && mouse_moved) { ss.pos = Pos{(int)r, (int)c}; sel = true; }
            if (hov && ImGui::IsMouseClicked(0) && !consumed_click) { consumed_click = true; ss.pos = Pos{(int)r, (int)c}; ss.press(k); }
            bool action = k.act != KeyAct::Char && k.act != KeyAct::Space;
            bool lit = (k.act == KeyAct::Shift && ss.page == Page::Upper) || (k.act == KeyAct::Symbols && ss.page == Page::Symbols);
            ImU32 bg = k.act == KeyAct::Done ? ui::mix(ui::PANEL3, ui::GOOD, 0.55f) : lit ? ui::mix(ui::PANEL3, ui::ACCENT, 0.6f) : action ? ui::PANEL2 : ui::PANEL3;
            if (sel) bg = ui::ACCENT;
            dl->AddRectFilled(k0, k1, bg, ui::sp(1.f));
            if (sel) ui::focus_ring(dl, k0, k1, ui::sp(1.f));
            ImFont* f = (k.act == KeyAct::Char) ? ui::F.h2 : ui::F.bold;
            float fs = f->FontSize;
            ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.f, k.label.c_str());
            dl->AddText(f, fs, ImVec2(k0.x + (w - ts.x) * 0.5f, k0.y + (kh - ts.y) * 0.5f), sel ? IM_COL32_WHITE : ui::TEXT, k.label.c_str());
            x += w + g;
        }
        y += kh + g;
    }
    y += sp1;
    hint_row(dl, ImVec2(x0, y), iw, true);
    ImGui::Dummy(ImVec2(1, 1));

    if (ss.cancelled) finish_text(false);
    else if (ss.done) finish_text(true);
    ImGui::EndPopup();
}

void draw_folder() {
    ImGuiIO& io = ImGui::GetIO();
    if (S.fb_need_popup) { ImGui::OpenPopup(FB_POPUP); S.fb_need_popup = false; }
    const float dw = io.DisplaySize.x, dh = io.DisplaySize.y;
    float W = std::min(dw - ui::sp(4.f), ui::sp(96.f)), H = std::min(dh - ui::sp(3.f), ui::sp(76.f));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::SetNextWindowPos(ImVec2(dw * 0.5f, dh * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui::sp(2.f));
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav;
    bool vis = ImGui::BeginPopupModal(FB_POPUP, nullptr, wf);
    ImGui::PopStyleVar(2);
    if (!vis) { shot_trace("osk-folder-hidden", "frame " + std::to_string(ImGui::GetFrameCount())); S.fb_open = false; return; }
    S.last_drawn = ImGui::GetFrameCount();
    FolderBrowser& fb = S.fb;

    bool click_on = false;
    int click_row = -1;
    for (int c : gather_cmds()) {
        switch (c) {
        case C_UP: fb.move(-1); break;
        case C_DOWN: fb.move(1); break;
        case C_LB: fb.move(-8); break;
        case C_RB: fb.move(8); break;
        case C_A: case C_RIGHT: fb.activate(); break;
        case C_B: case C_LEFT: fb.up(); break;
        case C_START: case C_Y: fb.choose_here(); break;
        case C_BACK: fb.cancelled = true; break;
        }
    }
    if (S.armed) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) fb.move(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) fb.move(1);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter)) fb.activate();
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) fb.up();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) fb.cancelled = true;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    dl->AddRect(wp, ImVec2(wp.x + W, wp.y + H), ui::with_alpha(ui::ACCENT, 0.5f), ui::sp(2.f), 0, 2.f);
    float pad = ui::sp(2.f), x0 = wp.x + pad, iw = W - 2 * pad, y = wp.y + pad;
    dl->AddText(ui::F.h2, ui::F.h2->FontSize, ImVec2(x0, y), ui::TEXT, fb.pick_files ? "Choose a file" : "Choose a folder");
    y += ui::F.h2->FontSize + ui::sp(0.5f);
    std::string where = fb.cwd.empty() ? "This PC" : fb.cwd;
    where = ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, where, iw);
    dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(x0, y), ui::TEXT2, where.c_str());
    y += ui::F.cap->FontSize + ui::sp(1.f);

    float hintsH = ui::sp(6.f), rowH = ui::sp(5.f), g = ui::sp(0.4f);
    float listH = wp.y + H - pad - hintsH - y;
    int vis_rows = std::max(1, (int)(listH / (rowH + g)));
    if (fb.sel != S.fb_last_sel) {
        if (fb.sel < S.fb_top) S.fb_top = fb.sel;
        if (fb.sel >= S.fb_top + vis_rows) S.fb_top = fb.sel - vis_rows + 1;
        S.fb_last_sel = fb.sel;
    }
    ImVec2 l0(x0, y), l1(x0 + iw, y + listH);
    if (ImGui::IsMouseHoveringRect(l0, l1) && io.MouseWheel != 0.f) S.fb_top -= (int)(io.MouseWheel * 2);
    S.fb_top = std::max(0, std::min(S.fb_top, std::max(0, (int)fb.entries.size() - vis_rows)));
    bool mouse_moved = io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f;
    for (int i = 0; i < vis_rows && S.fb_top + i < (int)fb.entries.size(); ++i) {
        int idx = S.fb_top + i;
        const FolderEntry& e = fb.entries[idx];
        ImVec2 r0(x0, y + i * (rowH + g)), r1(x0 + iw, r0.y + rowH);
        bool hov = ImGui::IsMouseHoveringRect(r0, r1) && ImGui::IsWindowHovered();
        if (hov && mouse_moved) fb.sel = idx;
        if (hov && ImGui::IsMouseClicked(0)) { fb.sel = idx; click_on = true; click_row = idx; }
        bool sel = idx == fb.sel;
        ImU32 bg = e.type == FolderEntry::Select ? ui::mix(ui::PANEL3, ui::GOOD, 0.45f) : ui::PANEL3;
        if (sel) bg = e.type == FolderEntry::Select ? ui::GOOD : ui::ACCENT;
        dl->AddRectFilled(r0, r1, bg, ui::sp(1.f));
        if (sel) ui::focus_ring(dl, r0, r1, ui::sp(1.f));
        ui::Icon ic = e.type == FolderEntry::File ? ui::Icon::Check : e.type == FolderEntry::Select ? ui::Icon::Check : e.type == FolderEntry::Up ? ui::Icon::ChevronL : ui::Icon::Folder;
        ui::icon(dl, ic, ImVec2(r0.x + rowH * 0.6f, r0.y + rowH * 0.5f), rowH * 0.5f, ui::TEXT);
        std::string label = e.type == FolderEntry::Drive ? "Drive " + e.name : e.name;
        ImFont* f = ui::F.bold;
        ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, label.c_str());
        dl->AddText(f, f->FontSize, ImVec2(r0.x + rowH * 1.3f, r0.y + (rowH - ts.y) * 0.5f), ui::TEXT, label.c_str());
    }
    if (fb.entries.empty()) dl->AddText(ui::F.body, ui::F.body->FontSize, l0, ui::TEXT3, "Nothing here.");
    if (click_on && click_row >= 0) fb.activate();
    hint_row(dl, ImVec2(x0, wp.y + H - pad - hintsH + ui::sp(1.f)), iw, false, fb.pick_files);
    ImGui::Dummy(ImVec2(1, 1));

    if (fb.done || fb.cancelled) {
        S.fb_result = fb.done ? fb.result : "";
        if (fb.done) S.last_folder = fb.pick_files ? parent_of(fb.result) : fb.result;
        S.fb_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void open_text(ImGuiID owner, Kind kind, const char* hint, const char* text, size_t size, ImGuiInputTextFlags flags) {
    S.ses.start(kind, text, size > 0 ? size - 1 : 0, (flags & ImGuiInputTextFlags_CharsUppercase) != 0, (flags & ImGuiInputTextFlags_CharsNoBlank) != 0);
    S.ses.clipboard.clear();
    if (SDL_HasClipboardText()) {
        if (char* cl = SDL_GetClipboardText()) { S.ses.clipboard = cl; SDL_free(cl); }
    }
    S.hint = hint && *hint ? hint : "Type here";
    S.owner = owner;
    S.mask = (flags & ImGuiInputTextFlags_Password) != 0;
    S.open = true;
    S.need_popup = true;
    S.finished = false;
    S.armed = false;
    S.last_drawn = ImGui::GetFrameCount();
}

} // namespace

bool input(const char* id, const char* hint, char* buf, size_t size, ImGuiInputTextFlags flags, Kind kind) {
    bool r = hint ? ImGui::InputTextWithHint(id, hint, buf, size, flags) : ImGui::InputText(id, buf, size, flags);
    const ImGuiID wid = ImGui::GetItemID();
    /* ImGui activates a nav-focused InputText on Enter only (A is a "tap"), so A on the focused field opens the keyboard itself. */
    const bool pad_a = !hub_input_blocked() && A.pad_mode && ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) && ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableGamepad;
    if (!S.open && !S.fb_open && (ImGui::IsItemActivated() || pad_a)) {
        bool mouse = ImGui::IsItemHovered() && ImGui::IsMouseClicked(0);
        if (pad_a || should_open(A.pad_mode, S.kb_seen, mouse)) {
            ImGui::ClearActiveID();
            open_text(wid, kind, hint, buf, size, flags);
            r = false;
        }
    }
    if (S.owner == wid) {
        if (S.open) draw_keyboard();
        if (S.finished) {
            S.finished = false;
            S.owner = 0;
            if (S.accepted) {
                std::snprintf(buf, size, "%s", S.ses.ed.text.c_str());
                return true;
            }
        }
    }
    return r;
}

std::string folder(const char* tag, bool clicked) {
    if (clicked && !S.open && !S.fb_open) {
        if (A.pad_mode) {
            S.fb_tag = tag;
            S.fb.pick_files = false;
            S.fb.filters.clear();
            S.fb.open(S.last_folder);
            S.fb_open = true;
            S.fb_need_popup = true;
            S.armed = false;
            S.fb_last_sel = -1;
            S.fb_top = 0;
            S.fb_result.clear();
            S.last_drawn = ImGui::GetFrameCount();
        } else {
            return pick_path(A.hwnd, true, nullptr, nullptr);
        }
    }
    if (S.fb_tag == tag) {
        if (S.fb_open) draw_folder();
        if (!S.fb_open && !S.fb_tag.empty() && (S.fb_need_popup == false) && S.fb.done + S.fb.cancelled) {
            std::string out = S.fb_result;
            S.fb_tag.clear();
            S.fb.done = S.fb.cancelled = false;
            return out;
        }
    }
    return "";
}

std::string file(const char* tag, bool clicked, const std::vector<std::string>& filters) {
    if (clicked && !S.open && !S.fb_open) {
        if (A.pad_mode) {
            S.fb_tag = tag;
            S.fb.pick_files = true;
            S.fb.filters = filters;
            S.fb.open(S.last_folder);
            S.fb_open = true;
            S.fb_need_popup = true;
            S.armed = false;
            S.fb_last_sel = -1;
            S.fb_top = 0;
            S.fb_result.clear();
            S.last_drawn = ImGui::GetFrameCount();
        } else {
            std::wstring spec;
            for (const auto& f : filters) { if (!spec.empty()) spec += L";"; spec += L"*"; for (char ch : f) spec += (wchar_t)ch; }
            if (spec.empty()) spec = L"*.*";
            return pick_path(A.hwnd, false, L"Files", spec.c_str());
        }
    }
    if (S.fb_tag == tag) {
        if (S.fb_open) draw_folder();
        if (!S.fb_open && (S.fb_need_popup == false) && S.fb.done + S.fb.cancelled) {
            std::string out = S.fb_result;
            S.fb_tag.clear();
            S.fb.done = S.fb.cancelled = false;
            S.fb.pick_files = false;
            return out;
        }
    }
    return "";
}

void note_keyboard() { S.kb_seen = true; }
bool keyboard_seen() { return S.kb_seen; }
bool is_open() { return S.open || S.fb_open; }
bool inject(const char* tok) {
    if (!is_open()) return false;
    static const char* names[] = {"up", "down", "left", "right", "a", "b", "x", "y", "start", "back", "lb", "rb"};
    std::string t = tok;
    if (t == "view") t = "back";
    for (int i = 0; i < C_N; ++i) if (t == names[i]) S.inject_q.push_back(i);
    return true;
}
std::string shot_text() { return S.open ? S.ses.ed.text : std::string(); }

bool hub_input_blocked() {
    if (g_headless) return false;
    static FocusGate gate;
    static Uint32 last_tick = 0xFFFFFFFFu;
    static bool cached = false;
    const Uint32 t = SDL_GetTicks();
    if (t == last_tick) return cached;   // one answer per millisecond keeps the gate's edge detection stable
    last_tick = t;
    cached = gate.blocked(A.win && (SDL_GetWindowFlags(A.win) & SDL_WINDOW_INPUT_FOCUS), t / 1000.0);
    return cached;
}

bool hub_pad_blocked() { return pad_blocked(hub_input_blocked(), qm_is_open()); }

void pre_frame(ImGuiIO& io) {
    static bool had_game = false;
    const bool launched = A.proc && !had_game;
    had_game = A.proc != nullptr;
    if ((launched || hub_input_blocked()) && (S.open || S.fb_open)) {   // a game launched / focus left: cancel the keyboard, nothing typed is kept
        S.open = S.fb_open = false;
        S.fb_tag.clear();
        S.owner = 0;
        S.finished = false;
    }
    if ((S.open || S.fb_open) && ImGui::GetFrameCount() - S.last_drawn > 3) {   // nobody is drawing us any more
        S.open = S.fb_open = false;
        S.fb_tag.clear();
        S.owner = 0;
    }
    bool want_off = S.open || S.fb_open;
    if (want_off && !S.saved) {
        S.saved_flags = io.ConfigFlags & NAV_MASK;
        io.ConfigFlags &= ~NAV_MASK;
        S.saved = true;
    } else if (!want_off && S.saved && pad_mask() == 0) {   // wait for the closing press to be released
        io.ConfigFlags |= S.saved_flags;
        S.saved = false;
    }
}

void draw_root() {
    if (!S.root) return;
    if (S.open) draw_keyboard();
    if (S.fb_open) draw_folder();
}

void shot_init() {
    const char* e = std::getenv("HUB_SHOT_OSK");
    if (!e) return;
    std::string spec = e, mode = spec, arg;
    size_t c = spec.find(':');
    if (c != std::string::npos) { mode = spec.substr(0, c); arg = spec.substr(c + 1); }
    S.root = true;
    S.last_drawn = ImGui::GetFrameCount();
    A.pad_mode = true;
    if (mode == "folder" || mode == "file") {
        S.fb.pick_files = mode == "file";
        S.fb.filters = {".json", ".zip", ".exe"};
        S.fb_tag = "shot";
        S.fb.open(arg);
        S.fb_open = true;
        S.fb_need_popup = true;
    } else {
        Kind k = mode == "room" ? Kind::Room : mode == "code" ? Kind::Code : Kind::Text;
        const bool pw = mode == "password";   // HUB_SHOT_OSK=password:<text>: QA that the preview is masked
        open_text(1, k, k == Kind::Room ? "Room code" : k == Kind::Code ? "Friend code" : pw ? "Password" : "Search games", arg.c_str(), k == Kind::Room ? 7 : 64,
                  k == Kind::Code ? ImGuiInputTextFlags_CharsUppercase : pw ? ImGuiInputTextFlags_Password : 0);
        if (mode == "symbols") S.ses.set_page(Page::Symbols);
    }
    if (const char* cm = std::getenv("HUB_SHOT_OSK_CMDS")) {
        std::stringstream ss(cm);
        std::string t;
        while (std::getline(ss, t, ',')) {
            static const char* names[] = {"up", "down", "left", "right", "a", "b", "x", "y", "start", "back", "lb", "rb"};
            for (int i = 0; i < C_N; ++i) if (t == names[i]) S.shot_cmds.push_back(i);
        }
    }
}

} // namespace osk
