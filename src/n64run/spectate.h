#pragma once
/* Spectating: a port-less watcher that follows a running session from a stream of CONFIRMED inputs.
 *
 * Why not a normal lockstep slot: a slot gates the players (a laggy watcher would stall the match), can only join at the
 * start, and eats one of the four slots' redundancy windows. Instead the spectator is not part of the slot mask at all:
 *  - the host (slot 0) records the input of every sample/frame the emulation has consumed (lockstep: sample n once complete;
 *    rollback: frame f once all inputs for f are confirmed, i.e. it can never be rolled back);
 *  - it streams that history to each spectator over the Session (host -> spectator only, spectator -> host only), starting at
 *    sample 0, so a spectator may join at any time: it boots the same ROM, replays the history unthrottled until it has caught up
 *    and then follows live. The emulators are deterministic, so replaying the confirmed inputs reproduces the players' game;
 *  - delivery is a tiny reliable stream over unreliable datagrams: cumulative acks, a bounded window above the spectator's
 *    consumed position, resend from the acked position when acks stall;
 *  - desync check: the host sends its periodic state hashes, the spectator compares with its own and reports them back, so
 *    "the spectator sees the same game" is verified by both ends.
 *
 * Pure logic (no emulator, no sockets): unit tested in test_spectate.cpp. Used by lockstep.cpp (N64 and retrorun lockstep) and by
 * the rollback engine's glue in retrorun.
 */

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace spectate {

constexpr uint8_t kPktData = 0x70;   // host -> spectator
constexpr uint8_t kPktAck = 0x71;    // spectator -> host
/* Live join (rollback hosts, retrorun): the spectator asks for a snapshot instead of the history from frame 0.
 *   Join   (spectator -> host)  [0x72, slot, version]   repeated every kKnockMs until the host answers
 *   NoLive (host -> spectator)  [0x73, 0, reason]       no snapshot: the stream starts at frame 0 as before (reason: JoinRefuse)
 * The snapshot itself travels as an rb::xfer transfer (retrorun/state_xfer.h, host slot 0 -> that spectator only) of the confirmed
 * state before frame F; the host streams the confirmed inputs from F (Host::start_peer_at) and the spectator buffers them from F
 * (Client::start_at) while the state downloads, loads it, then runs the buffered frames unpaced (no video/audio) until it is
 * kLiveLagSamples behind, and follows in real time from there. */
constexpr uint8_t kPktJoin = 0x72;
constexpr uint8_t kPktNoLive = 0x73;
enum JoinRefuse : uint8_t { kRefuseEarly = 1, kRefuseUnsupported = 2, kRefuseTooBig = 3 };
constexpr uint32_t kMinLiveFrames = 300;   // a match younger than this (5 s) is cheaper to replay from frame 0
constexpr uint32_t kKnockMs = 200;
constexpr uint32_t kHoldMs = 3000;         // live-capable host: wait this long for a Join/Ack before streaming from 0 anyway
constexpr int kMaxSnapshotsInFlight = 2;
constexpr int kMaxSpectators = 2;    // the server enforces the same cap per room
constexpr int kPorts = 4;
constexpr uint32_t kWindow = 1024;   // samples the host keeps in flight above the spectator's consumed position
constexpr uint32_t kMaxHistory = 4000000;   // ~18 h at 60 Hz; past this the host stops recording and spectating ends
constexpr uint32_t kLiveLagSamples = 12;    // a spectator this far behind the host runs unthrottled until it is within it

using SendFn = std::function<void(int slot, const uint8_t* data, int len)>;
using LogFn = std::function<void(const std::string&)>;

/* One consumed sample / confirmed frame: the input of every port (unused ports are 0). Lockstep "wide" packs the aux word into
 * the high 32 bits; rollback uses up to 8 bytes per port. */
struct Frame { uint64_t v[kPorts] = {0, 0, 0, 0}; };

class Host {
  public:
    /* port_mask: bit p = port p is present; vbytes: bytes per port value on the wire (4 or 8). */
    Host(uint8_t port_mask, int vbytes);

    /* Next sample/frame (consecutive, starting at 0). Ignored once kMaxHistory is reached. */
    void push(const uint64_t vals[kPorts]);
    uint32_t total() const { return (uint32_t)hist_.size(); }
    /* The host's own state hash at `sample` (kept: the last 64), sent to spectators and compared with theirs. */
    void add_hash(uint32_t sample, uint64_t hash);
    /* The session is over: tell the spectators the final length. */
    void finish() { fin_ = true; }
    bool finished() const { return fin_; }

    /* Drive the stream. spec_mask = bit s set for each spectator slot currently in the room. */
    void service(uint32_t now_ms, uint32_t spec_mask, const SendFn& send);
    /* A spectator's ack. Returns false if malformed. */
    bool on_ack(int from_slot, const uint8_t* d, size_t n, uint32_t now_ms);
    /* Every spectator in spec_mask has acknowledged the whole history (finish() waits for this, with a cap). */
    bool all_acked(uint32_t spec_mask) const;

    /* Spectator-reported hash comparisons. */
    uint32_t hashes_ok() const { return hash_ok_; }
    uint32_t hashes_bad() const { return hash_bad_; }
    void set_log(LogFn f) { log_ = std::move(f); }
    /* Position of a spectator (next sample it needs), -1 if unknown. */
    int64_t acked_of(int slot) const;
    uint32_t packets_sent() const { return sent_pkts_; }

    /* Live join. hold_ms > 0: a new spectator gets nothing until it acks (old client: stream from 0), the caller starts it at a
     * snapshot frame (start_peer_at) or releases it (release: stream from 0), or hold_ms passes. 0 (default) = stream at once. */
    void set_hold_ms(uint32_t ms) { hold_ms_ = ms; }
    /* Stream to `slot` from `frame` (<= total()): it loads a snapshot of the state before `frame`. */
    bool start_peer_at(int slot, uint32_t frame, uint32_t now_ms);
    void release(int slot);
    bool held(int slot) const;

  private:
    struct Peer { uint32_t acked = 0, consumed = 0, sent = 0, last_send = 0, last_progress = 0, last_hb = 0, hold_since = 0; bool fresh = true, hold = true; };
    uint32_t hold_ms_ = 0;
    size_t rec_bytes() const;
    void send_chunk(int slot, Peer& p, uint32_t now, const SendFn& send);

    uint8_t port_mask_;
    int nports_, vbytes_;
    std::vector<Frame> hist_;
    std::map<uint32_t, uint64_t> hashes_;
    std::map<int, Peer> peers_;
    bool fin_ = false;
    uint32_t hash_ok_ = 0, hash_bad_ = 0, sent_pkts_ = 0;
    LogFn log_;
};

class Client {
  public:
    Client() = default;

    /* A kPktData datagram from the host. */
    bool on_data(const uint8_t* d, size_t n, uint32_t now_ms);
    /* Sample n has been received and not yet released. */
    bool ready(uint32_t n) const { return n >= base_ && n < base_ + store_.size(); }
    uint64_t get(uint32_t n, int port) const;
    /* The emulation consumed every sample < n: free them and tell the host. */
    void consumed(uint32_t n);
    /* Nothing at/after n will ever arrive (the host finished and n is past the end). */
    bool finished(uint32_t n) const { return fin_ && have_total_ && n >= total_; }
    bool have_any() const { return have_total_; }
    /* Samples the host has produced that this spectator has not consumed yet (0 until the first packet). */
    uint32_t lag() const { return total_ > consumed_ ? total_ - consumed_ : 0; }
    uint32_t total() const { return total_; }
    uint32_t last_rx() const { return last_rx_; }
    /* Live join: the spectator starts at `frame` (it loads the state before it). Drops anything buffered. */
    void start_at(uint32_t frame);
    uint32_t base() const { return base_; }

    /* The spectator's own state hash at `sample`; compared as soon as the host's hash for it is known (either order). */
    void add_local_hash(uint32_t sample, uint64_t hash);
    uint32_t hashes_ok() const { return hash_ok_; }
    uint32_t hashes_bad() const { return hash_bad_; }
    bool poll_mismatch(uint32_t* sample, uint64_t* mine, uint64_t* host);

    /* Acks (throttled) and heartbeats to the host; my_slot goes into the packet. */
    void service(uint32_t now_ms, int my_slot, const SendFn& send);

  private:
    void compare(uint32_t sample);
    std::deque<Frame> store_;
    uint32_t base_ = 0, consumed_ = 0, total_ = 0, last_rx_ = 0, last_ack_ = 0, ack_sent_next_ = 0xFFFFFFFFu;
    bool have_total_ = false, fin_ = false, dirty_ = false;
    uint8_t port_mask_ = 0;
    std::map<uint32_t, uint64_t> host_h_, my_h_;
    std::vector<std::pair<uint32_t, uint64_t>> recent_mine_;
    struct Mis { uint32_t s; uint64_t mine, host; };
    std::deque<Mis> mis_;
    uint32_t hash_ok_ = 0, hash_bad_ = 0;
};

/* Host side: what to answer a Join. */
enum class JoinDecision { Snapshot, NoLiveEarly, NoLiveUnsupported, Busy, Ignore };
JoinDecision decide_join(bool live_capable, uint32_t total, int in_flight, bool slot_in_flight, bool engine_busy);

/* Spectator side of the live join: knock, download, play. Pure (the caller passes `now`). */
class JoinClient {
  public:
    enum class Mode { Knock, Download, Play };
    explicit JoinClient(bool want_live = true) : mode_(want_live ? Mode::Knock : Mode::Play) {}
    Mode mode() const { return mode_; }
    /* A Join is due now (and is counted as sent). */
    bool want_knock(uint32_t now);
    void on_nolive(uint8_t reason) { if (mode_ == Mode::Knock) { mode_ = Mode::Play; refuse_ = reason; } }
    /* Inputs streamed from frame 0 while still knocking: an old host without live join. */
    void on_legacy_data() { if (mode_ == Mode::Knock) { mode_ = Mode::Play; refuse_ = 0xFF; } }
    void on_begin(uint32_t frame, uint32_t now) { if (mode_ == Mode::Knock) { mode_ = Mode::Download; frame_ = frame; dl_since_ = now ? now : 1; } }
    void on_loaded() { if (mode_ == Mode::Download) { mode_ = Mode::Play; live_ = true; } }
    /* The download made no progress for too long. */
    bool download_stuck(uint32_t now, uint32_t last_progress, uint32_t limit_ms = 15000) const {
        return mode_ == Mode::Download && now - std::max(dl_since_, last_progress) > limit_ms;
    }
    bool acks_allowed() const { return mode_ != Mode::Knock; }
    bool playing() const { return mode_ == Mode::Play; }
    bool live() const { return live_; }
    uint32_t frame() const { return frame_; }
    uint8_t refuse_reason() const { return refuse_; }   // 0 none, JoinRefuse, 0xFF old host
    uint32_t first_knock() const { return first_knock_; }

  private:
    Mode mode_;
    bool live_ = false;
    uint32_t frame_ = 0, last_knock_ = 0, first_knock_ = 0, dl_since_ = 0;
    uint8_t refuse_ = 0;
};

/* Catch-up pacing: unpaced while more than kLiveLagSamples behind, then real time. Remembers when the spectator first got live. */
class CatchUp {
  public:
    /* true while the spectator should run unpaced (no video, no audio) */
    bool update(uint32_t lag, uint32_t now) {
        bool behind = lag > kLiveLagSamples;
        if (!behind && !live_once_) { live_once_ = true; live_at_ = now ? now : 1; }
        catching_ = behind;
        return behind;
    }
    bool live_once() const { return live_once_; }
    uint32_t live_at() const { return live_at_; }
    bool catching() const { return catching_; }

  private:
    bool catching_ = false, live_once_ = false;
    uint32_t live_at_ = 0;
};

}  // namespace spectate
