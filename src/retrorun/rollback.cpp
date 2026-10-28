#include "rollback.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rb {

uint64_t hash_bytes(const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xCBF29CE484222325ull;
    size_t words = bytes / 8;
    for (size_t i = 0; i < words; i++) {
        uint64_t w;
        std::memcpy(&w, p + i * 8, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (size_t i = words * 8; i < bytes; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static double qpc_us() {
    static double inv = 0;
    if (inv == 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); inv = 1e6 / (double)f.QuadPart; }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * inv;
}

static const uint64_t kFnvBasis = 0xCBF29CE484222325ull;
static const uint64_t kFnvPrime = 0x100000001B3ull;

Engine::Engine(const Config& cfg, const Sim& sim, const Transport& net) : cfg_(cfg), sim_(sim), net_(net) {
    cfg_.max_pred = std::max(1, std::min(kMaxPred, cfg_.max_pred));
    cfg_.delay = std::max(0, std::min(4, cfg_.delay));
    cfg_.val_bytes = cfg_.val_bytes >= 8 ? 8 : cfg_.val_bytes >= 6 ? 6 : 2;
    if (cfg_.val_bytes < 8) cfg_.event_mask = 0;
    ring_n_ = cfg_.max_pred + 4;
    trace_ = std::getenv("RB_TRACE") != nullptr;
    state_n_ = sim_.state_size();
    ring_.assign((size_t)ring_n_ * state_n_, 0);
    tmp_.assign(state_n_, 0);
    for (int p = 0; p < kPorts; p++) { conf_upto_[p] = -1; last_val_[p] = 0; }
    memset(used_, 0, sizeof used_);
    memset(fsum_, 0, sizeof fsum_);
    for (int s = 0; s < 8; s++) {
        peer_cur_[s] = 0; peer_adv_[s] = 0; peer_seen_[s] = false;
        peer_ts_[s] = 0; peer_ts_at_[s] = 0; rtt_[s] = 0; rttdev_[s] = 0;
        for (int p = 0; p < kPorts; p++) peer_ack_[s][p] = -1;
    }
    roll_out_ = roll_in_ = kFnvBasis;
    // Frames 0..D-1 are neutral for the local ports.
    for (int p = 0; p < kPorts; p++)
        if (local(p)) for (int f = 0; f < cfg_.delay; f++) put_local(p, f, 0);
    uint32_t now = net_.now_ms();
    last_progress_ = now;
    st_.delay_min = st_.delay_max = cfg_.delay;
}

void Engine::put_local(int p, int64_t f, uint64_t v) {
    in_[p][f % kInRing].tag = f;
    in_[p][f % kInRing].val = v;
    local_newest_ = std::max(local_newest_, f);
}

void Engine::put_remote(int p, int64_t f, uint64_t v) {
    if (f < 0 || has(p, f)) return;
    if (f <= conf_eff_ && f < (int64_t)cur_ - kFrRing + 1) return;   // ancient
    in_[p][f % kInRing].tag = f;
    in_[p][f % kInRing].val = v;
    if (f < (int64_t)cur_ && f >= floor_ && used_[f % kFrRing][p] != v) rb_to_ = rb_to_ < 0 ? f : std::min(rb_to_, f);
    while (has(p, conf_upto_[p] + 1)) {
        conf_upto_[p]++;
        last_val_[p] = get(p, conf_upto_[p]);
    }
}

void Engine::build_pads(int64_t f, uint64_t pads[kPorts]) {
    for (int p = 0; p < kPorts; p++) {
        if (!present(p)) { pads[p] = 0; continue; }
        int o = cfg_.owner[p];
        if (o != cfg_.my_slot && (done_mask_ >> o & 1u) && f >= (int64_t)drop_cut_[o]) pads[p] = 0;   // dropped guest: neutral from the agreed cutoff
        else pads[p] = has(p, f) ? get(p, f) : (last_val_[p] & ~cfg_.event_mask);
        used_[f % kFrRing][p] = pads[p];
    }
}

void Engine::sim_frame(int64_t f, bool visible) {
    uint64_t pads[kPorts];
    build_pads(f, pads);
    double t0 = qpc_us();
    sim_.step(pads, visible);
    st_.step_us += qpc_us() - t0;
    st_.step_n++;
    fsum_[f % kFrRing] = sim_.frame_sum ? sim_.frame_sum() : 0;
}

void Engine::do_rollback() {
    int64_t g = rb_to_;
    rb_to_ = -1;
    if (g < 0 || g >= (int64_t)cur_) return;
    if ((int64_t)cur_ - g >= ring_n_) {
        if (net_.log) net_.log("rollback: DESYNC target frame " + std::to_string(g) + " is older than the state ring");
        desync_ = true;
        return;
    }
    int depth = (int)(cur_ - g);
    double t0 = qpc_us();
    bool ok = sim_.load(ring_state(g), state_n_);
    double dt = qpc_us() - t0;
    st_.load_us += dt;
    st_.load_n++;
    if (trace_ && net_.log) net_.log("R rollback to " + std::to_string(g) + " at cur " + std::to_string(cur_));
    if (!ok) { if (net_.log) net_.log("rollback: unserialize failed"); desync_ = true; return; }
    for (int64_t f = g; f < (int64_t)cur_; f++) {
        if (f > g) {
            double s0 = qpc_us();
            sim_.save(ring_state(f), state_n_);
            st_.save_us += qpc_us() - s0;
            st_.save_n++;
        }
        sim_frame(f, false);
    }
    st_.rollbacks++;
    st_.resim_frames += depth;
    st_.depth_sum += depth;
    st_.max_depth = std::max(st_.max_depth, depth);
}

void Engine::pump_net() {
    if (net_.pump) net_.pump();
    int from;
    std::vector<uint8_t> m;
    while (net_.recv(&from, &m)) {
        if (m.empty() || from < 0 || from >= 8) continue;
        on_packet(from, m);
    }
}

static void w32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i))); }
static void w16(std::vector<uint8_t>& b, uint16_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
static void w64(std::vector<uint8_t>& b, uint64_t v) { for (int i = 0; i < 8; i++) b.push_back((uint8_t)(v >> (8 * i))); }
static uint32_t r32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t r16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint64_t r64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

void Engine::send_packet(bool force) {
    uint32_t now = net_.now_ms();
    /* While stalled at the prediction limit the packets are repeats of the same cumulative window: slow them down so a multi-second stall does not pile hundreds
     * of datagrams into the relay (which caps a room at 400 frames/s and would then deliver the backlog for much longer than the stall itself). */
    const uint32_t gap = stall_since_ ? std::max<uint32_t>(cfg_.send_gap_ms, 40) : cfg_.send_gap_ms;
    if (!force && now - last_send_ < gap) return;
    last_send_ = now;
    std::vector<uint8_t> b;
    b.push_back(kPktInput);
    b.push_back((uint8_t)cfg_.my_slot);
    w32(b, cur_);
    int adv = -1000;
    for (int s = 0; s < 8; s++)
        if (active_peer(s) && peer_seen_[s]) adv = std::max(adv, (int)cur_ - (int)peer_cur_[s]);
    w16(b, (uint16_t)(int16_t)(adv == -1000 ? 0 : adv));
    for (int p = 0; p < kPorts; p++) w32(b, (uint32_t)(int32_t)(present(p) && !local(p) ? conf_upto_[p] : -1));
    int nloc = 0;
    for (int p = 0; p < kPorts; p++) if (local(p) && local_newest_ >= 0) nloc++;
    /* one datagram: ~110 bytes of header/hashes/RTT, the rest split between the local ports (<= 100 frames each) */
    const int maxcnt = std::max(8, std::min(100, (900 - 24 * nloc) / std::max(1, nloc * cfg_.val_bytes)));
    b.push_back((uint8_t)nloc);
    for (int p = 0; p < kPorts; p++) {
        if (!local(p) || local_newest_ < 0) continue;
        int64_t start = local_newest_;
        bool any = false;
        for (int s = 0; s < 8; s++) {
            if (!active_peer(s)) continue;
            start = std::min(start, peer_ack_[s][p] + 1);
            any = true;
        }
        if (!any) start = local_newest_;
        start = std::max<int64_t>(start, local_newest_ - maxcnt + 1);
        start = std::min<int64_t>(start, local_newest_ - 2);
        start = std::max<int64_t>(start, 0);
        b.push_back((uint8_t)p);
        w32(b, (uint32_t)start);
        b.push_back((uint8_t)(local_newest_ - start + 1));
        for (int64_t f = start; f <= local_newest_; f++) {
            uint64_t v = has(p, f) ? get(p, f) : 0;
            w16(b, (uint16_t)v);
            if (cfg_.val_bytes > 2) { w16(b, (uint16_t)(v >> 16)); w16(b, (uint16_t)(v >> 32)); }
            if (cfg_.val_bytes > 6) w16(b, (uint16_t)(v >> 48));
        }
    }
    int nh = (int)std::min<size_t>(4, my_h_.size());
    b.push_back((uint8_t)nh);
    for (int i = 0; i < nh; i++) { const HEnt& e = my_h_[my_h_.size() - nh + i]; w32(b, e.frame | (uint32_t)(epoch_ & 3) << 30); w64(b, e.h); }
    // RTT probe: our send time, plus the newest timestamp heard from the first active peer and how long we held it (2-player echo; extra peers share it).
    w32(b, now);
    uint32_t echo = 0xFFFFFFFFu, hold = 0;
    for (int s = 0; s < 8; s++)
        if (active_peer(s) && peer_seen_[s]) { echo = peer_ts_[s]; hold = now - peer_ts_at_[s]; break; }
    w32(b, echo);
    w16(b, (uint16_t)std::min<uint32_t>(hold, 65535));
    net_.send(-1, b.data(), (int)b.size());
}

void Engine::on_packet(int from, const std::vector<uint8_t>& m) {
    const uint8_t* d = m.data();
    size_t n = m.size();
    if (from >= 0 && from < 8) last_rx_[from] = net_.now_ms() ? net_.now_ms() : 1;
    if (d[0] == kPktDrop) { drop_on_msg(from, d, n); return; }
    if (d[0] == xfer::kPkt) { resync_on_msg(from, d, n); return; }
    if (d[0] == kPktInput && from >= 0 && from < 8 && (ignore_mask_ >> from & 1u)) return;   // dropped: only relayed windows count
    if (d[0] == kPktReady) {
        if (n >= 2 && d[1] < 8) {
            ready_mask_ |= 1u << d[1];
            uint8_t r[2] = {kPktReady, (uint8_t)cfg_.my_slot};   // late joiner still waiting: answer
            net_.send(d[1], r, 2);
        }
        return;
    }
    if (d[0] == kPktBye) {
        if (n >= 2 && d[1] < 8 && d[1] == from && (int)d[1] != cfg_.my_slot) {
            left_mask_ |= 1u << d[1];
            if (net_.log) net_.log("partner in slot " + std::to_string((int)d[1]) + " ended the session");
        }
        return;
    }
    if (d[0] != kPktInput || n < 8 + 2 + 16 + 2) return;
    int slot = d[1];
    if (slot != from || slot >= 8 || slot == cfg_.my_slot || !(cfg_.slot_mask >> slot & 1u)) return;
    ready_mask_ |= 1u << slot;
    size_t o = 2;
    peer_cur_[slot] = r32(d + o); o += 4;
    peer_adv_[slot] = (int16_t)r16(d + o); o += 2;
    peer_seen_[slot] = true;
    for (int p = 0; p < kPorts; p++) {
        int32_t a = (int32_t)r32(d + o); o += 4;
        peer_ack_[slot][p] = std::max<int64_t>(peer_ack_[slot][p], a);
    }
    int np = d[o++];
    bool progress = false;
    for (int i = 0; i < np; i++) {
        if (o + 6 > n) return;
        int p = d[o++];
        uint32_t start = r32(d + o); o += 4;
        int cnt = d[o++];
        const size_t vb = (size_t)cfg_.val_bytes;
        if (o + (size_t)cnt * vb > n) return;
        bool ok = p < kPorts && cfg_.owner[p] == slot;
        for (int k = 0; k < cnt; k++) {
            uint64_t v = r16(d + o + k * vb);
            if (vb > 2) v |= (uint64_t)r16(d + o + k * vb + 2) << 16 | (uint64_t)r16(d + o + k * vb + 4) << 32;
            if (vb > 6) v |= (uint64_t)r16(d + o + k * vb + 6) << 48;
            if (ok) {
                int64_t before = conf_upto_[p];
                put_remote(p, (int64_t)start + k, v);
                if (conf_upto_[p] != before) progress = true;
            }
        }
        o += (size_t)cnt * vb;
    }
    if (progress) last_progress_ = net_.now_ms();
    if (o < n) {
        int nh = d[o++];
        for (int i = 0; i < nh && o + 12 <= n; i++) {
            { uint32_t tf = r32(d + o); if (hash_epoch_ok(tf)) compare_hash(slot, tf & 0x3FFFFFFFu, r64(d + o + 4)); }
            o += 12;
        }
        if (o + 10 <= n) {
            uint32_t now = net_.now_ms();
            peer_ts_[slot] = r32(d + o);
            peer_ts_at_[slot] = now;
            note_rtt(slot, r32(d + o + 4), r16(d + o + 8));
        }
    }
}

void Engine::note_rtt(int slot, uint32_t echo, uint32_t hold) {
    if (echo == 0xFFFFFFFFu) return;
    uint32_t now = net_.now_ms();
    int32_t r = (int32_t)(now - echo) - (int32_t)hold;
    if (r < 0 || r > 5000) return;
    if (rtt_[slot] <= 0) { rtt_[slot] = r; rttdev_[slot] = r / 4.0; }
    else {
        double e = r - rtt_[slot];
        rtt_[slot] += e / 8.0;
        rttdev_[slot] += (std::abs(e) - rttdev_[slot]) / 8.0;
    }
    double worst = 0;
    for (int s = 0; s < 8; s++) if (active_peer(s)) worst = std::max(worst, rtt_[s]);
    st_.rtt_ms = worst;
}

/* Adaptive input delay. Prediction hides up to pred_budget frames of one-way latency for free (a wrong guess costs a rollback, not input lag);
 * beyond that the local input delay grows so confirmed inputs still arrive inside the prediction window. The delay is a purely local choice:
 * every packet names the frames its inputs belong to, so a peer's delay changing never changes what the other peer simulates. Growing the delay repeats the
 * last pad over the new gap frames, shrinking it drops the pads that would land on frames already scheduled (see run_frame). */
void Engine::adapt_delay() {
    double worst = 0;
    for (int s = 0; s < 8; s++)
        if (active_peer(s) && rtt_[s] > 0) worst = std::max(worst, rtt_[s] + rttdev_[s]);
    if (worst <= 0) return;
    double owd_frames = worst / 2.0 / std::max(1.0, cfg_.frame_ms);
    int target = (int)std::ceil(owd_frames) - cfg_.pred_budget;
    target = std::max(0, std::min(std::min(4, cfg_.delay_max), target));
    int before = cfg_.delay;
    if (target > cfg_.delay) { cfg_.delay++; adapt_dn_ = 0; }                       // grow fast (one step per evaluation)
    else if (target < cfg_.delay) { if (++adapt_dn_ >= 4) { cfg_.delay--; adapt_dn_ = 0; } }   // shrink only after it stays low
    else { adapt_dn_ = 0; return; }
    if (cfg_.delay != before) {
        st_.delay_changes++;
        st_.delay_min = std::min(st_.delay_min, cfg_.delay);
        st_.delay_max = std::max(st_.delay_max, cfg_.delay);
        if (net_.log) net_.log("rollback: input delay " + std::to_string(before) + " -> " + std::to_string(cfg_.delay) + " at frame " + std::to_string(cur_) + " (rtt " + std::to_string((int)st_.rtt_ms) + " ms)");
    }
}

void Engine::compare_hash(int slot, uint32_t frame, uint64_t h) {
    for (auto& e : peer_h_[slot]) if (e.frame == frame) return;
    peer_h_[slot].push_back({frame, h});
    if (peer_h_[slot].size() > 16) peer_h_[slot].pop_front();
    for (auto& e : my_h_) {
        if (e.frame != frame) continue;
        st_.hashes_compared++;
        if (e.h != h) {
            char buf[160];
            snprintf(buf, sizeof buf, "DESYNC at confirmed frame %u: %s hashes differ (mine %016llx, slot %d %016llx)", frame,
                     cfg_.use_state_hash ? "state" : "output", (unsigned long long)e.h, slot, (unsigned long long)h);
            if (net_.log) net_.log(buf);
            request_resync("hash mismatch");
        }
    }
}

void Engine::advance_confirmed() {
    int64_t c = (int64_t)cur_ - 1;
    for (int p = 0; p < kPorts; p++)
        if (gating(p)) c = std::min(c, conf_upto_[p]);
    conf_eff_ = std::max(conf_eff_, c);
    while (roll_next_ <= conf_eff_) {
        int64_t f = roll_next_++;
        if (trace_ && net_.log) { char b[120]; snprintf(b, sizeof b, "T %lld %016llx %04x %04x", (long long)f, (unsigned long long)fsum_[f % kFrRing], (unsigned)used_[f % kFrRing][0], (unsigned)used_[f % kFrRing][1]); net_.log(b); }
        roll_out_ = (roll_out_ ^ fsum_[f % kFrRing]) * kFnvPrime;
        for (int p = 0; p < kPorts; p++) roll_in_ = (roll_in_ ^ used_[f % kFrRing][p]) * kFnvPrime;
        if (cfg_.hash_interval > 0 && (f + 1) % cfg_.hash_interval == 0) {
            if (cfg_.use_state_hash) pending_h_.push_back((uint32_t)(f + 1));
            else {
                my_h_.push_back({(uint32_t)(f + 1), roll_out_});
                if (my_h_.size() > 16) my_h_.pop_front();
                for (int s = 0; s < 8; s++)
                    for (auto& e : peer_h_[s]) if (e.frame == f + 1) { HEnt copy = e; compare_hash(s, copy.frame, copy.h); }
            }
        }
    }
}

void Engine::resolve_hashes() {
    while (!pending_h_.empty()) {
        uint32_t H = pending_h_.front();
        if (H > cur_) return;   // state before H not saved yet
        pending_h_.pop_front();
        if ((int64_t)cur_ - H >= ring_n_) { if (net_.log) net_.log("hash point " + std::to_string(H) + " fell out of the ring"); continue; }
        uint64_t h = hash_bytes(ring_state(H), state_n_);
        my_h_.push_back({H, h});
        if (my_h_.size() > 16) my_h_.pop_front();
        for (int s = 0; s < 8; s++)
            for (auto& e : peer_h_[s]) if (e.frame == H) { HEnt copy = e; compare_hash(s, copy.frame, copy.h); }
    }
}

bool Engine::start_barrier(uint32_t timeout_ms) {
    uint32_t t0 = net_.now_ms(), last = 0;
    ready_mask_ |= 1u << cfg_.my_slot;
    while ((ready_mask_ & cfg_.slot_mask) != cfg_.slot_mask) {
        uint32_t now = net_.now_ms();
        if (now - t0 > timeout_ms) return false;
        if (now - last >= 50 || last == 0) {
            last = now;
            uint8_t r[2] = {kPktReady, (uint8_t)cfg_.my_slot};
            net_.send(-1, r, 2);
        }
        if (net_.pump) net_.pump();
        int from;
        std::vector<uint8_t> m;
        while (net_.recv(&from, &m)) if (!m.empty() && from >= 0 && from < 8) on_packet(from, m);
        if (sim_.idle) sim_.idle();
        Sleep(2);
    }
    // Make sure the others see our ready too.
    uint8_t r[2] = {kPktReady, (uint8_t)cfg_.my_slot};
    net_.send(-1, r, 2);
    last_progress_ = net_.now_ms();
    for (int q = 0; q < 8; q++) last_rx_[q] = last_progress_ ? last_progress_ : 1;
    return true;
}

/* ---- desync recovery: the host's confirmed state replaces everybody's at an agreed frame ---- */
void Engine::request_resync(const char* why) {
    if (desync_) return;
    if (cfg_.max_resyncs <= 0 || epoch_ >= cfg_.max_resyncs || finishing_) { desync_ = true; return; }
    if (rs_ != Rs::None || rs_want_) return;
    if (net_.log) net_.log(std::string("resync: ") + why + ", asking the host for a state (resync " + std::to_string(epoch_ + 1) + " of " + std::to_string(cfg_.max_resyncs) + ")");
    rs_started_ = net_.now_ms() ? net_.now_ms() : 1;
    if (cfg_.my_slot == host_slot()) rs_want_ = true;
    else { rs_ = Rs::Wait; rs_last_ = 0; }
}

void Engine::xfer_send(const void* d, size_t n) { net_.send(-1, d, (int)n); }

void Engine::resync_fail(const char* why) {
    if (net_.log) net_.log(std::string("resync: failed (") + why + ")");
    rs_ = Rs::None; rs_want_ = false; snd_.stop(); rcv_.reset();
    desync_ = true;
}

/* Host: snapshot the state before frame F = the first frame whose inputs are not all confirmed yet (so it depends on confirmed inputs only), compress it, stream it. */
void Engine::resync_begin_host() {
    rs_want_ = false;
    int64_t F = roll_next_;
    if (F < (int64_t)cur_ - (ring_n_ - 1) || F > (int64_t)cur_) { resync_fail("confirmed frame outside the state ring"); return; }
    rs_raw_.assign(state_n_, 0);
    if (F == (int64_t)cur_) { if (!sim_.save(rs_raw_.data(), state_n_)) { resync_fail("save"); return; } }
    else std::memcpy(rs_raw_.data(), ring_state(F), state_n_);
    std::vector<uint8_t> z;
    xfer::Meta m;
    if (!std::getenv("RETRORUN_TEST_RESYNC_RAW") && sim_.pack_state && sim_.pack_state(rs_raw_.data(), state_n_, &z)) m.flags = 1;
    else z = rs_raw_;
    if (z.size() > cfg_.resync_max_bytes || z.size() > xfer::kMaxChunks * xfer::kChunk) { resync_fail("state too big to send"); return; }
    m.frame = (uint32_t)F; m.raw = (uint32_t)state_n_; m.comp = (uint32_t)z.size();
    m.chunks = (uint16_t)((z.size() + xfer::kChunk - 1) / xfer::kChunk);
    m.hash = hash_bytes(rs_raw_.data(), state_n_);
    m.a = rs_ro_ = roll_out_; m.b = rs_ri_ = roll_in_;
    rs_frame_ = (uint32_t)F;
    rs_extra_ms_ = (uint32_t)m.chunks * 20;   // a big state over the relay (100 chunks/s) legitimately takes a while
    uint32_t mask = 0;
    for (int s = 0; s < 8; s++) if (active_peer(s)) mask |= 1u << s;
    uint32_t now = net_.now_ms() ? net_.now_ms() : 1;
    bool rel = net_.relayed && net_.relayed();
    snd_.start((uint8_t)(epoch_ + 1), m, std::move(z), mask, now, rel ? 100.0 : 400.0);
    rs_ = Rs::Send;
    if (net_.log) net_.log("resync: sending the state of frame " + std::to_string(F) + " (" + std::to_string(m.raw) + " bytes, " + std::to_string(m.comp) + " on the wire, " + std::to_string(m.chunks) + " chunks)");
}

void Engine::resync_apply(uint32_t frame, const std::vector<uint8_t>& raw, uint64_t ro, uint64_t ri) {
    if (!sim_.load(raw.data(), raw.size())) { resync_fail("load"); return; }
    cur_ = frame; rb_to_ = -1; conf_eff_ = (int64_t)frame - 1; roll_next_ = frame; floor_ = frame;
    roll_out_ = ro; roll_in_ = ri;
    pending_h_.clear(); my_h_.clear();
    for (int s = 0; s < 8; s++) peer_h_[s].clear();
    epoch_++;
    st_.resyncs++;
    stall_since_ = 0; skew_ema_ = 0; skew_cur_ = 0xFFFFFFFFu; last_wait_ = frame;
    last_progress_ = net_.now_ms();
    rs_ = Rs::None; rs_want_ = false; rcv_.reset();
    if (net_.log) net_.log("resync: " + std::to_string(epoch_) + " done, everybody continues from frame " + std::to_string(frame));
    if (net_.notice) net_.notice("Resynced");
}

void Engine::resync_on_msg(int from, const uint8_t* d, size_t n) {
    if (n < 4 || from < 0 || from >= 8 || d[2] != from || !(cfg_.slot_mask >> from & 1u)) return;
    uint8_t sub = d[1], ep = d[3];
    const bool host = cfg_.my_slot == host_slot();
    uint32_t now = net_.now_ms() ? net_.now_ms() : 1;
    if (host) {
        if (sub == xfer::Req) {
            if (ep == (uint8_t)(epoch_ + 1) && rs_ == Rs::None && !rs_want_) {
                if (epoch_ >= cfg_.max_resyncs || finishing_ || cfg_.max_resyncs <= 0) { desync_ = true; return; }
                rs_started_ = now; rs_want_ = true;
            }
        } else if (sub == xfer::Ack && rs_ == Rs::Send && ep == snd_.epoch() && n >= 4 + 10) {
            snd_.on_ack(from, xfer::g16(d + 4), xfer::g64(d + 6));
        } else if (sub == xfer::Done) {
            if (rs_ == Rs::Send && ep == snd_.epoch()) {
                if (n >= 5 && d[4] == 0) { resync_fail("a player could not load the state"); return; }
                snd_.on_done(from);
            } else if (ep == (uint8_t)epoch_ && epoch_ > 0) {   // already applied here: the Go got lost
                auto g = xfer::header(xfer::Go, cfg_.my_slot, ep);
                net_.send(from, g.data(), (int)g.size());
            }
        }
        return;
    }
    if (from != host_slot()) return;
    if (sub == xfer::Begin) {
        xfer::Meta m;
        if (ep != (uint8_t)(epoch_ + 1) || !xfer::dec_begin(d, n, &m) || m.raw != state_n_) return;
        if (epoch_ >= cfg_.max_resyncs || finishing_) { desync_ = true; return; }
        if (rs_ == Rs::Loaded) return;
        if (rcv_.on_begin(ep, m)) {
            if (rs_ == Rs::None) rs_started_ = now;
            rs_extra_ms_ = (uint32_t)m.chunks * 20;
            rs_ = Rs::Recv;
            if (net_.log) net_.log("resync: receiving the host's state of frame " + std::to_string(m.frame));
        }
    } else if (sub == xfer::Chunk && rs_ == Rs::Recv && n >= 6) {
        rcv_.on_chunk(ep, xfer::g16(d + 4), d + 6, n - 6);
        last_progress_ = now;
    } else if (sub == xfer::Go && rs_ == Rs::Loaded && ep == rcv_.epoch()) {
        resync_apply(rcv_.meta().frame, rs_raw_, rcv_.meta().a, rcv_.meta().b);
    }
}

void Engine::resync_service(uint32_t now) {
    if (rs_ == Rs::None && !rs_want_) return;
    if (!now) now = 1;
    if (rs_started_ && now - rs_started_ > cfg_.resync_timeout_ms + rs_extra_ms_) { resync_fail("timed out"); return; }
    if (rs_want_ && rs_ == Rs::None) { if (rb_to_ < 0) resync_begin_host(); return; }
    if (rs_ == Rs::Wait) {
        if (now - rs_last_ >= 200) {
            rs_last_ = now;
            auto r = xfer::header(xfer::Req, cfg_.my_slot, (uint8_t)(epoch_ + 1));
            net_.send(host_slot(), r.data(), (int)r.size());
        }
    } else if (rs_ == Rs::Send) {
        for (int s = 0; s < 8; s++) if (!active_peer(s)) snd_.remove(s);
        snd_.set_rate(net_.relayed && net_.relayed() ? 100.0 : 400.0);
        snd_.service(now, cfg_.my_slot, [this](const uint8_t* p, size_t n) { xfer_send(p, n); });
        if (snd_.all_done()) {
            uint8_t ep = snd_.epoch();
            snd_.stop();
            resync_apply(rs_frame_, rs_raw_, rs_ro_, rs_ri_);
            if (!desync_) {
                auto g = xfer::header(xfer::Go, cfg_.my_slot, ep);
                for (int i = 0; i < 3; i++) net_.send(-1, g.data(), (int)g.size());
            }
        }
    } else if (rs_ == Rs::Recv) {
        uint16_t next; uint64_t bits;
        if (rcv_.ack_due(now, &next, &bits)) { auto a = xfer::enc_ack(cfg_.my_slot, rcv_.epoch(), next, bits); net_.send(host_slot(), a.data(), (int)a.size()); }
        if (rcv_.complete()) {
            const xfer::Meta& m = rcv_.meta();
            rs_raw_.assign(m.raw, 0);
            bool ok = true;
            if (m.flags & 1) ok = sim_.unpack_state && sim_.unpack_state(rcv_.payload().data(), rcv_.payload().size(), rs_raw_.data(), m.raw);
            else if (rcv_.payload().size() == m.raw) std::memcpy(rs_raw_.data(), rcv_.payload().data(), m.raw);
            else ok = false;
            if (ok && hash_bytes(rs_raw_.data(), m.raw) != m.hash) ok = false;
            if (!ok) {
                auto dn = xfer::header(xfer::Done, cfg_.my_slot, rcv_.epoch()); dn.push_back(0);
                net_.send(host_slot(), dn.data(), (int)dn.size());
                resync_fail("state did not verify");
                return;
            }
            rs_ = Rs::Loaded; rs_last_ = 0;
        }
    }
    if (rs_ == Rs::Loaded && now - rs_last_ >= 100) {
        rs_last_ = now;
        auto dn = xfer::header(xfer::Done, cfg_.my_slot, rcv_.epoch()); dn.push_back(1);
        net_.send(host_slot(), dn.data(), (int)dn.size());
    }
}

/* ---- mid-match drop of a silent guest (drop_agree.h): frames >= cutoff read 0 for its ports ---- */
void Engine::drop_begin(int slot) {
    if (drops_.count(slot) || (done_mask_ >> slot & 1u) || slot <= 0 || slot >= 8 || slot == cfg_.my_slot) return;
    int64_t prop = 0;
    bool first = true;
    for (int p = 0; p < kPorts; p++)
        if (present(p) && cfg_.owner[p] == slot) { int64_t k = conf_upto_[p] + 1; prop = first ? k : std::min(prop, k); first = false; }
    ignore_mask_ |= 1u << slot;
    uint32_t surv = cfg_.slot_mask & ~(1u << slot) & ~left_mask_ & ~ignore_mask_;
    for (auto& kv : drops_) kv.second.remove_survivor(slot);
    drops_[slot] = netdrop::Agreement(slot, surv | (1u << cfg_.my_slot), cfg_.my_slot, (uint32_t)prop);
    if (net_.log) net_.log("drop: slot " + std::to_string(slot) + " silent, proposing cutoff frame " + std::to_string((uint32_t)prop));
    last_drop_send_ = 0;
}

void Engine::drop_send(const netdrop::Agreement& ag) {
    netdrop::Msg m;
    m.from = (uint8_t)cfg_.my_slot; m.dead = (uint8_t)ag.dead();
    m.proposal = ag.my_proposal(); m.heard = ag.heard_mask();
    uint32_t hi = ag.window_hi();
    uint32_t lo = hi > (uint32_t)netdrop::kWindow ? hi - netdrop::kWindow : 0;
    m.first = lo;
    for (int p = 0; p < kPorts; p++) {
        if (!present(p) || cfg_.owner[p] != ag.dead()) continue;
        netdrop::PortWin w;
        w.port = (uint8_t)p;
        for (uint32_t f = lo; f < hi; f++) w.vals.push_back(has(p, f) ? get(p, f) : 0);
        m.ports.push_back(std::move(w));
    }
    uint8_t buf[1100];
    size_t n = netdrop::encode(kPktDrop, m, buf, sizeof buf);
    if (n) net_.send(-1, buf, (int)n);
}

void Engine::drop_on_msg(int from, const uint8_t* d, size_t n) {
    netdrop::Msg m;
    if (!netdrop::decode(kPktDrop, d, n, &m) || m.from != from || m.dead >= 8) return;
    if (m.dead == cfg_.my_slot) {   // the others gave up on us (we were silent for them, e.g. a multi-second stall): we cannot rejoin, so leave cleanly instead of playing on alone
        if (!lost_ && net_.log) net_.log("drop: the other players dropped this PC (silent too long) - leaving");
        lost_ = true;
        return;
    }
    if (m.dead == 0 || !(cfg_.slot_mask >> m.dead & 1u)) return;
    if ((done_mask_ >> m.dead & 1u) && !drops_.count(m.dead)) return;
    if (!drops_.count(m.dead)) drop_begin(m.dead);
    auto it = drops_.find(m.dead);
    if (it == drops_.end()) return;
    for (auto& w : m.ports) {
        if (w.port >= kPorts || cfg_.owner[w.port] != m.dead) continue;
        for (size_t i = 0; i < w.vals.size(); i++) {
            int64_t f = (int64_t)m.first + (int64_t)i;
            if ((done_mask_ >> m.dead & 1u) && f >= (int64_t)drop_cut_[m.dead]) continue;
            put_remote(w.port, f, w.vals[i] & vmask());
        }
    }
    it->second.on_msg(from, m.proposal, m.heard);
    if (it->second.decided() && !it->second.settled()) last_drop_send_ = 0;
}

void Engine::drop_service(uint32_t now) {
    if (drops_.empty()) return;
    bool send = now - last_drop_send_ >= 100;
    for (auto it = drops_.begin(); it != drops_.end();) {
        netdrop::Agreement& ag = it->second;
        int dead = it->first;
        if (ag.decided() && !(done_mask_ >> dead & 1u)) {
            uint32_t cut = ag.cutoff();
            drop_cut_[dead] = cut;
            done_mask_ |= 1u << dead;
            drop_done_at_[dead] = now ? now : 1;
            if (cut < cur_ && !desync_) rb_to_ = rb_to_ < 0 ? (int64_t)cut : std::min(rb_to_, (int64_t)cut);   // re-simulate frames already predicted past the cutoff
            if (net_.log) net_.log("drop: slot " + std::to_string(dead) + " disconnected, neutral from frame " + std::to_string(cut) + " - playing on");
        }
        bool done = ag.settled() || ((done_mask_ >> dead & 1u) && now - drop_done_at_[dead] > 8000);
        if (send && !done) drop_send(ag);
        if (done) it = drops_.erase(it); else ++it;
    }
    if (send) last_drop_send_ = now ? now : 1;
}

void Engine::drop_check(uint32_t now) {
    if (!cfg_.drop_silence_ms) return;
    /* Only drop a peer while we hear somebody else: if EVERY peer is silent the problem is our own link (a multi-second relay stall), and dropping
     * them all would split the session. The timeout (lost_) handles a link that never comes back. */
    bool heard_any = false;
    for (int s = 0; s < 8; s++) if (active_peer(s) && last_rx_[s] && now - last_rx_[s] < cfg_.drop_silence_ms / 2) heard_any = true;
    if (!heard_any) return;
    for (int s = 1; s < 8; s++) {
        if (!active_peer(s) || !last_rx_[s] || now - last_rx_[s] < cfg_.drop_silence_ms) continue;
        int others = 0;
        for (int q = 0; q < 8; q++) if (q != s && q != cfg_.my_slot && active_peer(q)) others++;
        if (others) drop_begin(s);
    }
}

void Engine::service() {
    pump_net();
    { uint32_t now = net_.now_ms(); drop_check(now); drop_service(now); }
    if (rb_to_ >= 0 && !desync_) do_rollback();
    advance_confirmed();
    resync_service(net_.now_ms());
    send_packet(false);
}

Engine::Status Engine::prepare() {
    service();
    uint32_t now = net_.now_ms();
    if (rs_ != Rs::None || rs_want_) { last_progress_ = now; return Status::Stalled; }   // paused while the state moves
    if ((int64_t)cur_ - 1 - conf_eff_ >= cfg_.max_pred) {
        // At the prediction limit: wait for the missing peer inputs.
        bool any_remote = false;
        for (int p = 0; p < kPorts; p++) if (gating(p)) any_remote = true;
        if (any_remote) {
            if (stall_since_ == 0) stall_since_ = now;
            st_.stalls++;
            send_packet(false);
            if (now - last_progress_ > cfg_.timeout_ms) lost_ = true;
            return Status::Stalled;
        }
    }
    if (stall_since_) { st_.stalled_ms += now - stall_since_; stall_since_ = 0; }
    // Time sync: the PC that is ahead in time waits an occasional frame.
    double best = -1e9;
    bool have = false;
    for (int s = 0; s < 8; s++) {
        if (!active_peer(s) || !peer_seen_[s]) continue;
        double my_adv = (double)cur_ - (double)peer_cur_[s];
        double skew = (my_adv - (double)peer_adv_[s]) / 2.0;
        best = std::max(best, skew);
        have = true;
    }
    if (have) {
        // One EMA step per simulated frame (prepare() runs many times while waiting). skew > 0: we are ahead of the peers.
        if (skew_cur_ != cur_) { skew_cur_ = cur_; skew_ema_ = skew_ema_ * 0.85 + best * 0.15; }
        st_.max_skew = std::max(st_.max_skew, skew_ema_);
        // GGPO-style gradual correction: the further ahead, the more often a frame is skipped (every 8 frames at ~1 ahead, every 2 at 4+).
        if (skew_ema_ > 0.7) {
            uint32_t gap = (uint32_t)std::max(2.0, std::min(8.0, 8.0 / skew_ema_));
            if (cur_ - last_wait_ >= gap) {
                last_wait_ = cur_;
                skew_ema_ -= 1.0;
                st_.sync_waits++;
                return Status::SyncWait;
            }
        }
    }
    return Status::Ready;
}

void Engine::run_frame() {
    if (sim_.poll_local) sim_.poll_local();
    if (cfg_.adaptive_delay && cur_ >= 20 && cur_ % 30 == 0) adapt_delay();
    const int64_t base = local_newest_;
    for (int p = 0; p < kPorts; p++) {
        if (!local(p)) continue;
        int64_t tf = (int64_t)cur_ + cfg_.delay;
        uint64_t v = sim_.read_local(p) & vmask();
        if (tf <= base) continue;   // delay shrank: that frame already has its pad
        uint64_t prev = has(p, base) ? get(p, base) : 0;
        for (int64_t g = base + 1; g < tf; g++) put_local(p, g, prev & ~cfg_.event_mask);   // delay grew: hold the last pad over the gap
        put_local(p, tf, v);
    }
    /* Over the relay a 4-PC room costs 12 server frames per input packet (the host forwards every client packet to the other clients): halve the rate there. */
    send_packet(!(cfg_.relay_half_rate && net_.relayed && net_.relayed() && (cur_ & 1u)));
    double t0 = qpc_us();
    sim_.save(ring_state(cur_), state_n_);
    st_.save_us += qpc_us() - t0;
    st_.save_n++;
    resolve_hashes();
    sim_frame(cur_, true);
    cur_++;
    st_.frames++;
    last_progress_ = std::max(last_progress_, net_.now_ms() - 0);
}

bool Engine::finish(uint32_t timeout_ms) {
    uint32_t t0 = net_.now_ms();
    bool ok = false;
    finishing_ = true;   // no new recovery while leaving
    for (;;) {
        service();
        send_packet(false);
        bool conf = true, acked = true;
        for (int p = 0; p < kPorts; p++)
            if (gating(p) && conf_upto_[p] < (int64_t)cur_ - 1) conf = false;
        for (int s = 0; s < 8; s++) {
            if (!active_peer(s)) continue;
            for (int p = 0; p < kPorts; p++)
                if (local(p) && peer_ack_[s][p] < local_newest_ - cfg_.delay) acked = false;
        }
        if (conf && acked && rb_to_ < 0) { ok = true; break; }
        if (net_.now_ms() - t0 > timeout_ms || desync_) break;
        if (sim_.idle) sim_.idle();
        Sleep(1);
    }
    advance_confirmed();
    uint8_t bye[2] = {kPktBye, (uint8_t)cfg_.my_slot};
    for (int i = 0; i < 3; i++) { net_.send(-1, bye, 2); if (net_.pump) net_.pump(); Sleep(10); }
    return ok;
}

/* ------------------------------------------------------------------ synctest */

SyncTestResult run_synctest(const Sim& sim, int frames, int depth, bool use_state_hash,
                            const std::function<void(int, uint64_t[kPorts])>& input,
                            const std::function<void(const std::string&)>& log) {
    SyncTestResult r;
    size_t n = sim.state_size();
    r.state_size = n;
    int R = depth + 2;
    std::vector<std::vector<uint8_t>> before(R, std::vector<uint8_t>(n));
    std::vector<uint8_t> tmp(n);
    std::vector<uint64_t> sum(R, 0), post(R, 0);
    std::vector<std::array<uint64_t, kPorts>> pads(R);
    bool dbg = std::getenv("RB_DIFF") != nullptr;
    std::vector<std::vector<uint8_t>> dbg_post(R);
    std::vector<uint8_t> dbg_last;
    double hash_t = 0;
    uint64_t hash_n = 0;
    auto timed_save = [&](uint8_t* dst) {
        double t = qpc_us();
        sim.save(dst, n);
        double d = qpc_us() - t;
        r.save_us += d; r.save_n++; r.save_us_max = std::max(r.save_us_max, d);
    };
    auto timed_step = [&](const uint64_t* p) {
        double t = qpc_us();
        sim.step(p, true);
        r.step_us += qpc_us() - t; r.step_n++;
    };
    use_state_hash = use_state_hash && (bool)sim.mem_hash;
    auto post_hash = [&]() -> uint64_t {
        if (!use_state_hash) return 0;
        double t = qpc_us();
        uint64_t h = sim.mem_hash();
        hash_t += qpc_us() - t; hash_n++;
        return h;
    };
    // Pass A: free-running reference (no rollbacks), recorded per frame. Pass B: same inputs, but rolling back `depth` frames after
    // every frame; every frame (first run and re-simulation) must equal the reference. This catches drift that only appears once
    // a state has been loaded (comparing a reload with an earlier reload would not).
    std::vector<uint64_t> ref_sum(frames), ref_post(frames);
    std::vector<std::array<uint64_t, kPorts>> all_pads(frames);
    std::vector<uint8_t> s0(n);
    int dumpf = std::getenv("RB_DUMPFRAME") ? atoi(std::getenv("RB_DUMPFRAME")) : -1;
    std::vector<uint8_t> dump_ref[4];
    sim.save(s0.data(), n);
    for (int f = 0; f < frames; f++) {
        input(f, all_pads[f].data());
        sim.save(tmp.data(), n);   // like the engine: the state is serialized before every frame (serializing is not free of side effects in some cores)
        timed_step(all_pads[f].data());
        ref_sum[f] = sim.frame_sum();
        ref_post[f] = post_hash();
        if (dumpf >= 0 && f >= dumpf && f <= dumpf + 3) { dump_ref[f - dumpf].resize(n); sim.save(dump_ref[f - dumpf].data(), n); }
    }
    sim.load(s0.data(), n);
    auto check = [&](int q, int from) {
        if (dumpf >= 0 && q >= dumpf && q <= dumpf + 3 && from == q && log) {
            std::vector<uint8_t> cur(n);
            sim.save(cur.data(), n);
            std::string offs; size_t nd = 0;
            for (size_t i = 0; i < n; i++) if (cur[i] != dump_ref[q - dumpf][i]) { if (nd < 40) offs += " " + std::to_string(i); nd++; }
            log("DUMP frame " + std::to_string(q) + ": " + std::to_string(nd) + " state bytes differ:" + offs);
        }
        uint64_t s2 = sim.frame_sum();
        uint64_t p2 = post_hash();
        r.checks++;
        if (s2 != ref_sum[q] || p2 != ref_post[q]) {
            r.mismatches++;
            if ((r.mismatches <= 10 || std::getenv("RB_ALLMISS")) && log) {
                char b[200];
                snprintf(b, sizeof b, "synctest MISMATCH frame %d (rollback from %d): video %016llx vs ref %016llx, memory %016llx vs ref %016llx", q, from,
                         (unsigned long long)s2, (unsigned long long)ref_sum[q], (unsigned long long)p2, (unsigned long long)ref_post[q]);
                log(b);
            }
        }
    };
    for (int f = 0; f < frames; f++) {
        int k = f % R;
        timed_save(before[k].data());
        if (const char* wr = std::getenv("RB_WRONGRUN")) {
            // deep misprediction: run K frames with wrong pads (a peer that predicted far ahead), then restore and play the real ones
            int K = std::max(1, atoi(wr));
            if (f % 3 == 0) {
                for (int j = 0; j < K; j++) {
                    uint64_t bad[kPorts];
                    for (int i = 0; i < kPorts; i++) bad[i] = all_pads[std::min(frames - 1, f + j)][i] ^ (uint64_t)(1 + ((f + j) * 7 & 0x3F3)) ^ ((uint64_t)((f + j) * 131 & 0xFF) << 16);
                    sim.save(tmp.data(), n);
                    timed_step(bad);
                }
                sim.load(before[k].data(), n);
            }
        }
        if (f % 2 == 0 && !std::getenv("RB_NOWRONG")) {
            // mispredicted frame: run it with wrong pads first (as a predicting peer does), then roll it back and replay it right
            uint64_t bad[kPorts];
            for (int i = 0; i < kPorts; i++) bad[i] = all_pads[f][i] ^ 0x1u ^ (uint64_t)(f & 0x3F0) ^ ((uint64_t)((f * 37) & 0xFF) << 24);
            timed_step(bad);
            sim.load(before[k].data(), n);
        }
        timed_step(all_pads[f].data());
        check(f, f);
        r.frames++;
        if (depth <= 0 || f + 1 < depth) continue;
        int g = f + 1 - depth;
        double t = qpc_us();
        bool ok = sim.load(before[g % R].data(), n);
        double d = qpc_us() - t;
        r.load_us += d; r.load_n++; r.load_us_max = std::max(r.load_us_max, d);
        if (!ok) { r.mismatches++; if (log) log("synctest: unserialize failed at frame " + std::to_string(f)); continue; }
        for (int q = g; q <= f; q++) {
            if (q > g) timed_save(before[q % R].data());
            timed_step(all_pads[q].data());
            check(q, f);
        }
    }
    auto avg = [](double s, uint64_t c) { return c ? s / (double)c : 0.0; };
    r.save_us = avg(r.save_us, r.save_n);
    r.load_us = avg(r.load_us, r.load_n);
    r.step_us = avg(r.step_us, r.step_n);
    r.hash_us = avg(hash_t, hash_n);
    return r;
}

}  // namespace rb
