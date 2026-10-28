#pragma once
/* Reliable savestate transfer over an unreliable datagram channel (direct UDP or the server relay), header only.
 *
 * Used by the rollback engine for desync recovery (the host sends its state, everybody loads it at an agreed frame) and meant to be reused for
 * drop-in (a friend joining a running 3-4 player room gets the same transfer, see Engine::xfer_* hooks in rollback.h).
 *
 * Wire format (every datagram): [kPkt, sub, from_slot, epoch, ...]
 *   Req   (guest -> host)     nothing more: "I saw a desync, please send me a state"
 *   Begin (host -> all)       frame u32, raw u32, comp u32, chunks u16, flags u8, hash u64, extra a u64, extra b u64
 *   Chunk (host -> all)       idx u16, data (<= kChunk bytes)
 *   Ack   (guest -> host)     next u16 (all chunks < next received), bitmap u64 (chunk next+i received)
 *   Done  (guest -> host)     ok u8 (1: assembled, size and hash verified; repeated until Go arrives)
 *   Go    (host -> guest)     the host has applied the state: apply yours now
 * The sender re-sends only chunks somebody has not acknowledged, paced by a token bucket (cheap on the relay's per-room frame cap).
 * Nothing here touches the clock or the socket: the caller passes `now` (ms) and a send callback, so it runs in the virtual-clock tests. */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rb {
namespace xfer {

constexpr uint8_t kPkt = 0x55;
constexpr size_t kChunk = 700;
constexpr size_t kMaxChunks = 6000;   // 4.2 MB compressed: bigger states (PS2) are not worth sending over a game connection

enum Sub : uint8_t { Req = 0, Begin = 1, Chunk = 2, Ack = 3, Done = 4, Go = 5 };

struct Meta {
    uint32_t frame = 0, raw = 0, comp = 0;
    uint16_t chunks = 0;
    uint8_t flags = 0;   // bit 0: payload is compressed
    uint64_t hash = 0;   // of the raw state
    uint64_t a = 0, b = 0;   // caller data riding along (the engine: the confirmed-frame hash chain at `frame`)
};

inline void p16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
inline void p32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }
inline void p64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; i++) v.push_back((uint8_t)(x >> (8 * i))); }
inline uint16_t g16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t g32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t g64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

inline std::vector<uint8_t> header(Sub s, int from, uint8_t epoch) { return {kPkt, (uint8_t)s, (uint8_t)from, epoch}; }

inline std::vector<uint8_t> enc_begin(int from, uint8_t ep, const Meta& m) {
    auto v = header(Begin, from, ep);
    p32(v, m.frame); p32(v, m.raw); p32(v, m.comp); p16(v, m.chunks); v.push_back(m.flags); p64(v, m.hash); p64(v, m.a); p64(v, m.b);
    return v;
}
inline bool dec_begin(const uint8_t* d, size_t n, Meta* m) {
    if (n < 4 + 4 + 4 + 4 + 2 + 1 + 24) return false;
    const uint8_t* p = d + 4;
    m->frame = g32(p); p += 4; m->raw = g32(p); p += 4; m->comp = g32(p); p += 4; m->chunks = g16(p); p += 2; m->flags = *p++;
    m->hash = g64(p); p += 8; m->a = g64(p); p += 8; m->b = g64(p);
    if (m->chunks == 0 || m->chunks > kMaxChunks || m->raw == 0) return false;
    if ((size_t)m->comp > (size_t)m->chunks * kChunk || (size_t)m->comp + kChunk <= (size_t)m->chunks * kChunk) return false;
    return true;
}
inline std::vector<uint8_t> enc_ack(int from, uint8_t ep, uint16_t next, uint64_t bits) {
    auto v = header(Ack, from, ep);
    p16(v, next); p64(v, bits);
    return v;
}

/* ---- sender ---- */
class Sender {
  public:
    bool active() const { return active_; }
    uint8_t epoch() const { return ep_; }
    const Meta& meta() const { return m_; }
    void start(uint8_t ep, const Meta& m, std::vector<uint8_t> payload, uint32_t receivers, uint32_t now, double chunks_per_sec) {
        active_ = true; ep_ = ep; m_ = m; data_ = std::move(payload); mask_ = receivers; done_ = 0;
        sent_.assign(m_.chunks, 0); ever_.assign(m_.chunks, 0);
        for (int s = 0; s < 8; s++) have_[s].assign(m_.chunks, 0);
        last_begin_ = 0; last_tick_ = now; credit_ = 4; cursor_ = 0; rate_ = chunks_per_sec; started_ = now;
    }
    void stop() { active_ = false; data_.clear(); }
    void set_rate(double r) { rate_ = r; }
    void remove(int slot) { mask_ &= ~(1u << slot); }
    bool all_done() const { return active_ && (mask_ & ~done_) == 0; }
    void on_ack(int slot, uint16_t next, uint64_t bits) {
        if (!active_ || slot < 0 || slot >= 8) return;
        for (size_t i = 0; i < std::min<size_t>(next, m_.chunks); i++) have_[slot][i] = 1;
        for (int i = 0; i < 64; i++) if ((bits >> i & 1u) && (size_t)next + i < m_.chunks) have_[slot][(size_t)next + i] = 1;
    }
    void on_done(int slot) { if (slot >= 0 && slot < 8) { done_ |= 1u << slot; for (auto& h : have_[slot]) h = 1; } }
    size_t chunks_sent() const { return n_sent_; }
    /* Emit whatever is due. out(const uint8_t*, size_t) broadcasts one datagram to every receiver. */
    template <class Out> void service(uint32_t now, int self, Out&& out) {
        if (!active_) return;
        if (last_begin_ == 0 || now - last_begin_ >= 250) { last_begin_ = now ? now : 1; auto b = enc_begin(self, ep_, m_); out(b.data(), b.size()); }
        credit_ = std::min(24.0, credit_ + (double)(now - last_tick_) * rate_ / 1000.0);
        last_tick_ = now;
        const uint32_t resend = 350;
        for (size_t guard = 0; guard < m_.chunks && credit_ >= 1.0; guard++) {
            size_t i = cursor_ % m_.chunks;
            cursor_ = i + 1;
            bool need = false;
            for (int s = 0; s < 8 && !need; s++) if ((mask_ & ~done_) >> s & 1u && !have_[s][i]) need = true;
            if (!need) continue;
            if (ever_[i] && now - sent_[i] < resend) continue;
            ever_[i] = 1; sent_[i] = now; credit_ -= 1.0; n_sent_++;
            std::vector<uint8_t> v = header(Chunk, self, ep_);
            p16(v, (uint16_t)i);
            size_t off = i * kChunk, len = std::min(kChunk, data_.size() - off);
            v.insert(v.end(), data_.begin() + off, data_.begin() + off + len);
            out(v.data(), v.size());
        }
    }
  private:
    bool active_ = false;
    uint8_t ep_ = 0;
    Meta m_;
    std::vector<uint8_t> data_;
    uint32_t mask_ = 0, done_ = 0, last_begin_ = 0, last_tick_ = 0, started_ = 0;
    std::vector<uint32_t> sent_;
    std::vector<uint8_t> ever_, have_[8];
    size_t cursor_ = 0, n_sent_ = 0;
    double credit_ = 0, rate_ = 100;
};

/* ---- receiver ---- */
class Receiver {
  public:
    bool active() const { return active_; }
    uint8_t epoch() const { return ep_; }
    const Meta& meta() const { return m_; }
    void reset() { active_ = false; got_.clear(); buf_.clear(); n_got_ = 0; }
    /* returns true when this Begin started a new transfer */
    bool on_begin(uint8_t ep, const Meta& m) {
        if (active_ && ep == ep_) return false;
        active_ = true; ep_ = ep; m_ = m;
        buf_.assign((size_t)m.comp, 0); got_.assign(m.chunks, 0); n_got_ = 0; last_ack_ = 0; dirty_ = true;
        return true;
    }
    void on_chunk(uint8_t ep, uint16_t idx, const uint8_t* d, size_t n) {
        if (!active_ || ep != ep_ || idx >= m_.chunks) return;
        size_t off = (size_t)idx * kChunk, want = std::min(kChunk, (size_t)m_.comp - off);
        if (n != want) return;
        if (!got_[idx]) { std::memcpy(buf_.data() + off, d, n); got_[idx] = 1; n_got_++; }
        dirty_ = true;
    }
    bool complete() const { return active_ && n_got_ == m_.chunks; }
    size_t received() const { return n_got_; }
    const std::vector<uint8_t>& payload() const { return buf_; }
    /* the acknowledgement to send now, if one is due (changed and >= 60 ms since the last, or 250 ms regardless) */
    bool ack_due(uint32_t now, uint16_t* next, uint64_t* bits) {
        if (!active_) return false;
        if (!((dirty_ && now - last_ack_ >= 60) || now - last_ack_ >= 250)) return false;
        last_ack_ = now ? now : 1; dirty_ = false;
        size_t n = 0;
        while (n < m_.chunks && got_[n]) n++;
        uint64_t b = 0;
        for (int i = 0; i < 64 && n + i < m_.chunks; i++) if (got_[n + i]) b |= 1ull << i;
        *next = (uint16_t)n; *bits = b;
        return true;
    }
  private:
    bool active_ = false, dirty_ = false;
    uint8_t ep_ = 0;
    Meta m_;
    std::vector<uint8_t> buf_;
    std::vector<uint8_t> got_;
    size_t n_got_ = 0;
    uint32_t last_ack_ = 0;
};

}  // namespace xfer
}  // namespace rb
