#pragma once
/* GGPO-style rollback netplay engine for retrorun (GB / GBC / GBA, also the dual-console link cores).
 *
 * Independent of SDL, libretro and hubnet: the emulator is a `Sim` (save / load / step), the network a `Transport`.
 * That is what lets test_rollback.cpp run two engines over a simulated lossy/laggy link on a virtual clock.
 *
 * Model
 *  - Frame f is one retro_run(). The pad polled at frame cur is scheduled for frame cur + D (D = input delay, 0..2).
 *  - Remote inputs not yet known are predicted by repeating the last confirmed value of that port.
 *  - The state before every frame is saved into a ring (max_pred + 4 slots). When a real input arrives that differs from the
 *    value a frame was simulated with, the engine loads the state before that frame and re-simulates up to the present with
 *    step(visible=false); only the newest frame is ever presented.
 *  - The simulation never runs more than max_pred frames past the last frame whose inputs are all confirmed (it stalls).
 *  - Input packets are cumulative: each carries, per local port, every frame the slowest peer has not acknowledged yet
 *    (up to 100) plus the sender's ack of the peers' ports, so loss only costs latency.
 *  - Desync checks run on confirmed frames only: every hash_interval frames, once all inputs < H are confirmed, a hash of the
 *    state before frame H (or, for cores whose serialized state is not byte-exact, a rolling hash of the video+audio output
 *    of frames < H) is exchanged and compared.
 *  - Time sync: every packet carries the sender's frame advantage; the peer that is ahead waits an occasional frame so the
 *    slower PC never has to rollback ever deeper.
 */

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <map>
#include <vector>

#include "drop_agree.h"
#include "state_xfer.h"

namespace rb {

constexpr int kPorts = 4;
constexpr int kMaxPred = 16;
constexpr uint8_t kPktInput = 0x51;
constexpr uint8_t kPktReady = 0x52;
constexpr uint8_t kPktBye = 0x53;
constexpr uint8_t kPktDrop = 0x54;   // netdrop::Msg (drop_agree.h)
/* Drop-in (a friend joins the running game into an empty port, see Engine::host_admit):
 *   Admit    host -> everybody  [0x56, 0, joiner, port, epoch, from u32, owners[4] i8 (after the admit), left_mask, done_mask, name_len, name...]
 *            from port `port` is played by slot `joiner` from frame `from` on; before it the port reads neutral. Repeated until acked.
 *   AdmitAck peer -> host       [0x57, my_slot, joiner]
 * Peers that do not advertise kCapDropIn (the caps byte after the slot in kPktReady) make the host refuse the join. */
constexpr uint8_t kPktAdmit = 0x56;
constexpr uint8_t kPktAdmitAck = 0x57;
constexpr uint8_t kCapDropIn = 1;
constexpr int kAdmitLeadFrames = 30;      // the admit frame is this many frames (plus the RTT) ahead of the host
constexpr int kGhostLeadFrames = 30;      // a joiner that is still catching up claims neutral input this far ahead of the players

uint64_t hash_bytes(const void* data, size_t bytes);
/* The rolling hash of the per-frame output checksums (Engine::confirmed_out_hash): start with kRollBasis, fold each frame with roll_step. */
constexpr uint64_t kRollBasis = 0xCBF29CE484222325ull;
inline uint64_t roll_step(uint64_t h, uint64_t frame_sum) { return (h ^ frame_sum) * 0x100000001B3ull; }

struct Sim {
    std::function<size_t()> state_size;
    std::function<bool(uint8_t*, size_t)> save;
    std::function<bool(const uint8_t*, size_t)> load;
    /* Run one frame with these pads. visible=false: re-simulation (suppress audio and presenting). */
    std::function<void(const uint64_t pads[kPorts], bool visible)> step;   // pads: one input value per port (Config::val_bytes wide)
    std::function<uint64_t()> frame_sum;           // checksum of the output (video+audio) of the frame just stepped
    std::function<uint64_t(int port)> read_local;  // current input of a locally owned port (low Config::val_bytes*8 bits are used)
    std::function<void()> poll_local;              // poll devices once per run_frame (before read_local)
    std::function<void()> idle;                    // while blocked (pump window events)
    std::function<uint64_t()> mem_hash;            // optional: hash of live emulator memory (synctest extra check)
    /* optional, desync recovery: deflate / inflate a savestate (without them the raw state is sent) */
    std::function<bool(const uint8_t*, size_t, std::vector<uint8_t>*)> pack_state;
    std::function<bool(const uint8_t*, size_t, uint8_t* out, size_t raw)> unpack_state;
};

struct Transport {
    std::function<void()> pump;
    std::function<void(int to, const void*, int)> send;   // to = -1: everybody else
    std::function<bool(int* from, std::vector<uint8_t>* out)> recv;
    std::function<uint32_t()> now_ms;
    std::function<void(const std::string&)> log;
    std::function<void(const std::string&)> notice;   // optional: short user-facing note ("Resynced")
    std::function<void(int slot, int port, uint32_t from, const std::string& name)> admitted;   // optional: a drop-in was applied (the runner updates its port map)
    std::function<bool()> relayed;   // optional: some peer is reached through the server relay (which caps a room at 400 frames/s): send every other frame only
};

struct Config {
    int owner[kPorts] = {0, -1, -1, -1};
    int my_slot = 0;
    uint32_t slot_mask = 1;
    int delay = 1;                 // initial input delay (frames); adaptive_delay moves it within 0..delay_max
    int max_pred = 8;              // 1..kMaxPred
    bool adaptive_delay = false;   // choose the delay from the measured RTT (see Engine::adapt_delay)
    int delay_max = 4;
    int pred_budget = 6;           // frames of one-way latency the prediction window absorbs before any input delay is added
    double frame_ms = 16.7;
    int hash_interval = 120;
    bool use_state_hash = true;
    uint32_t timeout_ms = 10000;
    uint32_t drop_silence_ms = 8000;   // a guest silent this long (while others are heard; longer than any relay stall, shorter than timeout_ms) is dropped when 3+ PCs play: its port reads neutral from an agreed frame on (0 = off)
    bool relay_half_rate = false;  // with Transport::relayed(): send the per-frame packet every other frame only (PlayStation: 4-PC rooms)
    uint32_t send_gap_ms = 8;      // min ms between the non-forced packets (the per-frame input packet is always sent); PlayStation 30: a relayed 4-PC room is capped at 400 frames/s
    uint64_t event_mask = 0;       // one-shot event bits of the input value (PlayStation disc swap): never copied into predicted or held frames
    int max_resyncs = 0;           // desync recovery: the host's state replaces everybody's, up to this many times per session (0 = a desync ends the game)
    uint32_t resync_max_bytes = 3000000;   // compressed state bigger than this is not sent (the desync ends the game as before)
    uint32_t resync_timeout_ms = 20000;
    uint8_t caps = kCapDropIn;     // advertised in kPktReady
    bool joiner = false;           // drop-in joiner: the engine only listens and claims neutral input until join_finish()
    int val_bytes = 2;             // bytes of one port's input on the wire: 2 (16 buttons: GB/GBA) 6 (PlayStation: buttons + 4 stick bytes) or 8 (+ 16 event bits)
};

struct Stats {
    uint64_t frames = 0;          // frames presented (distinct simulated frames)
    uint64_t rollbacks = 0;
    uint64_t resim_frames = 0;
    int max_depth = 0;
    uint64_t depth_sum = 0;
    uint64_t stalls = 0;          // prepare() calls that could not advance (prediction limit)
    uint64_t stalled_ms = 0;
    uint64_t sync_waits = 0;
    uint64_t delay_changes = 0;
    int delay_min = 99, delay_max = 0;   // range of the input delay used during the session
    double rtt_ms = 0;                   // smoothed RTT of the worst peer (0 = unmeasured)
    uint64_t hashes_compared = 0;
    double max_skew = 0;
    uint64_t resyncs = 0;
    uint64_t save_n = 0, load_n = 0;
    double save_us = 0, load_us = 0, step_us = 0;
    uint64_t step_n = 0;
};

class Engine {
  public:
    enum class Status { Ready, Stalled, SyncWait };

    Engine(const Config& cfg, const Sim& sim, const Transport& net);

    /* Exchange "ready" packets until every slot has been seen. */
    bool start_barrier(uint32_t timeout_ms);

    /* Pump the network, apply received inputs (rollback + re-simulation if needed), advance the confirmed frontier. */
    void service();
    /* service() + decide whether the next frame may run. */
    Status prepare();
    /* Poll the local pad, send inputs, save the state and simulate frame cur() visibly. Only after prepare() == Ready. */
    void run_frame();

    /* End of session: wait until every input < cur() is confirmed (and the peers have ours), tell the peers bye. */
    bool finish(uint32_t timeout_ms);

    uint32_t cur() const { return cur_; }
    bool desync() const { return desync_; }
    int resyncs() const { return epoch_; }               // completed desync recoveries
    bool resyncing() const { return rs_ != Rs::None; }   // game paused while a state is transferred
    /* Reusable state transfer (desync recovery now, drop-in later): the host snapshots the confirmed state, streams it, and everybody
     * loads it at the same frame. Hooks for a joiner: see resync_begin_host(). */
    bool connection_lost() const { return lost_; }
    uint64_t confirmed_out_hash() const { return roll_out_; }
    uint64_t confirmed_in_hash() const { return roll_in_; }
    const Stats& stats() const { return st_; }
    int delay() const { return cfg_.delay; }
    double rtt_ms() const { return st_.rtt_ms; }
    uint32_t dropped_mask() const { return done_mask_; }   // guests dropped mid-match (crashed); the game plays on without them
    bool peer_left() const { return left_mask_ != 0; }
    /* Every other player said bye: nobody is left to play with (a 3-4P game with one leaver keeps going with that port idle). */
    bool all_remote_left() const { for (int p = 0; p < kPorts; p++) if (present(p) && !local(p) && !(left_mask_ >> cfg_.owner[p] & 1u)) return false; return left_mask_ != 0; }
    /* Highest frame whose inputs are all confirmed and which has been simulated (-1 none). */
    int64_t confirmed_frame() const { return conf_eff_; }
    /* Spectating: called once per frame, in order from 0, when the frame becomes confirmed (all inputs known, never rolled back):
     * the pads it was simulated with. hash_sink gets each periodic output hash (frames < `frame`) as it is produced. */
    void set_confirmed_sink(std::function<void(uint32_t frame, const uint64_t pads[kPorts])> f) { confirmed_sink_ = std::move(f); }
    void set_hash_sink(std::function<void(uint32_t frame, uint64_t hash)> f) { hash_sink_ = std::move(f); }
    /* Spectator live join: copy the confirmed state before frame F = the first frame not confirmed by everybody (a memcpy from the state
     * ring, a core save only when F is the current frame), plus the confirmed hash chains at F. Every confirmed_sink frame < F has been
     * delivered. False while a resync is running or F fell out of the ring. Cheap enough to run between two frames. */
    bool snapshot_confirmed(std::vector<uint8_t>* raw, uint32_t* frame, uint64_t* out_hash, uint64_t* in_hash);
    int hash_interval() const { return cfg_.hash_interval; }

    /* ---- drop-in (party games). Host: after the joiner has the snapshot (spectate live-join path), host_admit() makes `port` belong to `slot`
     * from frame admit_frame() on, for every peer (kPktAdmit, repeated until acked). Frames before it read neutral, so the joiner needs no
     * history; it fast-forwards the snapshot to the live game with the confirmed-input stream and then calls join_finish(). While it does,
     * its engine (cfg.joiner) keeps claiming neutral input for its port ahead of the players, so nobody waits for it. ---- */
    bool peers_support_dropin() const;           // every other active player advertised kCapDropIn
    bool dropin_capable() const;                 // host: every other player advertised kCapDropIn and no drop-in / resync is in progress
    int free_port() const;                       // lowest port nobody owns (-1: none)
    uint32_t admit_frame() const;                // the frame a drop-in started now would begin at
    bool host_admit(int slot, int port, const std::string& name);
    void note_join_active(int slot);             // host: the joiner finished fast-forwarding and plays
    bool admit_pending() const { return !admits_.empty() || join_pending_mask_ != 0; }
    bool is_player(int slot) const { return slot >= 0 && slot < 8 && (cfg_.slot_mask >> slot & 1u); }
    uint32_t slot_mask() const { return cfg_.slot_mask; }
    int owner_of(int port) const { return port >= 0 && port < kPorts ? cfg_.owner[port] : -1; }
    /* Joiner side. */
    bool joining() const { return joining_; }
    bool join_admitted() const { return join_port_ >= 0; }   // the Admit arrived: port and start frame are known
    int join_port() const { return join_port_; }
    uint32_t join_from() const { return join_port_ >= 0 ? join_from_[join_port_] : 0; }
    uint32_t live_estimate() const;              // the players' current frame, as far as their packets tell
    /* The joiner has loaded the snapshot and replayed it up to `frame` (out/in = the confirmed hash chains there, pads = the confirmed pads of frame-1). */
    void join_finish(uint32_t frame, uint64_t out_hash, uint64_t in_hash, const uint64_t last_pads[kPorts]);

  private:
    static constexpr int kInRing = 512;
    static constexpr int kFrRing = 64;
    struct InEnt { int64_t tag = -1; uint64_t val = 0; };

    bool present(int p) const { return cfg_.owner[p] >= 0; }
    bool local(int p) const { return present(p) && cfg_.owner[p] == cfg_.my_slot; }
    bool has(int p, int64_t f) const { return f >= 0 && in_[p][f % kInRing].tag == f; }
    uint64_t get(int p, int64_t f) const { return in_[p][f % kInRing].val; }
    uint64_t vmask() const { return cfg_.val_bytes >= 8 ? ~0ull : ((1ull << (8 * cfg_.val_bytes)) - 1); }
    void put_local(int p, int64_t f, uint64_t v);
    void put_remote(int p, int64_t f, uint64_t v);
    void build_pads(int64_t f, uint64_t pads[kPorts]);
    void sim_frame(int64_t f, bool visible);
    void do_rollback();
    void pump_net();
    void on_packet(int from, const std::vector<uint8_t>& m);
    void send_packet(bool force);
    void adapt_delay();
    void note_rtt(int slot, uint32_t echo, uint32_t hold);
    void advance_confirmed();
    void resolve_hashes();
    void compare_hash(int slot, uint32_t frame, uint64_t h);
    uint8_t* ring_state(int64_t f) { return ring_.data() + (size_t)(f % ring_n_) * state_n_; }
    bool active_peer(int slot) const { return slot != cfg_.my_slot && (cfg_.slot_mask >> slot & 1u) && !(left_mask_ >> slot & 1u) && !(ignore_mask_ >> slot & 1u); }
    /* Does port p still hold back the confirmed frontier? (not for a left/dropped owner once every frame below the cutoff is known) */
    bool gating(int p) const {
        if (!present(p) || local(p)) return false;
        int o = cfg_.owner[p];
        if (left_mask_ >> o & 1u) return false;
        if (done_mask_ >> o & 1u) return conf_upto_[p] < (int64_t)drop_cut_[o] - 1;
        return true;
    }
    void apply_admit(int joiner, int port, uint32_t from, uint8_t epoch, const int8_t owners[kPorts], uint8_t left, uint8_t done, const std::string& name);
    void admit_on_msg(int from, const uint8_t* d, size_t n);
    void admit_service(uint32_t now);
    void join_service(uint32_t now);
    struct AdmitRec { int port = 0; uint32_t from = 0, last_send = 0, started = 0, ack_mask = 0; std::string name; };
    std::map<int, AdmitRec> admits_;
    uint32_t join_pending_mask_ = 0, join_pending_at_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t peer_caps_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool joining_ = false;
    int join_port_ = -1;
    uint32_t join_from_[kPorts] = {0, 0, 0, 0};
    uint32_t ghost_cur_ = 0;
    enum class Rs { None, Wait, Recv, Loaded, Send };
    void request_resync(const char* why);
    int host_slot() const { for (int s = 0; s < 8; s++) if (cfg_.slot_mask >> s & 1u) return s; return 0; }
    void resync_begin_host();
    void resync_on_msg(int from, const uint8_t* d, size_t n);
    void resync_service(uint32_t now);
    void resync_fail(const char* why);
    void resync_apply(uint32_t frame, const std::vector<uint8_t>& raw, uint64_t ro, uint64_t ri);
    void xfer_send(const void* d, size_t n);
    bool hash_epoch_ok(uint32_t tagged) const { return (tagged >> 30) == (uint32_t)(epoch_ & 3); }
    Rs rs_ = Rs::None;
    bool rs_want_ = false, finishing_ = false;
    int epoch_ = 0;
    int64_t floor_ = 0;                  // inputs below this frame are baked into the last resync state: never roll back past it
    uint32_t rs_started_ = 0, rs_last_ = 0, rs_extra_ms_ = 0;
    uint64_t rs_ro_ = 0, rs_ri_ = 0;
    uint32_t rs_frame_ = 0;
    std::vector<uint8_t> rs_raw_;
    xfer::Sender snd_;
    xfer::Receiver rcv_;
    void drop_check(uint32_t now);
    void drop_begin(int slot);
    void drop_service(uint32_t now);
    void drop_send(const netdrop::Agreement& ag);
    void drop_on_msg(int from, const uint8_t* d, size_t n);
    std::map<int, netdrop::Agreement> drops_;
    std::function<void(uint32_t, const uint64_t*)> confirmed_sink_;
    std::function<void(uint32_t, uint64_t)> hash_sink_;
    uint32_t ignore_mask_ = 0, done_mask_ = 0, drop_cut_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t last_rx_[8] = {0, 0, 0, 0, 0, 0, 0, 0}, last_drop_send_ = 0, drop_done_at_[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    Config cfg_;
    Sim sim_;
    Transport net_;
    size_t state_n_ = 0;
    int ring_n_ = 12;
    std::vector<uint8_t> ring_, tmp_;
    InEnt in_[kPorts][kInRing];
    int64_t conf_upto_[kPorts];       // highest f with inputs 0..f all known (remote ports)
    uint64_t last_val_[kPorts];
    uint64_t used_[kFrRing][kPorts];
    uint64_t fsum_[kFrRing];
    int64_t local_newest_ = -1;
    int64_t peer_ack_[8][kPorts];
    uint32_t peer_cur_[8];
    int peer_adv_[8];
    uint32_t peer_ts_[8], peer_ts_at_[8];   // last timestamp received from each peer and when (for the RTT echo)
    double rtt_[8], rttdev_[8];
    uint32_t last_adapt_ = 0;
    int adapt_dn_ = 0;
    bool peer_seen_[8];
    uint32_t cur_ = 0;
    int64_t rb_to_ = -1;
    int64_t conf_eff_ = -1;
    int64_t roll_next_ = 0;
    uint64_t roll_out_, roll_in_;
    std::deque<uint32_t> pending_h_;
    struct HEnt { uint32_t frame; uint64_t h; };
    std::deque<HEnt> my_h_;
    std::deque<HEnt> peer_h_[8];
    uint32_t left_mask_ = 0, ready_mask_ = 0;
    uint32_t last_send_ = 0, last_progress_ = 0, stall_since_ = 0;
    bool desync_ = false, lost_ = false, trace_ = false;
    double skew_ema_ = 0;
    uint32_t skew_cur_ = 0xFFFFFFFFu;
    uint32_t last_wait_ = 0;
    Stats st_;
};

/* Offline determinism test: every frame, roll back `depth` frames and re-simulate them, comparing the output checksum (and the
 * post-frame state hash when use_state_hash) of each re-simulated frame with the first run. */
struct SyncTestResult {
    uint64_t frames = 0, checks = 0, mismatches = 0;
    double save_us = 0, load_us = 0, step_us = 0, hash_us = 0;   // average per call
    size_t state_size = 0;
    uint64_t save_n = 0, load_n = 0, step_n = 0;
    double save_us_max = 0, load_us_max = 0;
};
SyncTestResult run_synctest(const Sim& sim, int frames, int depth, bool use_state_hash,
                            const std::function<void(int frame, uint64_t pads[kPorts])>& input,
                            const std::function<void(const std::string&)>& log);

}  // namespace rb
