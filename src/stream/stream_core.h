#pragma once
/* Pure (no Windows, no sockets) building blocks of remote play: packetizer + XOR FEC, frame reassembler with NACK
 * list, audio jitter buffer, NACK history, input codec, adaptive bitrate controller. Unit tested by stream_core_test. */
#include "stream_proto.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <deque>
#include <map>
#include <vector>

namespace rplay {

using Bytes = std::vector<uint8_t>;

inline void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
inline uint32_t get16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline uint32_t get32(const uint8_t* p) { return get16(p) | (get16(p + 2) << 16); }

/* ---- frame blob ---- */
struct FrameInfo { uint32_t cap_us = 0; uint16_t w = 0, h = 0; uint8_t flags = 0; uint8_t codec = kCodecH264; uint32_t seq = 0; };
Bytes make_frame_blob(const FrameInfo& fi, const uint8_t* au, size_t len);
bool parse_frame_blob(const Bytes& blob, FrameInfo* fi, const uint8_t** au, size_t* len);

/* ---- packetizer: blob -> datagrams (data fragments interleaved with parity). fec_group 0 = no FEC. ---- */
std::vector<Bytes> packetize_frame(uint32_t frame_id, bool key, const Bytes& blob, int fec_group);

/* ---- reassembler (guest) ---- */
struct MissingFrag { uint32_t frame_id; uint16_t idx; };
class Reassembler {
  public:
    /* Feed one 'V' datagram. Completed frames (in frame_id order, older incomplete frames are skipped once a newer one
     * completes) are appended to ready(). */
    void feed(const uint8_t* pkt, int len, uint32_t now_ms);
    bool pop(uint32_t* frame_id, Bytes* blob, bool* key);
    /* Fragments to NACK: frames older than `age_ms` that still lack data (max n). Each is returned at most once per
     * `retry_ms`. */
    std::vector<MissingFrag> nacks(uint32_t now_ms, uint32_t age_ms, uint32_t retry_ms, size_t max_n);
    /* Drop frames pending longer than `giveup_ms`; returns true if any frame was dropped (=> ask for a keyframe). */
    bool expire(uint32_t now_ms, uint32_t giveup_ms);
    uint64_t recovered_by_fec = 0, frames_done = 0, frames_dropped = 0, packets = 0, dup_packets = 0;
    bool need_key = false;   // set when a frame was lost for good; the app asks the host for a keyframe, then clears it
    uint32_t last_done_id() const { return last_done_; }
  private:
    struct Parity { bool have = false; int g = 0; Bytes data; };
    struct Pending {
        uint16_t count = 0; bool key = false; uint32_t first_ms = 0, last_ms = 0;
        std::vector<Bytes> frag; std::vector<uint8_t> have; uint16_t got = 0;
        std::map<uint16_t, Parity> parity;
        std::map<uint16_t, uint32_t> last_nack;
    };
    bool try_fec(Pending& p, uint16_t group);
    void flush(uint32_t now, bool force_gap);
    std::map<uint32_t, Pending> pend_;
    std::deque<std::pair<uint32_t, std::pair<bool, Bytes>>> ready_;
    uint32_t last_done_ = 0; bool any_done_ = false;
};

/* ---- host side history so NACKs can be answered ---- */
class NackHistory {
  public:
    explicit NackHistory(size_t max_frames = 90) : max_(max_frames) {}
    void store(uint32_t frame_id, std::vector<Bytes> pkts);
    const Bytes* find(uint32_t frame_id, uint16_t idx) const;   // idx = data fragment index
  private:
    size_t max_;
    std::deque<std::pair<uint32_t, std::vector<Bytes>>> q_;
};
Bytes make_nack(const std::vector<MissingFrag>& m);
std::vector<MissingFrag> parse_nack(const uint8_t* pkt, int len);

/* ---- audio ---- */
Bytes make_audio_pkt(uint8_t codec, uint32_t seq, uint32_t ts_ms, const uint8_t* data, size_t len);
struct AudioPkt { uint8_t codec; uint32_t seq, ts_ms; Bytes data; };
bool parse_audio_pkt(const uint8_t* pkt, int len, AudioPkt* out);

/* Jitter buffer: reorders by seq, waits `target` packets, reports lost seqs as empty packets (conceal = silence). */
class JitterBuffer {
  public:
    explicit JitterBuffer(int target_pkts = 3, int max_pkts = 16) : target_(target_pkts), max_(max_pkts) {}
    void push(const AudioPkt& p);
    /* Pop the next packet in order. Returns false while buffering/starved. `lost` = true => packet was missing and
     * `out` is empty (generate concealment). */
    bool pop(AudioPkt* out, bool* lost);
    size_t depth() const { return q_.size(); }
    uint64_t late = 0, lost_total = 0, dups = 0, overflow = 0;
    void set_target(int t) { target_ = t < 1 ? 1 : t; }
  private:
    std::map<uint32_t, AudioPkt> q_;
    uint32_t next_ = 0; bool started_ = false, primed_ = false, popped_ = false; int target_, max_;
};

/* ---- input ---- */
Bytes make_input_pkt(uint16_t seq, const PadState& pad);
bool parse_input_pkt(const uint8_t* pkt, int len, uint16_t* seq, PadState* pad);

/* ---- adaptive bitrate (host). Called once per stats report (~1 Hz). ---- */
struct StatsReport { uint16_t loss_permille = 0, recv_kbps = 0, jitter_ms = 0; uint8_t fps = 0; uint16_t rtt_ms = 0; };
Bytes make_stats_pkt(const StatsReport& s);
bool parse_stats_pkt(const uint8_t* pkt, int len, StatsReport* s);
class BitrateController {
  public:
    BitrateController(int start_kbps = 8000, int min_kbps = 3000, int max_kbps = 15000)
        : kbps_(start_kbps), min_(min_kbps), max_(max_kbps) {}
    int update(const StatsReport& s);   // returns the new target kbps
    int kbps() const { return kbps_; }
    int fec_group() const { return loss_ > 10 ? 4 : 8; }
  private:
    int kbps_, min_, max_; int clean_ = 0; int loss_ = 0; int last_jitter_ = 0;
};


/* ---- guest in-stream menu (View + Y on a pad, F1 on the keyboard). Pure so a test can drive it. ---- */
enum class MenuEv { Open, Up, Down, Ok, Back };
enum class MenuAct { None, Opened, Closed, ToggleStats, ToggleFullscreen, Leave };
struct GuestMenu {
    static constexpr int kItems = 4;   // 0 Resume, 1 Stats, 2 Fullscreen, 3 Leave
    static const char* item_name(int i) { static const char* n[kItems] = {"RESUME", "STATS", "FULLSCREEN", "LEAVE"}; return n[i < 0 || i >= kItems ? 0 : i]; }
    bool open = false; int sel = 0;
    MenuAct handle(MenuEv e) {
        if (!open) { if (e == MenuEv::Open) { open = true; sel = 0; return MenuAct::Opened; } return MenuAct::None; }
        switch (e) {
            case MenuEv::Open: case MenuEv::Back: open = false; return MenuAct::Closed;
            case MenuEv::Up: sel = (sel + kItems - 1) % kItems; return MenuAct::None;
            case MenuEv::Down: sel = (sel + 1) % kItems; return MenuAct::None;
            case MenuEv::Ok:
                if (sel == 1) return MenuAct::ToggleStats;
                if (sel == 2) return MenuAct::ToggleFullscreen;
                if (sel == 3) { open = false; return MenuAct::Leave; }
                open = false; return MenuAct::Closed;
        }
        return MenuAct::None;
    }
};
/* Raw pad bits the menu cares about; edge detection turns held buttons into MenuEv. */
struct MenuPadEdge {
    bool chord = false, up = false, down = false, ok = false, back = false;
    bool poll(bool back_btn, bool y_btn, bool up_btn, bool down_btn, bool ok_btn, bool cancel_btn, MenuEv* ev) {
        bool c = back_btn && y_btn;
        bool fire = false;
        if (c && !chord) { *ev = MenuEv::Open; fire = true; }
        else if (up_btn && !up) { *ev = MenuEv::Up; fire = true; }
        else if (down_btn && !down) { *ev = MenuEv::Down; fire = true; }
        else if (ok_btn && !ok) { *ev = MenuEv::Ok; fire = true; }
        else if (cancel_btn && !back) { *ev = MenuEv::Back; fire = true; }
        chord = c; up = up_btn; down = down_btn; ok = ok_btn; back = cancel_btn;
        return fire;
    }
};

/* Host status line for the hub: "streaming to Mickey - 12 Mbps - 48 ms". Pure formatting (the hub knows the names). */
inline std::string status_line(const std::vector<std::string>& names, int kbps, int rtt_ms) {
    if (names.empty()) return "";
    std::string who;
    for (size_t i = 0; i < names.size(); i++) who += (i ? (i + 1 == names.size() ? " and " : ", ") : "") + names[i];
    char b[64]; snprintf(b, sizeof b, "%.0f Mbps", kbps / 1000.0);
    return "streaming to " + who + " · " + b + (rtt_ms >= 0 ? " · " + std::to_string(rtt_ms) + " ms" : "");
}

} // namespace rplay
