/* Controller glyphs and battery. The pad that last had input decides whether hints read Xbox, PlayStation or Switch.
 * Screenshot aids: HUB_SHOT_FAKE_PADTYPE=xbox|ps|switch, HUB_SHOT_FAKE_BATTERY=empty|low|medium|full. */
#include "convenience.h"

namespace {

struct PadInfo {
    int active = 0;
    std::vector<uint32_t> sig;
    int battery = -1;
    bool warned_low = false;
    double next_poll = 0;
} PI;

uint32_t button_sig(SDL_GameController* c) {
    uint32_t s = 0;
    for (int k = 0; k < SDL_CONTROLLER_BUTTON_MAX && k < 32; ++k) if (SDL_GameControllerGetButton(c, (SDL_GameControllerButton)k)) s |= 1u << k;
    return s;
}

}  // namespace

const char* pad_kind_label() { return hubglyph::kind_name(ui::pad_kind); }

void padinfo_tick() {
    if (const char* f = std::getenv("HUB_SHOT_FAKE_PADTYPE")) {
        std::string s = f;
        ui::pad_kind = s.rfind("ps", 0) == 0 ? hubglyph::Kind::PlayStation : s.rfind("sw", 0) == 0 ? hubglyph::Kind::Switch : hubglyph::Kind::Xbox;
        if (const char* b = std::getenv("HUB_SHOT_FAKE_BATTERY")) {
            std::string v = b;
            PI.battery = v == "empty" ? 0 : v == "low" ? 1 : v == "medium" ? 2 : 3;
        }
        return;
    }
    if (A.pads.empty()) { PI.battery = -1; return; }
    if (PI.sig.size() != A.pads.size()) { PI.sig.assign(A.pads.size(), 0); PI.active = 0; }
    for (size_t i = 0; i < A.pads.size(); ++i) {
        uint32_t s = button_sig(A.pads[i]);
        if (s && s != PI.sig[i]) PI.active = (int)i;
        PI.sig[i] = s;
    }
    if (PI.active >= (int)A.pads.size()) PI.active = 0;
    SDL_GameController* c = A.pads[PI.active];
    ui::pad_kind = hubglyph::kind_from_sdl_type((int)SDL_GameControllerGetType(c));
    const double now = ImGui::GetTime();
    if (now >= PI.next_poll) {   // the power level changes slowly: ask every few seconds
        PI.next_poll = now + 5.0;
        SDL_Joystick* j = SDL_GameControllerGetJoystick(c);
        int lvl = j ? (int)SDL_JoystickCurrentPowerLevel(j) : -1;
        PI.battery = lvl;   // SDL_JOYSTICK_POWER_UNKNOWN is -1
    }
}

void battery_draw() {
    if (g_headless && !std::getenv("HUB_SHOT_FAKE_BATTERY")) return;
    if (!hubglyph::battery_shown(PI.battery)) { PI.warned_low = false; return; }
    if (!g_headless && (!A.focused || A.proc || game_active() || !hub_has_focus())) return;   // never over a game, never while the hub is in the background
    if (hubglyph::battery_low(PI.battery) && !PI.warned_low && (prompt_allowed() || g_headless)) {
        PI.warned_low = true;
        set_toast(std::string("Controller battery ") + (PI.battery == 0 ? "is empty" : "is low") + ".", false, 6.0);
    }
    if (!A.pad_mode && !hubglyph::battery_low(PI.battery)) return;   // quiet unless the pad is the one in use, or it is nearly flat
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float w = sp(5.f), h = sp(2.2f);
    ImVec2 p1(vp->Pos.x + vp->Size.x - sp(14.f), vp->Pos.y + sp(6.4f));   // left of the bell
    ImVec2 p0(p1.x - w, p1.y - h);
    const bool low = hubglyph::battery_low(PI.battery);
    const ImU32 col = low ? ui::BAD : ui::TEXT2;
    dl->AddRectFilled(ImVec2(p0.x - sp(0.5f), p0.y - sp(0.5f)), ImVec2(p1.x + sp(0.9f), p1.y + sp(0.5f)), ui::with_alpha(ui::PANEL, 0.85f), sp(0.6f));
    dl->AddRect(p0, p1, col, sp(0.3f), 0, 1.5f);
    dl->AddRectFilled(ImVec2(p1.x, p0.y + h * 0.3f), ImVec2(p1.x + sp(0.35f), p1.y - h * 0.3f), col);
    const int bars = hubglyph::battery_bars(PI.battery);
    const float bw = (w - sp(0.8f)) / 3.f;
    for (int i = 0; i < bars; ++i)
        dl->AddRectFilled(ImVec2(p0.x + sp(0.3f) + i * bw, p0.y + sp(0.3f)), ImVec2(p0.x + sp(0.3f) + (i + 1) * bw - sp(0.2f), p1.y - sp(0.3f)), col);
}
