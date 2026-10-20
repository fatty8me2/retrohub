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

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace n64run {

constexpr int kPorts = 4;
constexpr uint8_t kPktInput = 1;   // lockstep input packet (parsed by LockstepCore)
constexpr uint8_t kPktReady = 2;   // start barrier packet {type, slot}   (handled by the glue)

class LockstepCore {
  public:
    struct Config {
        int owner[kPorts] = {0, -1, -1, -1};  // owning slot per port, -1 = port not present
        int my_slot = 0;
        int num_slots = 1;     // PCs in the session (slots 0..num_slots-1) for hash comparison
        int delay = 2;         // D
        int redundancy = 8;    // minimum number of samples per port per packet
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
    int delay() const { return cfg_.delay; }

    /* Schedule this PC's inputs read at poll `cur_sample` for sample cur_sample + D.
     * buttons[] is indexed by port; only locally owned ports are used. */
    void schedule_local(uint32_t cur_sample, const uint32_t buttons[kPorts]);

    /* Report this PC's progress (= number of samples completed so far); sent in every packet so
     * that PCs with fast peers don't run further ahead than the redundancy window covers
     * (matters for slots that own no port and so never gate anybody). */
    void set_progress(uint32_t completed) { my_progress_ = completed; }
    int max_lead() const { return cfg_.delay + 2; }

    /* Newest sample this PC has scheduled (cur + D), valid after schedule_local. */
    uint32_t newest_local() const { return newest_local_; }

    /* Serialize an input packet (all local ports, redundancy window, hash trailer).
     * Returns bytes written (0 if no local port). */
    size_t build_packet(uint8_t* out, size_t cap) const;

    /* Parse a packet received from `from_slot`. Returns false if malformed / not an input packet. */
    bool on_packet(int from_slot, const uint8_t* data, size_t len);

    /* True once every present port has its value for sample n and no peer lags more than max_lead() samples behind. */
    bool ready(uint32_t n) const;
    uint32_t get(uint32_t n, int port) const;

    /* Desync hashes. */
    void add_local_hash(uint32_t sample, uint64_t hash);
    bool poll_mismatch(Mismatch* out);
    uint32_t hashes_compared() const { return compared_; }

  private:
    struct Entry { uint32_t tag = 0xFFFFFFFFu; uint32_t val = 0; };
    static constexpr uint32_t kRing = 1024;
    void put(int port, uint32_t sample, uint32_t val);
    bool has(int port, uint32_t sample) const;
    void put_hash(int slot, uint32_t sample, uint64_t h);

    struct HashRec { uint64_t h[kPorts] = {0, 0, 0, 0}; uint8_t known = 0; bool done = false; };

    Config cfg_;
    int lead_ = -1;
    std::vector<Entry> ring_[kPorts];
    uint32_t newest_local_ = 0;
    uint32_t my_progress_ = 0;
    uint32_t peer_progress_[kPorts] = {0, 0, 0, 0};
    bool any_local_ = false;
    std::map<uint32_t, HashRec> hashes_;
    std::vector<Mismatch> mismatches_;
    std::vector<std::pair<uint32_t, uint64_t>> recent_local_hashes_;
    uint32_t compared_ = 0;
};

}  // namespace n64run
