#pragma once
/* Connection test: STUN binding request/response and the direct-vs-relay verdict. Pure (no sockets). */
#include <cstdint>
#include <cstring>
#include <string>

namespace hubnt {

struct Mapped { bool ok = false; std::string ip; int port = 0; };

/* 20-byte RFC 5389 binding request with the given 12-byte transaction id. */
inline void build_request(const uint8_t tid[12], uint8_t out[20]) {
    out[0] = 0; out[1] = 1; out[2] = 0; out[3] = 0;
    out[4] = 0x21; out[5] = 0x12; out[6] = 0xA4; out[7] = 0x42;
    std::memcpy(out + 8, tid, 12);
}

/* Reads XOR-MAPPED-ADDRESS (or MAPPED-ADDRESS) IPv4 from a binding success response that echoes `tid`. */
inline Mapped parse_response(const uint8_t* m, int n, const uint8_t tid[12]) {
    Mapped r;
    if (n < 20 || m[0] != 0x01 || m[1] != 0x01 || std::memcmp(m + 4, "\x21\x12\xA4\x42", 4) || std::memcmp(m + 8, tid, 12)) return r;
    int len = (m[2] << 8) | m[3];
    if (20 + len > n) return r;
    for (int p = 20; p + 4 <= 20 + len;) {
        int t = (m[p] << 8) | m[p + 1], l = (m[p + 2] << 8) | m[p + 3];
        if (p + 4 + l > n) break;
        if ((t == 0x0020 || t == 0x0001) && l >= 8 && m[p + 5] == 1) {
            int port = (m[p + 6] << 8) | m[p + 7];
            uint8_t ip[4] = {m[p + 8], m[p + 9], m[p + 10], m[p + 11]};
            if (t == 0x0020) { port ^= 0x2112; ip[0] ^= 0x21; ip[1] ^= 0x12; ip[2] ^= 0xA4; ip[3] ^= 0x42; }
            r.ok = true; r.port = port;
            r.ip = std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." + std::to_string(ip[3]);
            return r;
        }
        p += 4 + ((l + 3) & ~3);
    }
    return r;
}

enum class Verdict { Direct, Relay, Blocked };
struct Result { Verdict v = Verdict::Blocked; std::string headline, detail; };

/* Same mapped address:port from two servers = the NAT keeps one mapping per local port ("cone"): peers can punch through (direct).
 * Different ports = a per-destination mapping ("symmetric"): hole punching usually fails, the relay is used.
 * One answer only: UDP works but consistency is unknown (direct is possible, relay is the safety net). None: UDP is blocked. */
inline Result verdict(const Mapped& a, const Mapped& b) {
    Result r;
    if (!a.ok && !b.ok) {
        r.v = Verdict::Blocked;
        r.headline = "UDP looks blocked";
        r.detail = "No answer from either test server. Online games will use the relay, which works but adds delay. A firewall or VPN may be blocking UDP.";
    } else if (a.ok && b.ok && a.ip == b.ip && a.port == b.port) {
        r.v = Verdict::Direct;
        r.headline = "Direct connection should work";
        r.detail = "Your router gives the same address to both servers, so friends can usually connect straight to you with the lowest delay.";
    } else if (a.ok && b.ok) {
        r.v = Verdict::Relay;
        r.headline = "Games will likely use the relay";
        r.detail = "Your router uses a different port for each destination (strict NAT). Direct connections often fail, so RetroHub falls back to the relay. It works, with a little more delay.";
    } else {
        r.v = Verdict::Direct;
        r.headline = "Probably direct (one test server answered)";
        r.detail = "UDP gets through, but only one test server replied, so the NAT type could not be confirmed. The relay covers you if a direct connection fails.";
    }
    return r;
}

inline const char* ping_text(int ms) {
    return ms < 0 ? "Lobby unreachable" : ms < 80 ? "Lobby ping: good" : ms < 200 ? "Lobby ping: fair" : "Lobby ping: slow";
}

}  // namespace hubnt
