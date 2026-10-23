#include "app.h"

/* ============================ main ============================ */

/* ---- headless screenshot harness (testing aid, no window is ever shown) ----
 * HUB_SCREENSHOT=<file.bmp>  renders with SDL's "dummy" video driver + software renderer, writes the frame, exits.
 * HUB_SHOT_SECS (default 4) wall-clock seconds to run first; HUB_SHOT_SIZE=WxH; HUB_SCALE=DPI scale (1, 1.5, 2);
 * HUB_SHOT_KEYS=right,right,down,enter,... keyboard-nav presses sent at 0.3s intervals before the shot;
 * HUB_SHOT_PAD=1 shows controller glyphs in the footer. */
void save_screenshot(SDL_Renderer* ren, const char* path) {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(ren, &w, &h);
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
}
ImGuiKey shot_key(const std::string& n) {
    if (n == "right") return ImGuiKey_RightArrow;
    if (n == "left") return ImGuiKey_LeftArrow;
    if (n == "up") return ImGuiKey_UpArrow;
    if (n == "down") return ImGuiKey_DownArrow;
    if (n == "enter") return ImGuiKey_Enter;
    if (n == "esc") return ImGuiKey_Escape;
    if (n == "f2") return ImGuiKey_F2;
    if (n == "i") return ImGuiKey_I;
    if (n == "f") return ImGuiKey_F;
    return ImGuiKey_None;
}

/* Screenshot aids that seed UI state before the first frame (friends / room / emulator dialogs). Never any network. */
void shot_init_ui() {
    A.st.show_friends = std::getenv("HUB_SHOT_FRIENDS") != nullptr;
    if (const char* se = std::getenv("HUB_SHOT_EMU")) {   // screenshot aid: show a 3DS / Wii U dialog (azahar | cemu | unzip | failed)
        std::string m = se;
        if (m == "azahar" || m == "cemu") { EMU.kind = m == "cemu" ? hubemu::Kind::Cemu : hubemu::Kind::Azahar; EMU.state = hubemu::Installer::Asking; }
        else if (m == "failed") { EMU.kind = hubemu::Kind::Azahar; EMU.error = "checksum mismatch: the download was rejected"; EMU.state = hubemu::Installer::Failed; }
        else if (m == "unzip") { UNZ.key = A.env_select; UNZ.state = UnzipJob::Asking; }
    }
    if (std::getenv("HUB_SHOT_FAKE_FRIENDS")) {
        fr_fake_init();
        if (std::getenv("HUB_SHOT_CHAT")) { fr_open_chat("miauid"); FR.chat_unread = 2; }
        if (std::getenv("HUB_SHOT_BELL")) FR.shot_bell = true;
    }
    if (std::getenv("HUB_SHOT_FAKE_ROOM")) {
        g_fake_room = true;
        if (const char* hcv = std::getenv("HUB_SHOT_HOSTCHANGE")) {   // screenshot aid: room right after a host change ("me" | "other")
            g_fake_other_host = std::string(hcv) == "other";
            std::string nm = g_fake_other_host ? "Mia" : "You";
            push_toast(hubui::host_changed_text(nm, !g_fake_other_host), ui::GOOD, 60.0, /*top=*/true);
        }
        A.in_room = true;
        A.code = "K7M2QX";
        A.page = Page::Online;
        if (std::getenv("HUB_SHOT_PICKER")) FR.open_picker = true;
        if (const char* rg = std::getenv("HUB_SHOT_ROOM_GAME")) A.selected_game = rg;   // e.g. smash64, remix
        if (A.selected_game == "remix" && std::getenv("HUB_SHOT_PICKS")) {   // fake picks for the match picker
            A.my_pick.p[0] = {0x1D, 2};
            A.picks[1].p[0] = {0x00, 0};
            A.picks[2].p[0] = {0x20, 4};
            A.rules.stage = 0x31;
            A.rules.stocks = 4;
        }
        if (std::getenv("HUB_SHOT_LOCALS2")) A.my_locals = 2;
    }
}

ShotDriver SH;

void ShotDriver::init(const char* shot_path) {
    path = shot_path;
    update = shot_path && std::getenv("HUB_SHOT_UPDATE");   // screenshot the update prompt
    t0 = GetTickCount64();
    if (shot_path) {
        if (const char* ss = std::getenv("HUB_SHOT_SECS")) secs = std::atof(ss);
        if (const char* ks = std::getenv("HUB_SHOT_KEYS")) {
            std::stringstream kss(ks);
            std::string tok;
            while (std::getline(kss, tok, ',')) if (shot_key(tok) != ImGuiKey_None) keys.push_back(shot_key(tok));
        }
        if (const char* pd = std::getenv("HUB_SHOT_PAD")) A.pad_mode = std::atoi(pd) != 0;
    }
    wheel = shot_path && std::getenv("HUB_SHOT_WHEEL") &&
            std::sscanf(std::getenv("HUB_SHOT_WHEEL"), "%f,%f,%f", &wx, &wy, &wn) == 3;
}

/* Called every frame after the SDL/ImGui backends' NewFrame: sends the scripted keys / wheel. */
void ShotDriver::pre_frame(ImGuiIO& io) {
    el = (GetTickCount64() - t0) / 1000.0;
    if (path && !keys.empty()) {   // scripted keyboard navigation, starting once the library has loaded
        double start = std::max(1.5, secs - 0.35 * (keys.size() + 1));
        if (key_down) { io.AddKeyEvent(keys[key_i - 1], false); key_down = false; }
        else if (key_i < keys.size() && el > start && el - last_key > 0.3 && A.scanned_once) {
            io.AddKeyEvent(keys[key_i++], true);
            key_down = true;
            last_key = el;
        }
    }
    if (wheel && wheel_stage == 0 && A.scanned_once && el > std::max(2.0, secs - 2.0)) {
        io.AddMousePosEvent(wx, wy);   // window pixels
        wheel_stage = 1;
    } else if (wheel && wheel_stage == 1) {
        if (!route_wheel(wx, wy, 0.f, -wn)) io.AddMouseWheelEvent(0.f, -wn);
        wheel_stage = 2;
        wheel_t = el;
    }
}

bool ShotDriver::ready() const {
    return el >= secs && (A.scanned_once || el > secs + 20 || std::getenv("HUB_SHOT_NOWAIT")) && key_i >= keys.size() && !key_down &&
           (!wheel || (wheel_stage == 2 && el - wheel_t > 1.5));
}
