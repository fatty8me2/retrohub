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
#include "resync.h"
#include "dropin.h"
#include "state_xfer.h"

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
    bool full_hash = false;        // spectator of a host that predates the framebuffer-masked hash (--full-hash): plain RDRAM hash
    bool spectator = false;        // port-less spectator: replays the host's confirmed-input stream (spectate.h); never part of the slot mask
    bool serve_spectators = true;  // host (slot 0, online): record the input history so spectators can join at any time
    uint8_t reserved_ports = 0;    // drop-in room (--dropin-room): ports the game sees as plugged in that nobody owns yet (neutral until a friend drops in; dropin.h)
    bool dropin_join = false;      // a drop-in joiner: a spectator until the host admits it, then a player from the agreed sample (dropin.h)
    std::string my_name;           // the joiner's name (sent with its knock)
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
    /* Live spectator join (retrorun): spectate::kPktJoin / kPktNoLive and state-transfer datagrams (first byte 0x55, state_xfer.h) between
     * the host and a spectator are handed here instead of to the lockstep core. Empty: they are dropped (no live join). */
    std::function<void(int from, const std::vector<uint8_t>& msg)> on_spectate_msg;
    /* Online only, emulation thread. on_lead_poll(n): the lead-port poll of sample n, at the same deterministic point as the desync hash
     * (before sample n's inputs are delivered; a savestate requested now is taken after them, i.e. it is the state before n + 1).
     * after_sample(n): the host recorded sample n for its spectators. Both may be empty. Used by n64run's live spectator join. */
    std::function<void(uint32_t n)> on_lead_poll;
    std::function<void(uint32_t n)> after_sample;
    /* Desync recovery (resync.h), all optional; without resync_fetch the capability is not announced. Emulation thread.
     * resync_save: host, at poll S-1: ask the core for a savestate (async; the state before sample S). false = refused.
     * resync_fetch: host, polled while stalled at S: 0 pending, 1 done (data = gzipped state, hash over it), -1 failed.
     * resync_load: guest: verify (raw size, hash) and have the core load the state; false = bad. The frontend calls Lockstep::resync_loaded()
     * from the core's load-complete callback.  resync_done: a recovery finished (OSD note). qa_poll: QA fault injection. */
    std::function<bool(uint32_t* job)> resync_save;
    std::function<int(uint32_t job, std::vector<uint8_t>* data, uint64_t* hash)> resync_fetch;
    std::function<bool(const std::vector<uint8_t>& data, uint64_t hash)> resync_load;
    std::function<void(uint32_t S, uint32_t stall_ms)> resync_done;
    std::function<void(uint32_t n)> qa_poll;
    /* Framebuffer-masked sync hash (fbmask.h): vi_poll at every lead poll (all roles, before the hash), vi_reset when a recovery state was applied
     * (host and guests alike, so every peer's tracked set restarts at the same poll). Announcing the capability needs vi_poll. */
    std::function<void(uint32_t n)> vi_poll;
    std::function<void()> vi_reset;
    /* Drop-in (dropin.h), emulation thread: a friend took (joined) or left (!joined) a port: OSD "<name> joined". */
    std::function<void(const std::string& name, int slot, int port, bool joined)> on_dropin;
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
    spectate::Client& spectate_client_mut() { return spec_cli_; }
    /* Spectator live join: the state before sample n has been loaded; continue from there (the inputs from n are in the client). */
    void spectate_start_at(uint32_t n) { sample_ = (int64_t)n - 1; if (spec_cli_.base() != n) spec_cli_.start_at(n); }   // keep what was buffered from n while the state downloaded
    /* n64run live join: the state before sample n arrives asynchronously (the core loads it at its next interrupt). Until
     * spectate_live_loaded() every poll returns neutral and does not count as a sample (the boot frames before the load are discarded). */
    void spectate_expect_live(uint32_t n) { live_wait_ = true; live_n_ = n; }
    void spectate_live_loaded() { if (live_wait_) { sample_ = (int64_t)live_n_ - 1; live_wait_ = false; } }
    bool spectate_live_waiting() const { return live_wait_; }

    /* Desync recovery. resync_loaded: the core finished loading the recovery state (guest; load-complete callback, emulation thread). */
    bool resync_waiting() const { return rs_wait_; }
    void resync_loaded(bool ok);
    int resyncs() const { return rs_.count(); }
    bool resync_active() const { return rs_.active(); }   // host: no spectator snapshot may start (the core's save job is wanted at S - 1)
    /* ---- drop-in (dropin.h) ---- */
    /* Slots that get the spectator stream: the server's spectators plus drop-in candidates (not yet players). */
    uint32_t spec_mask() const;
    /* Host: a knock (kPktJoin version 2) from `slot`. 0 = accepted (the caller queues the snapshot), else a dropin::Refuse reason. */
    int dropin_knock(int slot, uint8_t caps, const std::string& name);
    void dropin_forget(int slot, const char* why);   // the snapshot could not be delivered / the joiner is gone
    bool dropin_enabled() const { return opt_.reserved_ports != 0; }
    /* Joiner: failed (exit code for the hub: 9 full, 10 friend must update, 11 other) / promoted to a player. */
    int dropin_fail_rc() const { return dj_fail_rc_; }
    bool dropin_playing() const { return opt_.dropin_join && !opt_.spectator; }
    uint32_t dropin_slots() const { return dj_slots_; }
    /* the sync hash (hash_rdram) should skip the framebuffers: every player announced hash version 2 (a spectator: unless --full-hash) */
    bool masked_hash() const { return masked_; }

  private:
    void pump();
    /* recovery */
    resync::Ctl rs_;
    bool masked_ = false;
    uint8_t caps_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::atomic<bool> rs_wait_{false};
    rb::xfer::Sender rs_snd_;
    rb::xfer::Receiver rs_rcv_;
    uint32_t rs_job_ = 0, rs_t0_ = 0, rs_last_done_ = 0, rs_done_ep_ = 0x100, rs_last_pkt_ = 0, rs_last_hs_ = 0;
    uint32_t rs_bytes_ = 0;
    bool rs_dirty_ = false;
    uint32_t rs_hash_sample_ = 0;
    void rs_mismatch(uint32_t hash_sample);
    void rs_service(uint32_t now);
    void rs_on_ctl(int from, const std::vector<uint8_t>& msg);
    void rs_on_xfer(int from, const std::vector<uint8_t>& msg);
    void rs_send(int to, const resync::Msg& m);
    bool rs_barrier(uint32_t n);   // true: carry on with sample n; false: return neutral (guest loading)
    bool rs_host_barrier(uint32_t n);
    bool rs_guest_barrier(uint32_t n);
    void rs_fail(const char* why);
    bool rs_silent(uint32_t now) const;
    void rs_finish(uint32_t S);
    void send_packet();
    /* drop-in */
    void di_on_msg(int from, const std::vector<uint8_t>& msg);
    void di_service(uint32_t now);
    void di_begin_admit(int slot);
    void di_apply(const dropin::Msg& m);
    void di_abort(const char* why);
    void di_barrier(uint32_t n);
    void di_candidate_left(int slot, const char* why);
    void di_release(uint32_t n);
    void di_send(int to, const dropin::Msg& m);
    void dj_promote();
    void dj_fail(int rc, const char* why);
    uint32_t dj_mask_ = 0, dj_slots_ = 0, dj_since_[kPorts] = {0, 0, 0, 0};
    std::string dj_name_[kPorts];
    int dj_port_[kPorts] = {-1, -1, -1, -1};
    uint8_t dj_applied_id_[kPorts] = {0, 0, 0, 0}, dj_dead_id_[kPorts] = {0, 0, 0, 0};
    uint32_t dj_live_until_ = 0, dj_live_last_ = 0;
    bool dj_ok_ = false;
    dropin::Ctl dj_ctl_;
    uint32_t dj_abort_sends_ = 0, dj_last_abort_ = 0;
    dropin::Msg dj_abort_msg_;
    // joiner
    dropin::ReadyGate dj_gate_;
    bool dj_have_admit_ = false;
    dropin::Msg dj_adm_;
    uint32_t dj_ready_sent_ = 0, dj_t_ready_ = 0, dj_t_start_ = 0;
    int dj_fail_rc_ = 0;
    // stall measurement around a join (all players)
    uint32_t dj_seen_[kPorts] = {0, 0, 0, 0}, dj_real_[kPorts] = {0, 0, 0, 0};   // per drop-in port: samples since its first sample / of those with input
    bool dj_win_ = false;
    uint32_t dj_win_end_ = 0, dj_osd_f_ = 0, dj_osd_slot_ = 0, dj_barrier_ms_ = 0;
    uint64_t dj_win_stall0_ = 0;
    uint32_t dj_win_longest_ = 0;
    std::string dj_osd_name_;
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
    std::atomic<bool> live_wait_{false};
    uint32_t live_n_ = 0;
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
