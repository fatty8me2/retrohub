/* Discord Rich Presence transport: the local named pipe \\.\pipe\discord-ipc-0..9, handshake, SET_ACTIVITY frames (discord_logic.h).
 * One background thread; every failure is silent (Discord not running is the normal case). Nothing is sent unless discord_ui.cpp
 * passes a non-empty activity, which it only does when the user turned the feature on. */
#include "convenience.h"
#include "discord_logic.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

struct Ipc {
    std::mutex mu;
    std::condition_variable cv;
    std::string want;            // latest wanted activity JSON ("" = none)
    unsigned version = 0;
    std::atomic<int> state{0};   // 0 idle/not running, 1 connected
    bool started = false;
} G;

HANDLE open_pipe() {
    for (int i = 0; i < 10; ++i) {
        wchar_t name[64];
        std::swprintf(name, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
    }
    return INVALID_HANDLE_VALUE;
}
bool write_all(HANDLE h, const std::string& s) {
    DWORD w = 0;
    return WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr) && w == s.size();
}
/* Drains whatever Discord has answered (never blocks). */
bool drain(HANDLE h) {
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (!avail) return true;
        char buf[4096];
        DWORD got = 0;
        if (!ReadFile(h, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr)) return false;
    }
}
bool handshake(HANDLE h) {
    if (!write_all(h, hubdiscord::encode_frame(hubdiscord::Handshake, hubdiscord::handshake_json(hubdiscord::kClientId)))) return false;
    char hdr[8];
    DWORD got = 0;
    for (int i = 0; i < 40; ++i) {   // up to ~2 s for READY
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail >= 8) break;
        Sleep(50);
        if (i == 39) return false;
    }
    if (!ReadFile(h, hdr, 8, &got, nullptr) || got != 8) return false;
    uint32_t op = 0, n = 0;
    for (int i = 0; i < 4; ++i) { op |= (uint32_t)(unsigned char)hdr[i] << (8 * i); n |= (uint32_t)(unsigned char)hdr[4 + i] << (8 * i); }
    std::string body(n > 65536 ? 0 : n, '\0');
    DWORD r = 0;
    if (n > 65536 || (n && (!ReadFile(h, &body[0], n, &r, nullptr) || r != n))) return false;
    return op == hubdiscord::Frame && body.find("READY") != std::string::npos;
}

void worker() {
    HANDLE h = INVALID_HANDLE_VALUE;
    unsigned sent_version = 0;
    bool sent_something = false;
    auto last_connect_try = std::chrono::steady_clock::time_point{};
    auto last_send = std::chrono::steady_clock::time_point{};
    for (;;) {
        std::string want;
        unsigned ver;
        {
            std::unique_lock<std::mutex> lk(G.mu);
            G.cv.wait_for(lk, std::chrono::seconds(2));
            want = G.want;
            ver = G.version;
        }
        auto now = std::chrono::steady_clock::now();
        if (h != INVALID_HANDLE_VALUE && !drain(h)) { CloseHandle(h); h = INVALID_HANDLE_VALUE; G.state = 0; sent_version = 0; }
        if (want.empty() && !sent_something) { if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; G.state = 0; } continue; }
        if (h == INVALID_HANDLE_VALUE) {
            if (want.empty() || now - last_connect_try < std::chrono::seconds(20)) continue;   // retry slowly; nothing to connect for when cleared
            last_connect_try = now;
            if (!hubdiscord::client_id_valid(hubdiscord::kClientId)) continue;                // placeholder id: never talk to Discord
            h = open_pipe();
            if (h == INVALID_HANDLE_VALUE) continue;
            if (!handshake(h)) { CloseHandle(h); h = INVALID_HANDLE_VALUE; continue; }
            G.state = 1;
            sent_version = 0;
        }
        if (ver != sent_version && now - last_send >= std::chrono::seconds(5)) {   // Discord allows ~5 updates / 20 s
            std::string json = want.empty() ? hubdiscord::clear_json((int)GetCurrentProcessId(), std::to_string(ver)) : want;
            if (!write_all(h, hubdiscord::encode_frame(hubdiscord::Frame, json))) { CloseHandle(h); h = INVALID_HANDLE_VALUE; G.state = 0; sent_version = 0; continue; }
            sent_version = ver;
            sent_something = !want.empty();
            last_send = now;
        }
    }
}

}  // namespace

void discord_ipc_set(const std::string& activity_json) {
    std::lock_guard<std::mutex> lk(G.mu);
    if (G.want == activity_json) return;
    if (activity_json.empty() && !G.started) { G.want.clear(); return; }
    G.want = activity_json;
    ++G.version;
    if (!G.started) { G.started = true; std::thread(worker).detach(); }
    G.cv.notify_all();
}

std::string discord_ipc_status() {
    if (G.state == 1) return "Connected to Discord";
    return G.started ? "Waiting for Discord" : "";
}
