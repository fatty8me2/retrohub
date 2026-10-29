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
bool is_production_server(const std::string& url);   // true for the production Worker host
/* Test/QA harnesses: server = argv[1] > $RETROHUB_TEST_SERVER. Exits(2) with a clear message if neither is set, or if it is
 * production and RETROHUB_ALLOW_PROD_TESTS=1 is not set (tests create throwaway accounts that get auto-friended to the owner). */
std::string test_server_or_exit(int argc, char** argv);
bool is_qa_run();                                    // $RETROHUB_TEST_SERVER or $RETROHUB_AUTOTEST set
const char* default_server();       // "wss://smash-netplay.jumpcircuit.workers.dev"

/* 6-char room code from the shared alphabet [A-HJ-NP-Z2-9]. */
std::string new_room_code();
bool valid_room_code(const std::string& code);

struct Member { int slot; std::string name; };
/* One lobby event, delivered in server arrival order. Msg: a member's broadcast (from_slot, body JSON).
 * HostChange: the old host left and the server renumbered member `from_slot` (named `name`) to slot 0. Slots in
 * events before a HostChange use the old numbering; consumers must not reorder across it. */
struct LobbyEvent {
    enum Type { Msg, HostChange } type = Msg;
    int from_slot = -1;
    std::string body;   // Msg
    std::string name;   // HostChange
};

class LobbyClient {
  public:
    LobbyClient();
    ~LobbyClient();
    /* role_host: create room `code`; otherwise join it. */
    bool start(const std::string& server, const std::string& code, bool role_host, const std::string& name);
    /* Spectator seat in a lobby room (role=watch): not a member (never in members()), receives the room's `msg` events and, on
     * joining, the host's last launch message with "watch":true. Refused with spectators-full / spectate-busy / no-such-room. */
    bool start_watch(const std::string& server, const std::string& code, const std::string& name);
    /* Drop-in seat (role=watch&dropin=1): like start_watch, but the server also checks that the host's launch announced a free controller port
     * ("dropin" field) and refuses with no-free-port / host-too-old / no-such-game. The game itself is then joined as a real member (SessionConfig::dropin). */
    bool start_dropin(const std::string& server, const std::string& code, const std::string& name);
    void stop();
    void update();
    /* Optional "ident" frame (account uid + secret) sent right after every welcome so the room can auto-friend
     * the people who play together. Survives stop()/start(); "" = anonymous seat. */
    void set_ident_msg(const std::string& json_frame);

    bool connected() const;     // welcome received
    bool failed() const;
    std::string status() const; // human readable
    int my_slot() const;
    const std::vector<Member>& members() const;

    /* Broadcast a JSON object body ({"kind":...}) to the other members. */
    void send_msg(const std::string& body_json);
    /* Pop the next event (message or host change) in arrival order. When it is the HostChange that promotes
     * this client, my_slot() becomes 0 as the event is returned. Servers without migration close the socket
     * with 4001 instead, which surfaces as failed()/"The host left the room". */
    bool poll_event(LobbyEvent* out);
    /* Test hook: process one server text frame as if it had arrived on the socket. */
    void inject_server_text(const std::string& text);

  private:
    bool start_impl(const std::string& server, const std::string& code, bool role_host, const std::string& name, bool watch, bool dropin = false);
    bool handle_text(const std::string& text);
    void update_tail();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct SessionConfig {
    std::string server;   // wss://...
    std::string room;     // game room, e.g. "ABC234-1"
    int slot = 0;         // my slot (0 = host)
    int expected = 2;     // number of PCs in the session (slots may be sparse, e.g. {0,2})
    uint32_t ready_timeout_ms = 0;   // 0 = default (15 s): how long to wait for everyone before failing
    bool dropin = false;             // join a running game as a real member into a free port (server: same budget gate as spectating)
    bool keep_on_guest_bye = false;  // host: a guest's clean bye does not end the session (drop-in rooms: a friend may leave and another drop in)
    bool spectate = false;           // join as a port-less spectator (role=watch): the server picks the slot, we only talk to the host
    uint32_t silence_fail_ms = 0;    // 0 = default (10 s): guest gives up when the host is silent this long after ready
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
    /* A partner said a clean "bye" and left (or the host did): the session ended on purpose, not a failure. */
    bool ended() const;
    /* Tell the partners this end is intentional. Call once before stop(); sends repeat over a few updates. */
    void say_bye();
    std::string status() const;
    bool is_relayed(int slot) const;
    int ping_ms(int slot) const; // -1 unknown
    int my_slot() const;
    /* Slots currently held by spectators (the server marks them in `members`). They never count as players: not in ready(),
     * not reached by send(-1), and their bye / departure does not end the session. */
    uint32_t spectator_mask() const;
    bool is_spectator() const;

    /* RTT samples since the last clear (last 32), for the auto input delay. */
    bool ping_stats(int slot, PingStats* out) const;
    void clear_ping_stats();
    /* Remote play: media must never ride the Cloudflare relay. Call before start(); the relay is then never activated
     * (and HUBNET_FORCE_RELAY is ignored), so a failed hole punch ends in failed() instead of a costly fallback. */
    void set_no_relay(bool v);
    uint16_t local_port() const;   // the Session's UDP port (for UPnP mapping)
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
