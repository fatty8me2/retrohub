#pragma once
/* Nintendo DS local wireless over RetroHub rooms ("link mode").
 *
 * The melonDS DS core emulates the DS's local wireless (trades, battles, Union Room) by exchanging
 * timestamped MP frames through libretro's NETPACKET interface (RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE).
 * retrorun's link mode implements that interface on top of hubnet::Session (hole punch, relay fallback), so
 * nobody port-forwards. There is no lockstep: every PC runs its own emulator and only the wireless frames travel.
 *
 * A core packet can be up to 64 KB while Session datagrams are <= kMaxPayload, so packets are split into
 * fragments here. Pure logic (no I/O), tested by retrorun_ds_test.
 *
 * Wire format of one datagram:  0xD5 | msg_id (u16 LE) | index (u8) | count (u8) | payload
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dslink {

constexpr uint8_t kMagic = 0xD5;
constexpr size_t kHeader = 5;
constexpr size_t kMaxFragPayload = 1000;        // < hubnet::kMaxPayload (1100) minus the header
constexpr size_t kMaxMessage = kMaxFragPayload * 32;

/* Split one core packet into datagrams. Empty result = too large / empty. */
inline std::vector<std::vector<uint8_t>> split(uint16_t msg_id, const void* buf, size_t len) {
    std::vector<std::vector<uint8_t>> out;
    if (!buf || len == 0 || len > kMaxMessage) return out;
    size_t count = (len + kMaxFragPayload - 1) / kMaxFragPayload;
    const uint8_t* p = (const uint8_t*)buf;
    for (size_t i = 0; i < count; i++) {
        size_t n = std::min(kMaxFragPayload, len - i * kMaxFragPayload);
        std::vector<uint8_t> d(kHeader + n);
        d[0] = kMagic;
        d[1] = (uint8_t)(msg_id & 0xFF);
        d[2] = (uint8_t)(msg_id >> 8);
        d[3] = (uint8_t)i;
        d[4] = (uint8_t)count;
        for (size_t k = 0; k < n; k++) d[kHeader + k] = p[i * kMaxFragPayload + k];
        out.push_back(std::move(d));
    }
    return out;
}

/* Reassembles messages per sender. Datagrams are unreliable: an incomplete message is dropped once a newer one
 * from the same sender completes or the pending set grows past a few entries. Order is not guaranteed. */
class Reassembler {
  public:
    /* Feed one datagram from `from`. Returns true and fills *msg when a whole core packet is complete. */
    bool add(int from, const uint8_t* d, size_t n, std::vector<uint8_t>* msg) {
        if (n < kHeader || d[0] != kMagic) return false;
        uint16_t id = (uint16_t)(d[1] | (d[2] << 8));
        size_t idx = d[3], count = d[4];
        if (count == 0 || idx >= count || count > 32 || n - kHeader > kMaxFragPayload) return false;
        if (count == 1) {
            msg->assign(d + kHeader, d + n);
            return true;
        }
        Pending& p = pend_[{from, id}];
        if (p.frags.empty()) { p.frags.resize(count); p.seq = ++seq_; }
        if (p.frags.size() != count) { pend_.erase({from, id}); return false; }
        if (p.frags[idx].empty()) { p.frags[idx].assign(d + kHeader, d + n); p.have++; }
        if (p.have == count) {
            msg->clear();
            for (auto& f : p.frags) msg->insert(msg->end(), f.begin(), f.end());
            pend_.erase({from, id});
            return true;
        }
        while (pend_.size() > 8) {   // oldest goes
            auto oldest = pend_.begin();
            for (auto it = pend_.begin(); it != pend_.end(); ++it) if (it->second.seq < oldest->second.seq) oldest = it;
            pend_.erase(oldest);
        }
        return false;
    }
    size_t pending() const { return pend_.size(); }

  private:
    struct Pending { std::vector<std::vector<uint8_t>> frags; size_t have = 0; uint64_t seq = 0; };
    std::map<std::pair<int, uint16_t>, Pending> pend_;
    uint64_t seq_ = 0;
};

/* Type byte of a melonDS DS core MP frame (see melonds-ds src/libretro/net/mp.cpp: u64 timestamp, u8 aid,
 * u8 type, payload): 2 = host command, 1 = client reply, 0 = other. -1 if too short. */
inline int frame_type(const uint8_t* buf, size_t len) { return len >= 10 ? buf[9] : -1; }

}  // namespace dslink
