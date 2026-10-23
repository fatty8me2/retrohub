#pragma once
/* Profile -> N64 BUTTONS word for one pad (pure; unit tested against the old hardcoded mapping). */
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "buttons.h"
#include "controls.h"

namespace n64run {

class PadMap {
  public:
    explicit PadMap(const hubctl::Profile& p) : m_(p) {
        for (int i = 0; i < 14; i++) id_[i] = m_.id(kCtl[i].name);
        stick_ = m_.id("stick");
    }
    uint32_t word(const hubctl::PadState& st) const {
        uint32_t v = 0;
        for (int i = 0; i < 14; i++) if (m_.down(id_[i], st)) v |= kCtl[i].bit;
        float fx, fy;
        m_.stick(stick_, st, &fx, &fy);
        int ox = (int)std::lround(fx * 80.0f), oy = (int)std::lround(-fy * 80.0f);   // SDL: down is +; N64: up is +
        ox = std::max(-80, std::min(80, ox));
        oy = std::max(-80, std::min(80, oy));
        return v | pack_stick(ox, oy);
    }
  private:
    struct C { const char* name; uint32_t bit; };
    static constexpr C kCtl[14] = {
        {"a", BTN_A}, {"b", BTN_B}, {"z", BTN_Z}, {"start", BTN_START}, {"l", BTN_L}, {"r", BTN_R},
        {"c_up", BTN_C_UP}, {"c_down", BTN_C_DOWN}, {"c_left", BTN_C_LEFT}, {"c_right", BTN_C_RIGHT},
        {"dpad_up", BTN_U_DPAD}, {"dpad_down", BTN_D_DPAD}, {"dpad_left", BTN_L_DPAD}, {"dpad_right", BTN_R_DPAD}};
    hubctl::Mapper m_;
    int id_[14];
    int stick_;
};

}  // namespace n64run
