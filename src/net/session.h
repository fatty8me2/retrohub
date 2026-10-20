#pragma once
/* hubnet: room client for RetroHub (see docs/ARCHITECTURE.md, "/hub endpoint").
 *
 * LobbyClient  - the hub's room: members, broadcast JSON messages. WebSocket only.
 * Session      - a game session among up to 4 PCs, star topology (slot 0 = host):
 *                STUN + UDP hole punch to the host, WebSocket binary relay fallback,
 *                host forwards client<->client traffic. Unreliable datagrams.
 *
 * Both are non-blocking: call update() every tick from one thread. */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hubnet {

constexpr int kMaxSlots = 4;
constexpr int kMaxPayload = 1100;   // bytes per send()
const char* default_server();       // "wss://smash-netplay.jumpcircuit.workers.dev"

/* 6-char room code from the shared alphabet [A-HJ-NP-Z2-9]. */
std::string new_room_code();
bool valid_room_code(const std::string& code);

struct Member { int slot; std::string name; };

class LobbyClient {
  public:
    LobbyClient();
    ~LobbyClient();
    /* role_host: create room `code`; otherwise join it. */
    bool start(const std::string& server, const std::string& code, bool role_host, const std::string& name);
    void stop();
    void update();

    bool connected() const;     // welcome received
    bool failed() const;
    std::string status() const; // human readable
    int my_slot() const;
    const std::vector<Member>& members() const;

    /* Broadcast a JSON object body ({"kind":...}) to the other members. */
    void send_msg(const std::string& body_json);
    /* Pop one received {from, body_json}. */
    bool poll_msg(int* from_slot, std::string* body_json);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct SessionConfig {
    std::string server;   // wss://...
    std::string room;     // game room, e.g. "ABC234-1"
    int slot = 0;         // my slot (0 = host)
    int expected = 2;     // number of PCs in the session (slots may be sparse, e.g. {0,2})
};

struct PingStats { int n = 0, avg_ms = 0, min_ms = 0, max_ms = 0; };

class Session {
  public:
    Session();
    ~Session();
    bool start(const SessionConfig& cfg);
    void stop();
    void update();

    bool ready() const;          // every expected slot reachable
    bool failed() const;
    std::string status() const;
    bool is_relayed(int slot) const;
    int ping_ms(int slot) const; // -1 unknown
    int my_slot() const;

    /* RTT samples since the last clear (last 32), for the auto input delay. */
    bool ping_stats(int slot, PingStats* out) const;
    void clear_ping_stats();
    void set_ping_interval(uint32_t ms);   // default 500; min 20

    /* Unreliable datagram to one slot or -1 = all other slots. */
    void send(int to_slot, const void* data, int len);
    /* Pop one datagram. */
    bool recv(int* from_slot, std::vector<uint8_t>* out);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/* Monotonic milliseconds. */
uint32_t now_ms();

} // namespace hubnet
