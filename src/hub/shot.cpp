#include <sstream>
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
/* Headless runs normally never touch the network. HUB_SHOT_NET=1 allows the friends client / account calls / username claims, but ONLY
 * against a loopback server (the QA harness starts a local wrangler dev; production is unreachable from here by construction). */
bool shot_net() {
    if (!g_headless || !std::getenv("HUB_SHOT_NET")) return false;
    const std::string s = server_url();
    return s.rfind("ws://127.0.0.1", 0) == 0 || s.rfind("ws://localhost", 0) == 0;
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
    if (const char* pg = std::getenv("HUB_SHOT_PAGE")) {   // screenshot aid: open a page directly ("saves")
        if (std::string(pg) == "saves") A.page = Page::Saves;
        else if (std::string(pg) == "settings") A.page = Page::Settings;
    }
    if (std::getenv("HUB_SHOT_CTL_SYS") || std::getenv("HUB_SHOT_FAKE_PADS")) A.page = Page::Controllers;   // Controllers page aids: HUB_SHOT_CTL_SYS/_GAME/_DEMO/_CAPTURE, HUB_SHOT_FAKE_PADS=n
    if (const char* se = std::getenv("HUB_SHOT_EMU")) {   // screenshot aid: show a 3DS / Wii U dialog (azahar | cemu | unzip | failed)
        std::string m = se;
        if (m == "azahar" || m == "cemu") { EMU.kind = m == "cemu" ? hubemu::Kind::Cemu : hubemu::Kind::Azahar; EMU.state = hubemu::Installer::Asking; }
        else if (m == "failed") { EMU.kind = hubemu::Kind::Azahar; EMU.error = "checksum mismatch: the download was rejected"; EMU.state = hubemu::Installer::Failed; }
        else if (m == "unzip") { UNZ.key = A.env_select; UNZ.state = UnzipJob::Asking; }
        else if (m == "ps2bios") ps2_bios_ask("");
        else if (m == "ps1bios") ps1_bios_ask_shot();
    }
    if (std::getenv("HUB_SHOT_FAKE_FRIENDS")) {
        fr_fake_init();
        if (std::getenv("HUB_SHOT_CHAT")) { fr_open_chat("miauid"); FR.chat_unread = 2; }
        if (std::getenv("HUB_SHOT_BELL")) FR.shot_bell = true;
    }
    cloud_ui_shot_init();
    kt_shot_init();
    osk::shot_init();
    if (const char* rj = std::getenv("HUB_SHOT_REJOIN")) {   // screenshot aid: the disconnected Rejoin strip (no network)
        g_shot_rejoin = true;
        g_shot_rejoin_why = *rj ? rj : "Connection to the lobby server was lost";
        A.in_room = true;
        A.code = "K7M2QX";
        A.page = Page::Online;
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
        if (const char* rd = std::getenv("HUB_SHOT_READY")) {   // screenshot aid: slots that are ready, e.g. "1,2" ("me" = us)
            std::stringstream rs(rd);
            std::string tok;
            while (std::getline(rs, tok, ',')) {
                if (tok == "me") A.my_ready = true;
                else if (!tok.empty()) A.ready[std::atoi(tok.c_str())] = true;
            }
        }
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
        if (const char* pd = std::getenv("HUB_SHOT_PAD")) {
            std::string v = pd;
            if (v == "0" || v == "1") A.pad_mode = v == "1";
            else {
                std::stringstream pss(v);
                std::string tok;
                while (std::getline(pss, tok, ',')) if (!tok.empty()) pad_seq.push_back(tok);
                A.pad_mode = true;
            }
        }
    }
    if (const char* cl = std::getenv("HUB_SHOT_CLICK"); cl && shot_path) {
        std::string all = cl, tok;
        std::stringstream css(all);
        while (std::getline(css, tok, ';')) {
            int b = 0;
            if (!tok.empty() && (tok[0] == 'r' || tok[0] == 'R')) { b = 1; tok.erase(0, 1); }
            float x, y;
            if (std::sscanf(tok.c_str(), "%f,%f", &x, &y) == 2) clicks.push_back({x, y, b});
        }
    }
    wheel = shot_path && std::getenv("HUB_SHOT_WHEEL") &&
            std::sscanf(std::getenv("HUB_SHOT_WHEEL"), "%f,%f,%f", &wx, &wy, &wn) == 3;
}

static ImGuiKey pad_token_key(const std::string& n) {
    if (n == "up") return ImGuiKey_GamepadDpadUp;
    if (n == "down") return ImGuiKey_GamepadDpadDown;
    if (n == "left") return ImGuiKey_GamepadDpadLeft;
    if (n == "right") return ImGuiKey_GamepadDpadRight;
    if (n == "a") return ImGuiKey_GamepadFaceDown;
    if (n == "b") return ImGuiKey_GamepadFaceRight;
    if (n == "x") return ImGuiKey_GamepadFaceLeft;
    if (n == "y") return ImGuiKey_GamepadFaceUp;
    if (n == "lb") return ImGuiKey_GamepadL1;
    if (n == "rb") return ImGuiKey_GamepadR1;
    if (n == "lt") return ImGuiKey_GamepadL2;
    if (n == "rt") return ImGuiKey_GamepadR2;
    if (n == "start") return ImGuiKey_GamepadStart;
    if (n == "back" || n == "view") return ImGuiKey_GamepadBack;
    if (n == "pgdn") return ImGuiKey_PageDown;   // raw keys, to test ImGui's own paging without the LT/RT conversion
    if (n == "pgup") return ImGuiKey_PageUp;
    return ImGuiKey_None;
}

/* Generic trace line for UI actions that have no other observable effect headless (HUB_SHOT_TRACE). */
void shot_trace(const char* what, const std::string& arg) {
    static const char* tp = std::getenv("HUB_SHOT_TRACE");
    if (!tp) return;
    if (FILE* f = std::fopen(tp, "a")) { std::fprintf(f, "%s %s\n", what, arg.c_str()); std::fclose(f); }
}

void shot_trace_once(const char* what, const std::string& arg) {
    static std::set<std::string> seen;
    if (!std::getenv("HUB_SHOT_TRACE")) return;
    if (!seen.insert(std::string(what) + " " + arg).second) return;
    shot_trace(what, arg);
}

/* One trace line per injected pad press: where the page / focus ended up (HUB_SHOT_TRACE). */
void ShotDriver::pad_trace(const std::string& tok) {
    static const char* tp = std::getenv("HUB_SHOT_TRACE");
    if (!tp) return;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    static const char* pn[] = {"library", "online", "saves", "controllers", "settings", "about"};
    int pg = (int)A.page;
    bool modal = ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    char sy[32] = "-";   // scroll of the nearest scrollable window around the focus: "<scroll>/<max>"
    for (ImGuiWindow* w = g.NavWindow; w; w = w->ParentWindow)
        if (w->ScrollMax.y > 0.f) { std::snprintf(sy, sizeof sy, "%d/%d", (int)w->Scroll.y, (int)w->ScrollMax.y); break; }
    ImVec2 r = g.NavWindow ? g.NavWindow->NavRectRel[g.NavLayer].GetCenter() + g.NavWindow->Pos : ImVec2(0, 0);
    if (FILE* f = std::fopen(tp, "a")) {
        std::fprintf(f, "pad %s page=%s popup=%d friends=%d nav=%08x at=%d,%d osk=%d wiz=%d name=[%s] oskt=[%s] sy=%s win=%s\n", tok.c_str(), pg >= 0 && pg < 6 ? pn[pg] : "?", modal ? 1 : 0,
                     A.st.show_friends ? 1 : 0, (unsigned)g.NavId, g.NavId ? (int)r.x : -1, g.NavId ? (int)r.y : -1,
                     osk::is_open() ? 1 : 0, wizard_state(), A.st.name.c_str(), osk::shot_text().c_str(),
                     sy, g.NavWindow ? g.NavWindow->Name : "-");
        std::fclose(f);
    }
}

/* Called every frame after the SDL/ImGui backends' NewFrame: sends the scripted keys / wheel. */
void ShotDriver::pre_frame(ImGuiIO& io) {
    el = (GetTickCount64() - t0) / 1000.0;
    if (path && !pad_seq.empty()) {
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;   // headless: no device, so the SDL backend would clear this every frame
        if (pad_key != ImGuiKey_None) {
            io.AddKeyAnalogEvent(pad_key, false, 0.f);
            pad_key = ImGuiKey_None;
        } else if (pad_i <= pad_seq.size() && el > 1.5 && A.scanned_once && el - pad_t > 0.4) {
            if (!pad_last.empty()) { pad_trace(pad_last); pad_last.clear(); }   // the previous press has settled by now
            if (pad_i < pad_seq.size()) {
                const std::string& tok = pad_seq[pad_i++];
                pad_t = el;
                pad_last = tok;
                A.pad_mode = true;
                if (tok.rfind("shot:", 0) == 0) pending_shot = tok.substr(5);
                else if (osk::inject(tok.c_str())) {}   // the on-screen keyboard owns the pad while it is open (it reads the SDL pad itself)
                else if (ImGuiKey k = pad_token_key(tok); k != ImGuiKey_None) {
                    io.AddKeyAnalogEvent(k, true, 1.f);
                    pad_key = k;
                }
            } else ++pad_i;
        }
    }
    if (path && !keys.empty()) {   // scripted keyboard navigation, starting once the library has loaded
        double start = std::max(1.5, secs - 0.35 * (keys.size() + 1));
        if (key_down) { io.AddKeyEvent(keys[key_i - 1], false); key_down = false; }
        else if (key_i < keys.size() && el > start && el - last_key > 0.3 && A.scanned_once) {
            io.AddKeyEvent(keys[key_i++], true);
            key_down = true;
            last_key = el;
        }
    }
    /* Reachability QA (qa/reach_matrix.ps1): HUB_SHOT_HOSTLAUNCH=1 presses the room's Launch (host); HUB_SHOT_GUEST_MSG=<file> feeds a
     * guest the host's launch message (written by the host run as "host-msg" in HUB_SHOT_TRACE). Fake room only, never any network. */
    if (path && g_fake_room && A.scanned_once && el > 3.0 && !room_hook_done) {
        if (std::getenv("HUB_SHOT_HOSTLAUNCH") && is_host()) { room_hook_done = true; host_launch_selected(); }
        else if (const char* gm = std::getenv("HUB_SHOT_GUEST_MSG"); gm && !is_host()) {
            room_hook_done = true;
            std::ifstream f(gm, std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            while (!body.empty() && (body.back() == 0x0A || body.back() == 0x0D)) body.pop_back();
            guest_apply_launch(body);
        }
    }
    if (click_i < clicks.size() && A.scanned_once && el > std::max(2.0, secs - 1.5) && el - click_t > 0.35) {
        const Click& c = clicks[click_i];
        if (click_stage == 0) io.AddMousePosEvent(c.x, c.y);
        else if (click_stage == 1) io.AddMouseButtonEvent(c.btn, true);
        else { io.AddMouseButtonEvent(c.btn, false); ++click_i; }
        click_stage = (click_stage + 1) % 3;
        click_t = el;
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
    return el >= secs && (pad_seq.empty() || pad_i > pad_seq.size()) && pad_key == ImGuiKey_None && (A.scanned_once || el > secs + 20 || std::getenv("HUB_SHOT_NOWAIT")) && key_i >= keys.size() && !key_down && click_i >= clicks.size() && (clicks.empty() || el - click_t > 0.8) &&
           (!wheel || (wheel_stage == 2 && el - wheel_t > 1.5)) && (!userflow_active() || userflow_done());
}
