#pragma once
/* N64 drop-in: a friend joins a running n64run lockstep match into a controller port nobody plays (pure protocol logic, header only,
 * unit tested in test_dropin.cpp; glued into Lockstep in lockstep.cpp and the frontend's `live` namespace).
 *
 * Design: "a live spectator join that is promoted to a player at an agreed sample", with no stall for the players.
 *  1. Every PC of a drop-in room (hub flag --dropin-room) reports the empty ports to the game as plugged-in controllers from boot
 *     (LockstepCore::Config::reserved): the PIF can't be re-plugged later without desyncing, so presence is part of the room's setup. A reserved
 *     port reads neutral for everybody until a joiner takes it.
 *  2. The joiner (n64run --dropin) boots like a live spectator: the host saves a state at a lead poll on the core's worker thread (no stall),
 *     sends it (rb::xfer), streams the confirmed inputs from that sample, the joiner loads it and catches up unthrottled. All of that is the
 *     existing spectator machinery (frontend.cpp namespace live, spectate.h); the knock is kPktJoin version 2 and carries a capability + the name.
 *  3. Once the joiner is live (a few samples behind the host) it says Ready. The host then picks the join sample J (a bit more than a second
 *     ahead), the first sample F the port is supplied from (F = J + D + slack: the joiner's input is scheduled D samples ahead of its poll like
 *     everybody's, and the slack covers its transit delay), and broadcasts Admit{slot, port, J, F, D} to every player and the joiner, repeated until
 *     each acked. Every PC binds the port to the slot from F on (neutral before) and adds the slot to the gating/hash mask from F. Nobody stalls
 *     for the joiner before F; at F the slot gates like any other player.
 *  4. The joiner keeps replaying the stream until its poll J, where it turns into a player (Lockstep::dj_promote): same RDRAM, same sample, now
 *     scheduling its own input and exchanging input packets with everybody. It tells the host Live.
 *  5. A joiner that leaves (bye) or goes silent is dropped by the existing mid-match drop agreement (drop_agree.h); when the agreed cutoff is
 *     behind everybody the port is reserved again and the next friend can drop in. Before J nothing needs undoing; between Admit and F an Abort
 *     (the host's barrier at J waits for every ack and gives up after kAckLimitMs) unbinds the port.
 * Capability: kCapDropIn (a bit of the caps byte of the start-barrier "ready" packet). The host refuses a knock with kRefuseOldPeer when any player lacks it. */

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace n64run {
namespace dropin {

constexpr uint8_t kCapDropIn = 4;          // resync.h uses 1 and 2
constexpr uint8_t kKnockVersion = 2;       // kPktJoin [0x72, slot, 2, caps, name_len, name...]  (version 1 = plain spectator)
constexpr uint32_t kAdmitMargin = 75;      // samples between "joiner is ready" and J (the Admit has to reach everybody, the joiner has to reach J)
constexpr uint32_t kSlack = 12;            // F = J + D + kSlack + ceil(rtt / frame)
constexpr uint32_t kAckLimitMs = 2500;     // the host's barrier at J waits this long for the acks before it cancels the admit
constexpr uint32_t kReadyLag = 20;         // the joiner says Ready when it is at most this many samples behind the host (sustained; the host adds the lag to F)
constexpr uint32_t kReadySustain = 20;     // ... for this many samples in a row
constexpr uint32_t kReadyEveryMs = 200;
constexpr uint32_t kResendMs = 100;
constexpr uint32_t kNoAdmitMs = 25000;     // the joiner gives up when the host has not admitted it this long after Ready

/* kPktNoLive reasons added by drop-in (spectate.h has 1..3). The joiner maps them to the hub's exit codes: NoPort 9, OldPeer 10, anything else 11. */
enum Refuse : uint8_t { kRefuseNoPort = 4, kRefuseOldPeer = 5, kRefuseBusy = 6, kRefuseSlot = 7 };
inline int exit_code_for(uint8_t reason) { return reason == kRefuseNoPort ? 9 : reason == kRefuseOldPeer ? 10 : 11; }

enum Sub : uint8_t { Admit = 0, Ack = 1, Abort = 2, Ready = 3, Live = 4, Nack = 5 };

struct Msg {
    uint8_t sub = 0, from = 0, id = 0, slot = 0, port = 0, d = 0, flags = 0;
    uint32_t j = 0, f = 0, pos = 0;
    uint8_t mask = 0;                            // Admit: slots playing now (before this admit)
    int8_t owners[4] = {-1, -1, -1, -1};         // Admit: port -> slot now, -1 = reserved
    std::string name;
};
constexpr uint8_t kFlagResync = 1, kFlagMasked = 2;   // Admit.flags: the players' resync / masked-hash state (the joiner adopts it)

inline std::vector<uint8_t> encode(uint8_t type, const Msg& m) {
    std::vector<uint8_t> v = {type, m.sub, m.from, m.id, m.slot, m.port, m.d, m.flags};
    for (uint32_t x : {m.j, m.f, m.pos}) for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i)));
    v.push_back(m.mask);
    for (int i = 0; i < 4; i++) v.push_back((uint8_t)m.owners[i]);
    const size_t nl = m.name.size() > 24 ? 24 : m.name.size();
    v.push_back((uint8_t)nl);
    v.insert(v.end(), m.name.begin(), m.name.begin() + (long)nl);
    return v;
}
inline bool decode(uint8_t type, const uint8_t* d, size_t n, Msg* m) {
    if (n < 26 || d[0] != type || d[1] > Nack || d[2] > 7 || d[4] > 7 || d[5] > 3) return false;
    m->sub = d[1]; m->from = d[2]; m->id = d[3]; m->slot = d[4]; m->port = d[5]; m->d = d[6]; m->flags = d[7];
    std::memcpy(&m->j, d + 8, 4); std::memcpy(&m->f, d + 12, 4); std::memcpy(&m->pos, d + 16, 4);
    m->mask = d[20];
    for (int i = 0; i < 4; i++) m->owners[i] = (int8_t)d[21 + i];
    const size_t nl = d[25];
    if (n < 26 + nl) return false;
    m->name.assign((const char*)d + 26, nl);
    return true;
}

/* The knock's capability/name part: [0x72, slot, 2, caps, name_len, name...] */
inline std::vector<uint8_t> encode_knock(uint8_t pkt_join, int slot, uint8_t caps, const std::string& name) {
    std::vector<uint8_t> k = {pkt_join, (uint8_t)slot, kKnockVersion, caps};
    const size_t nl = name.size() > 24 ? 24 : name.size();
    k.push_back((uint8_t)nl);
    k.insert(k.end(), name.begin(), name.begin() + (long)nl);
    return k;
}
inline bool is_knock(const std::vector<uint8_t>& m) { return m.size() >= 5 && m[2] == kKnockVersion; }
inline uint8_t knock_caps(const std::vector<uint8_t>& m) { return m.size() >= 4 ? m[3] : 0; }
inline std::string knock_name(const std::vector<uint8_t>& m) {
    if (m.size() >= 5 && m.size() >= 5u + m[4]) return std::string((const char*)m.data() + 5, m[4]);
    return std::string();
}

/* First sample the joiner's port is supplied from. */
inline uint32_t first_sample(uint32_t j, int d, double rtt_ms, uint32_t joiner_lag = 0, double frame_ms = 1000.0 / 60.0) {
    uint32_t rtt_frames = rtt_ms <= 0 ? 0u : (uint32_t)((rtt_ms + frame_ms - 1.0) / frame_ms);
    return j + (uint32_t)(d < 0 ? 0 : d) + kSlack + rtt_frames + joiner_lag;   // the joiner reaches J as many samples after the host as it lags behind it
}

/* Host: one admit at a time. */
class Ctl {
  public:
    bool active() const { return active_; }
    const Msg& cur() const { return m_; }
    uint32_t need() const { return need_; }
    /* m: the Admit message (sub, slot, port, d, j, f, name, flags filled; id assigned here). need: slots that must ack (players + the joiner). */
    uint8_t begin(Msg m, uint32_t need_mask, uint32_t now) {
        id_ = (uint8_t)(id_ + 1 ? id_ + 1 : 1);
        m.sub = Admit; m.id = id_; m_ = m; need_ = need_mask; acked_ = 0; active_ = true; started_ = now ? now : 1; last_ = 0; aborting_ = false; sends_ = 0;
        return id_;
    }
    void on_ack(const Msg& a) { if (active_ && a.id == m_.id && a.from < 8) acked_ |= 1u << a.from; }
    bool all_acked() const { return active_ && (acked_ & need_) == need_; }
    uint32_t missing() const { return need_ & ~acked_; }
    bool send_due(uint32_t now) { if (!active_ || (!aborting_ && all_acked())) return false; if (last_ && now - last_ < kResendMs) return false; last_ = now ? now : 1; sends_++; return true; }
    bool timed_out(uint32_t now) const { return active_ && !all_acked() && now - started_ > kAckLimitMs; }
    /* the sample barrier is where the host waits: has the admit been pending too long to keep a stalled barrier? */
    uint32_t started() const { return started_; }
    void abort() { aborting_ = true; }
    bool aborting() const { return aborting_; }
    int aborts_sent() const { return sends_; }
    void finish() { active_ = false; aborting_ = false; }
    uint8_t id() const { return id_; }
  private:
    Msg m_;
    uint32_t need_ = 0, acked_ = 0, started_ = 0, last_ = 0;
    uint8_t id_ = 0;
    bool active_ = false, aborting_ = false;
    int sends_ = 0;
};

/* Joiner: "Ready" gating -- the lag must stay small for a while before the host is asked. */
class ReadyGate {
  public:
    void reset() { run_ = 0; }
    /* One call per consumed sample with the number of samples still buffered. */
    void on_sample(uint32_t lag) { if (lag <= kReadyLag) run_++; else run_ = 0; }
    bool ready() const { return run_ >= kReadySustain; }
  private:
    uint32_t run_ = 0;
};

}  // namespace dropin
}  // namespace n64run
