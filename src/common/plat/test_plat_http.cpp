// plat_http test: loopback HTTP/1.1 + WebSocket server on 127.0.0.1, no internet.
#include "plat/plat_crypto.h"
#include "plat/plat_http.h"
#include "plat/plat_sock.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace plat;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static std::string b64(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63];
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '=';
    }
    return o;
}

static std::atomic<bool> g_stop{false};

static bool read_exact(socket_t s, void* buf, size_t n) {
    char* p = (char*)buf;
    while (n) {
        if (g_stop) return false;
        pollfd pf{s, POLLIN, 0};
        int r = poll(&pf, 1, 100);
        if (r == 0) continue;
        if (r < 0) return false;
        ssize_t k = recv(s, p, n, 0);
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}
static bool send_all(socket_t s, const std::string& d) {
    size_t off = 0;
    while (off < d.size()) {
        ssize_t k = send(s, d.data() + off, d.size() - off, 0);
        if (k <= 0) return false;
        off += (size_t)k;
    }
    return true;
}

static std::string frame(int op, const std::string& payload) {
    std::string f;
    f += (char)(0x80 | op);
    if (payload.size() < 126) f += (char)payload.size();
    else if (payload.size() < 65536) { f += (char)126; f += (char)(payload.size() >> 8); f += (char)(payload.size() & 255); }
    else { f += (char)127; for (int i = 7; i >= 0; --i) f += (char)((uint64_t)payload.size() >> (8 * i)); }
    return f + payload;
}

static void ws_session(socket_t s) {
    for (;;) {
        unsigned char h[2];
        if (!read_exact(s, h, 2)) return;
        int op = h[0] & 15;
        uint64_t len = h[1] & 127;
        if (len == 126) { unsigned char e[2]; if (!read_exact(s, e, 2)) return; len = (e[0] << 8) | e[1]; }
        else if (len == 127) { unsigned char e[8]; if (!read_exact(s, e, 8)) return; len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | e[i]; }
        unsigned char mask[4] = {0, 0, 0, 0};
        if ((h[1] & 128) && !read_exact(s, mask, 4)) return;
        std::string pl(len, '\0');
        if (len && !read_exact(s, &pl[0], len)) return;
        for (size_t i = 0; i < pl.size(); ++i) pl[i] ^= mask[i & 3];
        if (op == 1 || op == 2) { if (!send_all(s, frame(op, pl))) return; }
        else if (op == 9) { if (!send_all(s, frame(10, pl))) return; }
        else if (op == 8) { send_all(s, frame(8, pl)); return; }
    }
}

static void handle(socket_t s) {
    std::string head;
    char c;
    while (head.size() < 65536 && (head.size() < 4 || head.compare(head.size() - 4, 4, "\r\n\r\n") != 0)) {
        if (!read_exact(s, &c, 1)) { sock_close(s); return; }
        head += c;
    }
    std::string reqline = head.substr(0, head.find("\r\n"));
    size_t sp1 = reqline.find(' '), sp2 = reqline.find(' ', sp1 + 1);
    std::string method = reqline.substr(0, sp1), path = reqline.substr(sp1 + 1, sp2 - sp1 - 1);
    auto header = [&](const std::string& name) {
        std::string low = head;
        for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
        size_t p = low.find("\r\n" + name + ":");
        if (p == std::string::npos) return std::string();
        p += name.size() + 3;
        size_t e = head.find("\r\n", p);
        std::string v = head.substr(p, e - p);
        while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        return v;
    };
    std::string body;
    std::string cl = header("content-length");
    if (!cl.empty()) { body.resize((size_t)atoll(cl.c_str())); if (!body.empty() && !read_exact(s, &body[0], body.size())) { sock_close(s); return; } }

    auto reply = [&](const std::string& status, const std::string& extra, const std::string& b) {
        send_all(s, "HTTP/1.1 " + status + "\r\n" + extra + "Content-Length: " + std::to_string(b.size()) + "\r\nConnection: close\r\n\r\n" + b);
    };
    if (path == "/hello" && method == "GET") reply("200 OK", "X-Custom-Thing: Value1\r\nContent-Type: text/plain\r\n", "hi");
    else if (path == "/big") {
        std::string b(3 << 20, '\0');
        for (size_t i = 0; i < b.size(); ++i) b[i] = (char)(i * 31 + (i >> 8));
        reply("200 OK", "", b);
    } else if (path == "/redir") reply("302 Found", "Location: /hello\r\n", "");
    else if (path == "/echo" && method == "POST") reply("200 OK", "", body);
    else if (path == "/slow") {
        while (!g_stop) { pollfd pf{s, POLLIN, 0}; if (poll(&pf, 1, 100) > 0) { char t[64]; if (recv(s, t, sizeof t, 0) <= 0) break; } }
    } else if (path == "/ws" || path == "/wsclose") {
        std::string key = header("sec-websocket-key");
        std::string acc = b64(hash(HashAlg::Sha1, key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
        send_all(s, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + acc + "\r\n\r\n");
        if (path == "/wsclose") {
            send_all(s, frame(8, std::string("\x0f\xa1", 2)));   // 4001
            ws_session(s);
        } else ws_session(s);
    } else reply("404 Not Found", "", "no");
    sock_close(s);
}

int main() {
    sock_init();
    socket_t ls = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 16) != 0) { std::printf("bind/listen failed\n"); return 1; }
    socklen sl = sizeof a;
    getsockname(ls, (sockaddr*)&a, &sl);
    int port = ntohs(a.sin_port);
    std::string base = "http://127.0.0.1:" + std::to_string(port);

    std::vector<std::thread> conns;
    std::mutex cm;
    std::thread acceptor([&] {
        while (!g_stop) {
            pollfd pf{ls, POLLIN, 0};
            if (poll(&pf, 1, 100) <= 0) continue;
            socket_t c = accept(ls, nullptr, nullptr);
            if (c == kBadSocket) continue;
            std::lock_guard<std::mutex> lk(cm);
            conns.emplace_back(handle, c);
        }
    });

    using namespace plat::http;
    {   // GET /hello
        Request r; r.url = base + "/hello";
        Response o;
        CHECK(perform(r, &o));
        CHECK(o.status == 200);
        CHECK(o.body == "hi");
        CHECK(o.headers["x-custom-thing"] == "Value1");
        CHECK(o.error.empty());
    }
    std::string full;
    {   // /big, no streaming
        Request r; r.url = base + "/big";
        Response o;
        CHECK(perform(r, &o));
        CHECK(o.body.size() == (3u << 20));
        full = o.body;
    }
    {   // /big streaming + progress
        Request r; r.url = base + "/big";
        std::string got; uint64_t total = 0, last = 0; int chunks = 0;
        r.on_data = [&](const char* d, size_t n) { got.append(d, n); ++chunks; return true; };
        r.on_progress = [&](uint64_t done, uint64_t t) { total = t; last = done; };
        Response o;
        CHECK(perform(r, &o));
        CHECK(got == full);
        CHECK(o.body.empty());
        CHECK(total == (3u << 20));
        CHECK(last == (3u << 20));
        CHECK(chunks > 1);
    }
    {   // redirects
        Request r; r.url = base + "/redir";
        Response o;
        CHECK(perform(r, &o));
        CHECK(o.status == 200 && o.body == "hi");
        r.follow_redirects = false;
        Response o2;
        CHECK(perform(r, &o2));
        CHECK(o2.status == 302);
        CHECK(o2.headers["location"] == "/hello");
    }
    {   // timeout
        Request r; r.url = base + "/slow"; r.timeout_ms = 800;
        Response o;
        auto t0 = std::chrono::steady_clock::now();
        CHECK(!perform(r, &o));
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(o.error == "timeout");
        CHECK(s < 3.0);
        Request r2; r2.url = base + "/slow"; r2.connect_timeout_ms = 500;
        Response o2;
        t0 = std::chrono::steady_clock::now();
        CHECK(!perform(r2, &o2));
        s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(o2.error == "timeout");
        CHECK(s < 3.0);
    }
    {   // cancel from on_data
        Request r; r.url = base + "/big";
        r.on_data = [&](const char*, size_t) { return false; };
        Response o;
        CHECK(!perform(r, &o));
        CHECK(o.error == "cancelled");
    }
    {   // POST echo
        Request r; r.method = "POST"; r.url = base + "/echo";
        r.headers.push_back({"Content-Type", "application/octet-stream"});
        r.body = std::string("a\0b\xff payload", 12);
        Response o;
        CHECK(perform(r, &o));
        CHECK(o.status == 200);
        CHECK(o.body == r.body);
    }
    {   // refused
        socket_t t = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in b{}; b.sin_family = AF_INET; b.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(t, (sockaddr*)&b, sizeof b);
        socklen l2 = sizeof b; getsockname(t, (sockaddr*)&b, &l2);
        int dead = ntohs(b.sin_port);
        sock_close(t);   // bound-then-closed port: nothing listens
        Request r; r.url = "http://127.0.0.1:" + std::to_string(dead) + "/";
        Response o;
        CHECK(!perform(r, &o));
        CHECK(!o.error.empty());
        std::printf("refused error: %s\n", o.error.c_str());
    }
    {   // 404 is still success
        Request r; r.url = base + "/nope";
        Response o;
        CHECK(perform(r, &o));
        CHECK(o.status == 404);
    }

    {   // WebSocket
        ws::Socket s;
        std::string err;
        CHECK(s.connect("ws://127.0.0.1:" + std::to_string(port) + "/ws", {}, 5000, &err));
        if (!err.empty()) std::printf("ws connect err: %s\n", err.c_str());
        CHECK(s.is_open());
        CHECK(s.send_text("hello ws"));
        ws::Message m = s.recv(3000);
        CHECK(m.kind == ws::Kind::Text && m.data == "hello ws");
        std::string bin("\x00\x01\x02\x00\xff", 5);
        CHECK(s.send_binary(bin.data(), bin.size()));
        m = s.recv(3000);
        CHECK(m.kind == ws::Kind::Binary && m.data == bin);
        std::string bigbin(200000, 'x');
        CHECK(s.send_binary(bigbin.data(), bigbin.size()));
        m = s.recv(5000);
        CHECK(m.kind == ws::Kind::Binary && m.data == bigbin);
        m = s.recv(200);
        CHECK(m.kind == ws::Kind::Timeout);
        s.close(1000, "bye");
        m = s.recv(3000);
        CHECK(m.kind == ws::Kind::Closed);
        CHECK(!s.is_open());
        CHECK(!s.send_text("late"));
    }
    {   // server-initiated close
        ws::Socket s;
        std::string err;
        CHECK(s.connect("ws://127.0.0.1:" + std::to_string(port) + "/wsclose", {}, 5000, &err));
        ws::Message m = s.recv(3000);
        CHECK(m.kind == ws::Kind::Closed);
        std::printf("server close code: %d\n", m.close_code);
        CHECK(m.close_code == 4001);
        CHECK(!s.is_open());
    }
    {   // failed connect + abort
        ws::Socket s;
        std::string err;
        CHECK(!s.connect("ws://127.0.0.1:" + std::to_string(port) + "/hello", {}, 3000, &err));
        CHECK(!err.empty());
        ws::Socket s2;
        CHECK(s2.connect("ws://127.0.0.1:" + std::to_string(port) + "/ws", {}, 5000, &err));
        s2.abort();
        CHECK(!s2.is_open());
    }

    g_stop = true;
    acceptor.join();
    {
        std::lock_guard<std::mutex> lk(cm);
        for (auto& t : conns) t.join();
    }
    sock_close(ls);
    std::printf(g_fail ? "plat_http_test: %d FAILED\n" : "plat_http_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
