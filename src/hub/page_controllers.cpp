#include "app.h"

#include "controls_sdl.h"

/* ---------- Controllers ---------- */

bool g_ctl_pads_dirty = true;   // the Controllers page re-reads its pad list

void refresh_pads() {
    for (auto* p : A.pads) SDL_GameControllerClose(p);
    A.pads.clear();
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i))
            if (SDL_GameController* c = SDL_GameControllerOpen(i)) A.pads.push_back(c);
    if (A.pad_sel >= (int)A.pads.size()) A.pad_sel = 0;
    g_ctl_pads_dirty = true;
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

namespace {

struct Tab { const char* key; const char* label; const char* long_name; };
const Tab kTabs[] = {{"n64", "N64", "Nintendo 64"}, {"retro", "GB / GBA / DS", "Game Boy, GBA and DS"},
                     {"gc", "GameCube", "GameCube"}, {"wiiu", "Wii U", "Wii U"}, {"3ds", "3DS", "Nintendo 3DS"}};
constexpr int kNumTabs = 5;
constexpr double kCaptureSecs = 8.0;

struct CtlState {
    hubctl::Store st;
    bool loaded = false;
    double last_draw = -100;
    int tab = 0;
    std::string game, game_title;          // per-game mode ("" = system controls)
    std::vector<hubctl::PadInfo> pads;
    double pads_at = -100;
    int fake_pads = 0;                     // HUB_SHOT_FAKE_PADS
    bool shot_nosave = false;              // HUB_SCREENSHOT run: never write controls.json
    hubctl::Capture cap;
    std::string cap_control, cap_label;
    double cap_t0 = 0;
    bool open_capture = false, fake_capture = false, demo_done = false;
    int dev = 0;                           // 0 = controller bindings, 1 = keyboard bindings
    bool cap_keyboard = false;
    bool key_rest[hubctl::kKeyStateSize] = {};   // keys already down when the keyboard capture began
    std::string notice;
    double notice_until = 0;
    bool assign_on = false;                // "Assign players" mode
    hubctl::PortAssign assign;
    std::map<std::string, bool> assign_prev;   // pad key -> A held last frame (rising edge = press)
    double assign_t = 0;                   // time of the last press (or of the start): the timeout counts from it
};
CtlState CTL;
void ctl_save();
void ctl_reload_pads();
constexpr double kAssignTimeoutSecs = 30.0;

SDL_GameController* open_handle_for(const hubctl::PadInfo& p) {
    if (p.sdl_index < 0) return nullptr;
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(p.sdl_index);
    for (auto* c : A.pads) if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)) == id) return c;
    return nullptr;
}

std::string pad_name_for_key(const std::string& key) {
    for (const auto& p : CTL.pads) if (p.key == key) return p.name;
    return key;
}

void start_assign() {
    ctl_reload_pads();   // pads may have changed since the page last looked
    CTL.assign.start(ImGui::GetTime());
    CTL.assign_t = ImGui::GetTime();
    CTL.assign_on = true;
    CTL.assign_prev.clear();
    SDL_GameControllerUpdate();
    for (const auto& p : CTL.pads) {   // a button already held when the mode opens does not count as a press
        SDL_GameController* c = open_handle_for(p);
        CTL.assign_prev[p.key] = c && SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A);
    }
}

/* Ends the mode and stores the ports assigned so far (the other pads keep their order after them). */
void finish_assign() {
    if (!CTL.assign.assigned().empty()) {
        std::vector<std::string> all;
        for (const auto& p : CTL.pads) all.push_back(p.key);
        hubctl::set_port_order(CTL.st, CTL.assign.order(all));
        ctl_save();
        g_ctl_pads_dirty = true;
    }
    CTL.assign.end();
    CTL.assign_on = false;
}

/* One frame of the mode: rising edge of A on a connected pad takes the next port; Esc or a timeout ends it. */
void poll_assign(double now) {
    if (!CTL.assign.active()) { finish_assign(); return; }
    SDL_GameControllerUpdate();
    for (const auto& p : CTL.pads) {
        SDL_GameController* c = open_handle_for(p);
        if (!c) continue;
        bool down = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) != 0;
        bool was = CTL.assign_prev[p.key];
        CTL.assign_prev[p.key] = down;
        if (down && !was && CTL.assign.press(p.key, now)) CTL.assign_t = now;
    }
    if (CTL.assign.finished() || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { finish_assign(); return; }
    if (!CTL.shot_nosave && now - CTL.assign_t >= kAssignTimeoutSecs) finish_assign();
}

void draw_assign_panel() {
    begin_panel("assign");
    section_title("Assign players");
    text_wrapped_disabled("Press A on the controller for each player in turn. The first controller to press A takes that port. "
                          "Esc or Done keeps the ports set so far; the mode also ends after 30 seconds without a press.");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    const int next = CTL.assign.next_port();
    ImGui::PushFont(ui::F.h2);
    if (CTL.assign.finished()) ImGui::TextUnformatted("All ports are set.");
    else ImGui::Text("Press A on the controller for Player %d", next);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, sp(0.75f)));
    for (int port = 1; port <= hubctl::PortAssign::kPorts; ++port) {
        const auto& got = CTL.assign.assigned();
        bool set = port <= (int)got.size();
        std::string line = "Port " + std::to_string(port) + "   " + (set ? pad_name_for_key(got[port - 1]) : (port == next && !CTL.assign.finished() ? "waiting for A" : "no controller"));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 o = ImGui::GetCursorScreenPos();
        float rh = sp(4.5f), rw = ImGui::GetContentRegionAvail().x;
        dl->AddRectFilled(o, o + ImVec2(rw, rh), set ? ui::PANEL2 : ui::scale_alpha(ui::PANEL2, 0.45f), sp(1.5f));
        dl->AddText(ui::F.bold, ui::F.bold->FontSize, o + ImVec2(sp(2.5f), (rh - ui::F.bold->FontSize) * 0.5f), set ? ui::TEXT : ui::TEXT2, line.c_str());
        ImGui::Dummy(ImVec2(rw, rh + sp(0.75f)));
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::TextDisabled("Ends in %d s without a press", (int)std::ceil(std::max(0.0, kAssignTimeoutSecs - (ImGui::GetTime() - CTL.assign_t))));
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (button("Done", ImVec2(sp(14.f), sp(5.f)), true, true, ui::Icon::Check)) finish_assign();
    ImGui::SameLine(0, sp(1.5f));
    if (button("Start over", ImVec2(sp(14.f), sp(5.f)), false, true, ui::Icon::Refresh)) start_assign();
    end_panel();
}

int tab_for_key(const std::string& sys) {
    for (int i = 0; i < kNumTabs; ++i) if (sys == kTabs[i].key) return i;
    return 0;
}

void ctl_save() {
    if (CTL.shot_nosave) return;
    if (!hubctl::save_store(CTL.st, hub::data_dir(), hub::saves::vault_dir())) {
        CTL.notice = "Could not save the controller settings.";
        CTL.notice_until = ImGui::GetTime() + 6.0;
    }
}

void ctl_reload_pads() {
    CTL.pads.clear();
    if (CTL.fake_pads > 0) {
        static const char* names[] = {"Xbox Wireless Controller", "DualSense Wireless Controller", "8BitDo Pro 2", "Switch Pro Controller"};
        std::vector<hubctl::PadInfo> base;
        std::vector<std::string> keys;
        for (int i = 0; i < CTL.fake_pads && i < 4; ++i) {
            hubctl::PadInfo p;
            p.sdl_index = -1;
            p.name = names[i];
            p.guid = p.key = "fake-pad-" + std::to_string(i);
            p.xinput = i == 0;
            base.push_back(p);
            keys.push_back(p.key);
        }
        for (int idx : hubctl::order_pads(CTL.st.port_order, keys)) { base[idx].port = (int)CTL.pads.size(); CTL.pads.push_back(base[idx]); }
    } else {
        CTL.pads = hubctl::list_pads(CTL.st.port_order);
    }
    CTL.pads_at = ImGui::GetTime();
    g_ctl_pads_dirty = false;
}

void ctl_move_pad(int from, int to) {
    if (from < 0 || to < 0 || from >= (int)CTL.pads.size() || to >= (int)CTL.pads.size() || from == to) return;
    std::vector<std::string> keys;
    for (auto& p : CTL.pads) keys.push_back(p.key);
    std::string k = keys[from];
    keys.erase(keys.begin() + from);
    keys.insert(keys.begin() + to, k);
    hubctl::set_port_order(CTL.st, keys);
    ctl_save();
    ctl_reload_pads();
}

bool has_override() { return !CTL.game.empty() && CTL.st.game_override.count(CTL.game) > 0; }

void ctl_begin_capture(const std::string& control, const std::string& label) {
    CTL.cap_keyboard = CTL.dev == 1;
    if (CTL.cap_keyboard) {
        int n = 0;
        const Uint8* k = SDL_GetKeyboardState(&n);
        for (int i = 0; i < hubctl::kKeyStateSize; ++i) CTL.key_rest[i] = k && i < n && k[i];
    }
    CTL.cap_control = control;
    CTL.cap_label = label;
    CTL.cap_t0 = ImGui::GetTime();
    if (!CTL.cap_keyboard) CTL.cap.start("");
    CTL.open_capture = true;
}

void draw_capture_modal() {
    if (CTL.open_capture) { CTL.open_capture = false; ImGui::OpenPopup("##ctl_capture"); }
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(sp(52.f), io.DisplaySize.x * 0.9f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    // NoNav: the pad button you press to bind must not also click anything in this dialog
    bool open = ImGui::BeginPopupModal("##ctl_capture", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);
    if (!open) return;
    double left = kCaptureSecs - (ImGui::GetTime() - CTL.cap_t0);
    bool waiting = CTL.cap_keyboard || CTL.cap.active() || CTL.fake_capture;
    bool close = false;
    std::string tok;
    if (CTL.cap_keyboard) {
        int n = 0;
        const Uint8* k = SDL_GetKeyboardState(&n);
        for (int i = 0; k && i < n && i < hubctl::kKeyStateSize && tok.empty(); ++i) {
            if (!k[i]) CTL.key_rest[i] = false;
            else if (!CTL.key_rest[i] && hubctl::key_name(i)[0] && i != SDL_SCANCODE_ESCAPE) tok = hubctl::key_name(i);
        }
    } else if (CTL.cap.active()) tok = CTL.cap.poll();
    if (!tok.empty()) {
        std::string err;
        bool ok = CTL.cap_keyboard ? hubctl::set_key(CTL.st, kTabs[CTL.tab].key, CTL.game, CTL.cap_control, tok, &err)
                                   : hubctl::set_mapping(CTL.st, kTabs[CTL.tab].key, CTL.game, CTL.cap_control, tok, &err);
        if (ok) ctl_save();
        close = true;
    }
    if (left <= 0 && !CTL.fake_capture) close = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
    ImGui::PushFont(ui::F.h2);
    ImGui::Text("Set \"%s\"", CTL.cap_label.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (waiting) {
        ImGui::TextUnformatted(CTL.cap_keyboard ? "Press the key you want to use." : "Press a button on your controller, or push a stick or trigger.");
        float frac = CTL.fake_capture ? 0.6f : (float)std::max(0.0, std::min(1.0, left / kCaptureSecs));
        ImVec2 o = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = sp(0.75f);
        ImGui::Dummy(ImVec2(w, h + sp(1.f)));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        o.y += sp(0.5f);
        dl->AddRectFilled(o, o + ImVec2(w, h), COL_PANEL2, h);
        dl->AddRectFilled(o, o + ImVec2(w * frac, h), COL_ACCENT, h);
        ImGui::TextDisabled(CTL.cap_keyboard ? "Cancels by itself in %d s. Click Cancel to stop now." : "Cancels by itself in %d s. Press Esc or click Cancel to stop now.", (int)std::ceil(std::max(0.0, left)));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("No controller found. Plug one in and try again.");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, sp(1.5f)));
    if (button("Cancel", ImVec2(sp(14.f), sp(5.f)), false, true, ui::Icon::Close)) close = true;
    if (close) { CTL.cap.cancel(); CTL.fake_capture = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void draw_pads_panel() {
    begin_panel("padlist");
    section_title("Connected controllers");
    ImGui::TextDisabled("Port 1 is Player 1. Reorder them here; the order is remembered for every system.");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (button("Assign players", ImVec2(0, sp(4.5f)), false, true, ui::Icon::Gamepad)) start_assign();
    ImGui::SameLine(0, sp(1.f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Press A on each controller in turn to set Player 1, 2, 3, 4.");
    ImGui::Dummy(ImVec2(0, sp(0.75f)));
    int move_from = -1, move_to = -1;
    for (size_t i = 0; i < CTL.pads.size(); ++i) {
        const hubctl::PadInfo& p = CTL.pads[i];
        ImGui::PushID((int)i);
        ImVec2 o = ImGui::GetCursorScreenPos();
        float rh = sp(5.5f), rw = ImGui::GetContentRegionAvail().x;
        bool sel = A.pad_sel == (int)i;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(o, o + ImVec2(rw, rh), sel ? ui::PANEL2 : ui::scale_alpha(ui::PANEL2, 0.45f), sp(1.5f));
        ui::icon(dl, ui::Icon::Gamepad, o + ImVec2(sp(2.5f), rh * 0.5f), sp(1.75f), sel ? ui::ACCENT : ui::TEXT2);
        char nm[300];
        std::snprintf(nm, sizeof nm, "%s%s", p.name.c_str(), p.xinput ? "  (Xbox)" : "");
        dl->AddText(ui::F.bold, ui::F.bold->FontSize, o + ImVec2(sp(5.f), (rh - ui::F.bold->FontSize) * 0.5f), ui::TEXT, nm);
        // click on the row to pick it for the test input
        ImGui::SetCursorScreenPos(o);
        if (ImGui::InvisibleButton("row", ImVec2(std::max(1.f, rw * 0.35f), rh))) A.pad_sel = (int)i;
        // buttons from the right edge
        float bw = sp(11.f), gap = sp(0.75f), bh = rh - sp(1.f);
        float x = o.x + rw - sp(0.5f);
        ImGui::SetCursorScreenPos(ImVec2(x - bw, o.y + sp(0.5f)));
        if (button("Move down", ImVec2(bw, bh), false, i + 1 < CTL.pads.size())) { move_from = (int)i; move_to = (int)i + 1; }
        x -= bw + gap;
        ImGui::SetCursorScreenPos(ImVec2(x - bw, o.y + sp(0.5f)));
        if (button("Move up", ImVec2(bw, bh), false, i > 0)) { move_from = (int)i; move_to = (int)i - 1; }
        x -= bw + gap;
        if (i > 0) {
            float aw = sp(15.f);
            ImGui::SetCursorScreenPos(ImVec2(x - aw, o.y + sp(0.5f)));
            if (button("Make Player 1", ImVec2(aw, bh), false, true)) { move_from = (int)i; move_to = 0; }
            x -= aw + gap;
        } else {
            float aw = sp(13.f);
            ImGui::SetCursorScreenPos(ImVec2(x - aw, o.y + (rh - sp(2.75f)) * 0.5f));
            pill("always Player 1", ui::mix(ui::GOOD, IM_COL32_BLACK, 0.35f));
            x -= aw + gap;
        }
        char pt[24];
        if (i < 4) std::snprintf(pt, sizeof pt, "Port %d", (int)i + 1); else std::snprintf(pt, sizeof pt, "unused");
        ImGui::SetCursorScreenPos(ImVec2(x - sp(6.f), o.y + (rh - sp(2.75f)) * 0.5f));
        pill(pt, i < 4 ? COL_ACCENT : COL_PANEL2);
        ImGui::SetCursorScreenPos(o + ImVec2(0, rh + sp(0.75f)));
        ImGui::Dummy(ImVec2(rw, 0));
        ImGui::PopID();
    }
    end_panel();
    if (move_from >= 0) ctl_move_pad(move_from, move_to);
}

void draw_empty_pads() {
    begin_panel("nopad", ImVec2(0, sp(30.f)));
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
    end_panel();
}

void draw_mapping_panel() {
    const std::string sys = kTabs[CTL.tab].key;
    const hubctl::Profile cur = hubctl::resolve(CTL.st, sys, CTL.game);
    const hubctl::Profile def = hubctl::default_profile(sys);
    const bool game_mode = !CTL.game.empty();
    const bool editable = !game_mode || has_override();
    begin_panel("mapping");
    section_title(game_mode ? std::string("Controls for this game") : std::string(kTabs[CTL.tab].long_name) + " controls");
    if (game_mode) {
        ImGui::TextDisabled("%s", CTL.game_title.c_str());
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        bool on = has_override();
        if (ImGui::Checkbox("Use different controls for this game", &on)) {
            if (on) hubctl::edit_profile(CTL.st, sys, CTL.game); else hubctl::reset(CTL.st, sys, CTL.game);
            ctl_save();
        }
        std::string sn = kTabs[CTL.tab].long_name;
        text_wrapped_disabled(on ? "Changes below apply to this game only. Turn this off to go back to your " + sn + " controls."
                                 : "This game uses your " + sn + " controls. Turn this on to give it its own.");
    } else {
        text_wrapped_disabled("Pick what each control does. Changes are saved at once and used the next time you start a game.");
    }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (!editable) { end_panel(); return; }

    const bool kb_ok = hubctl::has_keyboard(sys);
    if (!kb_ok) CTL.dev = 0;
    if (kb_ok) {
        const char* dl[2] = {"Controller", "Keyboard"};
        CTL.dev = ui::segmented("devtab", dl, 2, CTL.dev);
        text_wrapped_disabled(CTL.dev == 1 ? "The keyboard is a pad of its own for Player 1, used together with the first controller. Click Change, then press a key."
                                           : "Buttons and sticks of your controllers.");
        ImGui::Dummy(ImVec2(0, sp(0.75f)));
    } else {
        text_wrapped_disabled("The keyboard can't be remapped for this system: the emulator takes one input source per button.");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
    }
    const bool keys = CTL.dev == 1;

    const int cols = ImGui::GetContentRegionAvail().x > sp(95.f) ? 2 : 1;
    std::vector<const hubctl::ControlInfo*> analogs;
    if (ImGui::BeginTable("mapgrid", cols * 3, ImGuiTableFlags_SizingStretchProp)) {
        for (int c = 0; c < cols; ++c) {
            ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthStretch, 1.f);
            ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, sp(11.f));
        }
        int n = 0;
        for (const auto& ci : keys ? hubctl::key_controls_for(sys) : hubctl::controls_for(sys)) {
            if (ci.analog) { analogs.push_back(&ci); continue; }
            if (n % cols == 0) ImGui::TableNextRow(0, sp(5.5f));
            int c0 = (n % cols) * 3;
            ImGui::PushID(ci.name.c_str());
            ImGui::TableSetColumnIndex(c0);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + sp(1.1f));
            ImGui::TextUnformatted(ci.label.c_str());
            ImGui::TableSetColumnIndex(c0 + 1);
            const auto& cm = keys ? cur.keys : cur.buttons;
            const auto& dm = keys ? def.keys : def.buttons;
            auto it = cm.find(ci.name);
            std::string ex = it == cm.end() ? "" : it->second;
            auto dit = dm.find(ci.name);
            bool changed = dit == dm.end() || dit->second != ex;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + sp(1.1f));
            pill((keys ? hubctl::describe_keys(ex) : hubctl::describe_expr(ex)).c_str(), changed ? COL_ACCENT : ui::PANEL3);
            ImGui::TableSetColumnIndex(c0 + 2);
            if (button("Change", ImVec2(sp(10.f), sp(4.5f)))) ctl_begin_capture(ci.name, ci.label);
            ImGui::PopID();
            ++n;
        }
        ImGui::EndTable();
    }
    if (!keys && !analogs.empty()) {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, sp(0.75f)));
        for (const hubctl::ControlInfo* ci : analogs) {
            ImGui::PushID(ci->name.c_str());
            auto ait = cur.analogs.find(ci->name);
            hubctl::Analog a = ait == cur.analogs.end() ? hubctl::Analog() : ait->second;
            float dz = (float)a.deadzone, se = (float)a.sensitivity;
            settings_label(ci->label.c_str(), "Deadzone ignores small movements near the centre; sensitivity scales how far the stick counts.");
            bool edit = false;
            ImGui::SetNextItemWidth(sp(34.f));
            edit |= ImGui::SliderFloat("##dz", &dz, 0.f, 0.95f, "Deadzone  %.2f");
            bool done = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine(0, sp(2.f));
            if (sys == "n64") {   // the only system whose stick RetroHub itself scales; emulators take a deadzone at most
                ImGui::SetNextItemWidth(sp(34.f));
                edit |= ImGui::SliderFloat("##se", &se, 0.1f, 4.f, "Sensitivity  %.2f");
                done |= ImGui::IsItemDeactivatedAfterEdit();
            }
            if (edit) hubctl::set_analog(CTL.st, sys, CTL.game, ci->name, dz, se);
            if (done) ctl_save();
            ImGui::Dummy(ImVec2(0, sp(0.75f)));
            ImGui::PopID();
        }
    }
    if (!keys && sys != "n64" && !analogs.empty()) text_wrapped_disabled("Sensitivity is only available for N64: the emulators for this system only take a deadzone.");
    if (!keys && sys == "n64") {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        bool rm = cur.rumble;
        if (ImGui::Checkbox("Rumble Pak (offline only)", &rm)) { hubctl::set_rumble(CTL.st, sys, CTL.game, rm); ctl_save(); }
        text_wrapped_disabled("Shakes a controller that supports rumble in games that use the Rumble Pak. It takes the place of the Controller Pak for this game, "
                              "so memory pack saves are not used while it is on. Online games never use it.");
    }
    if (!game_mode) {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        bool custom = CTL.st.system_default.count(sys) > 0;
        if (button("Reset to defaults", ImVec2(0, sp(5.f)), false, custom, ui::Icon::Refresh)) {
            hubctl::reset(CTL.st, sys);
            ctl_save();
        }
        if (!custom) { ImGui::SameLine(0, sp(1.5f)); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Already using the defaults."); }
    }
    end_panel();
}

void draw_test_panel() {
    if (CTL.pads.empty()) return;
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Test input");
    ImGui::PopFont();
    ImGui::TextDisabled("Select a controller above, then move sticks and press buttons to see them light up.");
    SDL_GameController* pad = nullptr;
    int sel = std::min(A.pad_sel, (int)CTL.pads.size() - 1);
    if (sel >= 0 && CTL.pads[sel].sdl_index >= 0) {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(CTL.pads[sel].sdl_index);
        for (auto* c : A.pads) if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)) == id) pad = c;
    }
    ImVec2 o = ImGui::GetCursorScreenPos();
    float h = 230 * S;
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, o + ImVec2(ImGui::GetItemRectSize().x, h), COL_PANEL, 14 * S);
    auto ax = [&](SDL_GameControllerAxis a) { if (!pad) return 0.f; float v = SDL_GameControllerGetAxis(pad, a) / 32767.f; return std::max(-1.f, std::min(1.f, v)); };
    auto bt = [&](SDL_GameControllerButton b) { return pad && SDL_GameControllerGetButton(pad, b) != 0; };
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
    float lt = ax(SDL_CONTROLLER_AXIS_TRIGGERLEFT), rt = ax(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    dl->AddRectFilled(o + ImVec2(20 * S, 200 * S), o + ImVec2(20 * S + 100 * S, 212 * S), COL_PANEL2, 6 * S);
    dl->AddRectFilled(o + ImVec2(20 * S, 200 * S), o + ImVec2(20 * S + 100 * S * std::max(0.f, lt), 212 * S), COL_ACCENT, 6 * S);
    dl->AddRectFilled(o + ImVec2(450 * S, 200 * S), o + ImVec2(550 * S, 212 * S), COL_PANEL2, 6 * S);
    dl->AddRectFilled(o + ImVec2(450 * S, 200 * S), o + ImVec2(450 * S + 100 * S * std::max(0.f, rt), 212 * S), COL_ACCENT, 6 * S);
    dl->AddText(o + ImVec2(20 * S, 214 * S), IM_COL32(150, 152, 165, 255), "LT");
    dl->AddText(o + ImVec2(450 * S, 214 * S), IM_COL32(150, 152, 165, 255), "RT");
}

}  // namespace

/* Library details -> "Controls for this game". */
void open_controls_for(const hub::GameEntry& e) {
    std::string sys = hubctl::system_key(e.system);
    if (sys.empty()) { set_notice("There are no controller settings for this system."); return; }
    CTL.game = e.key;
    CTL.game_title = e.title;
    CTL.tab = tab_for_key(sys);
    CTL.loaded = false;   // re-read on the next frame
    A.page = Page::Controllers;
}

/* Writes the resolved profile for a launch and returns " --controls <file>" ("" when there is none: the runner then uses
 * its built-in mapping). Mapping only changes which pad source feeds which control; the input that is sent to other
 * players (n64run / retrorun netplay) is the same packed state as before. */
std::string controls_arg(const std::string& system_key, const std::string& game_key) {
    if (system_key.empty()) return "";
    hubctl::Store st;
    hubctl::load_store(hub::data_dir(), hub::saves::vault_dir(), &st);
    hubctl::Profile p = hubctl::resolve(st, system_key, game_key);
    std::string path = hub::data_dir() + "controls_run_" + system_key + ".json";
    if (!hubctl::write_profile_file(path, p)) return "";
    return " --controls " + quote(path);
}

static std::vector<hubemu::PadDesc> pad_descs(const std::vector<hubctl::PadInfo>& v) {
    std::vector<hubemu::PadDesc> out;
    for (auto& p : v) { hubemu::PadDesc d; d.name = p.name; d.guid = p.guid; d.key = p.key; d.xinput = p.xinput; out.push_back(d); }
    return out;
}

/* Dolphin: which device each port uses, following the RetroHub port order (XInput pads stay XInput, others go through
 * Dolphin's SDL backend). */
hubemu::DolphinPads dolphin_pads_for(const hubctl::Profile& prof) {
    hubctl::load_bundled_db();
    return hubemu::dolphin_pads_with(hubemu::detect_dolphin_pads(), pad_descs(hubctl::list_pads(prof.port_order)), pad_descs(hubctl::list_pads()));
}

/* Azahar: player 1's pad when the player chose a port order, else "any pad" as before. */
hubemu::AzPad azahar_pad_for(const hubctl::Profile& prof) {
    hubemu::AzPad pad;
    if (prof.port_order.empty()) return pad;
    hubctl::load_bundled_db();
    hubemu::azahar_pad_select(pad_descs(hubctl::list_pads(prof.port_order)), pad_descs(hubctl::list_pads()), &pad);
    return pad;
}

/* Profile for the external emulators' prepare_*: per-game override when the game has one. */
hubctl::Profile controls_profile(const std::string& system_key, const std::string& game_key) {
    hubctl::Store st;
    hubctl::load_store(hub::data_dir(), hub::saves::vault_dir(), &st);
    return hubctl::resolve(st, system_key, game_key);
}

void page_controllers() {
    static bool inited = false;
    if (!inited) {
        inited = true;
        CTL.shot_nosave = std::getenv("HUB_SCREENSHOT") != nullptr;
        if (const char* f = std::getenv("HUB_SHOT_FAKE_PADS")) CTL.fake_pads = std::atoi(f);
        if (const char* as = std::getenv("HUB_SHOT_ASSIGN")) {   // screenshot aid: assign mode with N fake presses already made
            start_assign();
            for (int i = 0, n = std::atoi(as); i < n; ++i) CTL.assign.press("fake-pad-" + std::to_string(i), ImGui::GetTime());
        }
        hubctl::load_bundled_db();
        if (const char* t = std::getenv("HUB_SHOT_CTL_SYS")) CTL.tab = tab_for_key(t);
        if (const char* g = std::getenv("HUB_SHOT_CTL_GAME")) {
            if (const hub::GameEntry* e = find_entry(g)) { CTL.game = e->key; CTL.game_title = e->title; CTL.tab = tab_for_key(hubctl::system_key(e->system)); }
        }
    }
    double now = ImGui::GetTime();
    if (!CTL.loaded || now - CTL.last_draw > 0.75) {   // page just opened: take the file as it is on disk
        hubctl::load_store(hub::data_dir(), hub::saves::vault_dir(), &CTL.st);
        CTL.loaded = true;
        g_ctl_pads_dirty = true;
    }
    if (!CTL.demo_done && std::getenv("HUB_SHOT_CTL_DEMO")) {   // screenshot aid: a few custom bindings (never saved)
        CTL.demo_done = true;
        for (const char* sys : {"n64", "retro", "gc"}) {
            std::string gm = (CTL.game.empty() || hubctl::system_key(find_entry(CTL.game) ? find_entry(CTL.game)->system : hub::System::N64) != sys) ? "" : CTL.game;
            if (std::string(sys) == "n64") { hubctl::set_key(CTL.st, sys, gm, "a", "space|x"); hubctl::set_key(CTL.st, sys, gm, "start", "return|kp_enter"); hubctl::set_mapping(CTL.st, sys, gm, "a", "b"); hubctl::set_mapping(CTL.st, sys, gm, "z", "rightshoulder"); hubctl::set_analog(CTL.st, sys, gm, "stick", 0.15, 1.25); }
            else if (std::string(sys) == "gc") { hubctl::set_mapping(CTL.st, sys, gm, "x", "x|rightx+"); hubctl::set_analog(CTL.st, sys, gm, "main_stick", 0.12, 1.0); }
            else hubctl::set_mapping(CTL.st, sys, gm, "a", "a|y");
        }
        if (!CTL.game.empty()) hubctl::edit_profile(CTL.st, kTabs[CTL.tab].key, CTL.game);
    }
    if (std::getenv("HUB_SHOT_CTL_KEYS")) CTL.dev = 1;   // screenshot aid: open on the Keyboard tab
    if (std::getenv("HUB_SHOT_CTL_CAPTURE")) {
        static bool once = false;
        if (!once) { once = true; CTL.fake_capture = CTL.fake_pads > 0; ctl_begin_capture("a", "A"); }
    }
    CTL.last_draw = now;
    if (g_ctl_pads_dirty || now - CTL.pads_at > 2.0) ctl_reload_pads();
    if (A.pad_sel >= (int)CTL.pads.size()) A.pad_sel = 0;

    bool game_mode = !CTL.game.empty();
    page_header(game_mode ? "Controls for this game" : "Controllers",
                game_mode ? "Give this game its own button layout, or keep using your system controls." : "Controller order and button layout for every system.");
    if (game_mode) {
        if (button("Back to all controllers", ImVec2(0, sp(4.5f)), false, true, ui::Icon::ChevronL)) { CTL.game.clear(); CTL.game_title.clear(); }
        ImGui::Dummy(ImVec2(0, sp(1.f)));
    }
    if (CTL.assign_on) poll_assign(now);
    if (CTL.assign_on) {   // "Assign players" replaces the page body until Done, Esc or the timeout
        draw_assign_panel();
        return;
    }
    if (!CTL.notice.empty() && now < CTL.notice_until) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextUnformatted(CTL.notice.c_str());
        ImGui::PopStyleColor();
    }

    if (!game_mode) {
        if (CTL.pads.empty()) draw_empty_pads(); else draw_pads_panel();
        ImGui::Dummy(ImVec2(0, sp(1.5f)));
        const char* labels[kNumTabs];
        for (int i = 0; i < kNumTabs; ++i) labels[i] = kTabs[i].label;
        CTL.tab = ui::segmented("systab", labels, kNumTabs, CTL.tab);
        ImGui::Dummy(ImVec2(0, sp(1.f)));
    }
    draw_mapping_panel();
    if (!game_mode) draw_test_panel();
    draw_capture_modal();
}

/* Library details, local multiplayer: the ports with their current pad. Returns true when "Reassign" was clicked. */
bool draw_players_strip(int players) {
    if (players < 2) return false;
    hubctl::Store st;
    hubctl::load_store(hub::data_dir(), hub::saves::vault_dir(), &st);
    std::vector<hubctl::PadInfo> pads = hubctl::list_pads(st.port_order);
    bool clicked = false;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Players:");
    for (int i = 0; i < players && i < hubctl::PortAssign::kPorts; ++i) {
        ImGui::SameLine(0, sp(1.f));
        bool has = i < (int)pads.size();
        std::string t = std::to_string(i + 1) + "  " + (has ? pads[i].name : std::string("none"));
        pill(t.c_str(), has ? COL_ACCENT : COL_PANEL2);
    }
    ImGui::SameLine(0, sp(1.5f));
    if (button("Reassign", ImVec2(0, sp(4.f)), false, true, ui::Icon::Gamepad)) clicked = true;
    return clicked;
}

/* Opens the Controllers page straight in "Assign players" (library details -> Reassign). */
void open_port_assign() {
    CTL.game.clear();
    CTL.game_title.clear();
    CTL.loaded = false;
    A.page = Page::Controllers;
    start_assign();
}
