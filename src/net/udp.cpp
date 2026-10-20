#include "net_internal.h"
#include "log.h"
#include "session.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace hubnet {
namespace detail {

static bool wsa_start()
{
    static bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}

std::string Addr::str() const
{
    char t[32];
    snprintf(t, sizeof(t), "%u.%u.%u.%u:%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port);
    return t;
}

bool parse_addr(const std::string& text, Addr* out)
{
    wsa_start();
    size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) return false;
    std::string host = text.substr(0, colon);
    long port = strtol(text.c_str() + colon + 1, nullptr, 10);
    if (port <= 0 || port > 65535) return false;
    in_addr a;
    if (inet_pton(AF_INET, host.c_str(), &a) != 1) {
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
        a = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    out->ip = ntohl(a.s_addr);
    out->port = (uint16_t)port;
    return true;
}

/* ---- UdpSocket ---- */

UdpSocket::UdpSocket() : sock_((uintptr_t)INVALID_SOCKET) {}
UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::is_open() const { return sock_ != (uintptr_t)INVALID_SOCKET; }

bool UdpSocket::open()
{
    close();
    if (!wsa_start()) return false;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    BOOL no = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &no, sizeof(no), nullptr, 0, &ret, nullptr, nullptr);
    int buf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&buf, sizeof(buf));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&buf, sizeof(buf));
    sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = 0;
    if (bind(s, (sockaddr*)&sa, sizeof(sa)) != 0) {
        closesocket(s);
        return false;
    }
    int len = sizeof(sa);
    getsockname(s, (sockaddr*)&sa, &len);
    port_ = ntohs(sa.sin_port);
    sock_ = (uintptr_t)s;
    return true;
}

void UdpSocket::close()
{
    if (is_open()) closesocket((SOCKET)sock_);
    sock_ = (uintptr_t)INVALID_SOCKET;
    port_ = 0;
}

bool UdpSocket::send_to(const Addr& to, const void* data, int len)
{
    if (!is_open()) return false;
    sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(to.ip);
    sa.sin_port = htons(to.port);
    return sendto((SOCKET)sock_, (const char*)data, len, 0, (sockaddr*)&sa, sizeof(sa)) == len;
}

int UdpSocket::recv_from(Addr* from, void* buf, int cap)
{
    if (!is_open()) return 0;
    sockaddr_in sa;
    int sl = sizeof(sa);
    int n = recvfrom((SOCKET)sock_, (char*)buf, cap, 0, (sockaddr*)&sa, &sl);
    if (n <= 0) return 0;
    from->ip = ntohl(sa.sin_addr.s_addr);
    from->port = ntohs(sa.sin_port);
    return n;
}

std::string UdpSocket::local_endpoint() const
{
    if (!is_open()) return "-";
    /* Connecting a throwaway UDP socket sends nothing but makes the OS pick the outbound interface. */
    SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe == INVALID_SOCKET) return "-";
    std::string out = "-";
    sockaddr_in remote = {};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
    if (connect(probe, (sockaddr*)&remote, sizeof(remote)) == 0) {
        sockaddr_in name = {};
        int nl = sizeof(name);
        if (getsockname(probe, (sockaddr*)&name, &nl) == 0 && name.sin_addr.s_addr != 0) {
            Addr a;
            a.ip = ntohl(name.sin_addr.s_addr);
            a.port = port_;
            out = a.str();
        }
    }
    closesocket(probe);
    return out;
}

/* ---- STUN ---- */

static const uint32_t kCookie = 0x2112A442;
static const uint32_t kRetryMs = 400, kGiveUpMs = 2000;

void Stun::start(UdpSocket* udp)
{
    std::random_device rd;
    for (auto& b : txid_) b = (uint8_t)rd();
    done_ = gave_up_ = false;
    public_ = "-";
    started_ = last_send_ = now_ms();
    started_ok_ = parse_addr("stun.cloudflare.com:3478", &server_);
    if (!started_ok_) { gave_up_ = true; net_log("STUN: can't resolve server"); return; }
    update(udp, started_);
}

void Stun::update(UdpSocket* udp, uint32_t now)
{
    if (done_ || gave_up_ || !started_ok_) return;
    if (now - started_ > kGiveUpMs) {
        gave_up_ = true;
        net_log("STUN: no answer, continuing without a public endpoint");
        return;
    }
    if (now == started_ || now - last_send_ >= kRetryMs) {
        uint8_t req[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
        memcpy(req + 8, txid_, 12);
        udp->send_to(server_, req, sizeof(req));
        last_send_ = now;
    }
}

bool Stun::feed(const uint8_t* msg, int size)
{
    if (size < 20 || msg[0] != 0x01 || msg[1] != 0x01 || msg[4] != 0x21 || msg[5] != 0x12 || msg[6] != 0xA4 ||
        msg[7] != 0x42 || memcmp(msg + 8, txid_, 12) != 0)
        return false;
    if (done_) return true;
    int length = (msg[2] << 8) | msg[3];
    int end = (20 + length < size) ? 20 + length : size;
    for (int at = 20; at + 4 <= end;) {
        int type = (msg[at] << 8) | msg[at + 1];
        int alen = (msg[at + 2] << 8) | msg[at + 3];
        const uint8_t* v = msg + at + 4;
        if (at + 4 + alen > end) break;
        if ((type == 0x0020 || type == 0x0001) && alen >= 8 && v[1] == 0x01) {
            uint16_t port = (uint16_t)((v[2] << 8) | v[3]);
            uint32_t addr = ((uint32_t)v[4] << 24) | (v[5] << 16) | (v[6] << 8) | v[7];
            if (type == 0x0020) {
                port ^= (uint16_t)(kCookie >> 16);
                addr ^= kCookie;
            }
            Addr a;
            a.ip = addr;
            a.port = port;
            public_ = a.str();
            done_ = true;
            net_log("STUN: public endpoint %s", public_.c_str());
            return true;
        }
        at += 4 + ((alen + 3) & ~3);
    }
    return true;
}

} // namespace detail
} // namespace hubnet
