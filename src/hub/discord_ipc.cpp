/* Discord Rich Presence transport: the local named pipe \\.\pipe\discord-ipc-0..9, handshake, SET_ACTIVITY frames (discord_logic.h).
 * One background thread; every failure is silent (Discord not running is the normal case). Nothing is sent unless discord_ui.cpp
 * passes a non-empty activity, which it only does when the user turned the feature on. */
#include "convenience.h"
#include "plat/plat.h"
#if RH_MAC
#include "winutil.h"
#include <cerrno>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif
#include "discord_logic.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct Ipc {
    std::mutex mu;
    std::condition_variable cv;
    std::string want;            // latest wanted activity JSON ("" = none)
    unsigned version = 0;
    std::atomic<int> state{0};   // 0 idle/not running, 1 connected
    bool started = false;
} G;

#if RH_WIN
using Conn = HANDLE;
const Conn kNoConn = kNoConn;

Conn open_pipe() {
    for (int i = 0; i < 10; ++i) {
        wchar_t name[64];
        std::swprintf(name, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != kNoConn) return h;
    }
    return kNoConn;
}
void conn_close(Conn h) { conn_close(h); }
bool write_all(Conn h, const std::string& s) {
    DWORD w = 0;
    return WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr) && w == s.size();
}
/* Bytes waiting to be read; false = the connection is gone. */
bool conn_avail(Conn h, uint32_t* avail) {
    DWORD a = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &a, nullptr)) return false;
    *avail = (uint32_t)a;
    return true;
}
bool conn_read(Conn h, void* buf, uint32_t n, uint32_t* got) {
    DWORD g = 0;
    const bool ok = ReadFile(h, buf, n, &g, nullptr) != FALSE;
    *got = (uint32_t)g;
    return ok;
}
#else
/* macOS: Discord listens on a Unix socket named discord-ipc-N in $XDG_RUNTIME_DIR, $TMPDIR or /tmp (the first that has one wins). Same framing. */
using Conn = int;
const Conn kNoConn = -1;

Conn open_pipe() {
    std::vector<std::string> dirs;
    for (const char* v : {"XDG_RUNTIME_DIR", "TMPDIR"}) {
        const std::string d = winutil::getenv_utf8(v);
        if (!d.empty()) dirs.push_back(d.back() == '/' ? d : d + "/");
    }
    dirs.push_back("/tmp/");
    for (const std::string& d : dirs) {
        for (int i = 0; i < 10; ++i) {
            const std::string path = d + "discord-ipc-" + std::to_string(i);
            sockaddr_un a{};
            a.sun_family = AF_UNIX;
            if (path.size() >= sizeof a.sun_path) continue;
            std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
            const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) continue;
            const int one = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);   // a closed Discord must not kill the hub with SIGPIPE
            if (::connect(fd, (sockaddr*)&a, (socklen_t)sizeof a) == 0) return fd;
            ::close(fd);
        }
    }
    return -1;
}
void conn_close(Conn h) { ::close(h); }
bool write_all(Conn h, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t w = ::send(h, s.data() + off, s.size() - off, 0);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return false; }
        off += (size_t)w;
    }
    return true;
}
bool conn_avail(Conn h, uint32_t* avail) {
    char b;
    const ssize_t r = ::recv(h, &b, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r == 0) return false;   // the peer closed
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { *avail = 0; return true; }
        return false;
    }
    int n = 0;
    if (::ioctl(h, FIONREAD, &n) != 0) return false;
    *avail = n > 0 ? (uint32_t)n : 0;
    return true;
}
bool conn_read(Conn h, void* buf, uint32_t n, uint32_t* got) {
    uint32_t total = 0;
    while (total < n) {
        const ssize_t r = ::recv(h, (char*)buf + total, n - total, 0);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; *got = total; return false; }
        total += (uint32_t)r;
    }
    *got = total;
    return true;
}
#endif

/* Drains whatever Discord has answered (never blocks). */
bool drain(Conn h) {
    for (;;) {
        uint32_t avail = 0;
        if (!conn_avail(h, &avail)) return false;
        if (!avail) return true;
        char buf[4096];
        uint32_t got = 0;
        if (!conn_read(h, buf, std::min<uint32_t>(avail, sizeof buf), &got)) return false;
    }
}
bool handshake(Conn h) {
    if (!write_all(h, hubdiscord::encode_frame(hubdiscord::Handshake, hubdiscord::handshake_json(hubdiscord::kClientId)))) return false;
    char hdr[8];
    uint32_t got = 0;
    for (int i = 0; i < 40; ++i) {   // up to ~2 s for READY
        uint32_t avail = 0;
        if (!conn_avail(h, &avail)) return false;
        if (avail >= 8) break;
        plat::sleep_ms(50);
        if (i == 39) return false;
    }
    if (!conn_read(h, hdr, 8, &got) || got != 8) return false;
    uint32_t op = 0, n = 0;
    for (int i = 0; i < 4; ++i) { op |= (uint32_t)(unsigned char)hdr[i] << (8 * i); n |= (uint32_t)(unsigned char)hdr[4 + i] << (8 * i); }
    std::string body(n > 65536 ? 0 : n, '\0');
    uint32_t r = 0;
    if (n > 65536 || (n && (!conn_read(h, &body[0], n, &r) || r != n))) return false;
    return op == hubdiscord::Frame && body.find("READY") != std::string::npos;
}

void worker() {
    Conn h = kNoConn;
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
        if (h != kNoConn && !drain(h)) { conn_close(h); h = kNoConn; G.state = 0; sent_version = 0; }
        if (want.empty() && !sent_something) { if (h != kNoConn) { conn_close(h); h = kNoConn; G.state = 0; } continue; }
        if (h == kNoConn) {
            if (want.empty() || now - last_connect_try < std::chrono::seconds(20)) continue;   // retry slowly; nothing to connect for when cleared
            last_connect_try = now;
            if (!hubdiscord::client_id_valid(hubdiscord::kClientId)) continue;                // placeholder id: never talk to Discord
            h = open_pipe();
            if (h == kNoConn) continue;
            if (!handshake(h)) { conn_close(h); h = kNoConn; continue; }
            G.state = 1;
            sent_version = 0;
        }
        if (ver != sent_version && now - last_send >= std::chrono::seconds(5)) {   // Discord allows ~5 updates / 20 s
            std::string json = want.empty() ? hubdiscord::clear_json((int)plat::current_pid(), std::to_string(ver)) : want;
            if (!write_all(h, hubdiscord::encode_frame(hubdiscord::Frame, json))) { conn_close(h); h = kNoConn; G.state = 0; sent_version = 0; continue; }
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
