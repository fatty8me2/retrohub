#include "link_sync.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "session.h"

namespace rlink {

uint64_t blob_hash(const uint8_t* p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    h ^= (uint64_t)n * 0x9E3779B97F4A7C15ull;
    return h;
}

template <class T> static void put(std::vector<uint8_t>& v, T x) {
    for (size_t i = 0; i < sizeof(T); i++) v.push_back((uint8_t)((uint64_t)x >> (8 * i)));
}
template <class T> static T get(const uint8_t* p) {
    uint64_t x = 0;
    for (size_t i = 0; i < sizeof(T); i++) x |= (uint64_t)p[i] << (8 * i);
    return (T)x;
}

static uint16_t nchunks_of(size_t size) { return (uint16_t)((size + kChunk - 1) / kChunk); }

LinkExchange::LinkExchange(int my_slot, uint32_t slot_mask, const std::vector<uint8_t>& mine) : me_(my_slot), mask_(slot_mask) {
    Owner& o = s_[me_ & 7];
    o.known = o.verified = true;
    o.buf = mine;
    o.size = (uint32_t)mine.size();
    o.hash = blob_hash(mine.data(), mine.size());
    o.nch = nchunks_of(mine.size());
}

uint8_t LinkExchange::have_mask() const {
    uint8_t m = 0;
    for (int i = 0; i < 8; i++) if (s_[i].verified) m |= (uint8_t)(1u << i);
    return m;
}

void LinkExchange::on_packet(int from, const uint8_t* d, size_t n) {
    if (n < 1) return;
    if (d[0] == kPktLinkStatus && n >= 3) {
        int slot = d[1];
        if (slot < 8 && slot == from) peer_mask_[slot] = d[2];
    } else if (d[0] == kPktLinkData && n >= 18) {
        int owner = d[1];
        uint16_t chunk = get<uint16_t>(d + 2), nch = get<uint16_t>(d + 4);
        uint32_t size = get<uint32_t>(d + 6);
        uint64_t hash = get<uint64_t>(d + 10);
        if (owner >= 8 || owner == me_ || !(mask_ & (1u << owner)) || size > kMaxBlob || nch != nchunks_of(size)) return;
        Owner& o = s_[owner];
        if (o.verified) return;
        if (!o.known || o.size != size || o.hash != hash) {
            o = Owner();
            o.known = true; o.size = size; o.hash = hash; o.nch = nch;
            o.have.assign(nch, false);
            o.buf.assign(size, 0);
        }
        size_t len = n - 18;
        if (size == 0) {
            o.verified = (hash == blob_hash(nullptr, 0));
            return;
        }
        if (chunk >= nch) return;
        size_t off = (size_t)chunk * kChunk;
        size_t want = std::min(kChunk, (size_t)size - off);
        if (len != want || o.have[chunk]) return;
        memcpy(o.buf.data() + off, d + 18, len);
        o.have[chunk] = true;
        if (++o.nhave == nch) {
            if (blob_hash(o.buf.data(), o.buf.size()) == o.hash) o.verified = true;
            else { o.have.assign(nch, false); o.nhave = 0; }   // corrupt: start over
        }
    }
}

bool LinkExchange::complete() const {
    for (int i = 0; i < 8; i++) {
        if (!(mask_ & (1u << i))) continue;
        if (!s_[i].verified) return false;
        if (i != me_ && (peer_mask_[i] & mask_) != mask_) return false;
    }
    return true;
}

bool LinkExchange::finished(uint32_t now_ms) const {
    return was_complete_ && now_ms - complete_since_ >= kGraceMs;
}

void LinkExchange::tick(uint32_t now_ms, const SendFn& send) {
    if (complete() && !was_complete_) { was_complete_ = true; complete_since_ = now_ms; }
    if (last_tick_ && now_ms - last_tick_ < kIntervalMs) return;
    last_tick_ = now_ms;
    std::vector<uint8_t> st = {kPktLinkStatus, (uint8_t)me_, have_mask()};
    send(st);
    // My blob: cycle its chunks until every peer reports holding it.
    bool needed = false;
    for (int i = 0; i < 8; i++)
        if (i != me_ && (mask_ & (1u << i)) && !(peer_mask_[i] & (1u << me_))) needed = true;
    if (!needed && !was_complete_) needed = false;
    if (!needed) return;
    const Owner& o = s_[me_];
    int nsend = o.nch ? std::min<int>(kDataPerTick, o.nch) : 1;
    for (int k = 0; k < nsend; k++) {
        uint16_t c = o.nch ? (uint16_t)(cursor_++ % o.nch) : 0;
        std::vector<uint8_t> p = {kPktLinkData, (uint8_t)me_};
        put<uint16_t>(p, c);
        put<uint16_t>(p, o.nch);
        put<uint32_t>(p, o.size);
        put<uint64_t>(p, o.hash);
        if (o.nch) {
            size_t off = (size_t)c * kChunk;
            size_t len = std::min(kChunk, (size_t)o.size - off);
            p.insert(p.end(), o.buf.begin() + off, o.buf.begin() + off + len);
        }
        send(p);
    }
}

bool run_link_exchange(hubnet::Session& s, int my_slot, uint32_t slot_mask, const std::vector<uint8_t>& mine, uint32_t timeout_ms,
                       std::vector<std::vector<uint8_t>>* out_blobs, const std::function<void()>& pump, std::string* err) {
    LinkExchange ex(my_slot, slot_mask, mine);
    SendFn send = [&s](const std::vector<uint8_t>& d) { s.send(-1, d.data(), (int)d.size()); };
    const uint32_t t0 = hubnet::now_ms();
    int rx_count = 0;
    std::vector<uint8_t> msg;
    int from = 0;
    for (;;) {
        s.update();
        while (s.recv(&from, &msg)) { ex.on_packet(from, msg.data(), msg.size()); rx_count++; }
        uint32_t now = hubnet::now_ms();
        ex.tick(now, send);
        if (pump) pump();
        static uint32_t last_dbg = 0;
        if (getenv("RETRORUN_LINK_DEBUG") && now - last_dbg > 2000) {
            last_dbg = now;
            fprintf(stderr, "link dbg slot %d: have %02x peers %02x %02x %02x %02x relayed0=%d rx=%d\n", my_slot, ex.have_bits(), ex.peer_bits(0), ex.peer_bits(1), ex.peer_bits(2), ex.peer_bits(3), s.ping_ms(my_slot ? 0 : 1) + 1000 * (int)s.is_relayed(my_slot ? 0 : 1), rx_count);
        }
        if (ex.finished(now)) break;
        if (s.failed()) { if (err) *err = "netplay failed during link save exchange"; return false; }
        if (now - t0 > timeout_ms) { if (err) *err = "timed out exchanging link saves"; return false; }
        Sleep(2);
    }
    if (out_blobs) {
        out_blobs->assign(8, {});
        for (int i = 0; i < 8; i++) if (slot_mask & (1u << i)) (*out_blobs)[i] = ex.blob(i);
    }
    return true;
}

}  // namespace rlink
