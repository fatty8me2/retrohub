#include "spectate.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace spectate {

namespace {
template <class T> void wr(uint8_t*& p, T v) { std::memcpy(p, &v, sizeof(T)); p += sizeof(T); }
template <class T> bool rd(const uint8_t*& p, const uint8_t* end, T* v) {
    if (static_cast<size_t>(end - p) < sizeof(T)) return false;
    std::memcpy(v, p, sizeof(T));
    p += sizeof(T);
    return true;
}
constexpr int kMaxPayload = 1100;
constexpr size_t kHeader = 1 + 1 + 4 + 1 + 1 + 1 + 4 + 1;   // type from first count mask vbytes total flags
constexpr int kMaxHashes = 4;
constexpr uint32_t kResendAfterMs = 300, kHeartbeatMs = 250, kMinSendGapMs = 30, kAckGapMs = 50;
}  // namespace

/* ---------------------------------------------------------------- Host */

Host::Host(uint8_t port_mask, int vbytes) : port_mask_(port_mask & 0xF), vbytes_(vbytes == 8 ? 8 : 4) {
    nports_ = 0;
    for (int p = 0; p < kPorts; p++) nports_ += port_mask_ >> p & 1;
}

size_t Host::rec_bytes() const { return (size_t)nports_ * (size_t)vbytes_; }

void Host::push(const uint64_t vals[kPorts]) {
    if (hist_.size() >= kMaxHistory) return;
    Frame f;
    for (int p = 0; p < kPorts; p++) f.v[p] = (port_mask_ >> p & 1) ? vals[p] : 0;
    hist_.push_back(f);
}

void Host::add_hash(uint32_t sample, uint64_t hash) {
    hashes_[sample] = hash;
    while (hashes_.size() > 64) hashes_.erase(hashes_.begin());
}

void Host::send_chunk(int slot, Peer& p, uint32_t now, const SendFn& send) {
    const size_t rb = std::max<size_t>(1, rec_bytes());
    uint32_t avail = total() > p.sent ? total() - p.sent : 0;
    uint32_t room = p.consumed + kWindow > p.sent ? p.consumed + kWindow - p.sent : 0;
    uint32_t cap = (uint32_t)((kMaxPayload - kHeader - 1 - kMaxHashes * 12) / rb);
    uint32_t count = std::min(std::min(avail, room), cap);
    uint8_t buf[kMaxPayload];
    uint8_t* w = buf;
    wr<uint8_t>(w, kPktData);
    wr<uint8_t>(w, 0);
    wr<uint32_t>(w, p.sent);
    wr<uint8_t>(w, (uint8_t)count);
    wr<uint8_t>(w, port_mask_);
    wr<uint8_t>(w, (uint8_t)vbytes_);
    wr<uint32_t>(w, total());
    wr<uint8_t>(w, fin_ ? 1 : 0);
    for (uint32_t i = 0; i < count; i++) {
        const Frame& f = hist_[p.sent + i];
        for (int port = 0; port < kPorts; port++) {
            if (!(port_mask_ >> port & 1)) continue;
            if (vbytes_ == 8) wr<uint64_t>(w, f.v[port]);
            else wr<uint32_t>(w, (uint32_t)f.v[port]);
        }
    }
    // newest hashes at or below what this packet makes available (so the spectator can compare when it reaches them)
    std::vector<std::pair<uint32_t, uint64_t>> hs;
    for (auto it = hashes_.rbegin(); it != hashes_.rend() && (int)hs.size() < kMaxHashes; ++it) hs.push_back(*it);
    wr<uint8_t>(w, (uint8_t)hs.size());
    for (auto& h : hs) { wr<uint32_t>(w, h.first); wr<uint64_t>(w, h.second); }
    send(slot, buf, (int)(w - buf));
    sent_pkts_++;
    p.sent += count;
    p.last_send = now ? now : 1;
    if (count) p.last_hb = p.last_send;
    else p.last_hb = p.last_send;
}

void Host::service(uint32_t now, uint32_t spec_mask, const SendFn& send) {
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (!(spec_mask >> it->first & 1u)) it = peers_.erase(it); else ++it;
    }
    for (int s = 0; s < 4; s++) {
        if (!(spec_mask >> s & 1u)) continue;
        Peer& p = peers_[s];
        if (p.fresh) { p.fresh = false; p.last_progress = now; }
        // acks stalled while data is outstanding: go back to the acked position
        if (p.sent > p.acked && now - p.last_progress > kResendAfterMs) { p.sent = p.acked; p.last_progress = now; }
        for (int i = 0; i < 16; i++) {
            uint32_t avail = total() > p.sent ? total() - p.sent : 0;
            uint32_t room = p.consumed + kWindow > p.sent ? p.consumed + kWindow - p.sent : 0;
            uint32_t n = std::min(avail, room);
            if (n == 0) break;
            // live: batch a few samples; catching up: full packets back to back
            if (n < 4 && now - p.last_send < kMinSendGapMs) break;
            send_chunk(s, p, now, send);
        }
        // heartbeat: carries total / fin / hashes when there is nothing to send
        if (now - p.last_hb >= kHeartbeatMs || (fin_ && p.acked < total() && now - p.last_hb >= 60)) send_chunk(s, p, now, send);
    }
}

bool Host::on_ack(int from, const uint8_t* d, size_t n, uint32_t now) {
    const uint8_t* p = d;
    const uint8_t* end = d + n;
    uint8_t type, slot, nh;
    uint32_t next, consumed;
    if (!rd(p, end, &type) || type != kPktAck || !rd(p, end, &slot) || slot != from) return false;
    if (!rd(p, end, &next) || !rd(p, end, &consumed) || !rd(p, end, &nh)) return false;
    auto it = peers_.find(from);
    if (it == peers_.end()) return true;   // not (yet) a known spectator
    Peer& pe = it->second;
    if (next < pe.acked) { pe.acked = next; pe.sent = next; pe.consumed = consumed; pe.last_progress = now; }   // restarted spectator
    if (next > pe.acked) { pe.acked = next; pe.last_progress = now; }
    if (pe.sent < pe.acked) pe.sent = pe.acked;
    pe.consumed = std::max(pe.consumed, std::min(consumed, next));
    for (int i = 0; i < nh; i++) {
        uint32_t s; uint64_t h;
        if (!rd(p, end, &s) || !rd(p, end, &h)) break;
        auto hi = hashes_.find(s);
        if (hi == hashes_.end()) continue;
        if (hi->second == h) hash_ok_++;
        else {
            hash_bad_++;
            if (log_) {
                char b[160];
                std::snprintf(b, sizeof b, "spectator slot %d DIFFERS at sample %u: host %016llx spectator %016llx", from, s, (unsigned long long)hi->second, (unsigned long long)h);
                log_(b);
            }
        }
    }
    return true;
}

bool Host::all_acked(uint32_t spec_mask) const {
    for (int s = 0; s < 4; s++) {
        if (!(spec_mask >> s & 1u)) continue;
        auto it = peers_.find(s);
        if (it == peers_.end() || it->second.acked < total()) return false;
    }
    return true;
}

int64_t Host::acked_of(int slot) const {
    auto it = peers_.find(slot);
    return it == peers_.end() ? -1 : (int64_t)it->second.acked;
}

/* ---------------------------------------------------------------- Client */

bool Client::on_data(const uint8_t* d, size_t n, uint32_t now) {
    const uint8_t* p = d;
    const uint8_t* end = d + n;
    uint8_t type, from, count, mask, vb, flags;
    uint32_t first, total;
    if (!rd(p, end, &type) || type != kPktData || !rd(p, end, &from) || !rd(p, end, &first) || !rd(p, end, &count) ||
        !rd(p, end, &mask) || !rd(p, end, &vb) || !rd(p, end, &total) || !rd(p, end, &flags))
        return false;
    if (from != 0 || (vb != 4 && vb != 8) || (mask & ~0xF)) return false;
    int np = 0;
    for (int i = 0; i < kPorts; i++) np += mask >> i & 1;
    last_rx_ = now ? now : 1;
    if (!have_total_ || total > total_) total_ = total;
    have_total_ = true;
    if (flags & 1) fin_ = true;
    port_mask_ = mask;
    uint32_t next = base_ + (uint32_t)store_.size();
    if ((size_t)(end - p) < (size_t)count * (size_t)np * vb) return false;
    if (count && first <= next && first + count > next) {
        uint32_t skip = next - first;
        for (uint32_t i = 0; i < count; i++) {
            Frame f;
            for (int port = 0; port < kPorts; port++) {
                if (!(mask >> port & 1)) continue;
                uint64_t v = 0;
                if (vb == 8) { uint64_t t; rd(p, end, &t); v = t; } else { uint32_t t; rd(p, end, &t); v = t; }
                f.v[port] = v;
            }
            if (i >= skip) store_.push_back(f);
        }
        dirty_ = true;
    } else {
        p += (size_t)count * (size_t)np * vb;
        if (count) dirty_ = true;   // out of order / duplicate: re-ack so the host moves us along
    }
    uint8_t nh;
    if (rd(p, end, &nh)) {
        for (int i = 0; i < nh; i++) {
            uint32_t s; uint64_t h;
            if (!rd(p, end, &s) || !rd(p, end, &h)) break;
            host_h_[s] = h;
            while (host_h_.size() > 64) host_h_.erase(host_h_.begin());
            compare(s);
        }
    }
    return true;
}

uint64_t Client::get(uint32_t n, int port) const {
    if (!ready(n) || port < 0 || port >= kPorts) return 0;
    return store_[n - base_].v[port];
}

void Client::consumed(uint32_t n) {
    while (base_ < n && !store_.empty()) { store_.pop_front(); base_++; }
    if (n > consumed_) consumed_ = n;
}

void Client::add_local_hash(uint32_t sample, uint64_t hash) {
    my_h_[sample] = hash;
    while (my_h_.size() > 64) my_h_.erase(my_h_.begin());
    recent_mine_.push_back({sample, hash});
    if (recent_mine_.size() > (size_t)kMaxHashes) recent_mine_.erase(recent_mine_.begin());
    compare(sample);
    dirty_ = true;
}

void Client::compare(uint32_t sample) {
    auto a = my_h_.find(sample), b = host_h_.find(sample);
    if (a == my_h_.end() || b == host_h_.end()) return;
    if (a->second == b->second) hash_ok_++;
    else { hash_bad_++; mis_.push_back({sample, a->second, b->second}); }
    my_h_.erase(a);
    host_h_.erase(b);
}

bool Client::poll_mismatch(uint32_t* sample, uint64_t* mine, uint64_t* host) {
    if (mis_.empty()) return false;
    *sample = mis_.front().s; *mine = mis_.front().mine; *host = mis_.front().host;
    mis_.pop_front();
    return true;
}

void Client::service(uint32_t now, int my_slot, const SendFn& send) {
    if (!have_total_) {
        if (now - last_ack_ < 250 && last_ack_) return;   // knock until the host starts streaming
    } else if (!(dirty_ && now - last_ack_ >= kAckGapMs) && now - last_ack_ < 250) {
        return;
    }
    uint32_t next = base_ + (uint32_t)store_.size();
    uint8_t buf[64];
    uint8_t* w = buf;
    wr<uint8_t>(w, kPktAck);
    wr<uint8_t>(w, (uint8_t)my_slot);
    wr<uint32_t>(w, next);
    wr<uint32_t>(w, consumed_);
    wr<uint8_t>(w, (uint8_t)recent_mine_.size());
    for (auto& h : recent_mine_) { wr<uint32_t>(w, h.first); wr<uint64_t>(w, h.second); }
    send(0, buf, (int)(w - buf));
    last_ack_ = now ? now : 1;
    ack_sent_next_ = next;
    dirty_ = false;
}

}  // namespace spectate
