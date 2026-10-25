#pragma once
/* PlayStation pad state as it travels through netplay: 16 RetroPad buttons + the two analog sticks of a DualShock.
 * A DualShock reports each axis as one byte, so the sticks are quantised to 8 bits (axis >> 8): every PC (the sender
 * too) feeds the core the SAME quantised value, and nothing the hardware could tell apart is lost.
 *   lockstep: u32 buttons + u32 aux (LockstepCore wide mode)        rollback: one u64 = buttons | aux << 16 (rb::Config::val_bytes = 6)
 * aux byte order: lx, ly, rx, ry (the order of retrorun's frame_stick[port][]). Pure header, unit tested (test_rollback.cpp). */
#include <cstdint>

namespace retrorun {

inline uint32_t pack_sticks(const int16_t s[4]) {
    uint32_t a = 0;
    for (int k = 0; k < 4; k++) a |= (uint32_t)(uint8_t)(int8_t)(s[k] >> 8) << (8 * k);
    return a;
}

inline void unpack_sticks(uint32_t aux, int16_t out[4]) {
    for (int k = 0; k < 4; k++) out[k] = (int16_t)((int16_t)(int8_t)(uint8_t)(aux >> (8 * k)) * 256);
}

inline uint64_t pack_pad(uint32_t buttons, const int16_t s[4]) { return (uint64_t)(buttons & 0xFFFFu) | ((uint64_t)pack_sticks(s) << 16); }

inline void unpack_pad(uint64_t v, uint32_t* buttons, int16_t out[4]) {
    *buttons = (uint32_t)(v & 0xFFFFu);
    unpack_sticks((uint32_t)(v >> 16), out);
}

/* One-shot events ride in the 16 spare high bits of a rollback input value (rb::Config::val_bytes = 8, event_mask = kEventMask):
 * bit 48 = disc swap, bits 49..56 = the disc index to insert. The value is idempotent (insert disc N), so a repeated event cannot swap twice. */
constexpr uint64_t kEventMask = 0xFFFFull << 48;
inline uint64_t disc_event(unsigned target) { return (1ull << 48) | ((uint64_t)(target & 0xFFu) << 49); }
inline bool disc_event_of(uint64_t v, unsigned* target) {
    if (!(v >> 48 & 1)) return false;
    *target = (unsigned)(v >> 49 & 0xFFu);
    return true;
}

}  // namespace retrorun
