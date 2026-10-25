#pragma once
/* Lockstep scheduling/buffering core. Independent of the emulator, SDL and the network:
 * feed it local samples and remote packets, ask it whether sample n is complete.
 *
 * Model (see docs/ARCHITECTURE.md, "Lockstep"):
 *  - sample index n is the number of lead-port polls so far (0-based).
 *  - the input a PC reads at poll n is scheduled for sample n + D (D = input delay).
 *  - samples 0..D-1 are neutral (0) for every port and need no communication.
 *  - every packet carries the last `redundancy` scheduled samples of every port this PC owns,
 *    so single packet losses are healed by the next packet. A blocked PC retransmits its newest
 *    packet periodically (the glue does that), which heals losses when nobody advances.
 *  - desync hashes (sample, 64-bit hash) ride on the same packets as a small trailer.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace n64run {

constexpr int kPorts = 4;
constexpr uint8_t kPktInput = 1;   // lockstep input packet (parsed by LockstepCore)
constexpr uint8_t kPktReady = 2;   // start barrier packet {type, slot}   (handled by the glue)
// 3..6 are used by save_sync.h
constexpr uint8_t kPktDelay = 7;      // host -> clients {type, slot, D}: the negotiated input delay
constexpr uint8_t kPktDelayAck = 8;   // client -> host {type, slot, D}
constexpr uint8_t kPktBye = 9;        // {type, slot, u32 newest}: sender's emulation ended cleanly; newest = last sample it ever scheduled
constexpr int kMaxDelay = 20;         // hard cap of the (adaptive) input delay, in samples

/* Slot set of a session: bit s = slot s is a PC in the session. Slots may be sparse ({0,2}).
 * Members = every owner in --ports, the host (slot 0) and my_slot. If `players` is larger than that,
 * the lowest unused slots are added (port-less spectators, legacy contiguous behaviour).
 * Returns 0 and fills *err if invalid (players smaller than the PCs found, slot > 3, ...). */
uint32_t derive_slot_mask(const int owner[kPorts], int my_slot, int players, std::string* err);
int popcount32(uint32_t v);

/* Auto input delay: D = ceil((rtt/2 + margin) / frame_ms) + 1, clamped to 1..10 (+1 if relayed). */
int auto_delay_from_rtt(double rtt_ms, double frame_ms, bool relayed = false, double margin_ms = 8.0);
/* Worst effective RTT for a star topology: the host's slowest ping, or the sum of the two slowest
 * (client<->client traffic crosses the host twice). */
double effective_rtt(std::vector<double> host_to_client_rtt);

/* Adaptive input delay policy (pure logic; time is passed in). Every PC feeds it its own stalls and polls
 * want(); the host also calls decide() with everybody's wants.
 *  - raise: >= 2 stalls of >= kMinStallMs within 3 s -> want D + ceil(longest stall / sample interval) (1..3 more); a single
 *    stall (peer hiccup, scene load) is ignored. At most one raise per 1.5 s so the previous one can take effect.
 *  - lower: no stall >= kMinStallMs for 12 s -> want D - 1, never below the negotiated floor; one step per 12 s. */
class DelayAdapter {
  public:
    static constexpr uint32_t kMinStallMs = 6;
    static constexpr uint32_t kMaxStallMs = 250;     // longer stalls are loads/hiccups, not jitter: a bigger D cannot fix them
    static constexpr uint32_t kQuietLowerMs = 4000;  // quiet time before stepping D down
    /* Sane ceiling for the adaptive D given the negotiated (RTT-derived) floor. */
    static int cap_for_floor(int floor_d) { return std::max(floor_d, std::min(8, floor_d + 3)); }
    explicit DelayAdapter(int floor_d = 1, int max_d = kMaxDelay) : floor_(floor_d), max_(max_d) {}
    /* One call per lead-port poll: `now` (ms) and the stall (ms) this poll spent waiting (0 if none). */
    void on_poll(uint32_t now, uint32_t stall_ms);
    /* My wanted D given the D currently in effect. */
    int want(int cur, uint32_t now);
    /* Host: combine wants (of every PC in `mask`, including mine) into the D to order. Changes at most once per 1.5 s. */
    int decide(int cur, const int wants[4], uint32_t mask, uint32_t now);
    double sample_ms() const { return avg_ms_; }
  private:
    int floor_, max_;
    uint32_t last_now_ = 0, last_stall_ = 0;
    double avg_ms_ = 16.7;
    uint32_t ev_t_[8] = {0}, ev_ms_[8] = {0};
    int ev_n_ = 0;                  // ring of recent stall events
    uint32_t last_event_ = 0;       // time of the last stall >= kMinStallMs (0 = none)
    uint32_t last_raise_ = 0, last_lower_ = 0, last_decide_change_ = 0;
    uint32_t start_ = 0;
    int want_val_ = -1;
    uint32_t want_until_ = 0;
    bool started_ = false;
};

class LockstepCore {
  public:
    struct Config {
        int owner[kPorts] = {0, -1, -1, -1};  // owning slot per port, -1 = port not present
        int my_slot = 0;
        int num_slots = 1;     // PCs in the session; used when slot_mask == 0 (slots 0..num_slots-1)
        uint32_t slot_mask = 0;   // explicit (possibly sparse) slot set; overrides num_slots
        int delay = 2;         // D
        int redundancy = 8;    // minimum number of samples per port per packet (raised to cover kMaxDelay)
        bool wide = false;     // every sample carries a second u32 ("aux", e.g. PlayStation analog sticks); all PCs must agree
    };

    struct Mismatch {
        uint32_t sample = 0;
        uint64_t hash[kPorts] = {0, 0, 0, 0};   // per slot
        uint8_t known = 0;                     // bitmask of slots
    };

    explicit LockstepCore(const Config& cfg);

    bool port_present(int p) const { return p >= 0 && p < kPorts && cfg_.owner[p] >= 0; }
    bool port_local(int p) const { return port_present(p) && cfg_.owner[p] == cfg_.my_slot; }
    int lead_port() const { return lead_; }          // lowest present port, -1 if none
    int delay() const { return delay_; }
    uint32_t slot_mask() const { return mask_; }

    /* Schedule this PC's inputs read at poll `cur_sample` for sample cur_sample + D.
     * buttons[] is indexed by port; only locally owned ports are used. */
    void schedule_local(uint32_t cur_sample, const uint32_t buttons[kPorts], const uint32_t aux[kPorts] = nullptr);

    /* Report this PC's progress (= number of samples completed so far); sent in every packet so
     * that PCs with fast peers don't run further ahead than the redundancy window covers
     * (matters for slots that own no port and so never gate anybody). */
    void set_progress(uint32_t completed) { my_progress_ = completed; }
    int max_lead() const { return std::max(delay_, kLeadFloor) + 2; }

    /* Newest sample this PC has scheduled (cur + D), valid after schedule_local. */
    uint32_t newest_local() const { return (uint32_t)newest_; }

    /* Adaptive input delay. Determinism does not depend on every PC using the same D at the same time: a sample's
     * value is whatever its owner scheduled and sent (receivers never overwrite), and when D grows the owner holds its
     * current input over the new gap, when D shrinks it simply schedules nothing until the old frontier is passed.
     * The host (slot 0) orders a new D (carried in every host packet, versioned by a generation byte); every PC applies
     * it at its next poll. Every PC reports the D it would like (want) in each packet. */
    void order_delay(int d);                 // host only
    int ordered_gen() const { return ord_gen_; }
    void set_want(int d) { want_ = (uint8_t)std::max(0, std::min(255, d)); }
    int peer_want(int slot) const { return slot >= 0 && slot < kPorts ? peer_want_[slot] : 0; }
    /* A peer said bye (it ended its emulation cleanly): stop gating on its progress. */
    void peer_left(int slot) { if (slot >= 0 && slot < kPorts) gate_mask_ &= ~(1u << slot); }
    /* How many samples a peer is ahead of sample n by its reported progress (0 if not ahead): inbox backlog indicator. */
    uint32_t peer_lead(int slot, uint32_t n) const { return slot >= 0 && slot < kPorts && peer_progress_[slot] > n ? peer_progress_[slot] - n : 0; }
    uint32_t delay_changes() const { return delay_changes_; }   // how many times D changed after start

    /* Serialize an input packet (all local ports, redundancy window, hash trailer).
     * Returns bytes written (0 if no local port). */
    size_t build_packet(uint8_t* out, size_t cap) const;

    /* Parse a packet received from `from_slot`. Returns false if malformed / not an input packet. */
    bool on_packet(int from_slot, const uint8_t* data, size_t len);

    /* True once every present port has its value for sample n and no peer lags more than max_lead() samples behind. */
    bool ready(uint32_t n) const;
    uint32_t get(uint32_t n, int port) const;
    uint32_t get_aux(uint32_t n, int port) const;   // 0 unless Config::wide

    /* Desync hashes. */
    void add_local_hash(uint32_t sample, uint64_t hash);
    bool poll_mismatch(Mismatch* out);
    uint32_t hashes_compared() const { return compared_; }

  private:
    static constexpr int kLeadFloor = 4;
    struct Entry { uint32_t tag = 0xFFFFFFFFu; uint32_t val = 0; uint32_t aux = 0; };
    static constexpr uint32_t kRing = 1024;
    void put(int port, uint32_t sample, uint32_t val, uint32_t aux = 0);
    bool has(int port, uint32_t sample) const;
    void put_hash(int slot, uint32_t sample, uint64_t h);

    struct HashRec { uint64_t h[kPorts] = {0, 0, 0, 0}; uint8_t known = 0; bool done = false; };

    Config cfg_;
    uint32_t mask_ = 1;
    uint32_t gate_mask_ = 1;   // slots whose progress gates ready()
    int lead_ = -1;
    std::vector<Entry> ring_[kPorts];
    int64_t newest_ = -1;         // newest sample this PC has scheduled
    int delay_ = 0;               // current D
    int initial_delay_ = 0;
    uint8_t ord_gen_ = 0;         // host's delay order generation
    uint8_t want_ = 0;
    uint8_t peer_want_[kPorts] = {0, 0, 0, 0};
    uint32_t delay_changes_ = 0;
    uint32_t my_progress_ = 0;
    uint32_t peer_progress_[kPorts] = {0, 0, 0, 0};
    bool any_local_ = false;
    std::map<uint32_t, HashRec> hashes_;
    std::vector<Mismatch> mismatches_;
    std::vector<std::pair<uint32_t, uint64_t>> recent_local_hashes_;
    uint32_t compared_ = 0;
};

}  // namespace n64run
