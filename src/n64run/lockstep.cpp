#include "lockstep.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "session.h"

namespace n64run {

uint64_t hash_block(const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xCBF29CE484222325ull;
    size_t words = bytes / 8;
    for (size_t i = 0; i < words; i++) {
        uint64_t w;
        std::memcpy(&w, p + i * 8, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    return h;
}

static LockstepCore::Config make_core_config(const LockstepOptions& o) {
    LockstepCore::Config c;
    for (int i = 0; i < kPorts; i++) c.owner[i] = o.owner[i];
    c.my_slot = o.my_slot;
    c.num_slots = o.num_slots;
    c.slot_mask = o.slot_mask;
    c.wide = o.wide;
    c.reserved = o.reserved_ports;
    c.delay = o.online ? o.delay : 0;   // offline: no input delay needed
    return c;
}

Lockstep::Lockstep(const LockstepOptions& opt, hubnet::Session* session, const LockstepHooks& hooks)
    : opt_(opt), core_(make_core_config(opt)), adapter_(opt.delay < 1 ? 1 : opt.delay, DelayAdapter::cap_for_floor(opt.delay < 1 ? 1 : opt.delay)), net_(opt.online ? session : nullptr), hooks_(hooks) {
    for (int p = 0; p < kPorts; p++)
        if (core_.port_local(p)) local_ports_[n_local_++] = p;
    logged_delay_ = core_.delay();
    masked_ = opt.spectator && !opt.full_hash && hooks.vi_poll && !std::getenv("N64RUN_NO_MASKED_HASH");
    dj_t_start_ = hubnet::now_ms() ? hubnet::now_ms() : 1;
    if (opt.online && opt.serve_spectators && !opt.spectator && opt.my_slot == 0 && session) {
        uint8_t pm = 0;
        for (int p = 0; p < kPorts; p++) if (core_.port_present(p)) pm |= (uint8_t)(1u << p);
        spec_host_.reset(new spectate::Host(pm, opt.wide ? 8 : 4));
        spec_host_->set_log(hooks_.log);
    }
}

void Lockstep::spec_service() {
    if (!net_) return;
    uint32_t now = hubnet::now_ms();
    if (spec_host_) {
        uint32_t sm = spec_mask();
        spec_host_->service(now, sm, [&](int slot, const uint8_t* d, int n) { net_->send(slot, d, n); });   // sm == 0 too: forgets departed spectators
    } else if (opt_.spectator) {
        spec_cli_.service(now, opt_.my_slot, [&](int slot, const uint8_t* d, int n) { net_->send(slot, d, n); });
    }
}

void Lockstep::spec_report() {
    uint32_t s; uint64_t mine, host;
    while (spec_cli_.poll_mismatch(&s, &mine, &host)) {
        desync_ = true;
        char b[200];
        std::snprintf(b, sizeof b, "DESYNC (spectator) at sample %u: %s hashes mine %016llx host %016llx", s, opt_.hash_label.c_str(), (unsigned long long)mine, (unsigned long long)host);
        if (hooks_.log) hooks_.log(b);
    }
}

void Lockstep::send_packet() {
    if (!net_) return;
    uint8_t buf[hubnet::kMaxPayload];
    core_.set_progress(sample_ < 0 ? 0u : (uint32_t)sample_);
    size_t n = core_.build_packet(buf, sizeof buf);
    if (n) net_->send(-1, buf, (int)n);
}

void Lockstep::pump() {
    if (!net_) return;
    net_->update();
    int from;
    std::vector<uint8_t> msg;
    while (net_->recv(&from, &msg)) {
        if (msg.empty()) continue;
        if (from >= 0 && from < kPorts) last_rx_[from] = hubnet::now_ms() ? hubnet::now_ms() : 1;
        if (msg[0] == kPktDrop) { drop_on_msg(from, msg, hubnet::now_ms()); continue; }
        if (msg[0] == kPktDelay && msg.size() >= 3 && opt_.my_slot != 0) {
            uint8_t ack[3] = {kPktDelayAck, (uint8_t)opt_.my_slot, msg[2]};   // late repeats of the host's D
            net_->send(0, ack, 3);
        } else if (msg[0] == kPktDropIn) {
            di_on_msg(from, msg);
        } else if (msg[0] == kPktBye && !opt_.spectator && msg.size() >= 2 && msg[1] < kPorts && (int)msg[1] == from && (dj_slots_ >> from & 1u)) {
            drop_begin(from, hubnet::now_ms());   // a drop-in player left: it drops like a silent guest (its port is reserved again once the cutoff has passed)
        } else if (msg[0] == kPktBye) {
            if (!opt_.spectator && msg.size() >= 2 && msg[1] < kPorts && (int)msg[1] == from && (dj_mask_ >> from & 1u)) { di_candidate_left(from, "said bye"); continue; }
            if (opt_.spectator || (spec_mask() >> (msg[1] & 3) & 1u)) continue;   // spectators come and go; a spectator's goodbye is not the match ending
            if (msg.size() >= 6 && msg[1] < kPorts && (int)msg[1] == from && msg[1] != opt_.my_slot) {
                uint32_t last; std::memcpy(&last, &msg[2], 4);
                if (!(bye_mask_ >> msg[1] & 1u) && hooks_.log) hooks_.log("partner in slot " + std::to_string((int)msg[1]) + " ended the session");
                bye_mask_ |= 1u << msg[1];
                bye_last_[msg[1]] = last;
                core_.peer_left(msg[1]);
            }
        } else if (msg[0] == kPktResync) {
            rs_on_ctl(from, msg);
        } else if (msg[0] == rb::xfer::kPkt && !opt_.spectator && from >= 0 && from < 8 && from != opt_.my_slot && (core_.slot_mask() >> from & 1u)) {
            rs_on_xfer(from, msg);   // a player's transfer is a desync recovery (spectators' are handled below)
        } else if (msg[0] == spectate::kPktJoin || msg[0] == spectate::kPktNoLive || msg[0] == 0x55 /* rb::xfer::kPkt */) {
            bool ok = opt_.spectator ? from == 0 : (from > 0 && (spec_mask() >> from & 1u));
            if (!ok && !opt_.spectator && opt_.my_slot == 0 && msg[0] == spectate::kPktJoin && from > 0 && from < kPorts && dropin::is_knock(msg) &&
                !(core_.slot_mask() >> from & 1u) && !(dj_slots_ >> from & 1u)) {   // a friend asks to drop in
                int r = dropin_knock(from, dropin::knock_caps(msg), dropin::knock_name(msg));
                if (r == 0) ok = true;
                else {
                    uint8_t nl[3] = {spectate::kPktNoLive, 0, (uint8_t)r};
                    net_->send(from, nl, 3);
                }
            }
            if (ok && hooks_.on_spectate_msg) hooks_.on_spectate_msg(from, msg);
        } else if (msg[0] == spectate::kPktAck) {
            if (spec_host_ && from > 0 && (spec_mask() >> from & 1u)) spec_host_->on_ack(from, msg.data(), msg.size(), hubnet::now_ms());
        } else if (msg[0] == spectate::kPktData) {
            if (opt_.spectator && from == 0) spec_cli_.on_data(msg.data(), msg.size(), hubnet::now_ms());
        } else if (msg[0] == kPktCheats) {
            // handshake leftovers: ignore
        } else if (msg[0] == kPktReady) {
            if (msg.size() >= 2 && msg[1] < 8) { ready_mask_ |= 1u << msg[1]; if (msg.size() >= 3) caps_[msg[1]] = msg[2]; }
        } else {
            core_.on_packet(from, msg.data(), msg.size());
        }
    }
    di_service(hubnet::now_ms());
}

bool Lockstep::start_barrier(uint32_t timeout_ms) {
    if (!net_ || opt_.spectator) return true;   // a spectator is not part of the start barrier: it boots whenever it likes
    uint32_t all = core_.slot_mask();
    ready_mask_ |= 1u << opt_.my_slot;
    const uint8_t my_cap = (uint8_t)(((hooks_.resync_fetch && hooks_.resync_load && hooks_.resync_save && !std::getenv("N64RUN_NO_RESYNC")) ? resync::kCapResync : 0) |
                                     ((hooks_.vi_poll && !std::getenv("N64RUN_NO_MASKED_HASH")) ? resync::kCapMaskedHash : 0) |
                                     ((opt_.reserved_ports && !std::getenv("N64RUN_NO_DROPIN_CAP")) ? dropin::kCapDropIn : 0));
    caps_[opt_.my_slot] = my_cap;
    uint32_t t0 = hubnet::now_ms(), last = 0, seen_all_at = 0;
    for (;;) {
        pump();
        uint32_t now = hubnet::now_ms();
        if (net_->failed()) return false;
        if (now - last >= 100 || last == 0) {
            uint8_t pkt[3] = {kPktReady, (uint8_t)opt_.my_slot, my_cap};   // an old peer reads the first two bytes only
            net_->send(-1, pkt, 3);
            last = now ? now : 1;
        }
        if ((ready_mask_ & all) == all) {
            // Keep announcing for a short grace period so slower peers also see us.
            if (!seen_all_at) seen_all_at = now ? now : 1;
            if (now - seen_all_at > 400) {
                uint32_t margin = resync::kDefaultMargin;
                if (const char* e = std::getenv("N64RUN_RESYNC_MARGIN")) margin = (uint32_t)std::atoi(e);
                rs_.configure(opt_.my_slot, all, caps_, margin);
                masked_ = true;
                for (int s = 0; s < 8; s++) if ((all >> s & 1u) && !(caps_[s] & resync::kCapMaskedHash)) masked_ = false;
                dj_ok_ = opt_.reserved_ports != 0;
                for (int s = 0; s < 8; s++) if ((all >> s & 1u) && !(caps_[s] & dropin::kCapDropIn)) dj_ok_ = false;
                if (hooks_.log && opt_.reserved_ports) hooks_.log(dj_ok_ ? "dropin: open ports are reserved; a friend can drop in (every peer supports it)" : "dropin: open ports are reserved, but a peer lacks drop-in: joins are refused");
                if (hooks_.log) hooks_.log(masked_ ? "sync hash: version 2 (framebuffers masked)" : "sync hash: version 1 (a peer lacks the masked hash)");
                if (hooks_.log) hooks_.log(rs_.enabled() ? "resync: desync recovery on (every peer supports it)" : "resync: desync recovery off (a peer lacks it)");
                return true;
            }
        }
        if (now - t0 > timeout_ms) return false;
        if (hooks_.idle) hooks_.idle();
        Sleep(5);
    }
}

void Lockstep::log_stall_summary() const {
    if (!hooks_.log || !net_) return;
    char buf[200];
    uint64_t frames = samples_done();
    std::snprintf(buf, sizeof buf, "netplay stalls: total %llu ms over %llu stalled samples, %llu samples (delay %d, %.2f ms stalled/sample, longest %u ms, final delay %d after %u changes)",
                  (unsigned long long)stall_ms_, (unsigned long long)stalled_, (unsigned long long)frames, opt_.delay,
                  frames ? (double)stall_ms_ / (double)frames : 0.0, longest_stall_, core_.delay(), core_.delay_changes());
    hooks_.log(buf);
}

/* ---- mid-match drop of a silent guest (drop_agree.h) ---- */
void Lockstep::drop_begin(int slot, uint32_t now) {
    if (drops_.count(slot) || core_.drop_done(slot) || slot <= 0 || slot >= kPorts || slot == opt_.my_slot) return;
    uint32_t prop = core_.begin_drop(slot);
    uint32_t surv = core_.slot_mask() & ~(1u << slot);
    for (int s = 0; s < kPorts; s++) if (core_.drop_begun(s)) surv &= ~(1u << s);
    for (auto& kv : drops_) kv.second.remove_survivor(slot);
    drops_[slot] = netdrop::Agreement(slot, surv | (1u << opt_.my_slot), opt_.my_slot, prop);
    if (hooks_.log) { char b[120]; std::snprintf(b, sizeof b, "drop: slot %d silent, proposing cutoff sample %u", slot, prop); hooks_.log(b); }
    last_drop_send_ = 0;
    (void)now;
}

void Lockstep::drop_send(const netdrop::Agreement& ag) {
    netdrop::Msg m;
    m.from = (uint8_t)opt_.my_slot; m.dead = (uint8_t)ag.dead();
    m.proposal = ag.my_proposal(); m.heard = ag.heard_mask();
    core_.fill_drop_window(ag.dead(), ag.window_hi(), &m);
    uint8_t buf[hubnet::kMaxPayload];
    size_t n = netdrop::encode(kPktDrop, m, buf, sizeof buf);
    if (n) net_->send(-1, buf, (int)n);
}

void Lockstep::drop_on_msg(int from, const std::vector<uint8_t>& msg, uint32_t now) {
    netdrop::Msg m;
    if (!netdrop::decode(kPktDrop, msg.data(), msg.size(), &m) || m.from != from || m.dead >= kPorts) return;
    if (m.dead == opt_.my_slot || m.dead == 0 || !(core_.slot_mask() >> m.dead & 1u)) return;
    if (core_.drop_done(m.dead) && !drops_.count(m.dead)) return;
    if (!drops_.count(m.dead)) drop_begin(m.dead, now);
    auto it = drops_.find(m.dead);
    if (it == drops_.end()) return;
    core_.apply_drop_window(m);
    it->second.on_msg(from, m.proposal, m.heard);
    if (it->second.decided() && !it->second.settled()) last_drop_send_ = 0;
}

void Lockstep::drop_service(uint32_t now) {
    if (drops_.empty()) return;
    bool send = now - last_drop_send_ >= 100;
    for (auto it = drops_.begin(); it != drops_.end();) {
        netdrop::Agreement& ag = it->second;
        int dead = it->first;
        if (ag.decided() && !core_.drop_done(dead)) {
            uint32_t cut = ag.cutoff();
            core_.finish_drop(dead, cut);
            dropped_mask_ |= 1u << dead;
            drop_done_at_[dead] = now;
            if (hooks_.log) { char b[120]; std::snprintf(b, sizeof b, "drop: slot %d disconnected, neutral from sample %u - playing on", dead, cut); hooks_.log(b); }
            if (hooks_.on_drop) hooks_.on_drop(dead, cut);
        }
        bool done = ag.settled() || (core_.drop_done(dead) && now - drop_done_at_[dead] > 8000);
        if (send && !done) drop_send(ag);
        if (done) it = drops_.erase(it); else ++it;
    }
    if (send) last_drop_send_ = now ? now : 1;
}

void Lockstep::drop_check(uint32_t now, uint32_t n) {
    (void)n;
    if (opt_.drop_silence_ms == 0) return;
    const uint32_t mask = core_.slot_mask();
    for (int s = 1; s < kPorts; s++) {
        if (s == opt_.my_slot || !(mask >> s & 1u) || core_.drop_begun(s) || (bye_mask_ >> s & 1u)) continue;
        if (!last_rx_[s] || now - last_rx_[s] < opt_.drop_silence_ms) continue;
        int others = 0;   // survivors besides me: with nobody else left the old timeout (exit 4) ends the session
        for (int q = 0; q < kPorts; q++) if (q != s && q != opt_.my_slot && (mask >> q & 1u) && !core_.drop_begun(q) && !(bye_mask_ >> q & 1u)) others++;
        if (!others) continue;
        drop_begin(s, now);
    }
}

void Lockstep::abort_with(const std::string& why) {
    if (lost_.exchange(true)) return;
    if (hooks_.log) hooks_.log(why);
    if (hooks_.request_stop) hooks_.request_stop();
}

void Lockstep::report_mismatches() {
    LockstepCore::Mismatch m;
    while (core_.poll_mismatch(&m)) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "DESYNC at sample %u: %s hashes %016llx %016llx %016llx %016llx",
                      m.sample, opt_.hash_label.c_str(), (unsigned long long)m.hash[0], (unsigned long long)m.hash[1],
                      (unsigned long long)m.hash[2], (unsigned long long)m.hash[3]);
        if (hooks_.log) hooks_.log(buf);
        if (rs_.stale(m.sample)) { if (hooks_.log) hooks_.log("resync: that check predates the last recovery; ignored"); continue; }
        resync::Act a = rs_.on_mismatch(m.sample, sample_ < 0 ? 0u : (uint32_t)sample_);
        if (a == resync::Act::Fallback) {
            desync_ = true;
            if (hooks_.log && rs_.count() >= resync::kMaxResyncs) hooks_.log("resync: giving up after " + std::to_string(rs_.count()) + " recoveries");
            if (opt_.strict_sync && hooks_.request_stop) hooks_.request_stop();
        } else {
            rs_hash_sample_ = m.sample;
            if (hooks_.log) hooks_.log("resync: recovery requested");
        }
    }
}

void Lockstep::rs_send(int to, const resync::Msg& m) {
    auto v = resync::encode(kPktResync, m);
    net_->send(to, v.data(), (int)v.size());
}

void Lockstep::rs_on_ctl(int from, const std::vector<uint8_t>& msg) {
    resync::Msg m, reply;
    if (opt_.spectator || !resync::decode(kPktResync, msg.data(), msg.size(), &m) || m.from != from) return;
    if (m.sub == resync::Abort && opt_.my_slot != 0 && std::getenv("RETROHUB_QA") && std::getenv("N64RUN_QA_LOSE_ABORTS")) {   // QA: the first N Aborts are "lost"
        static int lost = 0;
        if (lost < std::atoi(std::getenv("N64RUN_QA_LOSE_ABORTS"))) { lost++; if (hooks_.log) hooks_.log("resync: QA dropped an Abort"); return; }
    }
    resync::Act a = rs_.on_msg(m, sample_ < 0 ? 0u : (uint32_t)sample_, &reply);
    if (a == resync::Act::Reply) {
        rs_send(from, reply);
        if (reply.sub == resync::Abort && opt_.my_slot != 0) { desync_ = true; if (hooks_.log) hooks_.log("resync: we already passed the proposed sample; recovery off"); if (opt_.strict_sync && hooks_.request_stop) hooks_.request_stop(); }
    } else if (a == resync::Act::Fallback) {
        if (opt_.my_slot == 0) { resync::Msg ab; ab.sub = resync::Abort; ab.from = 0; ab.seq = rs_.seq(); for (int i = 0; i < 3; i++) rs_send(-1, ab); }
        rs_snd_.stop();
        rs_rcv_.reset();
        desync_ = true;
        if (hooks_.log) hooks_.log("resync: a peer aborted the recovery; the desync stays");
        if (opt_.strict_sync && hooks_.request_stop) hooks_.request_stop();
    }
}

void Lockstep::rs_fail(const char* why) {
    if (hooks_.log) hooks_.log(std::string("resync: failed (") + why + "): the desync stays");
    if (opt_.my_slot == 0 && rs_.active()) { resync::Msg ab; ab.sub = resync::Abort; ab.from = 0; ab.seq = rs_.seq(); for (int i = 0; i < 3; i++) rs_send(-1, ab); }
    rs_.abort();
    rs_snd_.stop();
    rs_rcv_.reset();
    desync_ = true;
    if (opt_.strict_sync && hooks_.request_stop) hooks_.request_stop();
}

void Lockstep::rs_on_xfer(int from, const std::vector<uint8_t>& m) {
    if (m.size() < 4 || m[2] != from) return;
    const uint32_t now = hubnet::now_ms();
    if (opt_.my_slot == 0) {
        if (!rs_snd_.active() || m[3] != rs_snd_.epoch()) return;
        if (m[1] == rb::xfer::Ack && m.size() >= 4 + 2 + 8) rs_snd_.on_ack(from, rb::xfer::g16(m.data() + 4), rb::xfer::g64(m.data() + 6));
        else if (m[1] == rb::xfer::Done && m.size() >= 5 && m[4] == 1) rs_snd_.on_done(from);
        return;
    }
    if (from != 0) return;
    if ((m[1] == rb::xfer::Begin || m[1] == rb::xfer::Chunk) && m[3] == rs_done_ep_) {   // we loaded it already: our Done was lost
        if (now - rs_last_done_ > 100) {
            auto v = rb::xfer::header(rb::xfer::Done, opt_.my_slot, (uint8_t)rs_done_ep_);
            v.push_back(1);
            net_->send(0, v.data(), (int)v.size());
            rs_last_done_ = now;
        }
        return;
    }
    if (!rs_.active() || m[3] != (uint8_t)(0xC0 | (rs_.seq() & 0x3F))) return;
    if (m[1] == rb::xfer::Begin) {
        rb::xfer::Meta mt;
        if (rb::xfer::dec_begin(m.data(), m.size(), &mt) && mt.frame == rs_.target() && rs_rcv_.on_begin(m[3], mt) && hooks_.log) {
            char b[120]; std::snprintf(b, sizeof b, "resync: receiving the state of sample %u (%u bytes, %u chunks)", mt.frame, mt.comp, (unsigned)mt.chunks);
            hooks_.log(b);
        }
    } else if (m[1] == rb::xfer::Chunk && m.size() >= 6) {
        rs_rcv_.on_chunk(m[3], rb::xfer::g16(m.data() + 4), m.data() + 6, m.size() - 6);
    }
}

void Lockstep::rs_service(uint32_t now) {
    if (!net_ || !rs_.enabled()) return;
    if (opt_.my_slot == 0) {
        if (rs_.propose_due(now)) rs_send(-1, rs_.propose_msg());
    } else {
        if (rs_.req_due(now)) rs_send(0, rs_.req_msg(rs_hash_sample_));
        uint16_t nx; uint64_t bits;
        if (rs_rcv_.active() && rs_rcv_.ack_due(now, &nx, &bits)) { auto v = rb::xfer::enc_ack(opt_.my_slot, rs_rcv_.epoch(), nx, bits); net_->send(0, v.data(), (int)v.size()); }
    }
}

void Lockstep::rs_finish(uint32_t S) {
    const uint32_t now = hubnet::now_ms();
    const uint32_t stall = now - rs_t0_;
    rs_.complete();
    if (hooks_.vi_reset) hooks_.vi_reset();
    for (int q = 0; q < kPorts; q++) last_rx_[q] = now ? now : 1;   // nobody was heard during the stall: not a silent guest
    last_poll_ = 0; debt_ms_ = 0;
    if (stall > longest_stall_) longest_stall_ = stall;
    if (hooks_.log) {
        char b[200];
        std::snprintf(b, sizeof b, "resync %d/%d complete: playing on from sample %u, stalled %u ms, state %u bytes", rs_.count(), resync::kMaxResyncs, S, stall, rs_bytes_);
        hooks_.log(b);
    }
    if (hooks_.resync_done) hooks_.resync_done(S, stall);
}

void Lockstep::resync_loaded(bool ok) {
    if (!rs_wait_) return;
    if (!ok) { rs_wait_ = false; abort_with("resync: the core could not load the recovery state"); return; }
    const uint32_t S = rs_.target();
    sample_ = (int64_t)S - 1;
    rs_finish(S);
    rs_wait_ = false;
}

bool Lockstep::rs_silent(uint32_t now) const {   // a peer we depend on has sent nothing for timeout_ms (2 players: nobody else could drop it)
    for (int s = 0; s < kPorts; s++) {
        if (s == opt_.my_slot || !(core_.slot_mask() >> s & 1u) || (dropped_mask_ >> s & 1u) || core_.drop_begun(s) || (bye_mask_ >> s & 1u)) continue;
        if (opt_.my_slot != 0 && s != 0) continue;   // a guest only needs the host
        if (last_rx_[s] && now - last_rx_[s] > opt_.timeout_ms) return true;
    }
    return false;
}

bool Lockstep::rs_barrier(uint32_t n) {
    rs_t0_ = hubnet::now_ms() ? hubnet::now_ms() : 1;
    if (hooks_.log) { char b[100]; std::snprintf(b, sizeof b, "resync: stalled at sample %u (%s)", n, opt_.my_slot == 0 ? "host" : "guest"); hooks_.log(b); }
    return opt_.my_slot == 0 ? rs_host_barrier(n) : rs_guest_barrier(n);
}

bool Lockstep::rs_host_barrier(uint32_t n) {
    if (std::getenv("RETROHUB_QA") && std::getenv("N64RUN_QA_HOST_ABORT")) { rs_fail("QA forced abort"); return true; }   // QA: exercise the Abort path
    std::vector<uint8_t> data;
    uint64_t hash = 0;
    bool have = false;
    uint32_t last_send = 0;
    uint32_t xfer_t0 = 0;
    for (;;) {
        if (hooks_.idle) hooks_.idle();
        pump();
        const uint32_t now = hubnet::now_ms();
        if (!rs_.active()) return !lost_;   // a guest aborted it (rs_on_ctl did the bookkeeping)
        if (net_->failed()) { abort_with("Connection lost during a desync recovery"); return false; }
        if (spec_host_) spec_service();
        rs_service(now);
        drop_check(now, n);
        drop_service(now);
        for (int s = 1; s < kPorts; s++) if (dropped_mask_ >> s & 1u) { rs_.drop(s); rs_snd_.remove(s); }
        if (rs_silent(now)) { abort_with("Connection lost (no input from peers for 10 s)"); return false; }
        if (!have && !rs_snd_.active()) {
            int r = hooks_.resync_fetch(rs_job_, &data, &hash);
            if (r < 0 || (r == 1 && (data.empty() || data.size() > rb::xfer::kMaxChunks * rb::xfer::kChunk))) { rs_fail("the core did not save the state"); return true; }
            have = r == 1;
        }
        if (have && !rs_snd_.active() && rs_.all_acked()) {
            rb::xfer::Meta m;
            m.frame = n; m.raw = m.comp = (uint32_t)data.size(); m.flags = 0; m.hash = hash;
            m.chunks = (uint16_t)((data.size() + rb::xfer::kChunk - 1) / rb::xfer::kChunk);
            bool rel = false;
            for (int s = 1; s < kPorts; s++) if ((rs_.guests() >> s & 1u) && net_->is_relayed(s)) rel = true;
            rs_bytes_ = (uint32_t)data.size();
            rs_snd_.start((uint8_t)(0xC0 | (rs_.seq() & 0x3F)), m, std::move(data), rs_.guests() & core_.slot_mask() & ~dropped_mask_, now ? now : 1, rel ? 150.0 : 1500.0);
            xfer_t0 = now;
            if (hooks_.log) { char b[140]; std::snprintf(b, sizeof b, "resync: state saved %u ms after the stall began; sending %u bytes in %u chunks (%s)", now - rs_t0_, m.comp, (unsigned)m.chunks, rel ? "relay" : "direct"); hooks_.log(b); }
        }
        if (rs_snd_.active()) {
            rs_snd_.service(now ? now : 1, 0, [&](const uint8_t* d, size_t len) {
                for (int s = 1; s < kPorts; s++) if (rs_.guests() >> s & 1u) net_->send(s, d, (int)len);
            });
            if (rs_snd_.all_done()) {
                if (hooks_.log) { char b[120]; std::snprintf(b, sizeof b, "resync: every guest has the state (transfer %u ms, %zu chunks sent for %u)", now - xfer_t0, rs_snd_.chunks_sent(), (unsigned)rs_snd_.meta().chunks); hooks_.log(b); }
                rs_snd_.stop();
                rs_finish(n);
                return true;
            }
        }
        if (now - last_send >= 50) { send_packet(); last_send = now; }
        if (now - rs_t0_ > 60000 || (!rs_snd_.active() && now - rs_t0_ > 12000)) { rs_fail("timeout"); return true; }
        Sleep(1);
    }
}

bool Lockstep::rs_guest_barrier(uint32_t n) {
    uint32_t last_send = 0, last_status = 0;
    for (;;) {
        if (hooks_.idle) hooks_.idle();
        pump();
        const uint32_t now = hubnet::now_ms();
        if (!rs_.active()) return !lost_;   // the host aborted: play on
        if (net_->failed()) { abort_with("Connection lost during a desync recovery"); return false; }
        rs_service(now);
        drop_check(now, n);
        drop_service(now);
        if (rs_silent(now)) { abort_with("Connection lost (no input from the host for 10 s)"); return false; }
        if (rs_rcv_.complete()) break;
        if (now - last_status >= 250) { rs_send(0, rs_.ack_msg()); last_status = now; }   // status poll: the host answers Abort if it gave up (a lost Abort cannot strand us)
        if (now - last_send >= 50) { send_packet(); last_send = now; }
        if (now - rs_t0_ > 45000) { rs_fail("timeout"); abort_with("resync: the state never arrived"); return false; }
        Sleep(1);
    }
    const rb::xfer::Meta mt = rs_rcv_.meta();
    const std::vector<uint8_t>& st = rs_rcv_.payload();
    rs_bytes_ = (uint32_t)st.size();
    rs_wait_ = true;
    if (st.size() != mt.raw || !hooks_.resync_load(st, mt.hash)) {
        rs_wait_ = false;
        abort_with("resync: the recovery state did not arrive intact");
        return false;
    }
    rs_done_ep_ = rs_rcv_.epoch();
    auto v = rb::xfer::header(rb::xfer::Done, opt_.my_slot, (uint8_t)rs_done_ep_);
    v.push_back(1);
    net_->send(0, v.data(), (int)v.size());
    rs_last_done_ = hubnet::now_ms();
    rs_rcv_.reset();
    sample_ = (int64_t)n - 1;   // sample n is delivered once the core has loaded the state (resync_loaded)
    return false;
}

bool Lockstep::left_blocks(uint32_t n) const {
    for (int s = 0; s < kPorts; s++)
        if ((bye_mask_ >> s & 1u) && n > bye_last_[s]) return true;
    return false;
}

void Lockstep::adapt(uint32_t now, uint32_t stall_ms) {
    adapter_.on_poll(now, stall_ms);
    if (!opt_.adaptive_delay) return;
    int mine = adapter_.want(core_.delay(), now);
    core_.set_want(mine);
    if (opt_.my_slot == 0) {
        int wants[4];
        for (int s = 0; s < 4; s++) wants[s] = s == 0 ? mine : (core_.peer_want(s) ? core_.peer_want(s) : core_.delay());
        int d = adapter_.decide(core_.delay(), wants, core_.slot_mask(), now);
        if (d != core_.delay()) core_.order_delay(d);
    }
    if (core_.delay() != logged_delay_) {
        if (hooks_.log) {
            char b[96];
            std::snprintf(b, sizeof b, "input delay %d -> %d (sample %u)", logged_delay_, core_.delay(), (unsigned)(sample_ + 1));
            hooks_.log(b);
        }
        logged_delay_ = core_.delay();
    }
}

void Lockstep::catch_up(uint32_t now) {
    if (!hooks_.set_limiter) return;
    if (last_poll_) {
        double iv = (double)(now - last_poll_);
        if (!catching_ && iv < base_ms_ + 100.0) base_ms_ += (std::min(iv, 100.0) - base_ms_) * 0.01;   // steady-state cost of one sample
        if (iv >= base_ms_ + 100.0 && iv < 5000.0 && !catching_) debt_ms_ = std::min(1500.0, debt_ms_ + (iv - base_ms_));   // hiccup / peer wait
        else if (catching_) debt_ms_ -= std::max(0.0, base_ms_ - iv);   // time saved per sample while unthrottled
    }
    last_poll_ = now;
    if (!catching_ && debt_ms_ > 40.0) {
        catching_ = true; catch_start_ = now;
        hooks_.set_limiter(false);
        if (hooks_.log) { char b[96]; std::snprintf(b, sizeof b, "catch-up: %.0f ms behind, running unthrottled", debt_ms_); hooks_.log(b); }
    } else if (catching_ && (debt_ms_ <= 0.0 || now - catch_start_ > 4000)) {
        catching_ = false; debt_ms_ = 0;
        hooks_.set_limiter(true);
        if (hooks_.log) hooks_.log("catch-up: done");
    }
}

void Lockstep::finish_spectators() {
    if (!net_ || lost_) return;
    if (spec_host_ && spec_mask() && !spec_finished_) {
        spec_finished_ = true;   // let spectators receive the tail of the history and the end marker
        spec_host_->finish();
        uint32_t t0 = hubnet::now_ms();
        while (hubnet::now_ms() - t0 < 4000 && !net_->failed()) {
            pump();
            spec_service();
            if (spec_host_->all_acked(spec_mask()) && hubnet::now_ms() - t0 > 300) break;
            Sleep(5);
        }
        if (hooks_.log) { char b[120]; std::snprintf(b, sizeof b, "spectate: served %u samples; spectator hashes ok %u, different %u", spec_host_->total(), spec_host_->hashes_ok(), spec_host_->hashes_bad()); hooks_.log(b); }
    }
}

void Lockstep::finish() {
    if (!net_ || lost_) return;
    if (opt_.spectator && !opt_.dropin_join) return;   // just leave: the host keeps the match going without us (a drop-in joiner says bye so the host frees the port at once)
    uint8_t bye[6] = {kPktBye, (uint8_t)opt_.my_slot, 0, 0, 0, 0};
    uint32_t newest = core_.newest_local();
    std::memcpy(bye + 2, &newest, 4);
    for (int i = 0; i < 6; i++) {
        send_packet();           // the input window first, so a partner still needing our last samples gets them
        net_->send(-1, bye, 6);
        uint32_t t0 = hubnet::now_ms();
        while (hubnet::now_ms() - t0 < 25) { pump(); Sleep(2); }
    }
    finish_spectators();
}

uint32_t Lockstep::aux(int port) const {
    if (opt_.spectator) return (lost_ || peer_left_ || sample_ < 0 || port < 0 || port >= kPorts) ? 0u : (uint32_t)(spec_cur_.v[port] >> 32);
    if (lost_ || peer_left_ || !core_.port_present(port) || sample_ < 0) return 0;
    return core_.get_aux((uint32_t)sample_, port);
}

uint32_t Lockstep::spec_get_keys(int port) {
    if (lost_ || peer_left_ || !core_.port_present(port)) return 0;
    if (live_wait_) {   // live join: the snapshot is not loaded yet; these boot polls are thrown away with the state they ran in
        if (port == core_.lead_port()) { pump(); spec_service(); }
        return 0;
    }
    if (port != core_.lead_port()) return sample_ < 0 ? 0u : (uint32_t)spec_cur_.v[port];
    uint32_t n = (uint32_t)(++sample_);
    if (!t_first_) t_first_ = hubnet::now_ms() ? hubnet::now_ms() : 1;
    if (hooks_.update_local) hooks_.update_local();
    pump();
    if (hooks_.vi_poll) hooks_.vi_poll(n);
    // Same deterministic point as the players: before the inputs of sample n are delivered.
    if (opt_.hash_interval > 0 && n > 0 && n % (uint32_t)opt_.hash_interval == 0) {
        uint64_t h;
        if (hooks_.hash_rdram && hooks_.hash_rdram(&h)) {
            spec_cli_.add_local_hash(n, h);
            if (hooks_.log) {
                char buf[96];
                std::snprintf(buf, sizeof buf, "sync check sample %u %s %016llx", n, opt_.hash_label.c_str(), (unsigned long long)h);
                hooks_.log(buf);
            }
        }
    }
    if (hooks_.on_lead_poll) hooks_.on_lead_poll(n);
    if (n > 0) spec_cli_.consumed(n);
    spec_service();
    if (!spec_cli_.ready(n)) {
        uint32_t t0 = hubnet::now_ms();
        while (!spec_cli_.ready(n)) {
            if (hooks_.idle) hooks_.idle();
            pump();
            spec_service();
            uint32_t now = hubnet::now_ms();
            if (spec_cli_.finished(n)) {
                peer_left_ = true;
                if (hooks_.log) hooks_.log("spectate: the match ended");
                if (hooks_.request_stop) hooks_.request_stop();
                return 0;
            }
            if (net_->ended()) {   // the host said goodbye: give straggling datagrams a moment, then end cleanly
                if (!spec_ended_at_) spec_ended_at_ = now ? now : 1;
                if (now - spec_ended_at_ > 1500) {
                    peer_left_ = true;
                    if (hooks_.log) hooks_.log("spectate: the host ended the session");
                    if (hooks_.request_stop) hooks_.request_stop();
                    return 0;
                }
            } else if (net_->failed() || now - std::max(t0, spec_cli_.last_rx()) > opt_.timeout_ms) {
                abort_with("Connection lost (spectating: no data from the host for 10 s)");
                return 0;
            }
            Sleep(1);
        }
    }
    for (int p = 0; p < kPorts; p++) spec_cur_.v[p] = core_.port_present(p) ? spec_cli_.get(n, p) : 0;
    if (opt_.dropin_join) {
        dj_gate_.on_sample(spec_cli_.lag());
        const uint32_t now = hubnet::now_ms();
        if (dj_have_admit_ && n >= dj_adm_.j) dj_fail(11, "missed the join sample");
        else if (!dj_have_admit_ && dj_gate_.ready() && now - dj_ready_sent_ >= dropin::kReadyEveryMs) {
            if (!dj_t_ready_) { dj_t_ready_ = now ? now : 1; if (hooks_.log) hooks_.log("dropin: caught up with the match; asking the host to admit us"); }
            dropin::Msg r; r.sub = dropin::Ready; r.from = (uint8_t)opt_.my_slot; r.slot = (uint8_t)opt_.my_slot; r.pos = n;
            di_send(0, r);
            dj_ready_sent_ = now ? now : 1;
        }
        if (dj_t_ready_ && !dj_have_admit_ && now - dj_t_ready_ > dropin::kNoAdmitMs) dj_fail(11, "the host never admitted us");
    }
    if (hooks_.set_limiter) {   // far behind the live game (late join / hiccup): run unthrottled until we are close
        bool behind = spec_cli_.lag() > spectate::kLiveLagSamples;
        if (behind == spec_live_) { spec_live_ = !behind; hooks_.set_limiter(!behind); if (hooks_.log && behind) { char b[96]; std::snprintf(b, sizeof b, "spectate: catching up, %u samples behind", spec_cli_.lag()); hooks_.log(b); } }
    }
    if (n > 0 && n % 600 == 0 && hooks_.log) { char b[96]; std::snprintf(b, sizeof b, "spectate: sample %u of %u (lag %u)", n, spec_cli_.total(), spec_cli_.lag()); hooks_.log(b); }
    spec_report();
    return (uint32_t)spec_cur_.v[port];
}

uint32_t Lockstep::get_keys(int port) {
    if (opt_.spectator) {
        if (opt_.dropin_join && !lost_ && port == core_.lead_port() && dj_have_admit_ && !live_wait_ && (uint32_t)(sample_ + 1) == dj_adm_.j) dj_promote();
        if (opt_.spectator) return spec_get_keys(port);
    }
    if (lost_ || peer_left_ || !core_.port_present(port)) return 0;
    if (rs_wait_) {   // recovery state requested: polls before the core has loaded it are thrown away with the state they ran in
        if (port == core_.lead_port()) {
            pump();
            const uint32_t now = hubnet::now_ms();
            if (now - rs_last_pkt_ >= 50) { send_packet(); rs_last_pkt_ = now; }
            if (now - rs_t0_ > 20000) { rs_wait_ = false; abort_with("resync: the core never finished loading the state"); }
        }
        return 0;
    }
    if (port != core_.lead_port()) {
        // Same poll round as the lead port: return the current sample's value.
        return sample_ < 0 ? 0u : core_.get((uint32_t)sample_, port);
    }

    uint32_t n = (uint32_t)(++sample_);
    if (!t_first_) {
        t_first_ = hubnet::now_ms() ? hubnet::now_ms() : 1;
        for (int q = 0; q < kPorts; q++) if (!last_rx_[q]) last_rx_[q] = t_first_;
    }
    if (net_) drop_service(hubnet::now_ms());

    if (hooks_.update_local) hooks_.update_local();
    pump();
    if (net_) {
        rs_service(hubnet::now_ms());
        if (rs_.barrier_at(n) && !rs_barrier(n)) return 0;
    }
    if (net_ && dj_ctl_.active() && opt_.my_slot == 0 && n == dj_ctl_.cur().j && !dj_ctl_.aborting()) di_barrier(n);
    if (net_ && dj_slots_) di_release(n);
    if (dj_osd_f_ && n >= dj_osd_f_) {
        if (hooks_.on_dropin && dj_osd_slot_ < kPorts) hooks_.on_dropin(dj_osd_name_, (int)dj_osd_slot_, dj_port_[dj_osd_slot_], true);
        dj_osd_f_ = 0;
    }
    if (dj_win_ && n >= dj_win_end_) {
        dj_win_ = false;
        if (hooks_.log) {
            char b[200];
            std::snprintf(b, sizeof b, "dropin: join window (Admit .. sample %u): stalled %llu ms in total (%llu ms before the joiner's first sample, %llu ms after), longest single stall %u ms, host barrier wait %u ms",
                          n, (unsigned long long)(stall_ms_ - dj_win_stall0_), (unsigned long long)dj_pre_ms_, (unsigned long long)dj_post_ms_, dj_win_longest_, dj_barrier_ms_);
            hooks_.log(b);
        }
    }
    if (hooks_.vi_poll) hooks_.vi_poll(n);

    // Desync hash, taken at a deterministic point (before inputs of sample n are delivered).
    if (net_ && opt_.hash_interval > 0 && n > 0 && n % (uint32_t)opt_.hash_interval == 0) {
        uint64_t h;
        if (hooks_.hash_rdram && hooks_.hash_rdram(&h)) {
            core_.add_local_hash(n, h);
            if (spec_host_) spec_host_->add_hash(n, h);
            if (hooks_.log) {
                char buf[96];
                std::snprintf(buf, sizeof buf, "sync check sample %u %s %016llx", n, opt_.hash_label.c_str(), (unsigned long long)h);
                hooks_.log(buf);
            }
        } else if (!hash_warned_) { hash_warned_ = true; if (hooks_.log) hooks_.log("state hashing unavailable; desync detection disabled"); }
    }
    if (net_ && hooks_.on_lead_poll) hooks_.on_lead_poll(n);
    if (net_ && hooks_.qa_poll) hooks_.qa_poll(n);
    if (net_ && rs_.save_due(n)) {   // host: the state taken at the next interrupt is the one before sample n + 1 = S
        rs_.mark_saved();
        bool ok = hooks_.resync_save && hooks_.resync_save(&rs_job_);
        for (uint32_t t0 = hubnet::now_ms(); !ok && hooks_.resync_save && hubnet::now_ms() - t0 < 3000 && !lost_;) {   // a spectator snapshot is still being written: its worker finishes on its own
            if (hooks_.idle) hooks_.idle();
            pump();
            Sleep(5);
            ok = hooks_.resync_save(&rs_job_);
        }
        if (!ok) rs_fail("the core refused to save");
    }

    if (net_ && opt_.my_slot == 1 && n > 0) {   // QA: periodic hiccup on slot 1 ("period_samples,ms"), RETROHUB_QA=1 only
        static int per = -1, ms = 0;
        if (per < 0) { per = 0; const char* e = std::getenv("RETROHUB_QA") ? std::getenv("N64RUN_QA_STALL") : nullptr; if (e) std::sscanf(e, "%d,%d", &per, &ms); }
        if (per > 0 && n % (uint32_t)per == 0) Sleep((DWORD)ms);
    }
    uint32_t btn[kPorts] = {0, 0, 0, 0}, ax[kPorts] = {0, 0, 0, 0};
    for (int i = 0; i < n_local_; i++) {
        btn[local_ports_[i]] = hooks_.read_local ? hooks_.read_local(i) : 0u;
        if (opt_.wide && hooks_.read_local_aux) ax[local_ports_[i]] = hooks_.read_local_aux(i);
    }
    core_.schedule_local(n, btn, ax);
    send_packet();

    uint32_t stall_this = 0;
    if (net_ && !core_.ready(n)) {
        uint32_t t0 = hubnet::now_ms(), last_send = t0;
        stalled_++;
        struct Acc { uint64_t& v; uint32_t t; ~Acc() { v += hubnet::now_ms() - t; } } acc{stall_ms_, t0};
        while (!core_.ready(n)) {
            if (hooks_.idle) hooks_.idle();
            pump();
            uint32_t now = hubnet::now_ms();
            if (bye_mask_ && left_blocks(n) && !core_.ready(n)) {   // a partner finished and will send nothing more
                peer_left_ = true;
                if (hooks_.log) hooks_.log("session ended by a partner");
                if (hooks_.request_stop) hooks_.request_stop();
                return 0;
            }
            drop_check(now, n);
            drop_service(now);
            if (net_->failed() || now - t0 > opt_.timeout_ms) {
                abort_with("Connection lost (no input from peers for 10 s)");
                return 0;
            }
            if (now - last_send >= 50) { send_packet(); last_send = now; }   // heal lost packets while stalled
            Sleep(1);
        }
        stall_this = hubnet::now_ms() - t0;
        if (stall_this > longest_stall_) longest_stall_ = stall_this;
        if (dj_win_) {
            if (stall_this > dj_win_longest_) dj_win_longest_ = stall_this;
            (n < dj_win_f_ ? dj_pre_ms_ : dj_post_ms_) += stall_this;
            if (stall_this >= 60 && hooks_.log) { char b[120]; std::snprintf(b, sizeof b, "dropin: stall of %u ms at sample %u (%s the joiner's first sample %u)", stall_this, n, n < dj_win_f_ ? "before" : "after", dj_win_f_); hooks_.log(b); }
        }
    }
    if (net_) adapt(hubnet::now_ms(), stall_this);
    if (net_) catch_up(hubnet::now_ms());
    if (net_ && n > 0 && n % 600 == 0 && hooks_.log) {   // periodic latency telemetry (~10 s)
        char b[160];
        int rtt = -1, lead = 0;
        for (int s = 0; s < kPorts; s++) {
            if (s == opt_.my_slot || !(core_.slot_mask() >> s & 1u)) continue;
            hubnet::PingStats st;
            if (net_->ping_stats(s, &st) && st.n > 0) rtt = std::max(rtt, st.avg_ms);
            lead = std::max(lead, (int)core_.peer_lead(s, n));
        }
        std::snprintf(b, sizeof b, "latency: sample %u wall %u ms (lag %d ms vs 60 Hz) delay %d (floor %d) rtt %d ms stalled %llu ms peer-lead %d",
                      n, (unsigned)(hubnet::now_ms() - t_first_), (int)((hubnet::now_ms() - t_first_) - (double)n * (1000.0 / 59.94)), core_.delay(), opt_.delay, rtt, (unsigned long long)stall_ms_, lead);
        hooks_.log(b);
    }
    if (net_) report_mismatches();
    for (int p = 0; p < kPorts; p++) {   // drop-in ports: how much real input has flowed since the port's first sample
        const uint32_t f = core_.port_first(p);
        if (f && n >= f) { dj_seen_[p]++; if (core_.get(n, p)) dj_real_[p]++; }
    }
    if (net_ && n > 0 && n % 600 == 0 && hooks_.log) {
        for (int p = 0; p < kPorts; p++)
            if (dj_seen_[p]) { char b[140]; std::snprintf(b, sizeof b, "dropin: input check port %d (slot %d): %u of %u samples since sample %u carry input", p, core_.port_owner(p), dj_real_[p], dj_seen_[p], core_.port_first(p)); hooks_.log(b); }
    }
    if (spec_host_) {   // sample n is final (every port known): record it for spectators
        uint64_t v[kPorts] = {0, 0, 0, 0};
        for (int p = 0; p < kPorts; p++)
            if (core_.port_present(p)) v[p] = (uint64_t)core_.get(n, p) | (opt_.wide ? (uint64_t)core_.get_aux(n, p) << 32 : 0ull);
        spec_host_->push(v);
        if (hooks_.after_sample) hooks_.after_sample(n);
        spec_service();
    }
    return core_.get(n, port);
}

}  // namespace n64run

namespace n64run {

bool negotiate_delay(hubnet::Session& s, const DelayNegotiation& dn, const std::function<void()>& idle,
                     const std::function<void(const std::string&)>& log, int* out_delay) {
    auto say = [&](const char* fmt, ...) {
        char b[256];
        va_list ap; va_start(ap, fmt);
        std::vsnprintf(b, sizeof b, fmt, ap);
        va_end(ap);
        if (log) log(b);
    };
    uint32_t t0 = hubnet::now_ms();
    if (dn.my_slot == 0) {
      int d = 2;
      if (dn.fixed_delay >= 0) {
        d = dn.fixed_delay > 255 ? 255 : dn.fixed_delay;
        say("input delay: host fixed D=%d", d);
      } else {
        s.set_ping_interval(100);
        s.clear_ping_stats();
        while (hubnet::now_ms() - t0 < dn.measure_ms) {
            s.update();
            if (s.failed()) return false;
            int from; std::vector<uint8_t> m;
            while (s.recv(&from, &m)) {}
            if (idle) idle();
            Sleep(5);
        }
        s.set_ping_interval(500);
        std::vector<double> rtts;
        bool relayed = false;
        for (int p = 1; p < 4; p++) {
            if (!(dn.slot_mask >> p & 1u)) continue;
            hubnet::PingStats st;
            double r = 60.0;
            if (s.ping_stats(p, &st) && st.n > 0) r = st.avg_ms + (st.max_ms - st.avg_ms) * 0.5;   // average, leaning on the worst sample
            else say("input delay auto: no ping samples from slot %d, assuming 60 ms", p);
            rtts.push_back(r);
            relayed |= s.is_relayed(p);
            say("input delay auto: slot %d rtt avg=%d min=%d max=%d n=%d%s", p, st.avg_ms, st.min_ms, st.max_ms, st.n, s.is_relayed(p) ? " (relay)" : "");
        }
        double rtt = effective_rtt(rtts);
        d = auto_delay_from_rtt(rtt, dn.frame_ms, relayed);
        say("input delay auto: rtt=%.0fms -> %d%s", rtt, d, relayed ? " (relay)" : "");
      }
        // Broadcast D until every client acked.
        uint32_t need = dn.slot_mask & ~1u, got = 0, last = 0, done_at = 0;
        for (;;) {
            s.update();
            if (s.failed()) return false;
            int from; std::vector<uint8_t> m;
            while (s.recv(&from, &m))
                if (m.size() >= 3 && m[0] == kPktDelayAck && m[2] == (uint8_t)d && from > 0 && from < 4) got |= 1u << from;
            uint32_t now = hubnet::now_ms();
            if ((got & need) == need) {
                if (!done_at) done_at = now ? now : 1;
                if (now - done_at > 300) break;   // let clients finish their ack linger before the next phase
            } else if (now - last >= 50 || !last) {
                uint8_t pkt[3] = {kPktDelay, 0, (uint8_t)d};
                s.send(-1, pkt, 3);
                last = now ? now : 1;
            }
            if (now - t0 > dn.timeout_ms) { say("input delay auto: clients did not acknowledge D"); return false; }
            if (idle) idle();
            Sleep(5);
        }
        *out_delay = d;
        return true;
    }
    // client: Session answers pings in update(); wait for the host's D, ack repeats for a moment
    int d = -1;
    uint32_t got_at = 0;
    bool host_moved_on = false;
    for (;;) {
        s.update();
        if (s.failed()) return false;
        int from; std::vector<uint8_t> m;
        while (s.recv(&from, &m)) {
            if (from == 0 && m.size() >= 3 && m[0] == kPktDelay) {
                d = m[2];
                if (!got_at) got_at = hubnet::now_ms() ? hubnet::now_ms() : 1;
                uint8_t ack[3] = {kPktDelayAck, (uint8_t)dn.my_slot, (uint8_t)d};
                s.send(0, ack, 3);
            } else if (from == 0 && d >= 0) {
                host_moved_on = true;   // any post-negotiation packet (save-sync / lockstep data): host has our ack
            }
        }
        uint32_t now = hubnet::now_ms();
        // Keep acking every D rebroadcast until the host moves on (it stops once it has our ack).
        // The 5 s cap is only a safety net.
        if (d >= 0 && (host_moved_on || now - got_at > 5000)) break;
        if (now - t0 > dn.timeout_ms) { say("input delay auto: host never sent D"); return false; }
        if (idle) idle();
        Sleep(5);
    }
    say("input delay auto: host chose D=%d", d);
    if (dn.fixed_delay >= 0 && dn.fixed_delay != d) say("input delay: ignoring local --delay %d, using host's D=%d", dn.fixed_delay, d);
    *out_delay = d;
    return true;
}

CheatSync exchange_cheat_hash(hubnet::Session& s, int my_slot, uint32_t slot_mask, uint64_t hash, uint32_t timeout_ms,
                              const std::function<void()>& idle, const std::function<void(const std::string&)>& log) {
    const uint32_t others = slot_mask & ~(1u << my_slot);
    if (!others) return CheatSync::Ok;
    uint64_t theirs[8] = {};
    uint32_t heard = 0;                  // peers whose hash I have
    uint32_t heard_me[8] = {};           // per peer: their heard mask (do they have mine?)
    uint32_t t0 = hubnet::now_ms(), last = 0, done_at = 0;
    for (;;) {
        s.update();
        if (s.failed()) return CheatSync::Failed;
        int from; std::vector<uint8_t> m;
        while (s.recv(&from, &m)) {
            if (m.size() < 11 || m[0] != kPktCheats || from < 0 || from >= 8 || !(others >> from & 1u)) continue;
            std::memcpy(&theirs[from], &m[3], 8);
            heard |= 1u << from;
            heard_me[from] = m[2];
        }
        uint32_t now = hubnet::now_ms();
        if ((heard & others) == others) {
            for (int p = 0; p < 8; p++) {
                if ((others >> p & 1u) && theirs[p] != hash) {
                    char b[160];
                    std::snprintf(b, sizeof b, "cheats: slot %d has a different cheat list (%016llx vs mine %016llx)", p,
                                  (unsigned long long)theirs[p], (unsigned long long)hash);
                    if (log) log(b);
                    return CheatSync::Mismatch;
                }
            }
        }
        bool acked = (heard & others) == others;
        for (int p = 0; p < 8 && acked; p++) if ((others >> p & 1u) && !(heard_me[p] >> my_slot & 1u)) acked = false;
        if (acked) {
            if (!done_at) done_at = now ? now : 1;
            if (now - done_at > 300) { if (log) log("cheats: all peers agree on the cheat list"); return CheatSync::Ok; }
        }
        if (now - last >= 50 || !last) {
            uint8_t pkt[11] = {kPktCheats, (uint8_t)my_slot, (uint8_t)heard};
            std::memcpy(pkt + 3, &hash, 8);
            s.send(-1, pkt, 11);
            last = now ? now : 1;
        }
        if (now - t0 > timeout_ms) { if (log) log("cheats: not every peer answered the cheat-list check in time"); return CheatSync::Timeout; }
        if (idle) idle();
        Sleep(5);
    }
}

}  // namespace n64run

/* ---------------------------------------------------------------- N64 drop-in (dropin.h) */
namespace n64run {

uint32_t Lockstep::spec_mask() const { return (net_ ? net_->spectator_mask() : 0u) | dj_mask_; }

void Lockstep::di_sync_excl() { if (net_) net_->set_broadcast_exclude(dj_mask_ & ~core_.slot_mask()); }

void Lockstep::di_send(int to, const dropin::Msg& m) {
    if (!net_) return;
    auto v = dropin::encode(kPktDropIn, m);
    net_->send(to, v.data(), (int)v.size());
}

int Lockstep::dropin_knock(int slot, uint8_t caps, const std::string& name) {
    if (!net_ || opt_.spectator || opt_.my_slot != 0 || !opt_.reserved_ports || !spec_host_) return dropin::kRefuseNoPort;
    if (slot < 1 || slot >= kPorts || (core_.slot_mask() >> slot & 1u)) return dropin::kRefuseSlot;
    if (dj_mask_ >> slot & 1u) return 0;   // a repeated knock
    if (!dj_ok_ || !(caps & dropin::kCapDropIn)) { if (hooks_.log) hooks_.log("dropin: slot " + std::to_string(slot) + " refused: a player's RetroHub is too old for drop-in"); return dropin::kRefuseOldPeer; }
    if (!core_.free_reserved()) { if (hooks_.log) hooks_.log("dropin: slot " + std::to_string(slot) + " refused: no free controller port"); return dropin::kRefuseNoPort; }
    if (rs_.active() || dj_ctl_.active() || dj_mask_) return dropin::kRefuseBusy;
    dj_mask_ |= 1u << slot;
    di_sync_excl();
    dj_name_[slot] = name;
    dj_since_[slot] = hubnet::now_ms() ? hubnet::now_ms() : 1;
    if (hooks_.log) hooks_.log("dropin: slot " + std::to_string(slot) + " (" + name + ") asks to join");
    return 0;
}

void Lockstep::dropin_forget(int slot, const char* why) {
    if (slot < 0 || slot >= kPorts || !(dj_mask_ >> slot & 1u)) return;
    di_candidate_left(slot, why);
}

void Lockstep::di_candidate_left(int slot, const char* why) {
    if (hooks_.log) hooks_.log("dropin: slot " + std::to_string(slot) + " is out of the join (" + why + ")");
    dj_mask_ &= ~(1u << slot);
    di_sync_excl();
    if (dj_ctl_.active() && !dj_ctl_.aborting() && dj_ctl_.cur().slot == slot) di_abort(why);
}

void Lockstep::di_begin_admit(int slot, uint32_t joiner_pos) {
    if (dj_ctl_.active() || rs_.active() || !(dj_mask_ >> slot & 1u)) return;
    const uint32_t fm = core_.free_reserved();
    int port = -1;
    for (int p = 0; p < kPorts; p++) if (fm >> p & 1u) { port = p; break; }
    if (port < 0) { uint8_t nl[3] = {spectate::kPktNoLive, 0, dropin::kRefuseNoPort}; net_->send(slot, nl, 3); di_candidate_left(slot, "no free port"); return; }
    const uint32_t now = hubnet::now_ms();
    dropin::Msg m;
    m.from = 0; m.slot = (uint8_t)slot; m.port = (uint8_t)port; m.d = (uint8_t)core_.delay();
    m.j = (uint32_t)(sample_ + 1) + dropin::kAdmitMargin;
    double rtt = 0;
    hubnet::PingStats st;
    if (net_->ping_stats(slot, &st) && st.n > 0) rtt = std::min(250.0, (double)st.min_ms + 20.0);   // the best round trip: the joiner's main thread was busy loading the state, so its average is inflated
    const uint32_t cur = (uint32_t)(sample_ + 1);
    const uint32_t lag = joiner_pos < cur ? std::min<uint32_t>(cur - joiner_pos, 200u) : 0u;
    m.f = dropin::first_sample(m.j, m.d, rtt, lag);
    m.flags = (uint8_t)((rs_.enabled() ? dropin::kFlagResync : 0) | (masked_ ? dropin::kFlagMasked : 0));
    m.mask = (uint8_t)core_.slot_mask();
    for (int p = 0; p < kPorts; p++) m.owners[p] = (int8_t)core_.port_owner(p);
    m.name = dj_name_[slot];
    uint32_t need = (core_.slot_mask() & ~1u & ~dropped_mask_) | (1u << slot);
    dj_ctl_.begin(m, need, now);
    if (hooks_.log) {
        char b[200];
        std::snprintf(b, sizeof b, "dropin: admitting slot %d on port %d: joins at sample %u (now %u), supplies input from sample %u, delay %d, rtt %.0f ms, joiner lag %u", slot, port, m.j, (unsigned)(sample_ + 1), m.f, m.d, rtt, lag);
        hooks_.log(b);
    }
    di_apply(dj_ctl_.cur());
    di_sync_excl();   // the joiner is a player now (in the mask): it gets the input stream so it can start without waiting
    di_send(-1, dj_ctl_.cur());
}

void Lockstep::di_apply(const dropin::Msg& m) {
    const int slot = m.slot, port = m.port;
    if (slot < 1 || slot >= kPorts || !core_.join_port(port, slot, m.f)) { if (hooks_.log) hooks_.log("dropin: cannot bind port " + std::to_string(port) + " to slot " + std::to_string(slot)); return; }
    dj_slots_ |= 1u << slot;
    dj_port_[slot] = port;
    dj_seen_[port] = dj_real_[port] = 0;
    dj_applied_id_[slot] = m.id;
    if (!m.name.empty()) dj_name_[slot] = m.name;
    caps_[slot] = (uint8_t)(((m.flags & dropin::kFlagResync) ? resync::kCapResync : 0) | ((m.flags & dropin::kFlagMasked) ? resync::kCapMaskedHash : 0) | dropin::kCapDropIn);
    const uint32_t now = hubnet::now_ms();
    last_rx_[slot] = now ? now : 1;
    bye_mask_ &= ~(1u << slot);
    dropped_mask_ &= ~(1u << slot);
    if (!rs_.active()) rs_.configure(opt_.my_slot, core_.slot_mask(), caps_);
    dj_osd_f_ = m.f; dj_osd_slot_ = (uint32_t)slot; dj_osd_name_ = dj_name_[slot];
    dj_win_ = true; dj_win_end_ = m.f + 240; dj_win_stall0_ = stall_ms_; dj_win_longest_ = 0; dj_barrier_ms_ = 0; dj_win_f_ = m.f; dj_pre_ms_ = dj_post_ms_ = 0;
    if (hooks_.log) {
        char b[160];
        std::snprintf(b, sizeof b, "dropin: port %d belongs to slot %d (%s) from sample %u (join sample %u, id %u)", port, slot, dj_name_[slot].c_str(), m.f, m.j, (unsigned)m.id);
        hooks_.log(b);
    }
}

void Lockstep::di_abort(const char* why) {
    if (!dj_ctl_.active() || dj_ctl_.aborting()) return;
    const dropin::Msg c = dj_ctl_.cur();
    if (hooks_.log) hooks_.log(std::string("dropin: admit cancelled (") + why + ")");
    dj_ctl_.abort();
    core_.cancel_join(c.port, c.slot);
    dj_slots_ &= ~(1u << c.slot);
    dj_port_[c.slot] = -1;
    dj_dead_id_[c.slot] = c.id;
    dj_applied_id_[c.slot] = 0;
    dj_mask_ &= ~(1u << c.slot);
    dj_osd_f_ = 0; dj_win_ = false;
    dj_abort_msg_ = c; dj_abort_msg_.sub = dropin::Abort;
    dj_abort_sends_ = 0; dj_last_abort_ = 0;
    if (!rs_.active()) rs_.configure(opt_.my_slot, core_.slot_mask(), caps_);
}

void Lockstep::di_barrier(uint32_t n) {
    (void)n;
    const uint32_t t0 = hubnet::now_ms();
    uint32_t last_send = 0;
    while (dj_ctl_.active() && !dj_ctl_.aborting() && !dj_ctl_.all_acked() && !lost_) {
        if (hooks_.idle) hooks_.idle();
        pump();
        const uint32_t now = hubnet::now_ms();
        if (net_->failed()) { abort_with("Connection lost during a drop-in"); return; }
        if (dj_ctl_.timed_out(now)) { di_abort("not every player acknowledged the Admit in time"); break; }
        if (now - last_send >= 50) { send_packet(); last_send = now; }
        Sleep(1);
    }
    dj_barrier_ms_ += hubnet::now_ms() - t0;
    if (dj_barrier_ms_ > 20 && hooks_.log) hooks_.log("dropin: the join sample waited " + std::to_string(dj_barrier_ms_) + " ms for the acks");
}

void Lockstep::di_service(uint32_t now) {
    if (!net_) return;
    if (opt_.spectator || opt_.my_slot != 0) {
        if (!opt_.spectator && opt_.dropin_join && dj_live_until_ && (int32_t)(now - dj_live_until_) < 0 && now - dj_live_last_ >= 100) {   // joiner: keep saying Live for a while
            dropin::Msg l; l.sub = dropin::Live; l.from = (uint8_t)opt_.my_slot; l.slot = (uint8_t)opt_.my_slot; l.id = dj_adm_.id;
            di_send(0, l);
            dj_live_last_ = now ? now : 1;
        }
        return;
    }
    for (int s = 1; s < kPorts; s++) {   // candidates that vanished
        if (!(dj_mask_ >> s & 1u)) continue;
        const uint32_t last = std::max(last_rx_[s], dj_since_[s]);
        if (now - last > 4000) di_candidate_left(s, "went silent");
        else if (now - dj_since_[s] > 90000 && !dj_ctl_.active()) di_candidate_left(s, "took too long");
    }
    if (!dj_ctl_.active()) return;
    if (dj_ctl_.aborting()) {
        if (now - dj_last_abort_ >= 100) {
            di_send(-1, dj_abort_msg_);
            dj_last_abort_ = now ? now : 1;
            if (++dj_abort_sends_ >= 8) dj_ctl_.finish();
        }
    } else if (dj_ctl_.send_due(now)) di_send(-1, dj_ctl_.cur());
    else if (dj_ctl_.all_acked() && sample_ >= 0 && (uint32_t)sample_ > dj_ctl_.cur().f + 90) {   // the joiner's Live was lost; it plays
        dj_mask_ &= ~(1u << dj_ctl_.cur().slot);
        dj_ctl_.finish();
    }
}

void Lockstep::di_on_msg(int from, const std::vector<uint8_t>& msg) {
    dropin::Msg m;
    if (!dropin::decode(kPktDropIn, msg.data(), msg.size(), &m) || m.from != from) return;
    if (opt_.spectator) {   // the joiner
        if (!opt_.dropin_join || from != 0) return;
        if (m.sub == dropin::Admit && m.slot == opt_.my_slot) {
            if (!dj_have_admit_) {
                if (!live_wait_ && (uint32_t)(sample_ + 1) >= m.j) {
                    dropin::Msg n; n.sub = dropin::Nack; n.from = (uint8_t)opt_.my_slot; n.id = m.id; di_send(0, n);
                    dj_fail(11, "the Admit arrived after its join sample");
                    return;
                }
                dj_adm_ = m; dj_have_admit_ = true;
                if (hooks_.log) { char b[160]; std::snprintf(b, sizeof b, "dropin: admitted on port %d: we play from sample %u (supplying input from %u), delay %d", m.port, m.j, m.f, m.d); hooks_.log(b); }
            }
            if (dj_adm_.id == m.id) { dropin::Msg a; a.sub = dropin::Ack; a.from = (uint8_t)opt_.my_slot; a.id = m.id; di_send(0, a); }
        } else if (m.sub == dropin::Abort && dj_have_admit_ && dj_adm_.id == m.id) dj_fail(11, "the host cancelled the join");
        return;
    }
    if (opt_.my_slot == 0) {
        if (m.sub == dropin::Ready) { if ((dj_mask_ >> from & 1u) && !dj_ctl_.active() && !(dj_slots_ >> from & 1u)) di_begin_admit(from, m.pos); }
        else if (m.sub == dropin::Ack) dj_ctl_.on_ack(m);
        else if (m.sub == dropin::Nack) { if (dj_ctl_.active() && m.id == dj_ctl_.id() && !dj_ctl_.aborting()) di_abort("a player got the Admit too late"); }
        else if (m.sub == dropin::Live) {
            if (dj_ctl_.active() && !dj_ctl_.aborting() && m.id == dj_ctl_.id() && dj_ctl_.cur().slot == from) {
                if (hooks_.log) hooks_.log("dropin: slot " + std::to_string(from) + " is playing");
                dj_mask_ &= ~(1u << from);
                dj_ctl_.finish();
            }
        }
        return;
    }
    if (from != 0) return;   // another player
    if (m.sub == dropin::Admit) {
        if (m.slot < 1 || m.slot >= kPorts || m.slot == opt_.my_slot) return;
        if (dj_dead_id_[m.slot] == m.id) return;
        dropin::Msg a; a.sub = dropin::Ack; a.from = (uint8_t)opt_.my_slot; a.id = m.id;
        if ((dj_slots_ >> m.slot & 1u) && dj_applied_id_[m.slot] == m.id) { di_send(0, a); return; }   // a repeat: our Ack was lost
        if ((dj_slots_ >> m.slot & 1u) || rs_.active()) return;   // busy (a recovery is running): the host repeats
        if (sample_ >= 0 && (uint32_t)(sample_ + 1) + 1 >= m.f) { a.sub = dropin::Nack; di_send(0, a); return; }
        di_apply(m);
        di_send(0, a);
    } else if (m.sub == dropin::Abort) {
        if (m.slot < kPorts) dj_dead_id_[m.slot] = m.id;
        if (m.slot < kPorts && (dj_slots_ >> m.slot & 1u) && dj_applied_id_[m.slot] == m.id) {
            core_.cancel_join(dj_port_[m.slot], m.slot);
            dj_slots_ &= ~(1u << m.slot); dj_port_[m.slot] = -1; dj_applied_id_[m.slot] = 0; dj_osd_f_ = 0; dj_win_ = false;
            if (!rs_.active()) rs_.configure(opt_.my_slot, core_.slot_mask(), caps_);
            if (hooks_.log) hooks_.log("dropin: the host cancelled the admit of slot " + std::to_string((int)m.slot));
        }
    }
}

void Lockstep::dj_fail(int rc, const char* why) {
    if (dj_fail_rc_) return;
    dj_fail_rc_ = rc;
    if (hooks_.log) hooks_.log(std::string("dropin: failed: ") + why);
    lost_ = true;
    if (hooks_.request_stop) hooks_.request_stop();
}

void Lockstep::dj_promote() {
    const dropin::Msg a = dj_adm_;
    core_.adopt_layout(a.owners, a.mask);
    core_.join_self(a.port, a.f, a.d);
    opt_.spectator = false;
    opt_.delay = a.d;
    opt_.online = true;
    for (int i = 0; i < kPorts; i++) local_ports_[i] = -1;
    local_ports_[0] = a.port;
    n_local_ = 1;
    adapter_ = DelayAdapter(a.d < 1 ? 1 : a.d, DelayAdapter::cap_for_floor(a.d < 1 ? 1 : a.d));
    opt_.adaptive_delay = !std::getenv("N64RUN_FIXED_DELAY");
    logged_delay_ = a.d;
    masked_ = (a.flags & dropin::kFlagMasked) != 0;
    const uint8_t cap = (uint8_t)(((a.flags & dropin::kFlagResync) ? resync::kCapResync : 0) | ((a.flags & dropin::kFlagMasked) ? resync::kCapMaskedHash : 0) | dropin::kCapDropIn);
    for (int s = 0; s < 8; s++) if (core_.slot_mask() >> s & 1u) caps_[s] = cap;
    rs_.configure(opt_.my_slot, core_.slot_mask(), caps_);
    const uint32_t now = hubnet::now_ms() ? hubnet::now_ms() : 1;
    for (int q = 0; q < kPorts; q++) last_rx_[q] = now;
    t_first_ = now;
    last_poll_ = 0; debt_ms_ = 0;
    dj_live_until_ = now + 1500; dj_live_last_ = 0;
    if (hooks_.log) {
        char b[200];
        std::snprintf(b, sizeof b, "dropin: playing as slot %d on port %d from sample %u, %u ms after asking (delay %d, slots %x)", opt_.my_slot, a.port, a.j, now - dj_t_ready_, a.d, core_.slot_mask());
        hooks_.log(b);
    }
    if (hooks_.on_dropin) hooks_.on_dropin(opt_.my_name, opt_.my_slot, a.port, true);
}

void Lockstep::di_release(uint32_t n) {
    for (int s = 1; s < kPorts; s++) {
        if (!(dj_slots_ >> s & 1u) || !core_.drop_done(s) || drops_.count(s)) continue;
        if (n <= core_.drop_cutoff(s) + 4) continue;
        const int port = dj_port_[s];
        core_.leave_port(port, s);
        if (port >= 0 && port < kPorts) dj_seen_[port] = dj_real_[port] = 0;
        dj_slots_ &= ~(1u << s); dj_port_[s] = -1; dj_applied_id_[s] = 0; dj_dead_id_[s] = 0;
        dropped_mask_ &= ~(1u << s); bye_mask_ &= ~(1u << s); last_rx_[s] = 0; drop_done_at_.erase(s);
        rs_.drop(s);
        if (hooks_.log) hooks_.log("dropin: slot " + std::to_string(s) + " left; port " + std::to_string(port) + " is free again");
        if (hooks_.on_dropin) hooks_.on_dropin(dj_name_[s], s, port, false);
    }
}

}  // namespace n64run
