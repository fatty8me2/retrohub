/* Session: star-topology game session (see session.h / docs/ARCHITECTURE.md).
 *
 * UDP packet types (first byte; relay frames carry the same packet prefixed by
 * the destination/sender slot byte added by the server):
 *   'H' hello    [H, from]
 *   'A' helloack [A, from]
 *   'D' data     [D, from, to (0xFF = all), payload...]
 *   'P' ping     [P, from, ts u32 LE]
 *   'Q' pong     [Q, from, ts u32 LE]
 *   'R' roster   [R, 0, mask]     host -> clients, bit s set = slot s reachable from the host
 *   'B' bye      [B, from]        clean end: the sender is leaving on purpose (not a failure)
 */
#include <chrono>
#include <algorithm>
#include "net_internal.h"
#include "log.h"
#include "session.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace hubnet {

using namespace detail;

namespace {

constexpr uint32_t kRelayAfterMs = 3000;
constexpr uint32_t kNotReadyFailMs = 15000;
constexpr uint32_t kSilenceFailMs = 10000;
constexpr uint32_t kDirectFreshMs = 3000;
constexpr uint32_t kRelayFreshMs = 3000;
constexpr uint32_t kPingMs = 500;
constexpr uint32_t kRosterMs = 250;
constexpr uint32_t kWsKeepaliveMs = 20000;
constexpr uint32_t kEndpointWaitMs = 2200;
constexpr size_t kMaxQueue = 8192;

struct Peer {
    bool present = false;
    Addr pub, loc;            // candidates from the server
    bool has_cand = false;
    Addr direct;              // verified direct address
    bool has_direct = false, has_relay = false;
    uint32_t last_direct = 0, last_relay = 0;
    uint32_t since = 0;       // when the peer first showed up in the room
    uint32_t last_rx = 0;
    uint32_t last_hello = 0, last_relay_hello = 0, last_ping = 0, last_roster = 0;
    bool relay_active = false;
    int ping = -1;
    int ring[32] = {0};       // recent RTT samples (ms)
    int ring_n = 0, ring_i = 0;
    void add_ping(int v) { ring[ring_i] = v; ring_i = (ring_i + 1) % 32; if (ring_n < 32) ring_n++; }
    void reset()
    {
        bool p = present;
        *this = Peer();
        present = p;
    }
};

int popcount(unsigned v)
{
    int n = 0;
    for (; v; v >>= 1) n += v & 1;
    return n;
}

void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Network simulator (QA): only active when HUBNET_SIM_ENABLE=1 is also set. HUBNET_SIM_LATENCY_MS / _JITTER_MS / _LOSS_PCT / _SEED, applied to every
 * outgoing datagram (direct UDP and relay frames). Relay frames stay in order (they ride TCP). */
struct SimPkt {
    uint32_t due;
    bool relay;
    Addr to;
    std::vector<uint8_t> data;
};

int env_int(const char* n, int def)
{
    const char* v = getenv(n);
    return (v && v[0]) ? atoi(v) : def;
}

} // namespace

struct Session::Impl {
    SessionConfig cfg;
    bool started = false, failed = false, ready_ever = false, ended = false;
    bool bye_from[kMaxSlots] = {};   // a partner said a clean 'bye' before leaving
    bool departing = false;          // the host left without a bye yet: wait briefly for one, then fail
    uint32_t depart_deadline = 0;
    std::string depart_why;
    bool force_relay = false;
    bool no_relay = false;   // media sessions (remote play): never fall back to the WebSocket relay
    std::string status = "idle";
    uint32_t start_ms = 0;
    int slot = 0;
    uint32_t spec_mask = 0;    // slots the server flagged as spectators
    bool spectator = false;    // this end is a spectator

    UdpSocket udp;
    Stun stun;
    WsClient ws;
    bool welcomed = false, endpoint_sent = false;
    uint32_t last_ws_keepalive = 0;
    uint32_t ws_retry_at = 0;      // join: retry while the host's room doesn't exist yet
    bool ws_retrying = false;

    Peer peers[kMaxSlots];
    std::deque<std::pair<int, std::vector<uint8_t>>> inbox;
    unsigned roster_mask = 0;      // client: last roster from host; host: computed
    uint32_t ping_every = kPingMs;

    // simulator
    int sim_latency = 0, sim_jitter = 0, sim_loss = 0;
    bool sim_on = false;
    std::mt19937 sim_rng;
    std::vector<SimPkt> sim_q;
    uint32_t sim_last_relay_due = 0;
    // stall spikes (QA): HUBNET_SIM_SPIKE_MS (length) every HUBNET_SIM_SPIKE_EVERY_MS (wall-clock phase, shared by peers on one PC), only after HUBNET_SIM_SPIKE_AT_MS since session start; outgoing datagrams of the
    // slot HUBNET_SIM_SPIKE_SLOT (-1: all) sent inside the window are held until it ends (a 4-5 s round-trip spike).
    int sim_spike_ms = 0, sim_spike_every = 0, sim_spike_at = 0, sim_spike_slot = -1;
    uint32_t sim_t0 = 0;
    // relay traffic meter (logged every 5 s once relaying): frames/bytes sent to / received from the lobby relay
    uint32_t rl_tx = 0, rl_rx = 0, rl_txb = 0, rl_rxb = 0, rl_at = 0;

    void sim_init(int slot_)
    {
        sim_latency = env_int("HUBNET_SIM_LATENCY_MS", 0);
        sim_jitter = env_int("HUBNET_SIM_JITTER_MS", 0);
        sim_loss = env_int("HUBNET_SIM_LOSS_PCT", 0);
        sim_spike_ms = env_int("HUBNET_SIM_SPIKE_MS", 0);
        sim_spike_every = env_int("HUBNET_SIM_SPIKE_EVERY_MS", 0);
        sim_spike_at = env_int("HUBNET_SIM_SPIKE_AT_MS", 10000);
        sim_spike_slot = env_int("HUBNET_SIM_SPIKE_SLOT", -1);
        if (sim_spike_slot >= 0 && sim_spike_slot != slot_) sim_spike_ms = 0;
        sim_t0 = now_ms();
        sim_on = env_int("HUBNET_SIM_ENABLE", 0) == 1 && (sim_latency > 0 || sim_jitter > 0 || sim_loss > 0 || sim_spike_ms > 0);   // opt-in so a stray env var cannot degrade real play
        int seed = env_int("HUBNET_SIM_SEED", 0);
        sim_rng.seed(seed ? (uint32_t)seed + (uint32_t)slot_ * 7919u : std::random_device()());
        sim_q.clear();
        sim_last_relay_due = 0;
        if (sim_on) net_log("Session %d: NETWORK SIMULATOR latency=%dms jitter=%dms loss=%d%%", slot_, sim_latency, sim_jitter, sim_loss);
    }
    /* Queue (or drop) one outgoing datagram. */
    void sim_out(bool relay, const Addr& to, const uint8_t* p, int len, uint32_t now)
    {
        if (sim_loss > 0 && (int)(sim_rng() % 100) < sim_loss) return;
        uint32_t d = (uint32_t)sim_latency;
        if (sim_jitter > 0) d += sim_rng() % (uint32_t)(sim_jitter + 1);
        uint32_t due = now + d;
        if (sim_spike_ms > 0 && (int32_t)(now - sim_t0) >= sim_spike_at) {
            // phase from the wall clock so every peer on this PC stalls in the same window (whole-network spike)
            int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            int64_t k = sim_spike_every > 0 ? ms % sim_spike_every : 0;
            if (sim_spike_every > 0 && k < sim_spike_ms) due = std::max(due, now + (uint32_t)(sim_spike_ms - k));
        }
        if (relay) {
            if ((int32_t)(due - sim_last_relay_due) < 0) due = sim_last_relay_due;
            sim_last_relay_due = due;
        }
        sim_q.push_back({due, relay, to, std::vector<uint8_t>(p, p + len)});
    }
    void sim_flush(uint32_t now)
    {
        for (size_t i = 0; i < sim_q.size();) {
            if ((int32_t)(now - sim_q[i].due) >= 0) {
                SimPkt pk = std::move(sim_q[i]);
                sim_q.erase(sim_q.begin() + i);
                if (pk.relay) ws.send_binary(pk.data.data(), (int)pk.data.size());
                else udp.send_to(pk.to, pk.data.data(), (int)pk.data.size());
            } else i++;
        }
    }
    void out_udp(const Addr& to, const uint8_t* p, int len)
    {
        if (sim_on) sim_out(false, to, p, len, now_ms());
        else udp.send_to(to, p, len);
    }

    bool is_host() const { return slot == 0; }
    bool talks_to(int s) const
    {
        if (s < 0 || s >= kMaxSlots || s == slot) return false;
        return is_host() ? s > 0 : s == 0;
    }

    void connect_ws()
    {
        std::string url = hub_url(cfg.server, cfg.room, is_host(), "n64run", slot, spectator) + "&caps=reasons";
        net_log("Session: connecting %s", url.c_str());
        ws.connect(url);
        welcomed = false;
        endpoint_sent = false;
    }

    bool direct_fresh(const Peer& p, uint32_t now) const { return p.has_direct && now - p.last_direct < kDirectFreshMs; }
    bool relay_fresh(const Peer& p, uint32_t now) const { return p.has_relay && now - p.last_relay < kRelayFreshMs; }
    bool reachable(int s, uint32_t now) const
    {
        const Peer& p = peers[s];
        return direct_fresh(p, now) || relay_fresh(p, now);
    }

    void fail(const std::string& why)
    {
        if (failed || ended) return;
        failed = true;
        status = why;
        net_log("Session %d: FAILED: %s", slot, why.c_str());
    }

    /* A partner left. If it said a clean bye first, the session simply ended; otherwise it is a failure. */
    void departed(int s, const std::string& why)
    {
        if (bye_from[s]) { end_clean(s); return; }
        // The bye can trail the membership change by a tick (UDP vs the lobby socket): give it a moment.
        if (!departing) {
            departing = true;
            depart_deadline = now_ms() + 1500;
            depart_why = why;
        }
    }
    void end_clean(int s)
    {
        if (ended) return;
        net_log("Session %d: slot %d ended the session", slot, s);
        ended = true;
        departing = false;
        status = "partner ended the session";
    }
    void resolve_departure(uint32_t now)
    {
        if (!departing || ended || failed) return;
        if (bye_from[0]) end_clean(0);
        else if (now >= depart_deadline) fail(depart_why);
    }

    /* ---- sending ---- */
    void send_raw(int s, const uint8_t* pkt, int len, uint32_t now)
    {
        Peer& p = peers[s];
        if (direct_fresh(p, now)) {
            out_udp(p.direct, pkt, len);
        } else if (p.relay_active) {
            send_relay(s, pkt, len);
        }
    }
    void send_relay(int s, const uint8_t* pkt, int len)
    {
        uint8_t buf[1300];
        if (len + 1 > (int)sizeof(buf)) return;
        buf[0] = (uint8_t)s;
        memcpy(buf + 1, pkt, len);
        if (sim_on) { sim_out(true, Addr(), buf, len + 1, now_ms()); return; }
        rl_tx++; rl_txb += (uint32_t)len + 1;
        if (!ws.send_binary(buf, len + 1)) { static int cnt = 0; if (cnt++ < 20) net_log("relay send failed"); }
    }
    void send_ctl(int s, char type, uint32_t now, const uint8_t* extra = nullptr, int extra_len = 0)
    {
        uint8_t pkt[16];
        pkt[0] = (uint8_t)type;
        pkt[1] = (uint8_t)slot;
        if (extra_len > 0) memcpy(pkt + 2, extra, extra_len);
        send_raw(s, pkt, 2 + extra_len, now);
    }

    /* ---- receiving ---- */
    void deliver(int from, const uint8_t* payload, int len)
    {
        if (inbox.size() >= kMaxQueue) inbox.pop_front();
        inbox.emplace_back(from, std::vector<uint8_t>(payload, payload + len));
    }

    void handle_packet(bool via_relay, int relay_from, const Addr& addr, const uint8_t* d, int n, uint32_t now)
    {
        if (n < 2) return;
        char type = (char)d[0];
        int from = d[1];
        if (from >= kMaxSlots || from == slot) return;
        if (type == 'E') {
            /* Relay-only broadcast from a guest: the lobby already fanned it out to every other PC, so it is delivered here and never re-forwarded. */
            if (via_relay && relay_from == from && n >= 3) deliver(from, d + 3, n - 3);
            return;
        }
        int path = from;
        if (type == 'D') {
            if (n < 3) return;
            path = is_host() ? from : 0;       // client only hears the host
        }
        if (via_relay) path = relay_from;
        if (!talks_to(path)) return;
        Peer& p = peers[path];
        p.last_rx = now;
        if (via_relay) {
            p.has_relay = true;
            p.last_relay = now;
        }
        switch (type) {
        case 'B':
            bye_from[path] = true;
            if (departing && path == 0) end_clean(0);
            else if (is_host() && !(spec_mask >> path & 1u)) end_clean(path);   // a guest signed off: the room session is over
            break;
        case 'H':
            if (via_relay) send_relay(path, reply('A'), 2);
            else {
                uint8_t a[2] = {'A', (uint8_t)slot};
                out_udp(addr, a, 2);
            }
            break;
        case 'A':
            if (!via_relay) {
                if (!p.has_direct) net_log("Session %d: direct path to slot %d via %s", slot, path, addr.str().c_str());
                p.has_direct = true;
                p.direct = addr;
                p.last_direct = now;
            }
            break;
        case 'P': {
            if (n < 6) break;
            uint8_t q[6] = {'Q', (uint8_t)slot};
            memcpy(q + 2, d + 2, 4);
            if (via_relay) send_relay(path, q, 6);
            else out_udp(addr, q, 6);
            break;
        }
        case 'Q':
            if (n < 6) break;
            if (!via_relay) {
                p.has_direct = true;
                p.direct = addr;
                p.last_direct = now;
            }
            p.ping = (int)(now - get32(d + 2));
            p.add_ping(p.ping);
            break;
        case 'R':
            if (n >= 3 && !is_host() && path == 0) {
                roster_mask = d[2];
                if ((roster_mask & (1u << slot)) && popcount(roster_mask) >= (spectator ? 2 : cfg.expected) && !ready_ever) {
                    ready_ever = true;
                    net_log("Session %d: ready (roster %02x)", slot, roster_mask);
                }
            }
            break;
        case 'D': {
            int to = d[2];
            const uint8_t* payload = d + 3;
            int plen = n - 3;
            if (is_host()) {
                if (to == 0 || to == 0xFF) deliver(from, payload, plen);
                if (to != 0) {
                    // forward to the other client(s)
                    for (int s = 1; s < kMaxSlots; s++) {
                        if (s == from || !peers[s].present) continue;
                        if (to == 0xFF && (spec_mask >> s & 1u)) continue;
                        if (to == 0xFF || to == s) send_raw(s, d, n, now);
                    }
                }
            } else if (to == slot || to == 0xFF) {
                deliver(from, payload, plen);
            }
            break;
        }
        default:
            break;
        }
    }
    const uint8_t* reply(char t)
    {
        static thread_local uint8_t b[2];
        b[0] = (uint8_t)t;
        b[1] = (uint8_t)slot;
        return b;
    }

    /* ---- server messages ---- */
    void on_ws_text(const std::string& text, uint32_t now)
    {
        Json j;
        if (!json_parse(text, &j) || j.type != Json::Object) return;
        std::string type = j.str("type");
        if (type == "welcome") {
            welcomed = true;
            ws_retrying = false;
            int s = (int)j.i64("slot", slot);
            if (s != slot) net_log("Session: server gave slot %d (wanted %d)", s, slot);
            slot = s;
            net_log("Session %d: welcome room %s", slot, j.str("room").c_str());
        } else if (type == "members") {
            bool seen[kMaxSlots] = {};
            uint32_t sm = 0;
            if (const Json* a = j.get("members"))
                for (const Json& m : a->a) {
                    int s = (int)m.i64("slot", -1);
                    if (s >= 0 && s < kMaxSlots) {
                        seen[s] = true;
                        const Json* sp = m.get("spec");
                        if (sp && sp->type == Json::Bool && sp->b) sm |= 1u << s;
                    }
                }
            spec_mask = sm & ~(1u << slot);
            for (int s = 0; s < kMaxSlots; s++) {
                if (s == slot) continue;
                if (seen[s] && !peers[s].present) {
                    peers[s].present = true;
                    peers[s].since = now;
                    net_log("Session %d: slot %d joined", slot, s);
                } else if (!seen[s] && peers[s].present) {
                    net_log("Session %d: slot %d left", slot, s);
                    peers[s].reset();
                    peers[s].present = false;
                    if (!is_host() && s == 0) departed(0, "The host left");
                }
            }
        } else if (type == "peers") {
            if (const Json* a = j.get("peers"))
                for (const Json& m : a->a) {
                    int s = (int)m.i64("slot", -1);
                    if (s < 0 || s >= kMaxSlots || s == slot) continue;
                    Peer& p = peers[s];
                    Addr pub, loc;
                    bool hp = parse_addr(m.str("public"), &pub), hl = parse_addr(m.str("local"), &loc);
                    if (hp) p.pub = pub; else p.pub = Addr();
                    if (hl) p.loc = loc; else p.loc = Addr();
                    if (!p.has_cand && (hp || hl))
                        net_log("Session %d: peer %d candidates pub=%s loc=%s", slot, s, m.str("public").c_str(),
                                m.str("local").c_str());
                    p.has_cand = hp || hl;
                }
        } else if (type == "error") {
            std::string reason = j.str("reason", "error");
            if ((reason == "no-such-room" || reason == "host-reconnecting") && !is_host()) {
                ws_retrying = true;
                ws_retry_at = now + 700;
                status = "waiting for host";
                return;
            }
            if (reason == "room-in-use") fail("Room code already in use");
            else if (reason == "room-full") fail("Room is full or slot taken");
            else if (reason == "bad-slot") fail("That player slot is already taken");
            else if (reason == "spectators-full") fail("This room already has the maximum number of spectators");
            else if (reason == "spectate-busy") fail("Spectating is paused: the server is busy, try again later");
            else if (reason == "host-left-game") fail("The host left - game over");
            else fail(reason);
        }
    }

    void pump_ws(uint32_t now)
    {
        std::string text;
        while (ws.poll_text(&text) && !failed && !ended) on_ws_text(text, now);
        std::vector<uint8_t> bin;
        while (ws.poll_binary(&bin)) {
            rl_rx++; rl_rxb += (uint32_t)bin.size();
            if (bin.size() < 3) continue;
            int from = bin[0];
            if (from >= kMaxSlots) continue;
            handle_packet(true, from, Addr(), bin.data() + 1, (int)bin.size() - 1, now);
        }
        if (ws.state() == WsState::Failed) {
            int code = ws.close_code();
            std::string err = ws.error();
            if (ws_retrying) {
                if (now >= ws_retry_at) { ws.close(); connect_ws(); ws_retrying = true; ws_retry_at = now + 700; }
            } else if (code == 4001) {
                departed(0, "The host left");
            } else if (!ready_ever) {
                fail(err.empty() ? "Lost connection to the lobby server" : err);
            } else if (welcomed) {
                net_log("Session %d: lobby socket lost after ready (%s); relay unavailable", slot, err.c_str());
                welcomed = false;
            }
        }
    }

    void pump_udp(uint32_t now)
    {
        uint8_t buf[1500];
        Addr from;
        for (int i = 0; i < 512; i++) {
            int n = udp.recv_from(&from, buf, sizeof(buf));
            if (n <= 0) break;
            if (stun.feed(buf, n)) continue;
            if (force_relay) continue;
            handle_packet(false, -1, from, buf, n, now);
        }
    }

    void maybe_send_endpoint(uint32_t now)
    {
        if (endpoint_sent || !welcomed || ws.state() != WsState::Open) return;
        std::string pub = "-", loc = "-";
        if (!force_relay) {
            if (!stun.done() && !stun.gave_up() && now - start_ms < kEndpointWaitMs) return;
            pub = stun.public_endpoint();
            loc = udp.local_endpoint();
        }
        ws.send_text("{\"type\":\"endpoint\",\"public\":\"" + pub + "\",\"local\":\"" + loc + "\"}");
        endpoint_sent = true;
        net_log("Session %d: endpoint sent public=%s local=%s", slot, pub.c_str(), loc.c_str());
    }

    void tick_peer(int s, uint32_t now)
    {
        Peer& p = peers[s];
        if (!talks_to(s) || !p.present) return;
        bool direct_ok = direct_fresh(p, now);

        if (!no_relay && !p.relay_active && !direct_ok && (force_relay || now - p.since >= kRelayAfterMs)) {
            p.relay_active = true;
            net_log("Session %d: slot %d falling back to relay", slot, s);
        }
        // UDP punching: both candidates, fast at first
        if (!force_relay && p.has_cand && !direct_ok) {
            uint32_t every = (now - p.since < kRelayAfterMs) ? 100 : 500;
            if (now - p.last_hello >= every) {
                p.last_hello = now;
                uint8_t h[2] = {'H', (uint8_t)slot};
                if (p.pub.valid()) out_udp(p.pub, h, 2);
                if (p.loc.valid() && !(p.loc == p.pub)) out_udp(p.loc, h, 2);
            }
        }
        // relay handshake
        if (p.relay_active && !relay_fresh(p, now) && welcomed && now - p.last_relay_hello >= 250) {
            p.last_relay_hello = now;
            uint8_t h[2] = {'H', (uint8_t)slot};
            send_relay(s, h, 2);
        }
        // ping
        if ((direct_ok || p.relay_active) && now - p.last_ping >= ping_every) {
            p.last_ping = now;
            uint8_t t[4];
            put32(t, now);
            send_ctl(s, 'P', now, t, 4);
        }
    }

    /* Tell every connected partner this end is intentional (sent a few times: UDP may drop). */
    void say_bye(uint32_t now)
    {
        for (int s = 0; s < kMaxSlots; s++)
            if (talks_to(s) && peers[s].present) {
                uint8_t b[2] = {'B', (uint8_t)slot};
                send_raw(s, b, 2, now);
            }
    }

    void update()
    {
        if (!started || failed || ended) return;
        uint32_t now = now_ms();
        if (rl_tx + rl_rx && now - rl_at >= 5000) {
            if (rl_at) net_log("Session %d: relay traffic %u frames/s sent (%u B/s), %u frames/s received (%u B/s)", slot, rl_tx / 5, rl_txb / 5, rl_rx / 5, rl_rxb / 5);
            rl_at = now; rl_tx = rl_rx = rl_txb = rl_rxb = 0;
        }
        if (sim_on) sim_flush(now);
        pump_ws(now);
        resolve_departure(now);
        if (failed || ended) return;
        pump_udp(now);
        stun.update(&udp, now);
        maybe_send_endpoint(now);
        for (int s = 0; s < kMaxSlots; s++) tick_peer(s, now);

        if (is_host()) {
            unsigned mask = 1, smask = 0;
            for (int s = 1; s < kMaxSlots; s++)
                if (peers[s].present && reachable(s, now)) { if (spec_mask >> s & 1u) smask |= 1u << s; else mask |= 1u << s; }
            roster_mask = mask;
            if (!ready_ever && popcount(mask) >= cfg.expected) {
                ready_ever = true;
                net_log("Session 0: ready (roster %02x)", mask);
            }
            for (int s = 1; s < kMaxSlots; s++) {
                Peer& p = peers[s];
                if (!((mask | smask) & (1u << s)) || now - p.last_roster < kRosterMs) continue;
                p.last_roster = now;
                uint8_t r = (uint8_t)(mask | (s == 0 ? 0 : (smask & (1u << s))));   // a spectator's roster shows the players plus itself
                send_ctl(s, 'R', now, &r, 1);
            }
        }

        if (ready_ever) {
            if (!is_host() && now - peers[0].last_rx > (cfg.silence_fail_ms ? cfg.silence_fail_ms : kSilenceFailMs)) fail("Lost connection to the host");
            status = "connected";
        } else {
            if (now - start_ms > (cfg.ready_timeout_ms ? cfg.ready_timeout_ms : kNotReadyFailMs)) {
                int absent = 0, present_n = 0;
                if (is_host()) {
                    for (int s = 1; s < kMaxSlots; s++) if (peers[s].present) present_n++;
                    absent = cfg.expected - 1 - present_n;
                }
                if (!is_host() && !peers[0].present) fail("The host never opened this game session (their RetroHub did not start it)");
                else if (is_host() && absent > 0)
                    fail(std::to_string(absent) + " player(s) never joined the game session (their RetroHub did not start it or lost the room connection)");
                else fail("Couldn't connect to the other players (the game room is reachable, but neither direct UDP nor the relay got through)");
            } else if (!welcomed) {
                if (!ws_retrying) status = "contacting server";
            } else {
                status = "connecting to players";
            }
        }
    }
};

Session::Session() : impl_(new Impl) {}
Session::~Session() { stop(); }

bool Session::start(const SessionConfig& cfg)
{
    stop();
    Impl& d = *impl_;
    d.cfg = cfg;
    if (d.cfg.server.empty()) d.cfg.server = default_server();
    if (cfg.spectate && cfg.slot == 0) return false;
    if (cfg.slot < 0 || cfg.slot >= kMaxSlots || cfg.expected < 1 || cfg.expected > kMaxSlots) return false;
    d.slot = cfg.slot;
    d.spectator = cfg.spectate && cfg.slot != 0;
    d.spec_mask = 0;
    d.failed = d.ready_ever = d.welcomed = d.endpoint_sent = d.ws_retrying = false;
    d.inbox.clear();
    for (Peer& p : d.peers) p = Peer();
    d.roster_mask = 0;
    d.ping_every = kPingMs;
    const char* fr = getenv("HUBNET_FORCE_RELAY");
    d.force_relay = fr && fr[0] && fr[0] != '0' && !d.no_relay;
    d.sim_init(d.slot);
    d.start_ms = now_ms();
    d.last_ws_keepalive = d.start_ms;
    d.status = "contacting server";
    if (!d.udp.open()) {
        d.failed = true;
        d.status = "Couldn't open a UDP socket";
        return false;
    }
    if (!d.force_relay && !loopback_mode()) d.stun.start(&d.udp);
    else d.stun = Stun();
    d.started = true;
    net_log("Session: start room=%s slot=%d expected=%d force_relay=%d udp port %u", d.cfg.room.c_str(), d.slot,
            d.cfg.expected, d.force_relay ? 1 : 0, d.udp.local_port());
    d.connect_ws();
    return true;
}

void Session::stop()
{
    Impl& d = *impl_;
    d.ws.close();
    d.udp.close();
    d.started = false;
}

void Session::update()
{
    Impl& d = *impl_;
    d.update();
    uint32_t now = now_ms();
    if (d.started && !d.failed && d.welcomed && now - d.last_ws_keepalive >= kWsKeepaliveMs) {
        d.last_ws_keepalive = now;
        d.ws.send_text("{\"type\":\"keepalive\"}");
    }
}

bool Session::ready() const
{
    const Impl& d = *impl_;
    if (!d.started || d.failed || !d.ready_ever) return false;
    if (d.is_host()) return popcount(d.roster_mask) >= d.cfg.expected;
    return true;
}
bool Session::failed() const { return impl_->failed; }
bool Session::ended() const { return impl_->ended; }
void Session::say_bye()
{
    Impl& d = *impl_;
    if (!d.started || d.failed) return;
    d.say_bye(now_ms());
}
std::string Session::status() const { return impl_->status; }
int Session::my_slot() const { return impl_->slot; }
uint32_t Session::spectator_mask() const { return impl_->spec_mask; }
bool Session::is_spectator() const { return impl_->spectator; }

bool Session::is_relayed(int slot) const
{
    const Impl& d = *impl_;
    if (slot < 0 || slot >= kMaxSlots || !d.talks_to(slot)) return false;
    const Peer& p = d.peers[slot];
    return !d.direct_fresh(p, now_ms()) && p.relay_active;
}

int Session::ping_ms(int slot) const
{
    const Impl& d = *impl_;
    if (slot < 0 || slot >= kMaxSlots) return -1;
    return d.peers[slot].ping;
}

uint16_t Session::local_port() const { return impl_->udp.local_port(); }
void Session::set_no_relay(bool v) { impl_->no_relay = v; }
void Session::set_ping_interval(uint32_t ms) { impl_->ping_every = ms < 20 ? 20 : ms; }

void Session::clear_ping_stats()
{
    for (Peer& p : impl_->peers) p.ring_n = p.ring_i = 0;
}

bool Session::ping_stats(int slot, PingStats* out) const
{
    const Impl& d = *impl_;
    if (slot < 0 || slot >= kMaxSlots || d.peers[slot].ring_n == 0) return false;
    const Peer& p = d.peers[slot];
    long sum = 0;
    out->n = p.ring_n;
    out->min_ms = out->max_ms = p.ring[0];
    for (int i = 0; i < p.ring_n; i++) {
        sum += p.ring[i];
        if (p.ring[i] < out->min_ms) out->min_ms = p.ring[i];
        if (p.ring[i] > out->max_ms) out->max_ms = p.ring[i];
    }
    out->avg_ms = (int)(sum / p.ring_n);
    return true;
}

void Session::send(int to_slot, const void* data, int len)
{
    Impl& d = *impl_;
    if (!d.started || d.failed || len < 0 || len > kMaxPayload) return;
    if (to_slot == d.slot) return;
    uint32_t now = now_ms();
    uint8_t pkt[3 + kMaxPayload];
    pkt[0] = 'D';
    pkt[1] = (uint8_t)d.slot;
    if (len) memcpy(pkt + 3, data, len);
    if (d.is_host()) {
        if (to_slot < 0) {   // broadcast reaches the players only; spectators get dedicated packets (to_slot >= 0)
            /* Broadcast with every present client reachable only through the relay: ONE relay frame to slot 0xFF (the lobby fans it out) instead of a copy per client. */
            int n = 0;
            bool all_relay = true;
            for (int s = 1; s < kMaxSlots; s++) {
                if (!d.peers[s].present || (d.spec_mask >> s & 1u)) continue;
                n++;
                if (d.direct_fresh(d.peers[s], now) || !d.peers[s].relay_active) all_relay = false;
            }
            if (n >= 2 && all_relay) {
                pkt[2] = 0xFF;
                d.send_relay(0xFF, pkt, 3 + len);
                return;
            }
        }
        for (int s = 1; s < kMaxSlots; s++) {
            if (!d.peers[s].present || (to_slot >= 0 && to_slot != s)) continue;
            if (to_slot < 0 && (d.spec_mask >> s & 1u)) continue;
            pkt[2] = (to_slot < 0) ? 0xFF : (uint8_t)s;
            d.send_raw(s, pkt, 3 + len, now);
        }
    } else if (d.spectator) {
        pkt[2] = 0;   // a spectator talks to the host only (the server drops anything else)
        d.send_raw(0, pkt, 3 + len, now);
    } else {
        if (to_slot < 0 && popcount(d.roster_mask) >= 3 && !d.direct_fresh(d.peers[0], now) && d.peers[0].relay_active) {
            /* Broadcast over the relay with 3+ PCs: ONE frame to everybody (the lobby fans it out) instead of one to the host that the host forwards twice. */
            pkt[0] = 'E';
            pkt[2] = 0xFF;
            d.send_relay(0xFF, pkt, 3 + len);
            return;
        }
        pkt[2] = (to_slot < 0) ? 0xFF : (uint8_t)to_slot;
        d.send_raw(0, pkt, 3 + len, now);
    }
}

bool Session::recv(int* from_slot, std::vector<uint8_t>* out)
{
    Impl& d = *impl_;
    if (d.inbox.empty()) return false;
    *from_slot = d.inbox.front().first;
    *out = std::move(d.inbox.front().second);
    d.inbox.pop_front();
    return true;
}

} // namespace hubnet
