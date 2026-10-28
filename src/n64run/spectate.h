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
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace spectate {

constexpr uint8_t kPktData = 0x70;   // host -> spectator
constexpr uint8_t kPktAck = 0x71;    // spectator -> host
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

  private:
    struct Peer { uint32_t acked = 0, consumed = 0, sent = 0, last_send = 0, last_progress = 0, last_hb = 0; bool fresh = true; };
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

}  // namespace spectate
