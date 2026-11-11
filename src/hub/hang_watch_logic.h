/* Lichii/Fritchii exit-hang watchdog decision (pure, header-only, no Win32). launch.cpp's lichii_hang_watch samples the clock, the window and the
 * process counters and feeds them to HangWatch::tick; it ends the emulator's job only when tick says End.
 *
 * Rule (unchanged since 1.40.9): once the game had a window and every window is gone, nothing happens for 15 s. After that the process is ended
 * when it was quiet for 10 s (no more than 2,000,000 units of CPU time AND 65,536 bytes of I/O since the last time it counted as busy), or
 * unconditionally 180 s after the window went away. */
#pragma once
#include <cstdint>

namespace hubhang {

constexpr uint64_t kGraceMs = 15000;           // nothing is ended in the first 15 s after the window went away
constexpr uint64_t kQuietMs = 10000;           // no CPU and no disk progress for this long = hung
constexpr uint64_t kCapMs = 180000;            // hard cap after the window went away
constexpr uint64_t kCpuStep = 2000000;         // CPU counter units (100 ns) that count as "busy" (0.2 s)
constexpr uint64_t kIoStep = 65536;            // I/O bytes that count as "busy"

/* Does one top-level window of the game process count as "the game still has a window" for the watchdog? A MINIMISED window has a 0x0
 * client rect but is very much alive (the player pressed Win+D / Alt-Tabbed out): it counts as present. Only a visible, non-tool window
 * with a non-empty client area, or a minimised one, counts; zero-size helper windows are ignored. */
inline bool window_counts_as_present(bool visible, bool iconic, long client_w, long client_h, bool tool_window) {
    if (!visible || tool_window) return false;
    if (iconic) return true;
    return client_w > 0 && client_h > 0;
}

enum class Verdict { Wait, End };
enum class Why { None, Quiet, Cap };

struct HangWatch {
    bool had_window = false;
    uint64_t gone_since = 0;   // 0 = window present (or never gone); note: a tick at now_ms == 0 is not distinguishable, as in the original
    uint64_t cpu_at_gone = 0, io_at_gone = 0, idle_since = 0;
    Why why = Why::None;       // set when tick returns End

    /* The watch is off (no process, wrong system, headless, Hookshot): forget everything. */
    void reset() { had_window = false; gone_since = 0; }

    /* One tick. cpu / io are only read when window_present is false (pass 0 otherwise); a failed counter read is reported as 0. */
    Verdict tick(uint64_t now, bool window_present, uint64_t cpu, uint64_t io) {
        why = Why::None;
        if (window_present) { had_window = true; gone_since = 0; return Verdict::Wait; }
        if (!had_window) return Verdict::Wait;
        if (!gone_since) { gone_since = now; cpu_at_gone = cpu; io_at_gone = io; idle_since = now; return Verdict::Wait; }
        if (cpu > cpu_at_gone + kCpuStep) { cpu_at_gone = cpu; idle_since = now; }
        if (io > io_at_gone + kIoStep) { io_at_gone = io; idle_since = now; }
        const bool quiet = now - idle_since >= kQuietMs, too_long = now - gone_since >= kCapMs;
        if (now - gone_since < kGraceMs || (!quiet && !too_long)) return Verdict::Wait;
        why = quiet ? Why::Quiet : Why::Cap;
        had_window = false;
        gone_since = 0;
        return Verdict::End;
    }
};

}  // namespace hubhang
