#include "lockstep_core.h"

#include <algorithm>
#include <cmath>
#include <functional>
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
    if (cfg_.slot_mask) mask_ = cfg_.slot_mask & 0xFu;
    else mask_ = (1u << cfg_.num_slots) - 1u;
    mask_ |= 1u << (cfg_.my_slot & 3);
    gate_mask_ = mask_;
    cfg_.redundancy = std::max(cfg_.redundancy, 2 * kMaxDelay + 4);   // covers any D (own + peer's) the adaptive delay may reach
    cfg_.redundancy = std::min(cfg_.redundancy, 120);
    delay_ = initial_delay_ = cfg_.delay;
    newest_ = (int64_t)cfg_.delay - 1;
    want_ = (uint8_t)cfg_.delay;
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
    int64_t target = (int64_t)cur_sample + delay_;
    // Fill every not-yet-scheduled sample up to the target with the current input. In steady state that is exactly
    // one sample; after D grew it holds the input over the gap; after D shrank it is empty (already scheduled).
    for (int64_t s = newest_ + 1; s <= target; s++)
        for (int p = 0; p < kPorts; p++)
            if (port_local(p)) put(p, (uint32_t)s, buttons[p]);
    if (target > newest_) newest_ = target;
    any_local_ = true;   // even a slot with no ports sends packets (hash trailer)
}

void LockstepCore::order_delay(int d) {
    d = std::max(0, std::min(kMaxDelay, d));
    if (d == delay_) return;
    delay_ = d;
    ord_gen_++;
    delay_changes_++;
}

bool LockstepCore::ready(uint32_t n) const {
    for (int p = 0; p < kPorts; p++)
        if (port_present(p) && !has(p, n)) return false;
    for (int s = 0; s < kPorts; s++)
        if ((gate_mask_ >> s & 1u) && s != cfg_.my_slot && (uint64_t)peer_progress_[s] + (uint64_t)max_lead() < n) return false;
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
    uint32_t newest_local_ = (uint32_t)newest_;
    uint32_t count = std::min<uint32_t>((uint32_t)cfg_.redundancy, newest_local_ + 1);
    int nports = 0;
    for (int p = 0; p < kPorts; p++) nports += port_local(p) ? 1 : 0;
    size_t need = 2 + 4 + 4 + 2 + (size_t)nports * (1 + 4 * (size_t)count) + 1 + recent_local_hashes_.size() * 12 + 3;
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
    // Adaptive-delay trailer (older peers ignore trailing bytes): host's ordered D + generation, my wanted D.
    wr<uint8_t>(p, ord_gen_);
    wr<uint8_t>(p, (uint8_t)delay_);
    wr<uint8_t>(p, want_);
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
            if (s < (uint32_t)initial_delay_) continue;   // prefilled neutral
            // Never overwrite an existing value for the same sample: inputs are immutable once known.
            if (!has(port, s)) put(port, s, v);
        }
    }
    uint8_t nh;
    if (rd(p, end, &nh)) {
        for (int i = 0; i < nh; i++) {
            uint32_t s; uint64_t h;
            if (!rd(p, end, &s) || !rd(p, end, &h)) return true;
            put_hash(slot, s, h);
        }
        uint8_t gen, d, want;
        if (rd(p, end, &gen) && rd(p, end, &d) && rd(p, end, &want)) {
            if (slot < kPorts) peer_want_[slot] = want;
            if (slot == 0 && cfg_.my_slot != 0 && (int8_t)(uint8_t)(gen - ord_gen_) > 0) {   // newer order from the host
                ord_gen_ = gen;
                int nd = std::min<int>(d, kMaxDelay);
                if (nd != delay_) { delay_ = nd; delay_changes_++; }
            }
        }
    }
    return true;
}

void LockstepCore::put_hash(int slot, uint32_t sample, uint64_t h) {
    if (slot < 0 || slot >= kPorts || !(mask_ >> slot & 1u)) return;
    HashRec& r = hashes_[sample];
    if (r.done) return;
    r.h[slot] = h;
    r.known |= (uint8_t)(1u << slot);
    uint8_t all = (uint8_t)mask_;
    if ((r.known & all) == all) {
        r.done = true;
        compared_++;
        bool same = true;
        int first = -1;
        for (int s = 0; s < kPorts; s++) {
            if (!(mask_ >> s & 1u)) continue;
            if (first < 0) first = s;
            else if (r.h[s] != r.h[first]) same = false;
        }
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

namespace n64run {

int popcount32(uint32_t v) { int n = 0; for (; v; v >>= 1) n += v & 1; return n; }

uint32_t derive_slot_mask(const int owner[kPorts], int my_slot, int players, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return 0u; };
    if (my_slot < 0 || my_slot >= kPorts) return fail("--slot must be 0..3");
    if (players < 1 || players > kPorts) return fail("--players must be 1..4 (number of PCs)");
    uint32_t mask = 1u | (1u << my_slot);
    for (int p = 0; p < kPorts; p++) {
        if (owner[p] < -1 || owner[p] >= kPorts) return fail("--ports value must be -1 or a slot 0..3");
        if (owner[p] >= 0) mask |= 1u << owner[p];
    }
    if (popcount32(mask) > players) return fail("--players is smaller than the number of PCs named by --slot/--ports");
    for (int s = 0; s < kPorts && popcount32(mask) < players; s++) mask |= 1u << s;   // port-less spectators
    return mask;
}

int auto_delay_from_rtt(double rtt_ms, double frame_ms, bool relayed, double margin_ms) {
    if (frame_ms < 1.0) frame_ms = 16.7;
    int d = (int)std::ceil((rtt_ms / 2.0 + margin_ms) / frame_ms) + 1;
    if (relayed) d += 1;
    return std::max(1, std::min(10, d));
}

double effective_rtt(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end(), std::greater<double>());
    return v.size() >= 2 ? v[0] + v[1] : v[0];
}

}  // namespace n64run

namespace n64run {

void DelayAdapter::on_poll(uint32_t now, uint32_t stall_ms) {
    if (!started_) { started_ = true; start_ = now; }
    if (last_now_) {
        double iv = (double)(now - last_now_) - (double)last_stall_;   // time spent running, not waiting
        if (iv >= 0.5 && iv < 100.0) avg_ms_ += (iv - avg_ms_) * 0.02;
    }
    last_now_ = now; last_stall_ = stall_ms;
    if (stall_ms >= kMinStallMs) {
        ev_t_[ev_n_ & 7] = now; ev_ms_[ev_n_ & 7] = stall_ms; ev_n_++;
        last_event_ = now;
    }
}

int DelayAdapter::want(int cur, uint32_t now) {
    cur = std::max(floor_, std::min(max_, cur));
    if (want_val_ != cur && now < want_until_ && want_val_ >= floor_) return want_val_;   // keep asking until it took effect
    // recent events
    int cnt = 0; uint32_t mx = 0;
    for (int i = 0; i < 8 && i < ev_n_; i++)
        if (now - ev_t_[i] <= 3000) { cnt++; mx = std::max(mx, ev_ms_[i]); }
    if (cnt >= 2 && (!last_raise_ || now - last_raise_ >= 1500)) {
        int add = (int)std::ceil((double)mx / std::max(1.0, avg_ms_));
        add = std::max(1, std::min(3, add));
        last_raise_ = now;
        last_lower_ = now;
        // Consume the events so the same stalls are not counted again.
        ev_n_ = 0;
        want_val_ = std::min(max_, cur + add); want_until_ = now + 3000;
        return want_val_;
    }
    if (cnt == 0 && cur > floor_) {
        uint32_t quiet_since = std::max(last_event_, std::max(last_lower_, std::max(last_raise_, start_)));
        if (now - quiet_since >= 12000) { last_lower_ = now; want_val_ = cur - 1; want_until_ = now + 3000; return want_val_; }
    }
    return cur;
}

int DelayAdapter::decide(int cur, const int wants[4], uint32_t mask, uint32_t now) {
    int hi = floor_;
    for (int s = 0; s < 4; s++) {
        if (!(mask >> s & 1u)) continue;
        hi = std::max(hi, wants[s]);
    }
    int target = cur;
    if (hi > cur) target = hi;          // anybody needs more: go up (to the largest ask)
    else if (hi < cur) target = cur - 1;   // everybody wants lower: one step down
    target = std::max(floor_, std::min(max_, target));
    if (target != cur) {
        if (last_decide_change_ && now - last_decide_change_ < (target > cur ? 1000u : 6000u)) return cur;
        last_decide_change_ = now ? now : 1;
    }
    return target;
}

}  // namespace n64run
