#include "app.h"
#include "wizard_logic.h"

/* ---------- First-run setup wizard: Welcome > Controller > Find my games > Name > Done ----------
 * Fully usable with only a gamepad (D-pad/stick moves focus, A = activate/next, B = back) and with mouse/keyboard.
 * Opens by itself once on a fresh install (hub.json setup_done=false and no games set up yet); Settings > "Run setup again"
 * re-opens it. Pure step rules live in wizard_logic.h (unit tested). Screenshot aids:
 *   HUB_SHOT_WIZARD=welcome|controller|test|games|name|done   open that step
 *   HUB_SHOT_FAKE_PAD=<n>[:button,button,...]   pretend n pads are connected (0 = none) with these held:
 *       a b x y lb rb back start guide ls rs up down left right, lt rt (full trigger), lx+ lx- ly+ ly- rx+ ... (stick throw) */

namespace {

using hubui::wiz::Step;

struct PadState {
    bool b[SDL_CONTROLLER_BUTTON_MAX] = {};
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
    bool any() const {
        for (bool v : b) if (v) return true;
        return std::abs(lx) > 0.3f || std::abs(ly) > 0.3f || std::abs(rx) > 0.3f || std::abs(ry) > 0.3f || lt > 0.3f || rt > 0.3f;
    }
};

struct Wizard {
    bool open = false, auto_tried = false, test = false, focus_primary = true;
    Step step = Step::Welcome;
    int fake_n = -1;   // -1 = use real pads
    PadState fake;
    bool find_launched = false;
} WZ;

float axis(SDL_GameController* c, SDL_GameControllerAxis a) {
    float v = SDL_GameControllerGetAxis(c, a) / 32767.f;
    return std::abs(v) < 0.12f ? 0.f : v;
}

int pad_count() { return WZ.fake_n >= 0 ? WZ.fake_n : (int)A.pads.size(); }

std::string pad_name(int i) {
    if (WZ.fake_n >= 0) return i == 0 ? "Xbox Wireless Controller" : "Xbox Wireless Controller (" + std::to_string(i + 1) + ")";
    const char* n = SDL_GameControllerName(A.pads[i]);
    return n && *n ? n : "Game controller";
}

PadState pad_state(int i) {
    PadState s;
    if (WZ.fake_n >= 0) return i == 0 ? WZ.fake : s;
    SDL_GameController* c = A.pads[i];
    for (int k = 0; k < SDL_CONTROLLER_BUTTON_MAX; ++k) s.b[k] = SDL_GameControllerGetButton(c, (SDL_GameControllerButton)k) != 0;
    s.lx = axis(c, SDL_CONTROLLER_AXIS_LEFTX);
    s.ly = axis(c, SDL_CONTROLLER_AXIS_LEFTY);
    s.rx = axis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    s.ry = axis(c, SDL_CONTROLLER_AXIS_RIGHTY);
    s.lt = std::max(0.f, axis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
    s.rt = std::max(0.f, axis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
    return s;
}

void parse_fake(const char* spec) {
    WZ.fake_n = std::atoi(spec);
    WZ.fake = PadState();
    const char* colon = std::strchr(spec, ':');
    if (!colon) return;
    std::string all = colon + 1, tok;
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t e = all.find(',', pos);
        tok = all.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? all.size() + 1 : e + 1;
        auto B = [&](SDL_GameControllerButton k) { WZ.fake.b[k] = true; };
        if (tok == "a") B(SDL_CONTROLLER_BUTTON_A);
        else if (tok == "b") B(SDL_CONTROLLER_BUTTON_B);
        else if (tok == "x") B(SDL_CONTROLLER_BUTTON_X);
        else if (tok == "y") B(SDL_CONTROLLER_BUTTON_Y);
        else if (tok == "lb") B(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        else if (tok == "rb") B(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        else if (tok == "back") B(SDL_CONTROLLER_BUTTON_BACK);
        else if (tok == "start") B(SDL_CONTROLLER_BUTTON_START);
        else if (tok == "guide") B(SDL_CONTROLLER_BUTTON_GUIDE);
        else if (tok == "ls") B(SDL_CONTROLLER_BUTTON_LEFTSTICK);
        else if (tok == "rs") B(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
        else if (tok == "up") B(SDL_CONTROLLER_BUTTON_DPAD_UP);
        else if (tok == "down") B(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        else if (tok == "left") B(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        else if (tok == "right") B(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        else if (tok == "lt") WZ.fake.lt = 1.f;
        else if (tok == "rt") WZ.fake.rt = 1.f;
        else if (tok == "lx+") WZ.fake.lx = 1.f;
        else if (tok == "lx-") WZ.fake.lx = -1.f;
        else if (tok == "ly+") WZ.fake.ly = 1.f;
        else if (tok == "ly-") WZ.fake.ly = -1.f;
        else if (tok == "rx+") WZ.fake.rx = 1.f;
        else if (tok == "rx-") WZ.fake.rx = -1.f;
        else if (tok == "ry+") WZ.fake.ry = 1.f;
        else if (tok == "ry-") WZ.fake.ry = -1.f;
    }
}

std::string pressed_text(const PadState& s) {
    static const struct { SDL_GameControllerButton k; const char* n; } names[] = {
        {SDL_CONTROLLER_BUTTON_A, "A"}, {SDL_CONTROLLER_BUTTON_B, "B"}, {SDL_CONTROLLER_BUTTON_X, "X"}, {SDL_CONTROLLER_BUTTON_Y, "Y"},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "LB"}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "RB"}, {SDL_CONTROLLER_BUTTON_BACK, "View"},
        {SDL_CONTROLLER_BUTTON_START, "Menu"}, {SDL_CONTROLLER_BUTTON_GUIDE, "Xbox"}, {SDL_CONTROLLER_BUTTON_LEFTSTICK, "Left stick click"},
        {SDL_CONTROLLER_BUTTON_RIGHTSTICK, "Right stick click"}, {SDL_CONTROLLER_BUTTON_DPAD_UP, "D-pad up"},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, "D-pad down"}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, "D-pad left"}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "D-pad right"}};
    std::string out;
    auto add = [&](const char* n) { out += (out.empty() ? "" : ", ") + std::string(n); };
    for (auto& e : names) if (s.b[e.k]) add(e.n);
    if (s.lt > 0.3f) add("LT");
    if (s.rt > 0.3f) add("RT");
    if (std::abs(s.lx) > 0.3f || std::abs(s.ly) > 0.3f) add("Left stick");
    if (std::abs(s.rx) > 0.3f || std::abs(s.ry) > 0.3f) add("Right stick");
    return out;
}

/* Vector pad diagram; every control lights when pressed. u = size of one grid unit. */
void draw_pad(ImDrawList* dl, ImVec2 c, float u, const PadState& s) {
    const ImU32 off = ui::PANEL3, on = ui::ACCENT, line = ui::HAIRLINE;
    auto P = [&](float x, float y) { return ImVec2(c.x + x * u, c.y + y * u); };
    auto B = [&](SDL_GameControllerButton k) { return s.b[k]; };
    // shoulders + triggers
    dl->AddRectFilled(P(-5.f, -3.6f), P(-2.2f, -2.9f), B(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? on : off, u * 0.35f);
    dl->AddRectFilled(P(2.2f, -3.6f), P(5.f, -2.9f), B(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? on : off, u * 0.35f);
    dl->AddRectFilled(P(-4.6f, -4.7f), P(-2.6f, -3.8f), ui::mix(off, on, s.lt), u * 0.35f);
    dl->AddRectFilled(P(2.6f, -4.7f), P(4.6f, -3.8f), ui::mix(off, on, s.rt), u * 0.35f);
    // body
    const ImU32 body = ui::PANEL2;
    dl->AddRectFilled(P(-5.f, -3.f), P(5.f, 2.2f), body, u * 2.2f);
    dl->AddCircleFilled(P(-4.1f, 2.2f), u * 2.3f, body, 40);
    dl->AddCircleFilled(P(4.1f, 2.2f), u * 2.3f, body, 40);
    dl->AddRect(P(-5.f, -3.f), P(5.f, 2.2f), line, u * 2.2f, 0, 1.5f);
    // sticks
    auto stick = [&](float x, float y, float dx, float dy, bool click) {
        dl->AddCircleFilled(P(x, y), u * 1.15f, ui::BG, 32);
        const bool moved = std::abs(dx) > 0.3f || std::abs(dy) > 0.3f;
        dl->AddCircleFilled(P(x + dx * 0.45f, y + dy * 0.45f), u * 0.8f, (moved || click) ? on : off, 32);
    };
    stick(-3.1f, -0.7f, s.lx, s.ly, B(SDL_CONTROLLER_BUTTON_LEFTSTICK));
    stick(1.6f, 1.5f, s.rx, s.ry, B(SDL_CONTROLLER_BUTTON_RIGHTSTICK));
    // d-pad
    const float dx = -1.6f, dy = 1.5f, a = 0.42f, l = 1.15f;
    dl->AddRectFilled(P(dx - a, dy - l), P(dx + a, dy), B(SDL_CONTROLLER_BUTTON_DPAD_UP) ? on : off, u * 0.12f);
    dl->AddRectFilled(P(dx - a, dy), P(dx + a, dy + l), B(SDL_CONTROLLER_BUTTON_DPAD_DOWN) ? on : off, u * 0.12f);
    dl->AddRectFilled(P(dx - l, dy - a), P(dx, dy + a), B(SDL_CONTROLLER_BUTTON_DPAD_LEFT) ? on : off, u * 0.12f);
    dl->AddRectFilled(P(dx, dy - a), P(dx + l, dy + a), B(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ? on : off, u * 0.12f);
    // face buttons with letters
    struct Face { float x, y; SDL_GameControllerButton k; const char* t; ImU32 col; };
    const Face faces[] = {{3.2f, 0.4f, SDL_CONTROLLER_BUTTON_A, "A", ui::GOOD}, {4.1f, -0.5f, SDL_CONTROLLER_BUTTON_B, "B", ui::BAD},
                          {2.3f, -0.5f, SDL_CONTROLLER_BUTTON_X, "X", ui::ACCENT}, {3.2f, -1.4f, SDL_CONTROLLER_BUTTON_Y, "Y", ui::WARN}};
    for (const Face& f : faces) {
        const bool p = B(f.k);
        dl->AddCircleFilled(P(f.x, f.y), u * 0.55f, p ? f.col : off, 24);
        ImFont* fn = ui::F.bold;
        float fs = u * 0.7f;
        ImVec2 ts = fn->CalcTextSizeA(fs, 1e5f, 0, f.t);
        dl->AddText(fn, fs, ImVec2(P(f.x, f.y).x - ts.x * 0.5f, P(f.x, f.y).y - ts.y * 0.5f), p ? IM_COL32(10, 10, 14, 255) : ui::TEXT2, f.t);
    }
    // centre buttons
    dl->AddCircleFilled(P(-0.9f, -0.8f), u * 0.28f, B(SDL_CONTROLLER_BUTTON_BACK) ? on : off, 16);
    dl->AddCircleFilled(P(0.9f, -0.8f), u * 0.28f, B(SDL_CONTROLLER_BUTTON_START) ? on : off, 16);
    dl->AddCircleFilled(P(0.f, -1.7f), u * 0.4f, B(SDL_CONTROLLER_BUTTON_GUIDE) ? ui::GOOD : off, 20);
}

void close_wizard(bool done) {
    WZ.open = false;
    WZ.test = false;
    if (done && !A.st.setup_done) { A.st.setup_done = true; mark_dirty(); }
    if (done && !A.st.find_games_done) { A.st.find_games_done = true; mark_dirty(); }
}

void goto_step(Step s) {
    WZ.step = s;
    WZ.test = false;
    WZ.focus_primary = true;
}

const char* kStepNames[hubui::wiz::kStepCount] = {"Welcome", "Controller", "Games", "Name", "Done"};

void steps_header(ImDrawList* dl, ImVec2 origin, float w) {
    const float r = ui::sp(0.5f), gap = ui::sp(2.2f);
    float total = gap * (hubui::wiz::kStepCount - 1);
    float x = origin.x + (w - total) * 0.5f;
    for (int i = 0; i < hubui::wiz::kStepCount; ++i) {
        const int cur = hubui::wiz::index(WZ.step);
        dl->AddCircleFilled(ImVec2(x + i * gap, origin.y), i == cur ? r * 1.4f : r, i <= cur ? ui::ACCENT : ui::PANEL3, 16);
    }
}

void controller_body(float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int n = pad_count();
    const float u = ui::sp(2.1f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float diag_w = u * 13.3f;
    if (n > 0 || WZ.test) {
        PadState st = n > 0 ? pad_state(0) : PadState();
        draw_pad(dl, ImVec2(p.x + diag_w * 0.5f + u * 0.4f, p.y + u * 5.0f), u, st);
        ImGui::Dummy(ImVec2(diag_w, u * 9.f));
        ImGui::SameLine(0, ui::sp(3.f));
        ImGui::BeginGroup();
        if (WZ.test) {
            ImGui::PushFont(ui::F.h2);
            ImGui::TextUnformatted("Press any button");
            ImGui::PopFont();
            text_wrapped_disabled("Each button, stick and trigger lights up on the picture as you press it.");
            std::string pt = pressed_text(st);
            ImGui::Dummy(ImVec2(0, ui::sp(1.f)));
            ImGui::PushFont(ui::F.bold);
            ImGui::TextColored(ImColor(pt.empty() ? ui::TEXT3 : ui::GOOD), "%s", pt.empty() ? "Nothing pressed yet" : pt.c_str());
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, ui::sp(2.f)));
            if (button("Done testing", ImVec2(ui::sp(20.f), ui::sp(5.f)), true, true, ui::Icon::Check)) { WZ.test = false; WZ.focus_primary = true; }
        } else {
            ui::icon(dl, ui::Icon::Check, ImVec2(ImGui::GetCursorScreenPos().x + ui::sp(1.5f), ImGui::GetCursorScreenPos().y + ui::sp(1.6f)), ui::sp(3.f), ui::GOOD);
            ImGui::Dummy(ImVec2(ui::sp(3.5f), 0));
            ImGui::SameLine();
            ImGui::PushFont(ui::F.h2);
            ImGui::TextColored(ImColor(ui::GOOD), "%s", hubui::wiz::pad_headline(n).c_str());
            ImGui::PopFont();
            ImGui::TextWrapped("%s", hubui::wiz::pad_hint(n, A.pad_mode).c_str());
            ImGui::Dummy(ImVec2(0, ui::sp(1.f)));
            for (int i = 0; i < n; ++i) {
                PadState ps = pad_state(i);
                const bool a = ps.b[SDL_CONTROLLER_BUTTON_A];
                ImVec2 q = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(ui::sp(2.f), ImGui::GetTextLineHeightWithSpacing()));
                dl->AddCircleFilled(ImVec2(q.x + ui::sp(0.8f), q.y + ImGui::GetTextLineHeight() * 0.5f), ui::sp(0.6f), a ? ui::GOOD : ui::PANEL3, 16);
                ImGui::SameLine();
                ImGui::PushFont(ui::F.bold);
                ImGui::Text("%s", hubui::wiz::player_label(i).c_str());
                ImGui::PopFont();
                ImGui::SameLine();
                ImGui::TextDisabled("%s", pad_name(i).c_str());
            }
            if (n >= 2) text_wrapped_disabled("Everyone press A on their controller to check it is the right player.");
            else text_wrapped_disabled("Playing with friends on this PC? Plug in more controllers and they show up here.");
            ImGui::Dummy(ImVec2(0, ui::sp(1.5f)));
            if (button("Test buttons", ImVec2(ui::sp(20.f), ui::sp(5.f)), false, true, ui::Icon::Gamepad)) { WZ.test = true; WZ.focus_primary = true; }
            ImGui::SameLine();
            if (button("Customize...", ImVec2(ui::sp(20.f), ui::sp(5.f)), false, true, ui::Icon::Gear)) {
                close_wizard(false);
                A.page = Page::Controllers;
                set_toast("Pick up setup later from Settings > Run setup again.", true);
            }
        }
        ImGui::EndGroup();
    } else {
        ImGui::BeginGroup();
        ImGui::PushFont(ui::F.h2);
        ImGui::TextUnformatted(hubui::wiz::pad_headline(0).c_str());
        ImGui::PopFont();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(w, ui::sp(60.f)));
        ImGui::TextWrapped("%s", hubui::wiz::pad_hint(0, false).c_str());
        ImGui::Dummy(ImVec2(0, ui::sp(1.f)));
        ImGui::TextDisabled("An Xbox Series X|S, Xbox One or any XInput controller works with no setup at all. "
                            "PlayStation and Switch pads work too; use Customize on the Controllers page any time.");
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        draw_pad(dl, ImVec2(p.x + w - diag_w * 0.6f, p.y + u * 5.2f), u * 0.9f, PadState());
    }
}

void wizard_window() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::BG);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##setup_wizard", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (WZ.focus_primary) ImGui::SetWindowFocus();

    const float card_w = std::min(ui::sp(100.f), vp->Size.x - ui::sp(6.f));
    const float card_h = std::min(ui::sp(62.f), vp->Size.y - ui::sp(4.f));
    ImVec2 c0((vp->Size.x - card_w) * 0.5f, (vp->Size.y - card_h) * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    ui::shadow(dl, wp + c0, wp + c0 + ImVec2(card_w, card_h), ui::sp(2.f));
    dl->AddRectFilled(wp + c0, wp + c0 + ImVec2(card_w, card_h), ui::PANEL, ui::sp(2.f));
    steps_header(dl, wp + c0 + ImVec2(0, ui::sp(2.2f)), card_w);

    const float pad = ui::sp(4.f);
    ImGui::SetCursorPos(c0 + ImVec2(pad, ui::sp(4.f)));
    ImGui::BeginChild("##wiz_body", ImVec2(card_w - pad * 2, card_h - ui::sp(4.f) - ui::sp(10.f)), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const float bw = card_w - pad * 2;
    bool primary_done = false;   // the footer primary is drawn after the body but must take initial focus first
    (void)primary_done;
    ImGui::PushFont(ui::F.title);
    switch (WZ.step) {
        case Step::Welcome: ImGui::TextUnformatted("Welcome to RetroHub"); break;
        case Step::Controller: ImGui::TextUnformatted(WZ.test ? "Test your controller" : "Your controller"); break;
        case Step::Games: ImGui::TextUnformatted("Find your games"); break;
        case Step::Name: ImGui::TextUnformatted("Pick your name"); break;
        case Step::Done: ImGui::TextUnformatted("You're all set"); break;
    }
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, ui::sp(1.5f)));

    switch (WZ.step) {
        case Step::Welcome: {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ui_draw_brand_logo(ImGui::GetWindowDrawList(), p, ui::sp(9.f));
            ImGui::Dummy(ImVec2(ui::sp(9.f), ui::sp(9.f)));
            ImGui::SameLine(0, ui::sp(3.f));
            ImGui::BeginGroup();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bw - ui::sp(14.f));
            ImGui::TextWrapped("One place for all your retro games, with online play and saves that follow you from PC to PC.");
            ImGui::Dummy(ImVec2(0, ui::sp(1.f)));
            ImGui::TextDisabled("This takes about a minute: check your controller, find your games, pick a name. "
                                "You can use a controller or the mouse and keyboard the whole way, and skip anything.");
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            break;
        }
        case Step::Controller: controller_body(bw); break;
        case Step::Games: {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bw);
            ImGui::TextWrapped("RetroHub can look through the usual places on this PC for game files. Only folder listings are read; "
                               "your files stay where they are and nothing is uploaded.");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, ui::sp(1.5f)));
            const size_t nf = A.st.rom_folders.size();
            if (nf) {
                ImGui::PushFont(ui::F.bold);
                ImGui::TextColored(ImColor(ui::GOOD), "%zu game %s added", nf, nf == 1 ? "folder" : "folders");
                ImGui::PopFont();
            }
            if (button(nf ? "Look for more games" : "Find my games", ImVec2(ui::sp(24.f), ui::sp(5.5f)), nf == 0, !A.scanning, ui::Icon::Search)) {
                find_games_open();
                WZ.find_launched = true;
            }
            ImGui::SameLine();
            add_rom_folder(button("Add a folder...", ImVec2(ui::sp(24.f), ui::sp(5.5f)), false, true, ui::Icon::Folder));
            break;
        }
        case Step::Name: {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bw);
            ImGui::TextWrapped("This is the name friends see in online rooms. You can change it any time in Settings.");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, ui::sp(1.f)));
            char nb[40];
            std::snprintf(nb, sizeof nb, "%s", A.st.name.c_str());
            ImGui::SetNextItemWidth(ui::sp(40.f));
            if (ImGui::InputText("##wiz_name", nb, 25)) { A.st.name = nb; mark_dirty(); }
            ImGui::Dummy(ImVec2(0, ui::sp(1.f)));
            if (ImGui::Checkbox("Automatically add people I play with as friends", &A.st.auto_friend)) mark_dirty();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bw);
            text_wrapped_disabled(A.pad_mode ? "Using a controller? The name above is taken from your Windows account; just press A to keep it."
                                             : "Tip: the name above is taken from your Windows account; keep it or type your own.");
            ImGui::PopTextWrapPos();
            break;
        }
        case Step::Done: {
            ImGui::PushFont(ui::F.bold);
            ImGui::TextColored(ImColor(ui::GOOD), "Everything is ready");
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, ui::sp(0.5f)));
            const int n = pad_count();
            ImGui::BulletText("Controller: %s", n > 0 ? (std::to_string(n) + (n == 1 ? " connected" : " connected")).c_str() : "none yet (plug one in any time)");
            ImGui::BulletText("Games: %zu folder%s", A.st.rom_folders.size(), A.st.rom_folders.size() == 1 ? "" : "s");
            ImGui::BulletText("Name: %s", A.st.name.empty() ? "(not set)" : A.st.name.c_str());
            ImGui::Dummy(ImVec2(0, ui::sp(1.5f)));
            text_wrapped_disabled("Run this again any time from Settings > Run setup again.");
            break;
        }
    }
    ImGui::EndChild();

    // footer: Back / Skip on the left, hints in the middle, primary on the right
    const float fy = c0.y + card_h - ui::sp(8.f);
    const ImVec2 bs(ui::sp(16.f), ui::sp(5.5f));
    const bool first = WZ.step == Step::Welcome, last = WZ.step == Step::Done;
    bool go_next = false, go_back = false, skip = false;
    ImGui::SetCursorPos(ImVec2(c0.x + pad, fy));
    if (!first && button("Back", bs, false, true, ui::Icon::ChevronL)) go_back = true;
    if (!first) ImGui::SameLine();
    if (!last && button("Skip setup", ImVec2(ui::sp(16.f), bs.y))) skip = true;
    const char* next_label = first ? "Get started" : last ? "Start playing" : WZ.step == Step::Controller && !WZ.test ? (pad_count() > 0 ? "Continue" : "Continue without") : "Next";
    ImGui::SetCursorPos(ImVec2(c0.x + card_w - pad - ui::sp(22.f), fy));
    if (WZ.focus_primary) { ImGui::SetKeyboardFocusHere(0); WZ.focus_primary = false; }
    if (button(next_label, ImVec2(ui::sp(22.f), bs.y), true, WZ.step != Step::Name || hubui::wiz::name_ok(A.st.name), last ? ui::Icon::Check : ui::Icon::ChevronR)) go_next = true;
    {   // hint strip
        float hx = c0.x + card_w * 0.5f - ui::sp(11.f);
        ImVec2 o = wp + ImVec2(hx, fy + bs.y * 0.5f - ImGui::GetTextLineHeight() * 0.5f);
        hx = ui::key_hint(dl, o, A.pad_mode, "A", "Enter", "Select");
        ui::key_hint(dl, o + ImVec2(hx + ui::sp(2.f), 0), A.pad_mode, "B", "Esc", "Back");
    }
    ImGui::SetCursorPos(ImVec2(0, 0));

    // B / Esc = back (or leave the button-test screen); only when no text box has the keyboard
    if (!ImGui::IsAnyItemActive() && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        if (WZ.test) { WZ.test = false; WZ.focus_primary = true; }
        else if (!first) go_back = true;
    }
    ImGui::End();

    if (skip) { close_wizard(true); return; }
    if (go_back) goto_step(hubui::wiz::back(WZ.step));
    else if (go_next) {
        if (last) close_wizard(true);
        else if (WZ.test) { WZ.test = false; WZ.focus_primary = true; }
        else goto_step(hubui::wiz::next(WZ.step));
    }
}

}  // namespace

void wizard_open() {
    WZ.open = true;
    WZ.test = false;
    WZ.step = Step::Welcome;
    WZ.focus_primary = true;
}

void draw_wizard() {
    if (!WZ.auto_tried) {
        const char* shot = std::getenv("HUB_SHOT_WIZARD");
        if (const char* fp = std::getenv("HUB_SHOT_FAKE_PAD")) parse_fake(fp);
        if (shot && A.scanned_once) {
            WZ.auto_tried = true;
            std::string s = shot;
            wizard_open();
            if (s == "controller") WZ.step = Step::Controller;
            else if (s == "test") { WZ.step = Step::Controller; WZ.test = true; }
            else if (s == "games") WZ.step = Step::Games;
            else if (s == "name") WZ.step = Step::Name;
            else if (s == "done") WZ.step = Step::Done;
        } else if (!shot && !g_headless && A.scanned_once) {
            WZ.auto_tried = true;
            if (hubui::wiz::should_autorun(A.st.setup_done, A.st.find_games_done, !A.st.rom_folders.empty())) wizard_open();
        }
    }
    if (!WZ.open) return;
    wizard_window();
}
