#include "lockstep_core.h"

#include <algorithm>
#include <cstring>

namespace n64run {

namespace {
template <class T> void wr(uint8_t*& p, T v) { std::memcpy(p, &v, sizeof(T)); p += sizeof(T); }
template <class T> bool rd(const uint8_t*& p, const uint8_t* end, T* v) {
    if (static_cast<size_t>(end - p) < sizeof(T)) return false;
    std::memcpy(v, p, sizeof(T));
    p += sizeof(T);
    return true;
}
}  // namespace

LockstepCore::LockstepCore(const Config& cfg) : cfg_(cfg) {
    if (cfg_.delay < 0) cfg_.delay = 0;
    if (cfg_.delay > 60) cfg_.delay = 60;   // ring/redundancy limits
    if (cfg_.num_slots < 1) cfg_.num_slots = 1;
    if (cfg_.num_slots > kPorts) cfg_.num_slots = kPorts;
    // Window must cover the maximum lead between peers (~D+1) with margin.
    cfg_.redundancy = std::max(cfg_.redundancy, 2 * cfg_.delay + 4);
    cfg_.redundancy = std::min(cfg_.redundancy, 120);
    for (int p = 0; p < kPorts; p++) {
        ring_[p].assign(kRing, Entry());
        if (cfg_.owner[p] >= 0) {
            if (lead_ < 0) lead_ = p;
            for (int s = 0; s < cfg_.delay; s++) put(p, (uint32_t)s, 0);   // neutral prefill
        }
    }
}

void LockstepCore::put(int port, uint32_t sample, uint32_t val) {
    Entry& e = ring_[port][sample % kRing];
    e.tag = sample;
    e.val = val;
}

bool LockstepCore::has(int port, uint32_t sample) const {
    return ring_[port][sample % kRing].tag == sample;
}

void LockstepCore::schedule_local(uint32_t cur_sample, const uint32_t buttons[kPorts]) {
    uint32_t target = cur_sample + (uint32_t)cfg_.delay;
    for (int p = 0; p < kPorts; p++)
        if (port_local(p)) put(p, target, buttons[p]);
    any_local_ = true;   // even a slot with no ports sends packets (hash trailer)
    newest_local_ = target;
}

bool LockstepCore::ready(uint32_t n) const {
    for (int p = 0; p < kPorts; p++)
        if (port_present(p) && !has(p, n)) return false;
    for (int s = 0; s < cfg_.num_slots; s++)
        if (s != cfg_.my_slot && (uint64_t)peer_progress_[s] + (uint64_t)max_lead() < n) return false;
    return true;
}

uint32_t LockstepCore::get(uint32_t n, int port) const {
    if (!port_present(port) || !has(port, n)) return 0;
    return ring_[port][n % kRing].val;
}

/* Packet layout (little endian):
 *   u8 type=1, u8 slot, u32 newest, u32 progress, u8 count, u8 nports
 *   nports x { u8 port, u32 vals[count] }   vals for samples newest-count+1 .. newest
 *   u8 nhash, nhash x { u32 sample, u64 hash }
 */
size_t LockstepCore::build_packet(uint8_t* out, size_t cap) const {
    if (!any_local_) return 0;
    uint32_t count = std::min<uint32_t>((uint32_t)cfg_.redundancy, newest_local_ + 1);
    int nports = 0;
    for (int p = 0; p < kPorts; p++) nports += port_local(p) ? 1 : 0;
    size_t need = 2 + 4 + 4 + 2 + (size_t)nports * (1 + 4 * (size_t)count) + 1 + recent_local_hashes_.size() * 12;
    if (need > cap) return 0;
    uint8_t* p = out;
    wr<uint8_t>(p, kPktInput);
    wr<uint8_t>(p, (uint8_t)cfg_.my_slot);
    wr<uint32_t>(p, newest_local_);
    wr<uint32_t>(p, my_progress_);
    wr<uint8_t>(p, (uint8_t)count);
    wr<uint8_t>(p, (uint8_t)nports);
    for (int port = 0; port < kPorts; port++) {
        if (!port_local(port)) continue;
        wr<uint8_t>(p, (uint8_t)port);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t s = newest_local_ - (count - 1) + i;
            wr<uint32_t>(p, ring_[port][s % kRing].tag == s ? ring_[port][s % kRing].val : 0u);
        }
    }
    wr<uint8_t>(p, (uint8_t)recent_local_hashes_.size());
    for (auto& h : recent_local_hashes_) { wr<uint32_t>(p, h.first); wr<uint64_t>(p, h.second); }
    return (size_t)(p - out);
}

bool LockstepCore::on_packet(int from_slot, const uint8_t* data, size_t len) {
    const uint8_t* p = data;
    const uint8_t* end = data + len;
    uint8_t type, slot, count, nports;
    uint32_t newest, progress;
    if (!rd(p, end, &type) || type != kPktInput) return false;
    if (!rd(p, end, &slot) || !rd(p, end, &newest) || !rd(p, end, &progress) || !rd(p, end, &count) || !rd(p, end, &nports)) return false;
    if (slot != from_slot || slot == cfg_.my_slot || count == 0 || count > newest + 1u) return false;
    if (slot < kPorts && progress > peer_progress_[slot]) peer_progress_[slot] = progress;
    for (int i = 0; i < nports; i++) {
        uint8_t port;
        if (!rd(p, end, &port)) return false;
        if (port >= kPorts || cfg_.owner[port] != slot) return false;
        for (uint32_t k = 0; k < count; k++) {
            uint32_t v;
            if (!rd(p, end, &v)) return false;
            uint32_t s = newest - (count - 1) + k;
            if (s < (uint32_t)cfg_.delay) continue;   // prefilled neutral
            // Never overwrite an existing value for the same sample: inputs are immutable once known.
            if (!has(port, s)) put(port, s, v);
        }
    }
    uint8_t nh;
    if (rd(p, end, &nh)) {
        for (int i = 0; i < nh; i++) {
            uint32_t s; uint64_t h;
            if (!rd(p, end, &s) || !rd(p, end, &h)) break;
            put_hash(slot, s, h);
        }
    }
    return true;
}

void LockstepCore::put_hash(int slot, uint32_t sample, uint64_t h) {
    if (slot < 0 || slot >= cfg_.num_slots) return;
    HashRec& r = hashes_[sample];
    if (r.done) return;
    r.h[slot] = h;
    r.known |= (uint8_t)(1u << slot);
    uint8_t all = (uint8_t)((1u << cfg_.num_slots) - 1u);
    if ((r.known & all) == all) {
        r.done = true;
        compared_++;
        bool same = true;
        for (int s = 1; s < cfg_.num_slots; s++) if (r.h[s] != r.h[0]) same = false;
        if (!same) {
            Mismatch m;
            m.sample = sample;
            m.known = r.known;
            for (int s = 0; s < kPorts; s++) m.hash[s] = r.h[s];
            mismatches_.push_back(m);
        }
    }
    while (hashes_.size() > 48) hashes_.erase(hashes_.begin());
}

void LockstepCore::add_local_hash(uint32_t sample, uint64_t hash) {
    recent_local_hashes_.push_back({sample, hash});
    if (recent_local_hashes_.size() > 3) recent_local_hashes_.erase(recent_local_hashes_.begin());
    put_hash(cfg_.my_slot, sample, hash);
}

bool LockstepCore::poll_mismatch(Mismatch* out) {
    if (mismatches_.empty()) return false;
    *out = mismatches_.front();
    mismatches_.erase(mismatches_.begin());
    return true;
}

}  // namespace n64run
