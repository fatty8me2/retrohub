#pragma once
/* hubnet internals: UDP socket, WebSocket client, STUN, tiny JSON. Not public API. */

#include "json.h"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hubnet {
namespace detail {

/* ---- addresses ---- */
struct Addr {
    uint32_t ip = 0;     // host byte order
    uint16_t port = 0;
    bool valid() const { return ip != 0 && port != 0; }
    bool operator==(const Addr& o) const { return ip == o.ip && port == o.port; }
    std::string str() const;
};
/* "a.b.c.d:port" or "host:port" (resolves). */
bool parse_addr(const std::string& text, Addr* out);

/* ---- UDP ---- */
class UdpSocket {
  public:
    UdpSocket();
    ~UdpSocket();
    bool open();                       // bind 0.0.0.0:0, non-blocking
    void close();
    bool is_open() const;
    bool send_to(const Addr& to, const void* data, int len);
    int recv_from(Addr* from, void* buf, int cap);   // >0 bytes, 0 nothing
    uint16_t local_port() const { return port_; }
    std::string local_endpoint() const;              // "lan-ip:port" or "-"
  private:
    uintptr_t sock_;
    uint16_t port_ = 0;
};

/* ---- STUN (RFC 5389 binding request) ---- */
class Stun {
  public:
    void start(UdpSocket* udp);
    void update(UdpSocket* udp, uint32_t now);   // retransmits
    bool feed(const uint8_t* msg, int size);     // true if it was a STUN packet
    bool done() const { return done_; }
    bool gave_up() const { return gave_up_; }
    const std::string& public_endpoint() const { return public_; }
  private:
    Addr server_;
    uint8_t txid_[12] = {};
    uint32_t started_ = 0, last_send_ = 0;
    bool done_ = false, gave_up_ = false, started_ok_ = false;
    std::string public_ = "-";
};

/* ---- WebSocket (WinHTTP, background receive thread) ---- */
enum class WsState { Closed, Connecting, Open, Failed };

class WsClient {
  public:
    WsClient();
    ~WsClient();
    bool connect(const std::string& url);
    void close();
    WsState state() const { return state_; }
    std::string error() const;
    int close_code() const { return close_code_; }   // server close code (0 = none)
    bool send_text(const std::string& text);
    bool send_binary(const void* data, int size);
    bool poll_text(std::string* out);
    bool poll_binary(std::vector<uint8_t>* out);
  private:
    struct Handles;
    void run(std::string url);
    void fail(const std::string& why);
    void close_handles();
    std::unique_ptr<Handles> h_;
    mutable std::mutex mu_;
    std::atomic<WsState> state_{WsState::Closed};
    std::atomic<int> close_code_{0};
    std::atomic<bool> stop_{false};
    std::string error_;
    std::deque<std::string> text_;
    std::deque<std::vector<uint8_t>> bin_;
    std::thread thread_;
};

/* ---- JSON: the shared reader (src/common/json.h); lobby messages may carry trailing bytes, so allow them ---- */
using Json = json::Value;
inline bool json_parse(const std::string& text, Json* out)
{
    json::ParseOptions o;
    o.allow_trailing = true;
    return json::parse(text, out, o);
}
std::string url_encode(const std::string& s);

/* QA-only switches (HUBNET_LOOPBACK) are honoured only with an explicit RETROHUB_QA=1, so a stray variable in a
 * normal user session can never change how the network code behaves. */
inline bool qa_mode() { const char* e = std::getenv("RETROHUB_QA"); return e && *e && std::string(e) != "0"; }
inline bool loopback_mode() { return qa_mode() && std::getenv("HUBNET_LOOPBACK") != nullptr; }

/* Join "server" + "/hub?..." */
std::string hub_url(const std::string& server, const std::string& room, bool host, const std::string& name, int slot);

} // namespace detail
} // namespace hubnet
