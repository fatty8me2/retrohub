#include "dsu_server.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace dsu {

uint32_t crc32(const uint8_t* p, size_t n) {
    static uint32_t tab[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); }
static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF); }
static void put64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back((x >> (8 * i)) & 0xFF); }
static void putf(std::vector<uint8_t>& v, float f) { uint32_t u; std::memcpy(&u, &f, 4); put32(v, u); }
static uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

std::vector<uint8_t> wrap(uint32_t id, const std::vector<uint8_t>& body, bool from_server) {
    std::vector<uint8_t> v;
    const char* m = from_server ? "DSUS" : "DSUC";
    v.insert(v.end(), m, m + 4);
    put16(v, kProtocolVersion);
    put16(v, (uint16_t)body.size());
    put32(v, 0);   /* CRC placeholder */
    put32(v, id);
    v.insert(v.end(), body.begin(), body.end());
    uint32_t c = crc32(v.data(), v.size());
    for (int i = 0; i < 4; ++i) v[8 + i] = (c >> (8 * i)) & 0xFF;
    return v;
}

bool parse(const uint8_t* p, size_t n, bool expect_server, uint32_t* type, std::vector<uint8_t>* payload, uint32_t* sender) {
    if (n < kHeaderSize + 4) return false;
    if (std::memcmp(p, expect_server ? "DSUS" : "DSUC", 4) != 0) return false;
    uint16_t ver = p[4] | (p[5] << 8), len = p[6] | (p[7] << 8);
    if (ver > kProtocolVersion || (size_t)len + kHeaderSize > n || len < 4) return false;
    std::vector<uint8_t> tmp(p, p + kHeaderSize + len);
    uint32_t want = get32(p + 8);
    std::memset(tmp.data() + 8, 0, 4);
    if (crc32(tmp.data(), tmp.size()) != want) return false;
    if (sender) *sender = get32(p + 12);
    *type = get32(p + kHeaderSize);
    if (payload) payload->assign(p + kHeaderSize + 4, p + kHeaderSize + len);
    return true;
}

static void shared_header(std::vector<uint8_t>& v, int slot, bool connected) {
    v.push_back((uint8_t)slot);
    v.push_back(connected ? 2 : 0);   /* 0 not connected, 2 connected */
    v.push_back(connected ? 2 : 0);   /* model: 2 = full gyro */
    v.push_back(connected ? 2 : 0);   /* connection: 2 = bluetooth */
    const uint8_t mac[6] = {0, 0, 0, 0, 0, (uint8_t)(slot + 1)};
    v.insert(v.end(), mac, mac + 6);
    v.push_back(connected ? 0x05 : 0);   /* battery: full */
}

std::vector<uint8_t> build_version(uint32_t id) {
    std::vector<uint8_t> b;
    put32(b, kMsgVersion);
    put16(b, kProtocolVersion);
    return wrap(id, b);
}

std::vector<uint8_t> build_info(uint32_t id, int slot, bool connected) {
    std::vector<uint8_t> b;
    put32(b, kMsgInfo);
    shared_header(b, slot, connected);
    b.push_back(0);
    return wrap(id, b);
}

static uint8_t to_u8(int16_t v) { return (uint8_t)(((int)v + 32768) >> 8); }

std::vector<uint8_t> build_data(uint32_t id, int slot, bool connected, uint32_t pkt, uint64_t ts_us, const Pad& p) {
    std::vector<uint8_t> b;
    put32(b, kMsgData);
    shared_header(b, slot, connected);
    b.push_back(connected ? 1 : 0);
    put32(b, pkt);
    uint8_t b1 = 0, b2 = 0;
    const uint16_t k = p.buttons;
    if (k & kLeft) b1 |= 0x80;
    if (k & kDown) b1 |= 0x40;
    if (k & kRight) b1 |= 0x20;
    if (k & kUp) b1 |= 0x10;
    if (k & kStart) b1 |= 0x08;
    if (k & kR3) b1 |= 0x04;
    if (k & kL3) b1 |= 0x02;
    if (k & kBack) b1 |= 0x01;
    if (k & kY) b2 |= 0x80;
    if (k & kB) b2 |= 0x40;
    if (k & kA) b2 |= 0x20;
    if (k & kX) b2 |= 0x10;
    if (k & kRB) b2 |= 0x08;
    if (k & kLB) b2 |= 0x04;
    if (p.rt > 127) b2 |= 0x02;
    if (p.lt > 127) b2 |= 0x01;
    b.push_back(b1); b.push_back(b2);
    b.push_back(0);   /* PS/home */
    b.push_back(0);   /* touch button */
    b.push_back(to_u8(p.lx)); b.push_back(to_u8(p.ly)); b.push_back(to_u8(p.rx)); b.push_back(to_u8(p.ry));
    b.push_back(k & kLeft ? 255 : 0); b.push_back(k & kDown ? 255 : 0); b.push_back(k & kRight ? 255 : 0); b.push_back(k & kUp ? 255 : 0);
    b.push_back(k & kY ? 255 : 0); b.push_back(k & kB ? 255 : 0); b.push_back(k & kA ? 255 : 0); b.push_back(k & kX ? 255 : 0);
    b.push_back(k & kRB ? 255 : 0); b.push_back(k & kLB ? 255 : 0); b.push_back(p.rt); b.push_back(p.lt);
    for (int i = 0; i < 12; ++i) b.push_back(0);   /* two touch points, unused */
    put64(b, ts_us);
    for (int i = 0; i < 3; ++i) putf(b, p.accel[i]);
    for (int i = 0; i < 3; ++i) putf(b, p.gyro[i]);
    return wrap(id, b);
}

std::vector<uint8_t> build_info_request(uint32_t cid, const std::vector<uint8_t>& slots) {
    std::vector<uint8_t> b;
    put32(b, kMsgInfo);
    put32(b, (uint32_t)slots.size());
    b.insert(b.end(), slots.begin(), slots.end());
    return wrap(cid, b, false);
}

std::vector<uint8_t> build_data_request(uint32_t cid, int slot) {
    std::vector<uint8_t> b;
    put32(b, kMsgData);
    b.push_back(slot < 0 ? 0 : 1);
    b.push_back(slot < 0 ? 0 : (uint8_t)slot);
    for (int i = 0; i < 6; ++i) b.push_back(0);
    return wrap(cid, b, false);
}

/* ------------------------------------------------------------------ */

using Clock = std::chrono::steady_clock;

struct DsuServer::Impl {
    SOCKET s = INVALID_SOCKET;
    uint16_t port = 0;
    uint32_t server_id = 0;
    std::thread th;
    std::atomic<bool> run{false};
    std::mutex mu;
    Pad pads[kSlots];
    bool conn[kSlots] = {};
    uint32_t pkt[kSlots] = {};
    bool announced[kSlots] = {};   /* clients were told this slot is connected: send one last 'disconnected' packet when it drops */
    struct Client { sockaddr_in addr; int slot; Clock::time_point seen; };   /* slot -1 = all */
    std::vector<Client> clients;
    std::atomic<unsigned long long> sent{0};
    Clock::time_point t0 = Clock::now();
    bool wsa = false;

    void send_to(const sockaddr_in& a, const std::vector<uint8_t>& v) {
        if (sendto(s, (const char*)v.data(), (int)v.size(), 0, (const sockaddr*)&a, sizeof a) > 0) ++sent;
    }
    void handle(const uint8_t* p, size_t n, const sockaddr_in& from) {
        uint32_t type, cid;
        std::vector<uint8_t> pl;
        if (!parse(p, n, false, &type, &pl, &cid)) return;
        std::lock_guard<std::mutex> g(mu);
        if (type == kMsgVersion) send_to(from, build_version(server_id));
        else if (type == kMsgInfo) {
            if (pl.size() < 4) return;
            uint32_t cnt = get32(pl.data());
            for (uint32_t i = 0; i < cnt && i < 4 && 4 + i < pl.size(); ++i) {
                int sl = pl[4 + i];
                if (sl < kSlots) send_to(from, build_info(server_id, sl, conn[sl]));
            }
        } else if (type == kMsgData) {
            int slot = -1;
            if (pl.size() >= 2 && (pl[0] & 1)) slot = pl[1] < kSlots ? pl[1] : -2;
            if (slot == -2) return;
            bool found = false;
            for (auto& c : clients)
                if (c.addr.sin_addr.s_addr == from.sin_addr.s_addr && c.addr.sin_port == from.sin_port && c.slot == slot) { c.seen = Clock::now(); found = true; }
            if (!found) clients.push_back({from, slot, Clock::now()});
        }
    }
    void push() {
        std::lock_guard<std::mutex> g(mu);
        auto now = Clock::now();
        for (size_t i = 0; i < clients.size();) {
            if (now - clients[i].seen > std::chrono::seconds(5)) { clients.erase(clients.begin() + i); continue; }
            for (int sl = 0; sl < kSlots; ++sl) {
                if (clients[i].slot != -1 && clients[i].slot != sl) continue;
                if (!conn[sl]) { if (announced[sl]) { uint64_t us0 = std::chrono::duration_cast<std::chrono::microseconds>(now - t0).count(); send_to(clients[i].addr, build_data(server_id, sl, false, pkt[sl]++, us0, pads[sl])); } continue; }
                uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(now - t0).count();
                send_to(clients[i].addr, build_data(server_id, sl, true, pkt[sl]++, us, pads[sl]));
            }
            ++i;
        }
        for (int sl = 0; sl < kSlots; ++sl) announced[sl] = conn[sl] && !clients.empty();
    }
    void loop() {
        auto last = Clock::now();
        uint8_t buf[512];
        while (run) {
            fd_set fs; FD_ZERO(&fs); FD_SET(s, &fs);
            timeval tv{0, 2000};
            if (select(0, &fs, nullptr, nullptr, &tv) > 0) {
                sockaddr_in from; int fl = sizeof from;
                int n = recvfrom(s, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
                if (n > 0) handle(buf, (size_t)n, from);
            }
            if (Clock::now() - last >= std::chrono::milliseconds(4)) { last = Clock::now(); push(); }   /* ~250 Hz */
        }
    }
};

DsuServer::DsuServer() : d_(new Impl) {}
DsuServer::~DsuServer() { stop(); delete d_; }

bool DsuServer::start(uint16_t port, bool loopback_only) {
    if (d_->run) return true;
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
    d_->wsa = true;
    d_->s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (d_->s == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(d_->s, (sockaddr*)&a, sizeof a) != 0) { closesocket(d_->s); d_->s = INVALID_SOCKET; return false; }
    int al = sizeof a;
    getsockname(d_->s, (sockaddr*)&a, &al);
    d_->port = ntohs(a.sin_port);
    d_->server_id = (uint32_t)GetTickCount() ^ 0x5EED5EEDu;
    d_->run = true;
    d_->th = std::thread([this] { d_->loop(); });
    return true;
}

void DsuServer::stop() {
    if (!d_->run.exchange(false)) return;
    if (d_->th.joinable()) d_->th.join();
    closesocket(d_->s);
    d_->s = INVALID_SOCKET;
}

void DsuServer::set_connected(int slot, bool on) { if (slot < 0 || slot >= kSlots) return; std::lock_guard<std::mutex> g(d_->mu); d_->conn[slot] = on; }
void DsuServer::set_pad(int slot, const Pad& p) { if (slot < 0 || slot >= kSlots) return; std::lock_guard<std::mutex> g(d_->mu); d_->pads[slot] = p; d_->conn[slot] = true; }
uint16_t DsuServer::port() const { return d_->port; }
int DsuServer::clients() const {
    std::lock_guard<std::mutex> g(d_->mu);
    int n = 0;
    for (auto& c : d_->clients) if (Clock::now() - c.seen <= std::chrono::seconds(5)) ++n;
    return n;
}
unsigned long long DsuServer::packets_sent() const { return d_->sent; }

}  // namespace dsu
