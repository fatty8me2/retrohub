#include "lichii.h"
#include "app.h"
#include "capture_hold_logic.h"
#include "ctl_pad_logic.h"
#include "glyph_logic.h"
#include "osk.h"
#include "controls_live.h"

#include "controls_sdl.h"
#include "pad_name_logic.h"

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
    dl->AddCircle(c, r, ui::veil(40/255.f), 40, 2);
    dl->AddCircleFilled(c + ImVec2(x, y) * (r * 0.7f), r * 0.28f, COL_ACCENT, 24);
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(c + ImVec2(-ts.x * 0.5f, r + 6 * S), ui::TEXT2, label);
}
void btn_dot(ImDrawList* dl, ImVec2 c, float r, bool on, const char* label) {
    dl->AddCircleFilled(c, r, on ? COL_ACCENT : COL_PANEL2, 24);
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(c - ts * 0.5f, on ? IM_COL32_WHITE : ui::TEXT2, label);
}

namespace {

struct Tab { const char* key; const char* label; const char* long_name; };
const Tab kTabs[] = {{"n64", "N64", "Nintendo 64"}, {"retro", "GB / GBA / DS", "Game Boy, GBA and DS"},
                     {"ps1", "PlayStation", "PlayStation (DualShock)"},
                     {"gc", "GameCube", "GameCube"}, {"wiiu", "Wii U", "Wii U"}, {"3ds", "3DS", "Nintendo 3DS"}, {"ps2", "PS2", "PlayStation 2"},
                     {"nes", "NES", "Nintendo Entertainment System"}, {"snes", "SNES", "Super Nintendo"}, {"psp", "PSP", "PlayStation Portable"}, {"switch", "Switch", "Nintendo Switch"},
                     {"switch2", "Switch 2", "Nintendo Switch 2"}};   // Switch 2 is its own console; it uses the Switch controls until it has its own
constexpr int kNumTabs = (int)(sizeof(kTabs) / sizeof(kTabs[0]));
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
    hubui::CaptureHold cap_hold;   // B / Back: tap = assign, hold 1 s = cancel
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
    hubui::AssignAGate assign_a;           // pad key -> A held last frame (rising edge = press)
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
    for (const auto& p : CTL.pads) if (p.key == key) return hubpadname::label(p.name, p.xinput);
    return key;
}

void start_assign() {
    ctl_reload_pads();   // pads may have changed since the page last looked
    CTL.assign.start(ImGui::GetTime());
    CTL.assign_t = ImGui::GetTime();
    CTL.assign_on = true;
    CTL.assign_a.clear();
    SDL_GameControllerUpdate();
    for (const auto& p : CTL.pads) {   // a button already held when the mode opens does not count as a press
        SDL_GameController* c = open_handle_for(p);
        CTL.assign_a.prime(p.key, c && SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A));
    }
}

/* Ends the mode and stores the ports assigned so far (the other pads keep their order after them). */
void finish_assign() {
    ImGui::SetKeyOwner(ImGuiKey_GamepadFaceDown, ImGuiKeyOwner_NoOwner);   // A goes back to navigation on every way out of the mode
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

/* One frame of the mode: rising edge of A on a connected pad takes the next port; B, Esc or a timeout ends it. */
void poll_assign(double now) {
    if (!CTL.assign.active() || (osk::hub_input_blocked() && !CTL.shot_nosave)) { finish_assign(); return; }   // unfocused: end the mode, an in-game press must not claim a port
    SDL_GameControllerUpdate();
    hubui::AssignFrameIn fin;
    fin.active = true;
    fin.b_pressed = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    fin.esc_pressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    fin.y_pressed = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false);
    fin.pad_mode = A.pad_mode;
    fin.all_ports_set = CTL.assign.finished();
    fin.timed_out = !CTL.shot_nosave && now - CTL.assign_t >= kAssignTimeoutSecs;
    const hubui::AssignFrameOut fout = hubui::assign_frame(fin);
    if (fout.end) {
        if (fout.swallow_b) {
            ImGui::SetKeyOwner(ImGuiKey_GamepadFaceRight, ImGuiKeyOwner_NoOwner, ImGuiInputFlags_LockThisFrame);   // this B ends the mode, it does not also leave the page
            ImGui::SetKeyOwner(ImGuiKey_Escape, ImGuiKeyOwner_NoOwner, ImGuiInputFlags_LockThisFrame);
        }
        finish_assign();
        return;
    }
    if (fout.start_over) { start_assign(); return; }
    ImGui::SetKeyOwner(ImGuiKey_GamepadFaceDown, ImGui::GetID("##assignA"));   // A claims ports here; it must not also press Done / Start over (asked for again every frame)
    if (!CTL.shot_nosave && ImGui::GetTime() - CTL.pads_at >= 0.5) {   // hot-plug: refresh the pad list during the mode
        ctl_reload_pads();
        std::vector<std::string> present;
        for (const auto& p : CTL.pads) present.push_back(p.key);
        CTL.assign.sync(present);   // unplugged pads lose their port
        for (const auto& p : CTL.pads) {   // newly plugged: a button already held does not count
            if (CTL.assign_a.known(p.key)) continue;
            SDL_GameController* c = open_handle_for(p);
            CTL.assign_a.prime(p.key, c && SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A));
        }
    }
    for (const auto& p : CTL.pads) {
        SDL_GameController* c = open_handle_for(p);
        if (!c) continue;
        bool down = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) != 0;
        /* ImGui navigation never sees A in this mode (locked above), so the press cannot also activate a focused button */
        if (CTL.assign_a.a_press(p.key, down, false) && CTL.assign.press(p.key, now)) CTL.assign_t = now;
    }
}

void draw_assign_panel() {
    begin_panel("assign");
    section_title("Assign players");
    if (A.pad_mode) {
        const std::string la = hubglyph::label(ui::pad_kind, "A"), lb = hubglyph::label(ui::pad_kind, "B"), ly = hubglyph::label(ui::pad_kind, "Y");
        text_wrapped_disabled("Press " + la + " on the controller for each player in turn. The first controller to press " + la + " takes that port. "
                              "Press " + lb + " when you are done to keep the ports set so far, or " + ly + " to start over. The mode also ends after 30 seconds without a press.");
    } else {
        text_wrapped_disabled("Press A on the controller for each player in turn. The first controller to press A takes that port. "
                              "Esc or Done keeps the ports set so far; the mode also ends after 30 seconds without a press.");
    }
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
        return;
    }
    controls_live_push();   // a built-in runner that is playing right now re-reads the profile on its next frame
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
    CTL.cap_hold = hubui::CaptureHold();
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
    } else if (CTL.cap.active()) {
        /* A pad user cancels by HOLDING B / Back for a second; a short press of them is assigned like any other button (the dialog is NoNav). */
        const double now = ImGui::GetTime();
        const std::string raw = CTL.cap.poll();
        const bool still_held = ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight) || ImGui::IsKeyDown(ImGuiKey_GamepadBack);
        const hubui::CaptureStep st = hubui::capture_hold_step(CTL.cap_hold, raw, still_held, now);
        if (st == hubui::CaptureStep::Commit) tok = CTL.cap_hold.tok;
        else if (st == hubui::CaptureStep::Cancel) close = true;
    }
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
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));   // wrapped: it ran off the dialog (1.40.0 UI QA)
        ImGui::TextWrapped(CTL.cap_keyboard ? "Cancels by itself in %d s. Click Cancel to stop now." : "Cancels by itself in %d s. Hold B to cancel, or press Esc or click Cancel.", (int)std::ceil(std::max(0.0, left)));
        ImGui::PopStyleColor();
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
    ImGui::TextDisabled("Port 1 is Player 1. Use Assign players, or Make Player 1 on a row; the order is remembered for every system.");
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
        const std::string nm = hubpadname::label(p.name, p.xinput);
        dl->AddText(ui::F.bold, ui::F.bold->FontSize, o + ImVec2(sp(5.f), (rh - ui::F.bold->FontSize) * 0.5f), ui::TEXT, nm.c_str());
        // click on the row to pick it for the test input
        ImGui::SetCursorScreenPos(o);
        if (ImGui::InvisibleButton("row", ImVec2(std::max(1.f, rw * 0.35f), rh), ImGuiButtonFlags_EnableNav)) A.pad_sel = (int)i;
        if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) ui::focus_ring(dl, o, o + ImVec2(rw, rh), sp(1.5f));
        // buttons from the right edge
        float gap = sp(0.75f), bh = rh - sp(1.f);
        float x = o.x + rw - sp(0.5f);
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

/* Names the emulator behind a Controllers tab ("Dolphin", "Cemu", ...); "the emulator" for the rest. */
static std::string emu_phrase(const std::string& tab) {
    if (tab == "gc") return "Dolphin";
    if (tab == "wiiu") return "Cemu";
    if (tab == "3ds") return "Azahar";
    if (tab == "ps2") return "PCSX2";
    if (tab == "psp") return "PPSSPP";
    if (tab == "switch" || tab == "switch2") return "Lichii";
    return "the emulator";
}

/* Which deadzone sliders reach the emulator (read from the config writers): GameCube writes a deadzone only above 0 (0 = Dolphin's own),
 * PS2 sends the left stick value as the pad's deadzone (0 = PCSX2's own) and ignores the right stick, PSP sends none. */
static bool deadzone_zero_is_default(const std::string& sys) { return sys == "gc" || sys == "ps2"; }
static bool deadzone_has_effect(const std::string& sys, const std::string& control) {
    if (sys == "psp") return false;
    if (sys == "ps2" && control == "right_stick") return false;
    return true;
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
        text_wrapped_disabled(sys == "wiiu" || sys == "3ds" || sys == "switch" || sys == "switch2"
                                  ? "RetroHub sets up controllers only for this system. It does not set up keyboard keys, so play with a controller, or set keys in " + emu_phrase(sys) + " yourself."
                                  : "The keyboard can't be remapped for this system.");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
    }
    const bool keys = CTL.dev == 1;

    const int cols = ImGui::GetContentRegionAvail().x > sp(95.f) ? 2 : 1;
    std::vector<const hubctl::ControlInfo*> analogs;
    const bool turbo_ui = !keys && hubctl::turbo_supported(sys);
    const int tc = turbo_ui ? 4 : 3;
    float label_w = sp(10.f);   // the label column fits the longest label
    for (const auto& ci : keys ? hubctl::key_controls_for(sys) : hubctl::controls_for(sys))
        if (!ci.analog) label_w = std::max(label_w, ImGui::CalcTextSize(ci.label.c_str()).x + sp(2.f));
    if (ImGui::BeginTable("mapgrid", cols * tc, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoClip)) {
        for (int c = 0; c < cols; ++c) {
            ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, label_w);
            ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 1.f);
            ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, sp(11.f));
            if (turbo_ui) ImGui::TableSetupColumn("t", ImGuiTableColumnFlags_WidthFixed, sp(11.f));
        }
        const float row_h = std::max(sp(5.5f), ImGui::GetFrameHeight() + sp(1.f));   // follows the UI size: a checkbox is never clipped
        float row_top = 0.f;
        /* Every cell is placed from the row's top edge, so a label, a pill, a button and a checkbox of the same row share one centre line. */
        auto put = [&](int col, float item_h) {
            ImGui::TableSetColumnIndex(col);
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, row_top + (row_h - item_h) * 0.5f));
        };
        int n = 0;
        for (const auto& ci : keys ? hubctl::key_controls_for(sys) : hubctl::controls_for(sys)) {
            if (ci.analog) { analogs.push_back(&ci); continue; }
            if (n % cols == 0) {
                ImGui::TableNextRow(0, row_h);
                ImGui::TableSetColumnIndex(0);
                row_top = ImGui::GetCursorScreenPos().y;
            }
            int c0 = (n % cols) * tc;
            ImGui::PushID(ci.name.c_str());
            put(c0, ImGui::GetTextLineHeight());
            ImGui::GetWindowDrawList()->AddText(ImGui::GetCursorScreenPos(), ImGui::GetColorU32(ImGuiCol_Text), ci.label.c_str());   // drawn straight: a text cell after a checkbox cell would inherit its baseline shift
            ImGui::Dummy(ImVec2(label_w, ImGui::GetTextLineHeight()));
            const auto& cm = keys ? cur.keys : cur.buttons;
            const auto& dm = keys ? def.keys : def.buttons;
            auto it = cm.find(ci.name);
            std::string ex = it == cm.end() ? "" : it->second;
            auto dit = dm.find(ci.name);
            bool changed = dit == dm.end() || dit->second != ex;
            put(c0 + 1, ui::F.cap->FontSize + sp(1.f));
            pill((keys ? hubctl::describe_keys(ex) : hubctl::describe_expr(ex)).c_str(), changed ? COL_ACCENT : ui::PANEL3);
            put(c0 + 2, sp(4.5f));
            if (button("Change", ImVec2(sp(10.f), sp(4.5f)))) ctl_begin_capture(ci.name, ci.label);
            if (turbo_ui) {
                put(c0 + 3, ImGui::GetFrameHeight());
                bool tb = hubctl::is_turbo(cur, ci.name);
                if (ImGui::Checkbox("Turbo", &tb)) { hubctl::set_turbo(CTL.st, sys, CTL.game, ci.name, tb); ctl_save(); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto-fire: hold the button to press it over and over (15 times a second).");
            }
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
            if (!deadzone_has_effect(sys, ci->name)) continue;   // RetroHub does not send this value to the emulator: no slider that does nothing
            ImGui::PushID(ci->name.c_str());
            auto ait = cur.analogs.find(ci->name);
            hubctl::Analog a = ait == cur.analogs.end() ? hubctl::Analog() : ait->second;
            float dz = (float)a.deadzone, se = (float)a.sensitivity;
            settings_label(ci->label.c_str(), "Deadzone ignores small movements near the centre; sensitivity scales how far the stick counts.");
            bool edit = false;
            ImGui::SetNextItemWidth(sp(34.f));
            edit |= ImGui::SliderFloat("##dz", &dz, 0.f, 0.95f, deadzone_zero_is_default(sys) && dz <= 0.f ? "Deadzone  Default" : "Deadzone  %.2f");
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
    if (!keys && turbo_ui) text_wrapped_disabled("Turbo buttons auto-fire while held. They are off in online games unless the room host allows them for everyone.");
    else if (!keys && !hubctl::turbo_supported(sys)) text_wrapped_disabled("Turbo buttons: not supported for this system (" + emu_phrase(sys) + " reads the controller itself).");
    if (!keys && sys == "ps2" && !analogs.empty()) text_wrapped_disabled("PCSX2 takes one deadzone for the controller. RetroHub uses the left stick value. Default leaves it to PCSX2.");
    else if (!keys && sys == "psp" && !analogs.empty()) text_wrapped_disabled("PPSSPP keeps its own deadzone. Change it in PPSSPP's settings.");
    else if (!keys && sys != "n64" && !analogs.empty()) text_wrapped_disabled(deadzone_zero_is_default(sys) ? "Sensitivity can only be adjusted for N64. Here you can set the deadzone. Default leaves it to " + emu_phrase(sys) + "."
                                                                                                          : "Sensitivity can only be adjusted for N64. Here you can set the deadzone.");
    if (!keys && sys == "n64") {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        bool rm = cur.rumble;
        if (ImGui::Checkbox("Rumble Pak (offline only)", &rm)) { hubctl::set_rumble(CTL.st, sys, CTL.game, rm); ctl_save(); }
        text_wrapped_disabled("Shakes a controller that supports rumble in games that use the Rumble Pak. It takes the place of the Controller Pak for this game, "
                              "so memory pack saves are not used while it is on. Online games never use it.");
    }
    if (sys == "nes" || sys == "snes") {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        bool lab = hubctl::label_layout_active(CTL.st, sys, CTL.game);
        if (ImGui::Checkbox("Use button labels (Xbox A = console A)", &lab)) { hubctl::set_label_layout(CTL.st, sys, CTL.game, lab); ctl_save(); }
        text_wrapped_disabled("Off (default): positional, like a Nintendo pad. Xbox A (bottom) presses the console's B and Xbox B (right) presses A. "
                              "On: buttons follow the letters printed on your controller.");
        if (sys == "snes") text_wrapped_disabled("Players 3 and 4 use the multitap, which turns on by itself when three or more controllers are connected.");
        else text_wrapped_disabled("Players 3 and 4 use the Four Score adapter, which turns on by itself when three or more controllers are connected.");
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

/* Wii U (Cemu) controls for a Cemu the user already has (moved here from the old Settings > Controllers tab). */
void draw_cemu_panel() {
    static hubemu::Install cm;
    static double looked_at = -100;
    static std::string looked_cemu;
    if (ImGui::GetTime() - looked_at > 2.0 || looked_cemu != A.st.cemu_exe) {
        std::string wiiu_game;
        for (auto& g : A.lib) if (g.system == hub::System::WiiU) { wiiu_game = g.path; break; }
        cm = hubemu::find(hubemu::Kind::Cemu, A.st.cemu_exe, wiiu_game);
        looked_at = ImGui::GetTime();
        looked_cemu = A.st.cemu_exe;
    }
    begin_panel("cemu_ctl");
    section_title("Wii U (Cemu) controls");
    if (cm.exe.empty() || cm.managed) text_wrapped_disabled("These options apply to a Cemu you already have installed; RetroHub's own Cemu always uses this page.");
    if (!cm.exe.empty() && !cm.managed) {
        if (ImGui::Checkbox("Let RetroHub manage Wii U controls", &A.st.cemu_manage_controls)) mark_dirty();
        text_wrapped_disabled("On: before each game RetroHub writes controller profiles from this page into your Cemu (Xbox and other "
                              "SDL pads). Your original controllerProfiles folder is first copied once to controllerProfiles.retrohub-backup. "
                              "Off: your Cemu controls are never touched.");
        if (button("Restore my Cemu controls", ImVec2(0, 0))) {
            const char* ad = std::getenv("APPDATA");
            bool ok = hubemu::cemu_restore_controls(hubemu::cemu_user_data_dir(cm.dir, ad ? ad : ""));
            set_toast(ok ? "Your Cemu controls were restored from the backup." : "No RetroHub backup of your Cemu controls exists yet.", ok, 4.0);
        }
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
        if (osk::hub_input_blocked()) pad = nullptr;   // unfocused: show an idle pad, never the game's inputs
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
    dl->AddText(o + ImVec2(20 * S, 214 * S), ui::TEXT2, "LT");
    dl->AddText(o + ImVec2(450 * S, 214 * S), ui::TEXT2, "RT");
}

}  // namespace

/* Library details -> "Controls for this game". */
std::string CTL_game_for_trace() { return CTL.game; }
void open_controls_for(const hub::GameEntry& e) {
    std::string sys = hubctl::console_key(e);
    if (sys.empty()) { set_notice("There are no controller settings for this system."); return; }
    CTL.game = e.key;
    CTL.game_title = e.title;
    CTL.tab = tab_for_key(sys);
    CTL.loaded = false;   // re-read on the next frame
    A.page = Page::Controllers;
}

std::pair<std::string, std::string> g_launch_controls;
std::string g_controls_test_dir;

/* A Controllers-page save while a built-in runner (retrorun / n64run) plays: rewrite the profile file the runner was started with and queue
 * ReloadControls. Only when the mapping or the port order of THAT game's resolved profile changed (editing another system or game does
 * nothing). Allowed online: the mapping is local and inputs travel as masks. Never touches a window. */
void controls_live_push() {
    if (!A.proc || A.run.controls_path.empty() || A.run.ctl_path.empty()) return;
    hubctl::Store st;
    if (g_controls_test_dir.empty()) hubctl::load_store(hub::data_dir(), hub::saves::vault_dir(), &st);
    else hubctl::load_store(g_controls_test_dir, "", &st);
    const hubctl::Profile next = hubctl::resolve(st, A.run.controls_sys, A.run.key);
    hubctl::Profile cur;
    const bool have = hubctl::read_profile_file(A.run.controls_path, &cur) && cur.system == next.system;
    if (!hubctlive::needs_reload(have, cur, next)) return;
    bool wrote = false;
    for (int i = 0; i < 5 && !(wrote = hubctl::write_profile_file(A.run.controls_path, next)); ++i) Sleep(40);   // the runner may be reading the file right now
    if (!wrote) {   // never fail silently: the next launch regenerates the file from the saved store anyway
        push_toast("Could not update the running game's controls. They apply the next time you start it.", COL_WARN, 6.0);
        return;
    }
    runctl::Command c;
    c.cmd = runctl::Cmd::ReloadControls;
    qm_send_command(c);
}

std::string controls_live_note() {
    if (!A.proc) return "";
    const hub::GameEntry* e = find_entry(A.run.key);
    const bool builtin = !A.run.controls_path.empty() && !A.run.ctl_path.empty();
    return hubctlive::note(hubctlive::mode_for(true, builtin), A.proc_title, e ? hubctlive::emulator_name(e->system) : "");
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
    g_launch_controls = {path, system_key};   // spawn_room() copies it into A.run when this launch really starts
    return " --controls " + quote(path) + (A.in_room && A.room_turbo && hubctl::turbo_supported(system_key) ? " --turbo-online" : "");
}

static std::vector<hubemu::PadDesc> pad_descs(const std::vector<hubctl::PadInfo>& v) {
    std::vector<hubemu::PadDesc> out;
    for (auto& p : v) { hubemu::PadDesc d; d.name = p.name; d.guid = p.guid; d.key = p.key; d.xinput = p.xinput; d.player = p.player_index; out.push_back(d); }
    return out;
}

/* Dolphin: which device each port uses, following the RetroHub port order (XInput pads stay XInput, others go through
 * Dolphin's SDL backend). */
hubemu::DolphinPads dolphin_pads_for(const hubctl::Profile& prof) {
    hubctl::load_bundled_db();
    return hubemu::dolphin_pads_with(hubemu::detect_dolphin_pads(), pad_descs(hubctl::list_pads(prof.port_order)), pad_descs(hubctl::list_pads()));
}

/* PCSX2: players in the RetroHub port order (at most the game's player count), SDL numbers as PCSX2 gives them (pcsx2_sdl_ids). */
hubemu::Ps2Pads ps2_pads_for(const hubctl::Profile& prof, int max_players) {
    hubctl::load_bundled_db();
    return hubemu::pcsx2_pads_with(pad_descs(hubctl::list_pads(prof.port_order)), pad_descs(hubctl::list_pads()), max_players);
}

/* Azahar: player 1's pad when the player chose a port order, else "any pad" as before. */
hubemu::AzPad azahar_pad_for(const hubctl::Profile& prof) {
    hubemu::AzPad pad;
    if (prof.port_order.empty()) return pad;
    hubctl::load_bundled_db();
    hubemu::azahar_pad_select(pad_descs(hubctl::list_pads(prof.port_order)), pad_descs(hubctl::list_pads()), &pad);
    return pad;
}

/* Lichii: player 1 = the first pad of the port order (or, with no stored order, of the default order: XInput first), as SDL guid + n-th pad
 * with that guid, plus that pad's raw joystick bindings read from SDL now (lichii.h SdlBinds). false = no pad connected. */
bool lichii_pad_for(const hubctl::Profile& prof, std::string* guid, int* port, hubemu::SdlBinds* binds) {
    hubctl::load_bundled_db();
    std::vector<hubctl::PadInfo> all = hubctl::list_pads();
    if (all.empty()) return false;
    std::vector<hubemu::PadDesc> by_sdl = pad_descs(all);
    std::vector<hubemu::PadDesc> order = prof.port_order.empty() ? hubemu::default_player_order(by_sdl) : pad_descs(hubctl::list_pads(prof.port_order));
    if (order.empty()) order = hubemu::default_player_order(by_sdl);
    if (!hubemu::lichii_pad_select(order, by_sdl, guid, port) || guid->empty()) return false;
    const hubctl::PadInfo* pick = nullptr;
    for (const auto& p : all) if (p.key == order[0].key) { pick = &p; break; }
    if (!pick) return true;
    if (pick->xinput) {   // Lichii's SDL3 reads an Xbox pad through XInput (guid driver 'x', XInput numbering), not through the RAWINPUT SDL2 reports here
        const std::vector<int> slots = hubemu::xinput_slots(by_sdl);
        int slot = -1;
        for (size_t i = 0; i < by_sdl.size(); ++i) if (by_sdl[i].key == pick->key) slot = slots[i];
        hubemu::XinputId ids[4];
        for (int s = 0; s < 4; ++s) ids[s] = hubemu::lichii_xinput_id(s);
        std::string xg; int xp = 0;
        if (hubemu::lichii_xinput_pad(ids, slot, &xg, &xp)) { *guid = xg; *port = xp; *binds = hubemu::lichii_xinput_binds(); return true; }
    }
    SDL_GameController* c = open_handle_for(*pick);
    if (!c) return true;
    auto conv = [](SDL_GameControllerButtonBind b) {
        hubemu::SdlBinds::B r;
        if (b.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON) { r.type = 'b'; r.idx = b.value.button; }
        else if (b.bindType == SDL_CONTROLLER_BINDTYPE_AXIS) { r.type = 'a'; r.idx = b.value.axis; }
        else if (b.bindType == SDL_CONTROLLER_BINDTYPE_HAT) { r.type = 'h'; r.idx = b.value.hat.hat; r.hat = b.value.hat.hat_mask; }
        return r;
    };
    for (int i = 0; i < 21 && i < SDL_CONTROLLER_BUTTON_MAX; ++i) binds->button[i] = conv(SDL_GameControllerGetBindForButton(c, (SDL_GameControllerButton)i));
    for (int i = 0; i < 6; ++i) binds->axis[i] = conv(SDL_GameControllerGetBindForAxis(c, (SDL_GameControllerAxis)i));
    binds->valid = true;
    *binds = hubemu::lichii_sdl3_binds(*guid, *binds);   // PlayStation / Switch Pro (HIDAPI): SDL3 reads the d-pad as hat 0, not SDL2's buttons 11-14
    return true;
}

/* XInput pads only, at their real XInput slots, in the RetroHub port order (stored order or SDL order). No XInput pad = the single pad on
 * slot 0 as before. PPSSPP takes player 1's slot from this (it reads other pads through DirectInput, which it does not map). */
hubemu::CemuPads cemu_pads_for(const hubctl::Profile& prof) {
    hubctl::load_bundled_db();
    return hubemu::cemu_pads_with(pad_descs(hubctl::list_pads(prof.port_order)));
}

/* Cemu (RetroHub's managed download and a user's own install alike): Wii U player N = the N-th pad of the port order. XInput pads by
 * their XInput slot, any other pad as an SDL player. No stored order = the XInput pads in slot order, then the other pads in SDL order (it used to be player 1 only). */
hubemu::CemuPads cemu_pads_for_own(const hubctl::Profile& prof) {
    hubctl::load_bundled_db();
    auto all = pad_descs(hubctl::list_pads());
    if (prof.port_order.empty()) return hubemu::cemu_pads_with_sdl(hubemu::default_player_order(all), all);   // no stored order: XInput pads first (player 1 = slot 0), then the rest
    return hubemu::cemu_pads_with_sdl(pad_descs(hubctl::list_pads(prof.port_order)), all);
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
            if (const hub::GameEntry* e = find_entry(g)) { CTL.game = e->key; CTL.game_title = e->title; CTL.tab = tab_for_key(hubctl::console_key(*e)); }
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
            std::string gm = (CTL.game.empty() || (find_entry(CTL.game) ? hubctl::console_key(*find_entry(CTL.game)) : std::string("n64")) != sys) ? "" : CTL.game;
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

    {   // a game is running: say whether a change reaches it now (built-in runners) or at the next start (external emulators)
        const std::string live = controls_live_note();
        if (!live.empty()) {
            const hub::GameEntry* re = find_entry(A.run.key);
            const std::string rs = re ? hubctl::console_key(*re) : std::string();
            if (rs == kTabs[CTL.tab].key || game_mode) {   // only for the system being edited
                ImGui::PushStyleColor(ImGuiCol_Text, A.run.controls_path.empty() ? ui::WARN : ui::GOOD);
                ImGui::TextWrapped("%s", live.c_str());
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, sp(0.5f)));
            }
        }
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
    if (!game_mode) draw_cemu_panel();
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
    ImGui::TextDisabled("Controllers:");
    for (int i = 0; i < players && i < hubctl::PortAssign::kPorts; ++i) {
        ImGui::SameLine(0, sp(1.f));
        bool has = i < (int)pads.size();
        std::string t = std::to_string(i + 1) + "  " + (has ? hubpadname::label(pads[i].name, pads[i].xinput) : std::string("no controller"));
        pill(t.c_str(), has ? COL_ACCENT : COL_PANEL2, has ? IM_COL32_WHITE : ui::PILL_FG);   // PILL_FG: white text on the pale Light-theme pill was invisible
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
