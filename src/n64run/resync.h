#pragma once
/* Lockstep desync recovery: the agreement protocol (pure logic, header only, unit tested in test_resync.cpp).
 *
 * When the RDRAM sync checks disagree, the host (slot 0) picks a sample S a little in the future ("propose"), everybody keeps playing until it
 * reaches the lead-port poll S and stalls there (before sample S is delivered). The host asked the core for a savestate at poll S - 1 (taken at
 * the next interrupt = the state before sample S, the same alignment as the live spectator join); it sends that state to every player with
 * rb::xfer (state_xfer.h, epoch 0xC0|seq), the players load it and tell the host with Done, the host resumes and so do they, from sample S.
 *
 * Messages (kPktResync): [type, sub, from_slot, seq, u32 S]
 *   Req     guest -> host   "my checks saw a desync": repeated until a Propose arrives
 *   Propose host -> all     seq, S: repeated until every guest acknowledged
 *   PropAck guest -> host   got it (S not yet reached)
 *   Abort   either way      this attempt is off (a guest already passed S, the save or the transfer failed); the match goes on as before
 * Capability: a 3rd byte in the start-barrier "ready" packet (kCapResync). Everybody in the slot mask must have it, otherwise the feature is
 * off and a desync behaves exactly as before (logged; --strict-sync ends the match). At most kMaxResyncs recoveries per match. */

#include <cstdint>
#include <cstring>
#include <vector>

namespace n64run {
namespace resync {

constexpr uint8_t kCapResync = 1;
constexpr int kMaxResyncs = 3;
constexpr uint32_t kDefaultMargin = 90;   // samples between the proposal and S (the lead of a peer is at most the input delay; this covers the transit)

enum Sub : uint8_t { Req = 0, Propose = 1, PropAck = 2, Abort = 3 };

struct Msg {
    uint8_t sub = 0, from = 0, seq = 0;
    uint32_t s = 0;
};

inline std::vector<uint8_t> encode(uint8_t type, const Msg& m) {
    std::vector<uint8_t> v = {type, m.sub, m.from, m.seq};
    for (int i = 0; i < 4; i++) v.push_back((uint8_t)(m.s >> (8 * i)));
    return v;
}
inline bool decode(uint8_t type, const uint8_t* d, size_t n, Msg* m) {
    if (n < 8 || d[0] != type || d[1] > Abort || d[2] > 7) return false;
    m->sub = d[1]; m->from = d[2]; m->seq = d[3];
    std::memcpy(&m->s, d + 4, 4);
    return true;
}

/* What the caller must do after feeding the controller an event. */
enum class Act { None, Reply /* send *reply to the sender (or everyone, see reply_all) */, Fallback /* recovery is off or exhausted: old behaviour */ };

class Ctl {
  public:
    /* caps: capability byte per slot (as received in "ready"), mine included. Enabled iff more than one slot and every slot has kCapResync. */
    void configure(int my_slot, uint32_t slot_mask, const uint8_t caps[8], uint32_t margin = kDefaultMargin, int max_resyncs = kMaxResyncs) {
        my_ = my_slot; mask_ = slot_mask; margin_ = margin; max_ = max_resyncs;
        int n = 0;
        bool all = true;
        for (int s = 0; s < 8; s++) if (slot_mask >> s & 1u) { n++; if (!(caps[s] & kCapResync)) all = false; }
        enabled_ = all && n > 1;
    }
    bool enabled() const { return enabled_ && !failed_; }
    bool host() const { return my_ == 0; }
    int count() const { return count_; }
    bool exhausted() const { return count_ >= max_; }
    bool active() const { return active_; }
    uint32_t target() const { return s_; }
    uint8_t seq() const { return seq_; }
    uint32_t floor() const { return floor_; }
    uint32_t guests() const { return mask_ & ~1u; }
    bool all_acked() const { return (acked_ & guests()) == guests(); }
    uint32_t acked_mask() const { return acked_; }
    /* Hash samples below the floor were computed before the last recovery: their mismatches are history. */
    bool stale(uint32_t hash_sample) const { return hash_sample < floor_; }

    /* My own checks reported a mismatch at hash_sample while I am at sample cur. Returns:
     *   Fallback: do the old thing (log, strict stop); None: recovery is under way (ignore); Reply: guest must (re)send Req. */
    Act on_mismatch(uint32_t hash_sample, uint32_t cur) {
        if (stale(hash_sample)) return Act::None;
        if (!enabled_ || failed_) return Act::Fallback;
        if (active_) return Act::None;
        if (count_ >= max_) { failed_ = true; return Act::Fallback; }
        if (host()) { start(cur); return Act::None; }
        want_req_ = true;
        return Act::Reply;
    }
    /* guest: should a Req go out now (rate limited to every 150 ms while no Propose has come)? */
    bool req_due(uint32_t now) {
        if (!want_req_ || active_ || !enabled()) return false;
        if (last_req_ && now - last_req_ < 150) return false;
        last_req_ = now ? now : 1;
        return true;
    }
    /* host: should the Propose be (re)sent now (every 100 ms until all guests acked)? */
    bool propose_due(uint32_t now) {
        if (!host() || !active_ || all_acked()) return false;
        if (last_prop_ && now - last_prop_ < 100) return false;
        last_prop_ = now ? now : 1;
        return true;
    }
    Msg propose_msg() const { Msg m; m.sub = Propose; m.from = (uint8_t)my_; m.seq = seq_; m.s = s_; return m; }
    Msg req_msg(uint32_t hash_sample) const { Msg m; m.sub = Req; m.from = (uint8_t)my_; m.s = hash_sample; return m; }

    /* A control message from `from`; cur = my last polled sample. If it needs an answer, *reply is filled. */
    Act on_msg(const Msg& m, uint32_t cur, Msg* reply) {
        if (!enabled_ || failed_) return Act::None;
        if (host()) {
            if (!(guests() >> m.from & 1u)) return Act::None;
            if (m.sub == Req) { if (!active_ && !stale(m.s) && count_ < max_) start(cur); else if (!active_ && !stale(m.s) && count_ >= max_) failed_ = true; }
            else if (m.sub == PropAck && active_ && m.seq == seq_) acked_ |= 1u << m.from;
            else if (m.sub == Abort && active_ && m.seq == seq_) { abort(); return Act::Fallback; }
            return Act::None;
        }
        if (m.from != 0) return Act::None;
        if (m.sub == Propose) {
            if (active_ && m.seq == seq_) { *reply = ack(); return Act::Reply; }   // a repeat: our ack was lost
            if (active_ || (have_seq_ && m.seq == seq_)) return Act::None;           // an old attempt's repeat
            if (m.s <= cur) { Msg a; a.sub = Abort; a.from = (uint8_t)my_; a.seq = m.seq; a.s = m.s; *reply = a; failed_ = true; return Act::Reply; }   // already past S
            active_ = true; seq_ = m.seq; have_seq_ = true; s_ = m.s; want_req_ = false;
            *reply = ack();
            return Act::Reply;
        }
        if (m.sub == Abort && active_ && m.seq == seq_) { abort(); failed_ = true; return Act::Fallback; }
        return Act::None;
    }
    /* The recovery at S is complete on this peer. */
    void complete() { count_++; floor_ = s_; active_ = false; want_req_ = false; acked_ = 0; }
    /* Give the attempt up (timeout, save/transfer failure, a guest passed S): the match continues as before and recovery stays off. */
    void abort() { active_ = false; failed_ = true; want_req_ = false; }
    bool failed() const { return failed_; }
    /* host: poll n is the one before S: ask the core for the savestate now. */
    bool save_due(uint32_t n) const { return host() && active_ && !saved_ && n + 1 == s_; }
    void mark_saved() { saved_ = true; }
    /* is poll n the barrier (the lead-port poll of sample S)? */
    bool barrier_at(uint32_t n) const { return active_ && n == s_; }

  private:
    void start(uint32_t cur) {
        active_ = true; seq_ = (uint8_t)(seq_ + 1); have_seq_ = true; s_ = cur + margin_; acked_ = 0; saved_ = false; last_prop_ = 0;
    }
    Msg ack() const { Msg a; a.sub = PropAck; a.from = (uint8_t)my_; a.seq = seq_; a.s = s_; return a; }
    int my_ = 0, count_ = 0, max_ = kMaxResyncs;
    uint32_t mask_ = 1, margin_ = kDefaultMargin, s_ = 0, floor_ = 0, acked_ = 0, last_req_ = 0, last_prop_ = 0;
    uint8_t seq_ = 0;
    bool enabled_ = false, failed_ = false, active_ = false, have_seq_ = false, want_req_ = false, saved_ = false;
};

}  // namespace resync
}  // namespace n64run
