#pragma once
/* Performance / online overlay text for the runners (Cmd::Overlay): FPS, ping, input delay, rollback frames.
 * Pure, header-only, no SDL. The runner feeds frame() once per presented frame and net numbers when it has them,
 * then shows text() with osd::set_stats(). Display only: nothing here touches emulation, so it is allowed online. */
#include <cstdint>
#include <cstdio>
#include <string>

namespace perfov {

struct Sample {
    double fps = 0;
    double rtt_ms = 0;        // 0 = unknown / offline
    int delay = 0;            // input delay in frames, 0 = unknown / offline
    double resim_per_s = -1;  // rollback re-simulated frames per second, < 0 = not a rollback session
};

inline std::string format(const Sample& s) {
    char b[96];
    std::string t;
    std::snprintf(b, sizeof b, "%d FPS", (int)(s.fps + 0.5));
    t = b;
    if (s.rtt_ms > 0) { std::snprintf(b, sizeof b, " | PING %d ms", (int)(s.rtt_ms + 0.5)); t += b; }
    if (s.delay > 0) { std::snprintf(b, sizeof b, " | DELAY %d", s.delay); t += b; }
    if (s.resim_per_s >= 0) { std::snprintf(b, sizeof b, " | ROLLBACK %d/s", (int)(s.resim_per_s + 0.5)); t += b; }
    return t;
}

/* Frames per second over a sliding window, refreshed every `window_ms`. */
class FpsMeter {
  public:
    explicit FpsMeter(uint64_t window_ms = 500) : win_(window_ms) {}
    /* Call once per presented frame with a millisecond clock. Returns true when fps() was just refreshed. */
    bool frame(uint64_t now_ms) {
        if (!started_) { started_ = true; t0_ = now_ms; n_ = 0; return false; }
        ++n_;
        if (now_ms - t0_ < win_) return false;
        fps_ = (double)n_ * 1000.0 / (double)(now_ms - t0_);
        t0_ = now_ms; n_ = 0;
        return true;
    }
    double fps() const { return fps_; }
    void reset() { started_ = false; fps_ = 0; n_ = 0; }
  private:
    uint64_t win_, t0_ = 0;
    uint64_t n_ = 0;
    double fps_ = 0;
    bool started_ = false;
};

/* Per-second rate of a cumulative counter, sampled when the FpsMeter window rolls over. */
class RateMeter {
  public:
    double update(uint64_t now_ms, uint64_t total) {
        if (!started_ || total < last_total_ || now_ms <= last_ms_) { started_ = true; last_ms_ = now_ms; last_total_ = total; return rate_; }
        rate_ = (double)(total - last_total_) * 1000.0 / (double)(now_ms - last_ms_);
        last_ms_ = now_ms; last_total_ = total;
        return rate_;
    }
    double rate() const { return rate_; }
  private:
    bool started_ = false;
    uint64_t last_ms_ = 0, last_total_ = 0;
    double rate_ = 0;
};

}  // namespace perfov
