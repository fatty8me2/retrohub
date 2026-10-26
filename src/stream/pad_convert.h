#pragma once
/* Guest pad (PadState, Nintendo layout) -> DSU pad (XInput layout) for Cemu's DSUController. Cemu's default RetroHub profile maps the
 * Wii U A button to the pad's EAST button (DSU "B" bit), B to south ("A" bit), X to north ("Y"), Y to west ("X"). */
#include "dsu_server.h"
#include "stream_proto.h"

namespace rplay {

inline dsu::Pad to_dsu(const PadState& p) {
    dsu::Pad d;
    uint16_t b = 0;
    auto on = [&](uint16_t m) { return (p.buttons & m) != 0; };
    if (on(kPadA)) b |= dsu::kB;
    if (on(kPadB)) b |= dsu::kA;
    if (on(kPadX)) b |= dsu::kY;
    if (on(kPadY)) b |= dsu::kX;
    if (on(kPadL)) b |= dsu::kLB;
    if (on(kPadR)) b |= dsu::kRB;
    if (on(kPadPlus)) b |= dsu::kStart;
    if (on(kPadMinus)) b |= dsu::kBack;
    if (on(kPadUp)) b |= dsu::kUp;
    if (on(kPadDown)) b |= dsu::kDown;
    if (on(kPadLeft)) b |= dsu::kLeft;
    if (on(kPadRight)) b |= dsu::kRight;
    if (on(kPadStickL)) b |= dsu::kL3;
    if (p.buttons2 & kPad2StickR) b |= dsu::kR3;
    d.buttons = b;
    d.lx = p.lx; d.ly = p.ly; d.rx = p.rx; d.ry = p.ry;
    d.lt = (p.lt || !on(kPadZL)) ? p.lt : 255;   // digital ZL/ZR without an analog value still counts as pressed
    d.rt = (p.rt || !on(kPadZR)) ? p.rt : 255;
    const float k = 57.2957795f;
    for (int i = 0; i < 3; i++) { d.gyro[i] = p.gyro[i] * k; d.accel[i] = p.accel[i] / 9.80665f; }
    return d;
}

/* Guest slot K (1..3) is DSU slot K-1 and Cemu player K+1. */
inline int dsu_slot_for_guest(int guest_slot) { return guest_slot >= 1 && guest_slot <= 3 ? guest_slot - 1 : -1; }

}  // namespace rplay
