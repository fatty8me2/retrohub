#include "dolphin_speed.h"
#include "dolphin_speed_logic.h"
#include "dsu_server.h"
#include <windows.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace {
std::mutex g_mu;
std::unique_ptr<dsu::DsuServer> g_srv;
std::atomic<bool> g_busy{false};   // one burst at a time

void set_buttons(uint16_t b) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_srv) return;
    dsu::Pad p;
    p.buttons = b;
    g_srv->set_pad(hubds::kSlot, p);
}
}  // namespace

bool dolphin_speed_start() {
    std::lock_guard<std::mutex> g(g_mu);
    if (g_srv) return true;
    auto s = std::make_unique<dsu::DsuServer>();
    bool ok = false;
    for (int i = 0; i < hubds::kPortTries && !ok; ++i)
        if (s->start((uint16_t)(hubds::kPort + i), true)) { ok = true; hubds::active_port() = hubds::kPort + i; }
    if (!ok) return false;
    s->set_pad(hubds::kSlot, dsu::Pad());   // connected, nothing pressed
    g_srv = std::move(s);
    return true;
}

void dolphin_speed_stop() {
    std::unique_ptr<dsu::DsuServer> s;
    { std::lock_guard<std::mutex> g(g_mu); s = std::move(g_srv); }
    if (s) s->stop();
}

bool dolphin_speed_running() { std::lock_guard<std::mutex> g(g_mu); return g_srv != nullptr; }

bool dolphin_speed_connected() { std::lock_guard<std::mutex> g(g_mu); return g_srv && g_srv->clients() > 0; }

void dolphin_speed_send(const std::vector<int>& vks, std::function<void(int)> done_cb) {
    if (vks.empty() || !dolphin_speed_running() || g_busy.exchange(true)) { if (done_cb) done_cb(-1); return; }
    std::vector<hubqm::DolphinKey> keys;
    for (int sc : vks) keys.push_back(hubds::key_from_scan(sc));
    std::thread([keys, done_cb]() {
        int waited = 0;
        while (!dolphin_speed_connected() && waited < 4000) { Sleep(50); waited += 50; }
        if (!dolphin_speed_connected()) { g_busy = false; if (done_cb) done_cb(-1); return; }
        Sleep(60);   // the client re-registers once a second; the stream is already flowing
        int done = 0;
        for (const hubds::Step& st : hubds::press_steps(keys)) {
            set_buttons(st.buttons);
            Sleep((DWORD)st.ms);
            if (st.buttons) ++done;
        }
        set_buttons(0);
        g_busy = false;
        if (done_cb) done_cb(done);
    }).detach();
}
