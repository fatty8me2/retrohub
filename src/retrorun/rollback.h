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
#include <vector>

namespace rb {

constexpr int kPorts = 4;
constexpr int kMaxPred = 16;
constexpr uint8_t kPktInput = 0x51;
constexpr uint8_t kPktReady = 0x52;
constexpr uint8_t kPktBye = 0x53;

uint64_t hash_bytes(const void* data, size_t bytes);

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
};

struct Transport {
    std::function<void()> pump;
    std::function<void(int to, const void*, int)> send;   // to = -1: everybody else
    std::function<bool(int* from, std::vector<uint8_t>* out)> recv;
    std::function<uint32_t()> now_ms;
    std::function<void(const std::string&)> log;
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
    bool relay_half_rate = false;  // with Transport::relayed(): send the per-frame packet every other frame only (PlayStation: 4-PC rooms)
    uint32_t send_gap_ms = 8;      // min ms between the non-forced packets (the per-frame input packet is always sent); PlayStation 30: a relayed 4-PC room is capped at 400 frames/s
    uint64_t event_mask = 0;       // one-shot event bits of the input value (PlayStation disc swap): never copied into predicted or held frames
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
    bool connection_lost() const { return lost_; }
    uint64_t confirmed_out_hash() const { return roll_out_; }
    uint64_t confirmed_in_hash() const { return roll_in_; }
    const Stats& stats() const { return st_; }
    int delay() const { return cfg_.delay; }
    double rtt_ms() const { return st_.rtt_ms; }
    bool peer_left() const { return left_mask_ != 0; }
    /* Every other player said bye: nobody is left to play with (a 3-4P game with one leaver keeps going with that port idle). */
    bool all_remote_left() const { for (int p = 0; p < kPorts; p++) if (present(p) && !local(p) && !(left_mask_ >> cfg_.owner[p] & 1u)) return false; return left_mask_ != 0; }
    /* Highest frame whose inputs are all confirmed and which has been simulated (-1 none). */
    int64_t confirmed_frame() const { return conf_eff_; }

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
    bool active_peer(int slot) const { return slot != cfg_.my_slot && (cfg_.slot_mask >> slot & 1u) && !(left_mask_ >> slot & 1u); }

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
