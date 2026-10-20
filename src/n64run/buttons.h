#pragma once
/* N64 controller word as used by mupen64plus' BUTTONS union (m64p_plugin.h):
 *   bit 0 R_DPAD, 1 L_DPAD, 2 D_DPAD, 3 U_DPAD, 4 START, 5 Z, 6 B, 7 A,
 *   8 R_CBUTTON, 9 L_CBUTTON, 10 D_CBUTTON, 11 U_CBUTTON, 12 R_TRIG, 13 L_TRIG,
 *   14-15 reserved, bits 16..23 X_AXIS (signed 8), bits 24..31 Y_AXIS (signed 8, + = up).
 * Verified against the 2.6.0 header by n64run_lockstep_test. */
#include <cstdint>

namespace n64run {
enum : uint32_t {
    BTN_R_DPAD = 1u << 0, BTN_L_DPAD = 1u << 1, BTN_D_DPAD = 1u << 2, BTN_U_DPAD = 1u << 3,
    BTN_START = 1u << 4, BTN_Z = 1u << 5, BTN_B = 1u << 6, BTN_A = 1u << 7,
    BTN_C_RIGHT = 1u << 8, BTN_C_LEFT = 1u << 9, BTN_C_DOWN = 1u << 10, BTN_C_UP = 1u << 11,
    BTN_R = 1u << 12, BTN_L = 1u << 13,
};
inline uint32_t pack_stick(int x, int y) {
    return ((uint32_t)(uint8_t)(int8_t)x << 16) | ((uint32_t)(uint8_t)(int8_t)y << 24);
}
}  // namespace n64run
