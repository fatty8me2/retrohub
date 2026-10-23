#include "local_input.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>

#include "buttons.h"
#include "controls_sdl.h"

namespace n64run {

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
    if (!map_) map_.reset(new PadMap(profile_));
    SDL_GameControllerEventState(SDL_IGNORE);   // we poll state; don't flood the shared event queue
    inited_ = true;
    rescan();
    return true;
}

void LocalInput::rescan() {
    for (void* p : pads_) SDL_GameControllerClose((SDL_GameController*)p);
    pads_.clear();
    for (auto* gc : hubctl::open_pads(profile_.port_order)) pads_.push_back(gc);
    pad_state_.assign(pads_.size(), 0);
    last_joysticks_ = SDL_NumJoysticks();
}

void LocalInput::set_profile(const hubctl::Profile& p) {
    profile_ = p;
    map_.reset(new PadMap(profile_));
}

static uint32_t read_pad(SDL_GameController* gc, const PadMap& m) {
    hubctl::PadState st;
    hubctl::read_state(gc, &st);
    return m.word(st);
}

void LocalInput::update() {
    if (!inited_) return;
    SDL_PumpEvents();
    if (SDL_NumJoysticks() != last_joysticks_) rescan();
    SDL_GameControllerUpdate();
    for (size_t i = 0; i < pads_.size(); i++) pad_state_[i] = read_pad((SDL_GameController*)pads_[i], *map_);

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
