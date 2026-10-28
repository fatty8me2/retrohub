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
    if (!keys_) keys_.reset(new hubctl::KeyMapper(profile_));
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
    keys_.reset(new hubctl::KeyMapper(profile_));
}

void LocalInput::rumble(int owned_index, bool on) {
    if (owned_index < 0 || owned_index >= (int)pads_.size()) return;
    SDL_GameController* gc = (SDL_GameController*)pads_[owned_index];
    if (on) SDL_GameControllerRumble(gc, 0xB000, 0xB000, 60000); else SDL_GameControllerRumble(gc, 0, 0, 0);
}

void LocalInput::update() {
    if (!inited_) return;
    SDL_PumpEvents();
    if (SDL_NumJoysticks() != last_joysticks_) rescan();
    SDL_GameControllerUpdate();
    bool combo = false;
    boost_ = false;
    view_y_ = false;
    for (size_t i = 0; i < pads_.size(); i++) {
        hubctl::PadState st;
        hubctl::read_state((SDL_GameController*)pads_[i], &st);
        if (st.b[hubctl::kBtnBack] && st.b[hubctl::kBtnYButton]) view_y_ = true;
        pad_state_[i] = map_->word(st);
        if (hubctl::exit_combo_down(st)) combo = true;
        if (hubctl::boost_combo_down(st)) boost_ = true;
    }
    if (combo_.update(combo, (long long)SDL_GetTicks64())) exit_fired_ = true;

    const Uint8* k = SDL_GetKeyboardState(nullptr);
    uint32_t v = 0;
    if (k && keys_) {
        static const struct { const char* n; uint32_t bit; } kb[] = {
            {"a", BTN_A}, {"b", BTN_B}, {"z", BTN_Z}, {"start", BTN_START}, {"l", BTN_L}, {"r", BTN_R},
            {"c_up", BTN_C_UP}, {"c_left", BTN_C_LEFT}, {"c_down", BTN_C_DOWN}, {"c_right", BTN_C_RIGHT},
            {"dpad_up", BTN_U_DPAD}, {"dpad_left", BTN_L_DPAD}, {"dpad_down", BTN_D_DPAD}, {"dpad_right", BTN_R_DPAD}};
        for (auto& e : kb) if (keys_->down(keys_->id(e.n), k)) v |= e.bit;
        auto dn = [&](const char* n) { return keys_->down(keys_->id(n), k); };
        int sx = (dn("stick_right") ? 80 : 0) - (dn("stick_left") ? 80 : 0);
        int sy = (dn("stick_up") ? 80 : 0) - (dn("stick_down") ? 80 : 0);
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
