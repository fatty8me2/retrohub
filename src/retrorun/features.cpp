#include "features.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace retrofeat {

static const int kTimingWindow = 120;      // frames averaged before the speed verdict
static const double kRewindMaxShare = 0.25; // capture cost may use at most 25% of a frame
static const double kFrameMaxShare = 0.85;  // with run-ahead, a whole frame may use at most 85% of the period

Features::Features(Hooks h, Settings s) : h_(std::move(h)), s_(std::move(s)) {
    if (!s_.offline) return;   // refused online: stays inert
    if (s_.rewind != 2) {
        int secs = s_.rewind == 1 ? rwd::forced_seconds(s_.system) : rwd::auto_seconds(s_.system);
        rewind_on_ = secs > 0;
    }
    ra_frames_ = runahead::effective(s_.runahead, s_.system);
    if (!h_.state_size || h_.state_size() == 0) { rewind_on_ = false; ra_frames_ = 0; }
}

uint64_t Features::now() const {
    if (h_.now_us) return h_.now_us();
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool Features::ensure_buffers() {
    size_t n = h_.state_size ? h_.state_size() : 0;
    if (!n) return false;
    if (n != state_bytes_) {
        state_bytes_ = n;
        scratch_.assign(n, 0);
        if (rewind_on_) {
            int secs = s_.rewind == 1 ? rwd::forced_seconds(s_.system) : rwd::auto_seconds(s_.system);
            ring_.reset(new rwd::Ring(n, (size_t)(secs * (s_.fps > 1 ? s_.fps : 60.0)), rwd::kMaxBytes));
        }
    }
    return true;
}

void Features::disable_rewind(const std::string& why) {
    rewind_on_ = false;
    rewinding_ = false;
    ring_.reset();
    if (h_.corner) h_.corner("");
    note(why, Note::Warn);
}

bool Features::set_held(bool down) {
    if (down == held_) return true;
    held_ = down;
    if (down && !rewind_on_) {
        if (!announced_) { announced_ = true; note(s_.offline ? "Rewind is off for this game" : "Rewind is not available online", Note::Warn); }
        return false;
    }
    rewinding_ = held_ || latched_;
    if (!rewinding_ && h_.corner) h_.corner("");
    return true;
}

bool Features::set_latched(bool on) {
    if (on && !rewind_on_) {
        note(s_.offline ? "Rewind is off for this game" : "Rewind is not available online", Note::Warn);
        return false;
    }
    latched_ = on;
    rewinding_ = held_ || latched_;
    if (!rewinding_ && h_.corner) h_.corner("");
    return true;
}

void Features::rewind_step() {
    std::vector<uint8_t> st;
    bool have = false;
    if (ring_ && ring_->step_back(&st)) {
        have = true;
        steps_back_++;
    } else if (ring_ && ring_->newest(&st)) {
        have = true;   // oldest state reached: hold there
        if (latched_) { latched_ = false; rewinding_ = held_; note("Rewind: reached the start of the history"); }
    }
    if (have) h_.load(st.data(), st.size());
    if (h_.speculative) h_.speculative(true);
    h_.run();   // draws the restored moment; discarded by the next step / the next normal frame
    if (h_.speculative) h_.speculative(false);
    if (h_.corner) {
        char b[48];
        std::snprintf(b, sizeof b, "REWIND %.1fs", (double)steps_back_ / (s_.fps > 1 ? s_.fps : 60.0));
        h_.corner(b);
    }
}

void Features::frame() {
    if (!rewind_on_ && ra_frames_ == 0) { h_.run(); if (h_.real_frame) h_.real_frame(); return; }
    if (!ensure_buffers()) { h_.run(); if (h_.real_frame) h_.real_frame(); return; }
    if (rewinding_) { rewind_step(); return; }
    steps_back_ = 0;

    const uint64_t t0 = now();
    h_.run();
    if (h_.real_frame) h_.real_frame();   // the core is on the real frame here; run-ahead has not touched it yet
    const uint64_t t_run = now();
    if (!h_.save(scratch_.data(), scratch_.size())) { disable_rewind("Rewind stopped: the core cannot save states"); ra_frames_ = 0; return; }
    if (rewind_on_) ring_->push(scratch_.data());
    const uint64_t t_cap = now();
    if (ra_frames_ > 0) runahead::speculate(ra_frames_, h_.run, h_.speculative, h_.load, scratch_);
    const uint64_t t_end = now();

    if (s_.measure && !timing_done_) {
        cap_us_ += t_cap - t_run;      // serialize + delta (+ the state copy run-ahead needs anyway)
        total_us_ += t_end - t0;       // the whole frame
        if (++win_n_ >= kTimingWindow) {
            timing_done_ = true;
            const double period = 1e6 / (s_.fps > 1 ? s_.fps : 60.0);
            if (ra_frames_ > 0 && (double)total_us_ / win_n_ > period * kFrameMaxShare) {
                ra_frames_ = 0;
                note("Run-ahead turned off: this PC is too slow for it", Note::Warn);
            }
            if (rewind_on_ && (double)cap_us_ / win_n_ > period * kRewindMaxShare)
                disable_rewind("Rewind turned off: this PC is too slow for it");
        }
    }
}

/* ------------------------------------------------------------------ scripted checks */

bool Features::selftest_rewind(int frames, int back, std::string* report) {
    char b[200];
    if (!rewind_on_ || !h_.video_hash) { *report = "rewind not active for this system"; return false; }
    if (frames < back + 3) frames = back + 3;
    std::vector<uint64_t> vh;
    for (int i = 0; i < frames; i++) { frame(); vh.push_back(h_.video_hash()); }
    int bad_back = 0, bad_replay = 0;
    set_latched(true);
    for (int j = 1; j <= back; j++) {
        frame();
        if (h_.video_hash() != vh[(size_t)(frames - j)]) bad_back++;
    }
    set_latched(false);
    int replayed = 0;
    for (int m = 1; m < back; m++) {
        frame();
        if (h_.video_hash() != vh[(size_t)(frames - back + m)]) bad_replay++;
        replayed++;
    }
    std::snprintf(b, sizeof b, "REWINDTEST frames %d back %d: rewound-frame mismatches %d, replay mismatches %d of %d, ring %zu states %zu KB",
                  frames, back, bad_back, bad_replay, replayed, ring_ ? ring_->frames() : 0, ring_ ? ring_->bytes() / 1024 : 0);
    *report = b;
    return bad_back == 0 && bad_replay == 0;
}

bool Features::selftest_runahead(int frames, std::string* report) {
    char b[200];
    const int n = ra_frames_;
    if (n <= 0 || !h_.video_hash || !h_.state_size) { *report = "run-ahead not active for this system"; return false; }
    std::vector<uint8_t> s0(h_.state_size());
    if (!h_.save(s0.data(), s0.size())) { *report = "core cannot save states"; return false; }
    std::vector<uint64_t> base, ahead;
    for (int f = 0; f < frames + n; f++) { h_.run(); base.push_back(h_.video_hash()); }
    h_.load(s0.data(), s0.size());
    for (int f = 0; f < frames; f++) { frame(); ahead.push_back(h_.video_hash()); }
    int bad = 0;
    for (int f = 0; f < frames; f++) if (ahead[(size_t)f] != base[(size_t)(f + n)]) bad++;
    std::snprintf(b, sizeof b, "RUNAHEADTEST %d frames, run-ahead %d: output vs normal shifted by %d frames mismatches %d", frames, n, n, bad);
    *report = b;
    return bad == 0;
}

}  // namespace retrofeat
