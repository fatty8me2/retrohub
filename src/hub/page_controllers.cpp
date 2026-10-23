#include "app.h"

/* ---------- Controllers ---------- */

void refresh_pads() {
    for (auto* p : A.pads) SDL_GameControllerClose(p);
    A.pads.clear();
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i))
            if (SDL_GameController* c = SDL_GameControllerOpen(i)) A.pads.push_back(c);
    if (A.pad_sel >= (int)A.pads.size()) A.pad_sel = 0;
}

void stick(ImDrawList* dl, ImVec2 c, float r, float x, float y, const char* label) {
    dl->AddCircleFilled(c, r, COL_PANEL2, 40);
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, 40), 40, 2);
    dl->AddCircleFilled(c + ImVec2(x, y) * (r * 0.7f), r * 0.28f, COL_ACCENT, 24);
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(c + ImVec2(-ts.x * 0.5f, r + 6 * S), IM_COL32(150, 152, 165, 255), label);
}
void btn_dot(ImDrawList* dl, ImVec2 c, float r, bool on, const char* label) {
    dl->AddCircleFilled(c, r, on ? COL_ACCENT : COL_PANEL2, 24);
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(c - ts * 0.5f, on ? IM_COL32_WHITE : IM_COL32(150, 152, 165, 255), label);
}

void page_controllers() {
    page_header("Controllers", "Connected game controllers and the N64 port each one will use.");
    if (A.pads.empty()) {
        begin_panel("nopad", ImVec2(0, sp(30.f)));
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 o = ImGui::GetWindowPos();
            float w = ImGui::GetWindowWidth(), cx = o.x + w * 0.5f, y = o.y + sp(4.f);
            dl->AddCircleFilled(ImVec2(cx, y + sp(4.f)), sp(5.5f), with_alpha(ui::ACCENT, 0.10f), 48);
            dl->AddCircleFilled(ImVec2(cx, y + sp(4.f)), sp(4.f), with_alpha(ui::ACCENT, 0.22f), 48);
            ui::icon(dl, ui::Icon::Gamepad, ImVec2(cx, y + sp(4.f)), sp(5.f), mix(ui::ACCENT, IM_COL32_WHITE, 0.5f));
            centered_text(dl, ui::F.h2, cx, y + sp(10.5f), ui::TEXT, "No controller yet");
            auto lines = ui::wrap_lines(ui::F.body, ui::F.body->FontSize,
                                        "Plug in or pair a controller and it shows up here right away. The first one becomes port 1, the second port 2, and so on. "
                                        "You can also use the D-pad and A / B to move around RetroHub itself.",
                                        std::min(w - sp(6.f), sp(60.f)), 3);
            float ty = y + sp(10.5f) + ui::F.h2->FontSize + sp(1.f);
            for (auto& ln : lines) {
                centered_text(dl, ui::F.body, cx, ty, ui::TEXT2, ln.c_str());
                ty += ui::F.body->FontSize * 1.3f;
            }
        }
        end_panel();
        return;
    }
    begin_panel("padlist");
    for (size_t i = 0; i < A.pads.size(); ++i) {
        const char* nm = SDL_GameControllerName(A.pads[i]);
        char line[256];
        std::snprintf(line, sizeof line, "%d.  %s", (int)i, nm ? nm : "Controller");
        if (ImGui::Selectable(line, A.pad_sel == (int)i, 0, ImVec2(0, 36 * S))) A.pad_sel = (int)i;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 110 * S);
        if (i < 4) { char pt[16]; std::snprintf(pt, sizeof pt, "Port %d", (int)i + 1); pill(pt, COL_ACCENT); }
        else pill("unused", COL_PANEL2);
    }
    end_panel();

    ImGui::Dummy(ImVec2(0, 8 * S));
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Test input");
    ImGui::PopFont();
    ImGui::TextDisabled("Move sticks and press buttons to see them light up.");
    SDL_GameController* pad = A.pads[std::min(A.pad_sel, (int)A.pads.size() - 1)];
    ImVec2 o = ImGui::GetCursorScreenPos();
    float h = 230 * S;
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, o + ImVec2(ImGui::GetItemRectSize().x, h), COL_PANEL, 14 * S);
    auto ax = [&](SDL_GameControllerAxis a) { float v = SDL_GameControllerGetAxis(pad, a) / 32767.f; return std::max(-1.f, std::min(1.f, v)); };
    auto bt = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(pad, b) != 0; };
    float r = 48 * S;
    stick(dl, o + ImVec2(100 * S, 100 * S), r, ax(SDL_CONTROLLER_AXIS_LEFTX), ax(SDL_CONTROLLER_AXIS_LEFTY), "Left stick");
    stick(dl, o + ImVec2(360 * S, 130 * S), r * 0.8f, ax(SDL_CONTROLLER_AXIS_RIGHTX), ax(SDL_CONTROLLER_AXIS_RIGHTY), "Right stick (C)");
    float bx = 560 * S, by = 100 * S, br = 20 * S;
    btn_dot(dl, o + ImVec2(bx + 50 * S, by), br, bt(SDL_CONTROLLER_BUTTON_B), "B");
    btn_dot(dl, o + ImVec2(bx, by + 50 * S), br, bt(SDL_CONTROLLER_BUTTON_A), "A");
    btn_dot(dl, o + ImVec2(bx, by - 50 * S), br, bt(SDL_CONTROLLER_BUTTON_Y), "Y");
    btn_dot(dl, o + ImVec2(bx - 50 * S, by), br, bt(SDL_CONTROLLER_BUTTON_X), "X");
    float dx = 700 * S, dy = 100 * S;
    btn_dot(dl, o + ImVec2(dx, dy - 40 * S), br * 0.8f, bt(SDL_CONTROLLER_BUTTON_DPAD_UP), "^");
    btn_dot(dl, o + ImVec2(dx, dy + 40 * S), br * 0.8f, bt(SDL_CONTROLLER_BUTTON_DPAD_DOWN), "v");
    btn_dot(dl, o + ImVec2(dx - 40 * S, dy), br * 0.8f, bt(SDL_CONTROLLER_BUTTON_DPAD_LEFT), "<");
    btn_dot(dl, o + ImVec2(dx + 40 * S, dy), br * 0.8f, bt(SDL_CONTROLLER_BUTTON_DPAD_RIGHT), ">");
    btn_dot(dl, o + ImVec2(230 * S, 30 * S), br * 0.8f, bt(SDL_CONTROLLER_BUTTON_LEFTSHOULDER), "L");
    btn_dot(dl, o + ImVec2(490 * S, 30 * S), br * 0.8f, bt(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER), "R");
    btn_dot(dl, o + ImVec2(270 * S, 190 * S), br * 0.8f, bt(SDL_CONTROLLER_BUTTON_BACK), "Bk");
    btn_dot(dl, o + ImVec2(330 * S, 190 * S), br * 0.8f, bt(SDL_CONTROLLER_BUTTON_START), "St");
    // triggers
    float lt = ax(SDL_CONTROLLER_AXIS_TRIGGERLEFT), rt = ax(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    dl->AddRectFilled(o + ImVec2(20 * S, 200 * S), o + ImVec2(20 * S + 100 * S, 212 * S), COL_PANEL2, 6 * S);
    dl->AddRectFilled(o + ImVec2(20 * S, 200 * S), o + ImVec2(20 * S + 100 * S * std::max(0.f, lt), 212 * S), COL_ACCENT, 6 * S);
    dl->AddRectFilled(o + ImVec2(450 * S, 200 * S), o + ImVec2(550 * S, 212 * S), COL_PANEL2, 6 * S);
    dl->AddRectFilled(o + ImVec2(450 * S, 200 * S), o + ImVec2(450 * S + 100 * S * std::max(0.f, rt), 212 * S), COL_ACCENT, 6 * S);
    dl->AddText(o + ImVec2(20 * S, 214 * S), IM_COL32(150, 152, 165, 255), "LT");
    dl->AddText(o + ImVec2(450 * S, 214 * S), IM_COL32(150, 152, 165, 255), "RT");
}

