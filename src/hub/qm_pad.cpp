/* Direct controller reads for the quick menu and its pad combos (View+Y, View+LB). Independent of the OS focus and of ImGui:
 * SDL game controllers (background events are on, main.cpp) OR-ed with XInput slots 0-3. In the headless end-to-end test only the
 * test's virtual SDL joystick is read (never the user's real pads, never XInput). */
#include "app.h"
#include "qm_pad.h"

#include <SDL.h>
#include <Xinput.h>

namespace {
SDL_JoystickID g_only = -1;   // >= 0: read only this joystick (qm_e2e)
bool g_no_xinput = false;
uint32_t g_taps = 0;

bool wanted(SDL_GameController* c) {
    if (!c) return false;
    if (g_only < 0) return true;
    return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)) == g_only;
}
uint32_t from_xinput(WORD b, BYTE lt, BYTE rt, SHORT lx, SHORT ly) {
    uint32_t m = 0;
    auto set = [&](bool on, int bit) { if (on) m |= 1u << bit; };
    set(b & XINPUT_GAMEPAD_A, SDL_CONTROLLER_BUTTON_A);
    set(b & XINPUT_GAMEPAD_B, SDL_CONTROLLER_BUTTON_B);
    set(b & XINPUT_GAMEPAD_X, SDL_CONTROLLER_BUTTON_X);
    set(b & XINPUT_GAMEPAD_Y, SDL_CONTROLLER_BUTTON_Y);
    set(b & XINPUT_GAMEPAD_BACK, SDL_CONTROLLER_BUTTON_BACK);
    set(b & XINPUT_GAMEPAD_START, SDL_CONTROLLER_BUTTON_START);
    set(b & XINPUT_GAMEPAD_LEFT_THUMB, SDL_CONTROLLER_BUTTON_LEFTSTICK);
    set(b & XINPUT_GAMEPAD_RIGHT_THUMB, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    set(b & XINPUT_GAMEPAD_LEFT_SHOULDER, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    set(b & XINPUT_GAMEPAD_RIGHT_SHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    set(b & XINPUT_GAMEPAD_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_UP);
    set(b & XINPUT_GAMEPAD_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    set(b & XINPUT_GAMEPAD_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    set(b & XINPUT_GAMEPAD_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    set(lt > 160, qmpad::kLT);
    set(rt > 160, qmpad::kRT);
    set(ly > 16000, qmpad::kStickUp);     // XInput: +y is up
    set(ly < -16000, qmpad::kStickDown);
    set(lx < -16000, qmpad::kStickLeft);
    set(lx > 16000, qmpad::kStickRight);
    return m;
}
}  // namespace

void qm_pad_restrict(int only_instance, bool no_xinput) { g_only = only_instance; g_no_xinput = no_xinput; }

uint32_t qm_pad_raw() {
    uint32_t m = 0;
    for (auto* c : A.pads) {
        if (!wanted(c)) continue;
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX && b < 24; ++b)
            if (SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b)) m |= 1u << b;
        if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) m |= 1u << qmpad::kLT;
        if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) m |= 1u << qmpad::kRT;
        const int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX), ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        if (ly < -16000) m |= 1u << qmpad::kStickUp;      // SDL: -y is up
        if (ly > 16000) m |= 1u << qmpad::kStickDown;
        if (lx < -16000) m |= 1u << qmpad::kStickLeft;
        if (lx > 16000) m |= 1u << qmpad::kStickRight;
    }
    if (!g_no_xinput && !g_headless) {
        for (DWORD i = 0; i < 4; ++i) {
            XINPUT_STATE xs{};
            if (XInputGetState(i, &xs) != ERROR_SUCCESS) continue;
            m |= from_xinput(xs.Gamepad.wButtons, xs.Gamepad.bLeftTrigger, xs.Gamepad.bRightTrigger, xs.Gamepad.sThumbLX, xs.Gamepad.sThumbLY);
        }
    }
    return m;
}

void qm_pad_event(const SDL_Event& e) {
    if (e.type != SDL_CONTROLLERBUTTONDOWN || e.cbutton.button >= 24) return;
    if (g_only >= 0 && e.cbutton.which != g_only) return;
    g_taps |= 1u << e.cbutton.button;
}
uint32_t qm_pad_take_taps() { uint32_t t = g_taps; g_taps = 0; return t; }

uint32_t qm_nav_bits(uint32_t raw) {
    auto has = [&](int bit) { return (raw >> bit) & 1u; };
    uint32_t n = 0;
    if (has(SDL_CONTROLLER_BUTTON_DPAD_UP) || has(qmpad::kStickUp)) n |= hubqm::NB_UP;
    if (has(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || has(qmpad::kStickDown)) n |= hubqm::NB_DOWN;
    if (has(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || has(qmpad::kStickLeft)) n |= hubqm::NB_LEFT;
    if (has(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || has(qmpad::kStickRight)) n |= hubqm::NB_RIGHT;
    if (has(SDL_CONTROLLER_BUTTON_A)) n |= hubqm::NB_A;
    if (has(SDL_CONTROLLER_BUTTON_B)) n |= hubqm::NB_B;
    return n;
}

uint32_t qm_keyboard_nav_bits() {
    if (g_headless || !A.win || SDL_GetKeyboardFocus() != A.win) return 0;   // the keyboard only counts while RetroHub has it
    int n = 0;
    const Uint8* ks = SDL_GetKeyboardState(&n);
    auto k = [&](SDL_Scancode s) { return (int)s < n && ks[s]; };
    uint32_t m = 0;
    if (k(SDL_SCANCODE_UP)) m |= hubqm::NB_UP;
    if (k(SDL_SCANCODE_DOWN)) m |= hubqm::NB_DOWN;
    if (k(SDL_SCANCODE_LEFT)) m |= hubqm::NB_LEFT;
    if (k(SDL_SCANCODE_RIGHT)) m |= hubqm::NB_RIGHT;
    if (k(SDL_SCANCODE_RETURN) || k(SDL_SCANCODE_KP_ENTER) || k(SDL_SCANCODE_SPACE)) m |= hubqm::NB_A;
    if (k(SDL_SCANCODE_ESCAPE) || k(SDL_SCANCODE_BACKSPACE)) m |= hubqm::NB_B;
    return m;
}
