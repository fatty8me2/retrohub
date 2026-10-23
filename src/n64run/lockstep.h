#pragma once
/* Lockstep runtime: glues LockstepCore to hubnet::Session, local input and the emulator.
 *
 * ASSUMPTION (documented in input_plugin.cpp): the core polls controllers once per "frame
 * poll" in port order 0..3 over the present ports. The sample index n therefore advances on
 * each GetKeys call for the lowest present port (the "lead port"). GetKeys for the other ports
 * within the same poll return that sample's value. Because emulation is deterministic, the number
 * of lead-port polls between any two guest-visible events is identical on all PCs. */

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "lockstep_core.h"

namespace hubnet { class Session; }

namespace n64run {

struct LockstepOptions {
    int owner[kPorts] = {0, -1, -1, -1};
    int my_slot = 0;
    int num_slots = 1;
    uint32_t slot_mask = 0;        // explicit (sparse) slot set; 0 = slots 0..num_slots-1
    int delay = 2;
    bool online = false;
    bool strict_sync = false;
    bool adaptive_delay = true;    // raise/lower the input delay from observed stalls (host orders, all follow)
    int hash_interval = 600;       // samples between RDRAM hashes
    uint32_t timeout_ms = 10000;   // no progress -> "Connection lost"
    std::string hash_label = "state";   // desync log text: "<label> hashes"
};

struct LockstepHooks {
    std::function<uint32_t(int owned_index)> read_local;      // packed BUTTONS for the owned_index-th local port
    std::function<void()> update_local;                       // poll devices once per sample
    std::function<void()> idle;                               // called while blocked (pump SDL events)
    std::function<bool(uint64_t* out)> hash_rdram;            // false -> hashing unavailable
    std::function<void()> request_stop;                       // M64CMD_STOP
    std::function<void(const std::string&)> log;
};

class Lockstep {
  public:
    Lockstep(const LockstepOptions& opt, hubnet::Session* session, const LockstepHooks& hooks);

    /* Online only: exchange "ready" packets until every slot has been seen (or timeout). */
    bool start_barrier(uint32_t timeout_ms);

    /* Called by the input plugin's GetKeys. Returns the packed BUTTONS word. */
    uint32_t get_keys(int port);

    bool present(int port) const { return core_.port_present(port); }
    bool local(int port) const { return core_.port_local(port); }

    /* Stall statistics (time spent blocked waiting for peers' inputs). */
    uint64_t stall_ms() const { return stall_ms_; }
    uint64_t stalled_samples() const { return stalled_; }
    uint64_t samples_done() const { return (uint64_t)(sample_ + 1); }
    void log_stall_summary() const;

    /* Call once after emulation ended (online): tells the others "bye" so they end cleanly instead of timing out. */
    void finish();
    bool peer_left() const { return peer_left_; }          // a partner ended the session (clean)
    int current_delay() const { return core_.delay(); }
    uint32_t delay_changes() const { return core_.delay_changes(); }
    uint32_t longest_stall_ms() const { return longest_stall_; }

    bool connection_lost() const { return lost_; }
    bool desync_detected() const { return desync_; }
    uint32_t sample() const { return (uint32_t)(sample_ + 1); }

  private:
    void pump();
    void send_packet();
    void report_mismatches();
    void abort_with(const std::string& why);

    LockstepOptions opt_;
    LockstepCore core_;
    DelayAdapter adapter_;
    hubnet::Session* net_;
    LockstepHooks hooks_;
    int64_t sample_ = -1;
    int local_ports_[kPorts] = {-1, -1, -1, -1};
    int n_local_ = 0;
    uint32_t ready_mask_ = 0;
    std::atomic<bool> lost_{false};
    bool desync_ = false;
    bool hash_warned_ = false;
    uint64_t stall_ms_ = 0, stalled_ = 0;
    uint32_t longest_stall_ = 0;
    int logged_delay_ = 0;
    uint32_t bye_mask_ = 0, bye_last_[kPorts] = {0, 0, 0, 0};
    bool peer_left_ = false;
    void adapt(uint32_t now, uint32_t stall_ms);
    bool left_blocks(uint32_t n) const;
};

/* Auto input delay negotiation (call after Session::ready(), before creating Lockstep).
 * The host measures RTT to every client for measure_ms, computes D (auto_delay_from_rtt) and sends it to
 * everybody; clients wait for it. All PCs must use auto (or all a fixed --delay and skip this).
 * Logs "input delay auto: rtt=XXms -> D". */
struct DelayNegotiation {
    int my_slot = 0;
    uint32_t slot_mask = 1;
    double frame_ms = 1000.0 / 60.0;
    uint32_t measure_ms = 1500;
    uint32_t timeout_ms = 20000;
    int fixed_delay = -1;   // host: >=0 skips measuring and broadcasts this D. Clients ignore it and always adopt the host's D.
};
bool negotiate_delay(hubnet::Session& session, const DelayNegotiation& dn, const std::function<void()>& idle,
                     const std::function<void(const std::string&)>& log, int* out_delay);

/* 64-bit hash of a memory block (word-wise multiply/xorshift). */
uint64_t hash_block(const void* data, size_t bytes);

}  // namespace n64run
