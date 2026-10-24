#include "lockstep.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
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
    c.delay = o.online ? o.delay : 0;   // offline: no input delay needed
    return c;
}

Lockstep::Lockstep(const LockstepOptions& opt, hubnet::Session* session, const LockstepHooks& hooks)
    : opt_(opt), core_(make_core_config(opt)), adapter_(opt.delay < 1 ? 1 : opt.delay, DelayAdapter::cap_for_floor(opt.delay < 1 ? 1 : opt.delay)), net_(opt.online ? session : nullptr), hooks_(hooks) {
    for (int p = 0; p < kPorts; p++)
        if (core_.port_local(p)) local_ports_[n_local_++] = p;
    logged_delay_ = core_.delay();
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
        if (msg[0] == kPktDelay && msg.size() >= 3 && opt_.my_slot != 0) {
            uint8_t ack[3] = {kPktDelayAck, (uint8_t)opt_.my_slot, msg[2]};   // late repeats of the host's D
            net_->send(0, ack, 3);
        } else if (msg[0] == kPktBye) {
            if (msg.size() >= 6 && msg[1] < kPorts && (int)msg[1] == from && msg[1] != opt_.my_slot) {
                uint32_t last; std::memcpy(&last, &msg[2], 4);
                if (!(bye_mask_ >> msg[1] & 1u) && hooks_.log) hooks_.log("partner in slot " + std::to_string((int)msg[1]) + " ended the session");
                bye_mask_ |= 1u << msg[1];
                bye_last_[msg[1]] = last;
                core_.peer_left(msg[1]);
            }
        } else if (msg[0] == kPktReady) {
            if (msg.size() >= 2 && msg[1] < 8) ready_mask_ |= 1u << msg[1];
        } else {
            core_.on_packet(from, msg.data(), msg.size());
        }
    }
}

bool Lockstep::start_barrier(uint32_t timeout_ms) {
    if (!net_) return true;
    uint32_t all = core_.slot_mask();
    ready_mask_ |= 1u << opt_.my_slot;
    uint32_t t0 = hubnet::now_ms(), last = 0, seen_all_at = 0;
    for (;;) {
        pump();
        uint32_t now = hubnet::now_ms();
        if (net_->failed()) return false;
        if (now - last >= 100 || last == 0) {
            uint8_t pkt[2] = {kPktReady, (uint8_t)opt_.my_slot};
            net_->send(-1, pkt, 2);
            last = now ? now : 1;
        }
        if ((ready_mask_ & all) == all) {
            // Keep announcing for a short grace period so slower peers also see us.
            if (!seen_all_at) seen_all_at = now ? now : 1;
            if (now - seen_all_at > 400) return true;
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

void Lockstep::abort_with(const std::string& why) {
    if (lost_.exchange(true)) return;
    if (hooks_.log) hooks_.log(why);
    if (hooks_.request_stop) hooks_.request_stop();
}

void Lockstep::report_mismatches() {
    LockstepCore::Mismatch m;
    while (core_.poll_mismatch(&m)) {
        desync_ = true;
        char buf[256];
        std::snprintf(buf, sizeof buf, "DESYNC at sample %u: %s hashes %016llx %016llx %016llx %016llx",
                      m.sample, opt_.hash_label.c_str(), (unsigned long long)m.hash[0], (unsigned long long)m.hash[1],
                      (unsigned long long)m.hash[2], (unsigned long long)m.hash[3]);
        if (hooks_.log) hooks_.log(buf);
        if (opt_.strict_sync && hooks_.request_stop) hooks_.request_stop();
    }
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

void Lockstep::finish() {
    if (!net_ || lost_) return;
    uint8_t bye[6] = {kPktBye, (uint8_t)opt_.my_slot, 0, 0, 0, 0};
    uint32_t newest = core_.newest_local();
    std::memcpy(bye + 2, &newest, 4);
    for (int i = 0; i < 6; i++) {
        send_packet();           // the input window first, so a partner still needing our last samples gets them
        net_->send(-1, bye, 6);
        uint32_t t0 = hubnet::now_ms();
        while (hubnet::now_ms() - t0 < 25) { pump(); Sleep(2); }
    }
}

uint32_t Lockstep::get_keys(int port) {
    if (lost_ || peer_left_ || !core_.port_present(port)) return 0;
    if (port != core_.lead_port()) {
        // Same poll round as the lead port: return the current sample's value.
        return sample_ < 0 ? 0u : core_.get((uint32_t)sample_, port);
    }

    uint32_t n = (uint32_t)(++sample_);

    if (hooks_.update_local) hooks_.update_local();
    pump();

    // Desync hash, taken at a deterministic point (before inputs of sample n are delivered).
    if (net_ && opt_.hash_interval > 0 && n > 0 && n % (uint32_t)opt_.hash_interval == 0) {
        uint64_t h;
        if (hooks_.hash_rdram && hooks_.hash_rdram(&h)) {
            core_.add_local_hash(n, h);
            if (hooks_.log) {
                char buf[96];
                std::snprintf(buf, sizeof buf, "sync check sample %u %s %016llx", n, opt_.hash_label.c_str(), (unsigned long long)h);
                hooks_.log(buf);
            }
        } else if (!hash_warned_) { hash_warned_ = true; if (hooks_.log) hooks_.log("state hashing unavailable; desync detection disabled"); }
    }

    uint32_t btn[kPorts] = {0, 0, 0, 0};
    for (int i = 0; i < n_local_; i++)
        btn[local_ports_[i]] = hooks_.read_local ? hooks_.read_local(i) : 0u;
    core_.schedule_local(n, btn);
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
            if (net_->failed() || now - t0 > opt_.timeout_ms) {
                abort_with("Connection lost (no input from peers for 10 s)");
                return 0;
            }
            if (now - last_send >= 50) { send_packet(); last_send = now; }   // heal lost packets while stalled
            Sleep(1);
        }
        stall_this = hubnet::now_ms() - t0;
        if (stall_this > longest_stall_) longest_stall_ = stall_this;
    }
    if (net_) adapt(hubnet::now_ms(), stall_this);
    if (net_ && n > 0 && n % 600 == 0 && hooks_.log) {   // periodic latency telemetry (~10 s)
        char b[160];
        int rtt = -1, lead = 0;
        for (int s = 0; s < kPorts; s++) {
            if (s == opt_.my_slot || !(core_.slot_mask() >> s & 1u)) continue;
            hubnet::PingStats st;
            if (net_->ping_stats(s, &st) && st.n > 0) rtt = std::max(rtt, st.avg_ms);
            lead = std::max(lead, (int)core_.peer_lead(s, n));
        }
        std::snprintf(b, sizeof b, "latency: sample %u delay %d (floor %d) rtt %d ms stalled %llu ms peer-lead %d",
                      n, core_.delay(), opt_.delay, rtt, (unsigned long long)stall_ms_, lead);
        hooks_.log(b);
    }
    if (net_) report_mismatches();
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

}  // namespace n64run
