#pragma once
/* Rewind (retrorun, offline only): a ring of compressed emulator states, newest first.
 *
 * Storage: the newest state is kept whole; every older state is a sparse XOR delta against its newer neighbour
 * (zero-run + literal-run encoding over 64-bit words). Stepping back applies the newest delta, so there are no keyframes and
 * dropping the oldest entry costs nothing. Pure logic: no SDL, no core, unit tested (test_rewind.cpp). */
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace rwd {

/* Seconds of history by system ("gba","gb","gbc","nes","snes" = 20; "ps1","nds" and anything else = 0 = off by default). */
int auto_seconds(const std::string& system);
/* Seconds used when the user forces rewind on for a system that is off by default (small: those states are huge). */
int forced_seconds(const std::string& system);
constexpr size_t kMaxBytes = 192u << 20;   // hard cap on compressed history + the one whole state

/* "auto" / "on" / "off" -> 0 / 1 / 2 (false for anything else). */
bool parse_mode(const std::string& s, int* mode);

class Ring {
  public:
    Ring(size_t state_bytes, size_t max_frames, size_t max_bytes = kMaxBytes);
    /* Append the newest state (state_bytes long). Evicts the oldest while over max_frames / max_bytes. */
    void push(const uint8_t* state);
    /* Drop the newest state and return the one before it. False (out untouched) when only one state is left. */
    bool step_back(std::vector<uint8_t>* out);
    /* The newest state still held (empty ring: false). */
    bool newest(std::vector<uint8_t>* out) const;
    size_t frames() const { return has_cur_ ? entries_.size() + 1 : 0; }
    size_t bytes() const;                    // compressed deltas + the whole newest state
    size_t state_bytes() const { return state_bytes_; }
    size_t max_frames() const { return max_frames_; }
    size_t max_bytes() const { return max_bytes_; }
    void clear();

  private:
    void evict();
    size_t state_bytes_, words_, max_frames_, max_bytes_;
    std::vector<uint64_t> cur_;
    bool has_cur_ = false;
    std::deque<std::vector<uint8_t>> entries_;   // entries_[i]: XOR delta between state i and state i+1 (oldest first)
    size_t delta_bytes_ = 0;
};

}  // namespace rwd
