#pragma once
/* Profile -> RetroPad bitmask for one pad (pure; unit tested against the old hardcoded mapping). */
#include <cstdint>

#include "controls.h"
#include "third_party/libretro.h"

namespace retrorun {

class RetroPad {
  public:
    explicit RetroPad(const hubctl::Profile& p) : m_(p), k_(p) {
        for (int i = 0; i < 16; i++) { id_[i] = m_.id(kCtl[i].name); kid_[i] = k_.id(kCtl[i].name); }
        ls_ = m_.id("left_stick");
        rs_ = m_.id("right_stick");
    }
    uint32_t mask(const hubctl::PadState& st) const {
        uint32_t r = 0;
        for (int i = 0; i < 16; i++) if (m_.down(id_[i], st)) r |= 1u << kCtl[i].retro;
        return r;
    }
    /* Analog stick ("left_stick" / "right_stick" controls of the ps1 profile) as libretro axes -32767..32767, screen orientation
     * (down = +y). Profiles without the control (every non-PlayStation system) give 0. */
    void sticks(const hubctl::PadState& st, int16_t out[4]) const {
        const int ids[2] = {ls_, rs_};
        for (int s = 0; s < 2; s++) {
            float x = 0, y = 0;
            if (ids[s] >= 0) m_.stick(ids[s], st, &x, &y);
            auto cv = [](float v) { return (int16_t)(v > 1.f ? 32767 : v < -1.f ? -32767 : (int)(v * 32767.f)); };
            out[s * 2] = cv(x);
            out[s * 2 + 1] = cv(y);
        }
    }
    /* Keyboard pad: state = SDL_GetKeyboardState. */
    uint32_t key_mask(const uint8_t* state) const {
        uint32_t r = 0;
        for (int i = 0; i < 16; i++) if (k_.down(kid_[i], state)) r |= 1u << kCtl[i].retro;
        return r;
    }
  private:
    struct C { const char* name; unsigned retro; };
    static constexpr C kCtl[16] = {
        {"b", RETRO_DEVICE_ID_JOYPAD_B}, {"a", RETRO_DEVICE_ID_JOYPAD_A}, {"y", RETRO_DEVICE_ID_JOYPAD_Y}, {"x", RETRO_DEVICE_ID_JOYPAD_X},
        {"l", RETRO_DEVICE_ID_JOYPAD_L}, {"r", RETRO_DEVICE_ID_JOYPAD_R}, {"l2", RETRO_DEVICE_ID_JOYPAD_L2}, {"r2", RETRO_DEVICE_ID_JOYPAD_R2},
        {"l3", RETRO_DEVICE_ID_JOYPAD_L3}, {"r3", RETRO_DEVICE_ID_JOYPAD_R3}, {"start", RETRO_DEVICE_ID_JOYPAD_START},
        {"select", RETRO_DEVICE_ID_JOYPAD_SELECT}, {"up", RETRO_DEVICE_ID_JOYPAD_UP}, {"down", RETRO_DEVICE_ID_JOYPAD_DOWN},
        {"left", RETRO_DEVICE_ID_JOYPAD_LEFT}, {"right", RETRO_DEVICE_ID_JOYPAD_RIGHT}};
    hubctl::Mapper m_;
    hubctl::KeyMapper k_;
    int id_[16];
    int kid_[16];
    int ls_ = -1, rs_ = -1;
};

}  // namespace retrorun
