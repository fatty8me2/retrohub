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
#include <map>
#include <memory>
#include <string>

#include "lockstep_core.h"
#include "spectate.h"

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
    bool wide = false;             // second u32 per sample and port (hooks.read_local_aux; PlayStation analog sticks)
    bool adaptive_delay = true;    // raise/lower the input delay from observed stalls (host orders, all follow)
    int hash_interval = 600;       // samples between RDRAM hashes
    uint32_t drop_silence_ms = 3000;   // a guest silent this long while we are blocked on it is dropped (3+ PCs): its port goes neutral, the rest play on
    uint32_t timeout_ms = 10000;   // no progress -> "Connection lost"
    bool spectator = false;        // port-less spectator: replays the host's confirmed-input stream (spectate.h); never part of the slot mask
    bool serve_spectators = true;  // host (slot 0, online): record the input history so spectators can join at any time
    std::string hash_label = "state";   // desync log text: "<label> hashes"
};

struct LockstepHooks {
    std::function<uint32_t(int owned_index)> read_local;      // packed BUTTONS for the owned_index-th local port
    std::function<uint32_t(int owned_index)> read_local_aux;  // wide only: the second word for the owned_index-th local port
    std::function<void()> update_local;                       // poll devices once per sample
    std::function<void()> idle;                               // called while blocked (pump SDL events)
    std::function<bool(uint64_t* out)> hash_rdram;            // false -> hashing unavailable
    std::function<void(bool)> set_limiter;                    // speed limiter on/off (catch-up after a stall); may be empty
    std::function<void(int slot, uint32_t cutoff)> on_drop;   // a guest dropped mid-match; its port is neutral from sample `cutoff` on (may be empty)
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
    /* Wide only: second word of `port` for the sample the last lead-port get_keys() returned (call after the whole poll round). */
    uint32_t aux(int port) const;

    bool present(int port) const { return core_.port_present(port); }
    bool local(int port) const { return core_.port_local(port); }

    /* Stall statistics (time spent blocked waiting for peers' inputs). */
    uint64_t stall_ms() const { return stall_ms_; }
    uint64_t stalled_samples() const { return stalled_; }
    uint64_t samples_done() const { return (uint64_t)(sample_ + 1); }
    void log_stall_summary() const;

    /* Call once after emulation ended (online): tells the others "bye" so they end cleanly instead of timing out. */
    void finish();
    /* Host with spectators: send them the tail of the history and the end marker (waits up to ~4 s for their acks). finish() calls it; runners that
     * end without finish() call it directly. Safe to call twice. */
    void finish_spectators();
    bool peer_left() const { return peer_left_; }          // a partner ended the session (clean)
    int current_delay() const { return core_.delay(); }
    uint32_t delay_changes() const { return core_.delay_changes(); }
    uint32_t longest_stall_ms() const { return longest_stall_; }

    uint32_t dropped_mask() const { return dropped_mask_; }
    bool connection_lost() const { return lost_; }
    bool desync_detected() const { return desync_; }
    uint32_t sample() const { return (uint32_t)(sample_ + 1); }
    bool spectating() const { return opt_.spectator; }
    spectate::Host* spectate_host() { return spec_host_.get(); }
    const spectate::Client& spectate_client() const { return spec_cli_; }

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
    uint32_t t_first_ = 0;
    /* Catch-up: a stall/hiccup leaves the emulation behind the 60 Hz wall clock (the core's limiter never makes the time up),
     * so the game would drift into slow motion by the sum of all hiccups. debt_ms_ = time lost, repaid by running unthrottled. */
    double base_ms_ = 16.7, debt_ms_ = 0;
    uint32_t last_poll_ = 0, catch_start_ = 0;
    bool catching_ = false;
    void catch_up(uint32_t now);
    uint32_t bye_mask_ = 0, bye_last_[kPorts] = {0, 0, 0, 0};
    bool peer_left_ = false;
    void adapt(uint32_t now, uint32_t stall_ms);
    /* spectating */
    uint32_t spec_get_keys(int port);
    void spec_service();
    void spec_report();
    std::unique_ptr<spectate::Host> spec_host_;   // host: confirmed-input history + stream to spectators
    spectate::Client spec_cli_;                    // spectator: the stream
    spectate::Frame spec_cur_;
    bool spec_live_ = true, spec_finished_ = false;
    uint32_t spec_ended_at_ = 0;
    bool left_blocks(uint32_t n) const;
    void drop_check(uint32_t now, uint32_t n);
    void drop_begin(int slot, uint32_t now);
    void drop_service(uint32_t now);
    void drop_send(const netdrop::Agreement& ag);
    void drop_on_msg(int from, const std::vector<uint8_t>& msg, uint32_t now);
    std::map<int, netdrop::Agreement> drops_;
    std::map<int, uint32_t> drop_done_at_;
    uint32_t last_rx_[kPorts] = {0, 0, 0, 0}, last_drop_send_ = 0, dropped_mask_ = 0;
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

/* Netplay handshake: every PC announces the hash of its effective cheat list (cheats::set_hash; an empty list is
 * cheats::kEmptyHash) and checks it equals everybody else's. Run once after the session is ready, before the emulator
 * starts. Ok = all peers agree; Mismatch = somebody differs (cheats would desync: abort the session); Timeout = not
 * every peer answered in time (logged; the RDRAM/state desync check still covers it). */
enum class CheatSync { Ok, Mismatch, Timeout, Failed };
CheatSync exchange_cheat_hash(hubnet::Session& session, int my_slot, uint32_t slot_mask, uint64_t hash, uint32_t timeout_ms,
                              const std::function<void()>& idle, const std::function<void(const std::string&)>& log);

/* 64-bit hash of a memory block (word-wise multiply/xorshift). */
uint64_t hash_block(const void* data, size_t bytes);

}  // namespace n64run
