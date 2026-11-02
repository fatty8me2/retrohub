/* --match: scripted jump into a Smash 64 / Smash Remix VS battle.
 *
 *   --match "stage=<id>;stocks=<n>;p0=<fighter>:<costume>;p1=...;p2=...;p3=..."
 *
 * Numbers accept decimal or 0x hex. Absent players are not present. The driver is a pure state machine over
 * RDRAM (no emulator calls) so it is unit testable and runs identically on every peer: it is stepped once per
 * VI from the core frame callback, and the VI count is the same on all peers under lockstep. */
#pragma once

#include <cstdint>
#include <string>

namespace n64run {

struct MatchSpec {
    int stage = 0;
    int stocks = 3;
    int time = 100;               // minutes, 100 = infinite
    bool present[4] = {false, false, false, false};
    int fighter[4] = {0, 0, 0, 0};
    int costume[4] = {0, 0, 0, 0};
    bool valid() const { int n = 0; for (bool b : present) n += b; return n >= 2; }
};

// Parse; on failure returns false and sets *err.
bool parse_match_spec(const std::string& s, MatchSpec* out, std::string* err);

/* RDRAM view: `ram` is the host RDRAM block (word-swapped like mupen64plus keeps it). */
class MatchDriver {
public:
    MatchDriver(const MatchSpec& spec, const int owner[4]) : spec_(spec) { for (int i = 0; i < 4; i++) owner_[i] = owner[i]; }
    // Call once per VI (vi = 1,2,3...). Returns a log line to print, or empty.
    std::string step(uint8_t* ram, long long vi);
    bool done() const { return phase_ == kDone; }
    const MatchSpec& spec() const { return spec_; }

private:
    enum Phase { kWait, kDone };
    void write_vs(uint8_t* ram);
    MatchSpec spec_;
    int owner_[4];
    Phase phase_ = kWait;
    int last_scene_ = -1;
    long long scene_vi_ = 0;
};

// RDRAM accessors (guest virtual or physical address).
uint8_t ram_r8(const uint8_t* ram, uint32_t a);
uint32_t ram_r32(const uint8_t* ram, uint32_t a);
void ram_w8(uint8_t* ram, uint32_t a, uint8_t v);
void ram_w32(uint8_t* ram, uint32_t a, uint32_t v);

}  // namespace n64run
