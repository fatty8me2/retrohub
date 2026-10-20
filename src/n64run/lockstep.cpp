#include "lockstep.h"

#include <windows.h>

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
    c.delay = o.online ? o.delay : 0;   // offline: no input delay needed
    return c;
}

Lockstep::Lockstep(const LockstepOptions& opt, hubnet::Session* session, const LockstepHooks& hooks)
    : opt_(opt), core_(make_core_config(opt)), net_(opt.online ? session : nullptr), hooks_(hooks) {
    for (int p = 0; p < kPorts; p++)
        if (core_.port_local(p)) local_ports_[n_local_++] = p;
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
        if (msg[0] == kPktReady) {
            if (msg.size() >= 2 && msg[1] < 8) ready_mask_ |= 1u << msg[1];
        } else {
            core_.on_packet(from, msg.data(), msg.size());
        }
    }
}

bool Lockstep::start_barrier(uint32_t timeout_ms) {
    if (!net_) return true;
    uint32_t all = (1u << opt_.num_slots) - 1u;
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
        std::snprintf(buf, sizeof buf, "DESYNC at sample %u: RDRAM hashes %016llx %016llx %016llx %016llx",
                      m.sample, (unsigned long long)m.hash[0], (unsigned long long)m.hash[1],
                      (unsigned long long)m.hash[2], (unsigned long long)m.hash[3]);
        if (hooks_.log) hooks_.log(buf);
        if (opt_.strict_sync && hooks_.request_stop) hooks_.request_stop();
    }
}

uint32_t Lockstep::get_keys(int port) {
    if (lost_ || !core_.port_present(port)) return 0;
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
                std::snprintf(buf, sizeof buf, "sync check sample %u rdram %016llx", n, (unsigned long long)h);
                hooks_.log(buf);
            }
        } else if (!hash_warned_) { hash_warned_ = true; if (hooks_.log) hooks_.log("RDRAM hashing unavailable; desync detection disabled"); }
    }

    uint32_t btn[kPorts] = {0, 0, 0, 0};
    for (int i = 0; i < n_local_; i++)
        btn[local_ports_[i]] = hooks_.read_local ? hooks_.read_local(i) : 0u;
    core_.schedule_local(n, btn);
    send_packet();

    if (net_ && !core_.ready(n)) {
        uint32_t t0 = hubnet::now_ms(), last_send = t0;
        while (!core_.ready(n)) {
            if (hooks_.idle) hooks_.idle();
            pump();
            uint32_t now = hubnet::now_ms();
            if (net_->failed() || now - t0 > opt_.timeout_ms) {
                abort_with("Connection lost (no input from peers for 10 s)");
                return 0;
            }
            if (now - last_send >= 50) { send_packet(); last_send = now; }   // heal lost packets while stalled
            Sleep(1);
        }
    }
    if (net_) report_mismatches();
    return core_.get(n, port);
}

}  // namespace n64run
