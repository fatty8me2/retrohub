#include "local_input.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>

#include "buttons.h"

namespace n64run {

static void analog(int rx, int ry, int* ox, int* oy) {
    const float kDead = 0.08f;
    float x = std::max(-1.0f, std::min(1.0f, rx / 32767.0f));
    float y = std::max(-1.0f, std::min(1.0f, ry / 32767.0f));
    float mag = std::sqrt(x * x + y * y);
    if (mag <= kDead) { *ox = *oy = 0; return; }
    float scaled = std::min(1.0f, (mag - kDead) / (1.0f - kDead));
    float k = scaled / mag;
    *ox = (int)std::lround(x * k * 80.0f);
    *oy = (int)std::lround(-y * k * 80.0f);    // SDL: down is +; N64: up is +
    *ox = std::max(-80, std::min(80, *ox));
    *oy = std::max(-80, std::min(80, *oy));
}

int count_pads() {
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)) SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    int n = 0;
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) n++;
    return n;
}

LocalInput::~LocalInput() {
    for (void* p : pads_) SDL_GameControllerClose((SDL_GameController*)p);
    pads_.clear();
}

bool LocalInput::init() {
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) return false;
    }
    SDL_GameControllerEventState(SDL_IGNORE);   // we poll state; don't flood the shared event queue
    inited_ = true;
    rescan();
    return true;
}

void LocalInput::rescan() {
    for (void* p : pads_) SDL_GameControllerClose((SDL_GameController*)p);
    pads_.clear();
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_GameController* gc = SDL_GameControllerOpen(i);
        if (gc) pads_.push_back(gc);
    }
    pad_state_.assign(pads_.size(), 0);
    last_joysticks_ = SDL_NumJoysticks();
}

static uint32_t read_pad(SDL_GameController* gc) {
    uint32_t v = 0;
    auto B = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(gc, b) != 0; };
    auto A = [&](SDL_GameControllerAxis a) { return (int)SDL_GameControllerGetAxis(gc, a); };
    if (B(SDL_CONTROLLER_BUTTON_A)) v |= BTN_A;
    if (B(SDL_CONTROLLER_BUTTON_X) || B(SDL_CONTROLLER_BUTTON_B) || B(SDL_CONTROLLER_BUTTON_Y)) v |= BTN_B;
    if (B(SDL_CONTROLLER_BUTTON_START)) v |= BTN_START;
    if (A(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) v |= BTN_Z;
    if (B(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) v |= BTN_L;
    if (B(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) || A(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) v |= BTN_R;
    if (B(SDL_CONTROLLER_BUTTON_DPAD_UP)) v |= BTN_U_DPAD;
    if (B(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) v |= BTN_D_DPAD;
    if (B(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) v |= BTN_L_DPAD;
    if (B(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) v |= BTN_R_DPAD;
    int rx = A(SDL_CONTROLLER_AXIS_RIGHTX), ry = A(SDL_CONTROLLER_AXIS_RIGHTY);
    if (rx > 16384) v |= BTN_C_RIGHT;
    if (rx < -16384) v |= BTN_C_LEFT;
    if (ry > 16384) v |= BTN_C_DOWN;
    if (ry < -16384) v |= BTN_C_UP;
    int ox, oy;
    analog(A(SDL_CONTROLLER_AXIS_LEFTX), A(SDL_CONTROLLER_AXIS_LEFTY), &ox, &oy);
    return v | pack_stick(ox, oy);
}

void LocalInput::update() {
    if (!inited_) return;
    SDL_PumpEvents();
    if (SDL_NumJoysticks() != last_joysticks_) rescan();
    SDL_GameControllerUpdate();
    for (size_t i = 0; i < pads_.size(); i++) pad_state_[i] = read_pad((SDL_GameController*)pads_[i]);

    const Uint8* k = SDL_GetKeyboardState(nullptr);
    uint32_t v = 0;
    if (k) {
        if (k[SDL_SCANCODE_X]) v |= BTN_A;
        if (k[SDL_SCANCODE_C]) v |= BTN_B;
        if (k[SDL_SCANCODE_Z]) v |= BTN_Z;
        if (k[SDL_SCANCODE_RETURN]) v |= BTN_START;
        if (k[SDL_SCANCODE_A]) v |= BTN_L;
        if (k[SDL_SCANCODE_S]) v |= BTN_R;
        if (k[SDL_SCANCODE_I]) v |= BTN_C_UP;
        if (k[SDL_SCANCODE_J]) v |= BTN_C_LEFT;
        if (k[SDL_SCANCODE_K]) v |= BTN_C_DOWN;
        if (k[SDL_SCANCODE_L]) v |= BTN_C_RIGHT;
        if (k[SDL_SCANCODE_T]) v |= BTN_U_DPAD;
        if (k[SDL_SCANCODE_F]) v |= BTN_L_DPAD;
        if (k[SDL_SCANCODE_G]) v |= BTN_D_DPAD;
        if (k[SDL_SCANCODE_H]) v |= BTN_R_DPAD;
        int sx = (k[SDL_SCANCODE_RIGHT] ? 80 : 0) - (k[SDL_SCANCODE_LEFT] ? 80 : 0);
        int sy = (k[SDL_SCANCODE_UP] ? 80 : 0) - (k[SDL_SCANCODE_DOWN] ? 80 : 0);
        if (sx && sy) { sx = sx > 0 ? 57 : -57; sy = sy > 0 ? 57 : -57; }   // keep within the gate
        v |= pack_stick(sx, sy);
    }
    kbd_state_ = v;
}

uint32_t LocalInput::read(int owned_index) const {
    uint32_t pad = owned_index >= 0 && owned_index < (int)pad_state_.size() ? pad_state_[owned_index] : 0;
    if (owned_index != 0) return pad;
    // Merge keyboard into the first port: buttons OR'd, analog from whichever is non-zero (pad wins).
    uint32_t btn = (pad | kbd_state_) & 0xFFFFu;
    uint32_t stick = (pad & 0xFFFF0000u) ? (pad & 0xFFFF0000u) : (kbd_state_ & 0xFFFF0000u);
    return btn | stick;
}

}  // namespace n64run
