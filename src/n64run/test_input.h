#pragma once
/* Scripted local input for QA (--test-input / N64RUN_TEST_INPUT). Replaces pads/keyboard as the source of the
 * local controller word, so the stream goes through the normal lockstep local-input path.
 *
 *   random:<seed>   deterministic pseudo-random stream; buttons Start/A/B/Z/L/R/C*, d-pad, analog stick, each pick
 *                   held for 5..40 samples. Start is forced for 6 samples every ~240 samples so menus advance.
 *                   Seed is mixed with the lockstep slot and the owned-port index, so peers differ.
 *   file:<path>     text lines "sample,buttons,x,y" (# comments). buttons = number (0x hex ok) or names joined by
 *                   '+' or '|' (A B Z START L R CU CD CL CR UP DOWN LEFT RIGHT). A line applies from `sample` on
 *                   (sample = 0-based index of the local sample, i.e. one per emulated input poll). */

#include <cstdint>
#include <string>
#include <vector>

namespace n64run {

class TestInput {
  public:
    /* Parse a spec; returns false (and fills *err) on a bad spec / unreadable file. */
    bool configure(const std::string& spec, int slot, std::string* err);
    bool active() const { return mode_ != Mode::None; }
    /* Advance one sample (call once per lockstep sample). */
    void update();
    /* Controller word for the `owned_index`-th locally owned port for the current sample. */
    uint32_t read(int owned_index);

    static uint32_t parse_buttons(const std::string& s, bool* ok);

  private:
    enum class Mode { None, Random, File };
    struct Gen { uint64_t rng = 0; int left = 0; uint32_t word = 0; int next_start = 0; int start_left = 0; };
    struct Row { long long sample; uint32_t word; };
    uint32_t step(Gen& g);
    Mode mode_ = Mode::None;
    uint64_t seed_ = 0;
    long long sample_ = -1;
    Gen gens_[4];
    bool gen_init_[4] = {false, false, false, false};
    long long gen_sample_[4] = {-1, -1, -1, -1};
    uint32_t cached_[4] = {0, 0, 0, 0};
    std::vector<Row> rows_;
};

}  // namespace n64run
