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
#include <winsock2.h>   // WSAEventSelect (Session::set_wake_event)
#include <chrono>
#include <algorithm>
#include "net_internal.h"
#include "ident_env.h"
#include "log.h"
#include "session.h"
#include "reconnect.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <vector>

namespace hubnet {

using namespace detail;

/* Plain text for a server reason code (defined in lobby.cpp, same library). "" = a code we do not know: shown as it is. */
std::string server_reason_text(const std::string& reason);

/* The `caps` URL parameter of a game-room socket. The server reads it as a comma list, so "gameauth" is ignored by older servers; it is only sent
 * with an account proof (a seat that cannot prove an account would just be closed by a server that checks). */
std::string game_room_caps(bool have_ident) { return have_ident ? "&caps=reasons,gameauth" : "&caps=reasons"; }

namespace {

/* The relay is the free Cloudflare plan's, and its daily budget can run out (the Worker then answers a relayed frame with
 * relay_denied "server-busy"). Say that, instead of the "Connection lost" the players would see after the timeouts. */
const char* const kRelayBusyInfo = "The free server is busy, so the relay is off right now. Direct connections still work.";
const char* const kRelayBusyFail = "The free server is busy, so the relay is off right now, and a direct connection to the other players could not be made. Try again in a little while.";
const char* const kRelayBusyLost = "The free server is busy, so the relay stopped carrying the game. Direct connections still work. Try again in a little while.";
const char* const kRelayCut = "The free server cut the relay off because this PC was sending too much through it.";

constexpr uint32_t kRelayBusyGraceMs = 4000;   // after relay_denied "server-busy", wait this long for a direct path before giving up
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
    uint32_t ws_gone_at = 0;  // vanished from the server roster (socket blip?) while the direct UDP path may still be alive
    uint32_t since = 0;       // when the peer first showed up in the room
    uint32_t last_rx = 0;
    uint32_t last_hello = 0, last_relay_hello = 0, last_ping = 0, last_roster = 0;
    bool relay_active = false;
    /* Proof of who is sending over direct UDP. A peer sends us a random token over the relay (frames there carry the server's own sender stamp, so only that
     * peer can have sent it, and only we received it). Its direct hello / ack / ping / pong frames carry the token; the source address of a frame that proves it
     * becomes the peer's bound address, and data from that peer must then come from it. A peer that never sends a token (an older RetroHub) keeps the old rules. */
    bool tok_known = false;
    uint32_t tok = 0;
    bool has_bound = false;
    Addr bound;
    uint32_t tok_sends = 0, last_tok_send = 0;
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
    bool verdict_done = false, verdict_left = false;   // await_host_verdict(): asked once
    bool departing = false;          // the host left without a bye yet: wait briefly for one, then fail
    uint32_t depart_deadline = 0;
    std::string depart_why;
    bool force_relay = false;
    bool no_relay = false;   // media sessions (remote play): never fall back to the WebSocket relay
    std::string status = "idle";
    uint32_t start_ms = 0;
    int slot = 0;
    uint32_t spec_mask = 0;    // slots the server flagged as spectators
    uint32_t bcast_excl = 0;   // members a host broadcast skips (set_broadcast_exclude)
    bool spectator = false;    // this end is a spectator

    UdpSocket udp;
    Stun stun;
    WsClient ws;
    bool welcomed = false, endpoint_sent = false;
    uint32_t last_ws_keepalive = 0;
    uint32_t ws_retry_at = 0;      // join: retry while the host's room doesn't exist yet
    bool ws_retrying = false;
    WsReconnect rc;                // post-ready socket blip: reconnect while the UDP path keeps playing
    bool relay_busy = false;       // the server answered a relayed frame with relay_denied "server-busy": its free daily budget is used up
    uint32_t relay_busy_at = 0;    // ... and when it first said so

    Peer peers[kMaxSlots];
    uint32_t spoof_dropped = 0;    // frames dropped because their sender byte did not match where they came from
    uint32_t unproven_seen = 0;    // direct frames accepted from a peer whose token we do not have yet (older RetroHub, or its token is still on the way)
    uint32_t my_tok = 0;           // this session's random token, sent privately to each peer over the relay
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

    std::string ident;   // proof-of-account frame for this server (ident_env.h); empty = none. Never logged.
    int ident_busy_tries = 0;

    void connect_ws()
    {
        std::string url = hub_url(cfg.server, cfg.room, is_host(), "n64run", slot, spectator, cfg.dropin) + game_room_caps(!ident.empty());
        net_log("Session: connecting %s", url.c_str());
        ws.connect(url);
        welcomed = false;
        endpoint_sent = false;
    }

    bool direct_fresh(const Peer& p, uint32_t now) const { return p.has_direct && now - p.last_direct < kDirectFreshMs; }
    bool relay_fresh(const Peer& p, uint32_t now) const { return p.has_relay && now - p.last_relay < kRelayFreshMs; }
    /* Some partner can only be reached through the relay (no direct path right now). */
    bool depends_on_relay(uint32_t now) const
    {
        for (int s = 0; s < kMaxSlots; s++)
            if (s != slot && talks_to(s) && peers[s].present && peers[s].relay_active && !direct_fresh(peers[s], now)) return true;
        return false;
    }
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
        uint8_t pkt[24];
        pkt[0] = (uint8_t)type;
        pkt[1] = (uint8_t)slot;
        if (extra_len > 0) memcpy(pkt + 2, extra, extra_len);
        send_raw(s, pkt, 2 + extra_len, now);
    }

    /* ---- receiving ---- */
    /* Host: is another player (not a spectator, not `except`) still in the room and has not said bye? */
    bool other_player_remains(int except) const
    {
        for (int s = 1; s < kMaxSlots; s++)
            if (s != except && s != slot && peers[s].present && !bye_from[s] && !(spec_mask >> s & 1u)) return true;
        return false;
    }
    bool tokens_on() const { return !no_relay && my_tok != 0; }
    void send_token(int s, uint32_t now)
    {
        Peer& p = peers[s];
        uint8_t t[6] = {'T', (uint8_t)slot};
        put32(t + 2, my_tok);
        p.last_tok_send = now;
        p.tok_sends++;
        send_relay(s, t, 6);
    }
    void note_spoof(char type, int claimed, const char* how)
    {
        spoof_dropped++;
        if (spoof_dropped <= 10 || spoof_dropped % 1000 == 0)
            net_log("Session %d: dropped a '%c' frame claiming to be slot %d that did not come from it (%s, %u so far)", slot, type, claimed, how, spoof_dropped);
    }
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
        /* Who really sent this frame? The sender byte inside it is only a claim. Relay: the server rewrites the first byte of every
         * frame to the sender's real slot (relay_from), so the claim must match it. The one exception is a guest hearing the host:
         * the host forwards other guests' data unchanged, and it checked the sender itself. Direct UDP: the datagram must come from
         * the address that slot announced to the server (or the one we already verified). */
        const bool host_forward = via_relay ? (!is_host() && relay_from == 0 && type == 'D')
                                            : (!is_host() && type == 'D' && from != 0);
        if (via_relay) {
            if (from != relay_from && !host_forward) { note_spoof(type, from, "relay"); return; }
        } else if (type == 'T') {
            return;   // tokens are only accepted over the relay
        } else {
            Peer& pp = peers[path];
            if (pp.tok_known) {
                if (type == 'H' || type == 'A' || type == 'P' || type == 'Q') {   // a direct handshake frame must carry the sender's token; it binds the address
                    const int off = (type == 'P' || type == 'Q') ? 6 : 2;
                    if (n < off + 4 || get32(d + off) != pp.tok) { note_spoof(type, from, "udp, wrong or missing token"); return; }
                    if (!pp.has_bound || !(pp.bound == addr)) net_log("Session %d: slot %d is now bound to %s", slot, path, addr.str().c_str());
                    pp.has_bound = true;
                    pp.bound = addr;
                } else if (!(pp.has_bound && pp.bound == addr)) {   // data / roster / bye: only from the address that proved itself
                    note_spoof(type, from, "udp, not from the slot's proven address");
                    return;
                }
            } else if (tokens_on()) {
                /* No token from this slot (yet): honest frames from an older RetroHub, or ahead of the token. Accepted as before, but counted. */
                if (unproven_seen++ < 5) net_log("Session %d: direct '%c' frame from slot %d before its token is known (not announced yet or an older RetroHub)", slot, type, from);
            }
        }
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
            else if (is_host() && !(spec_mask >> path & 1u) && !cfg.keep_on_guest_bye && !other_player_remains(path)) end_clean(path);   // the last guest signed off: the room session is over (3-4 players: the others play on)
            break;
        case 'T':
            if (via_relay && n >= 6) {
                const uint32_t nt = get32(d + 2);
                const bool changed = !p.tok_known || nt != p.tok;
                if (changed) { p.tok = nt; p.tok_known = nt != 0; p.has_bound = false; }   // a repeat of the same token must not unbind a working path
                /* Answer so the peer learns ours: a new token gets one answer, a repeat only while our own sends are within the normal budget; never more than 12 in all. */
                if (tokens_on() && now - p.last_tok_send >= 500 && p.tok_sends < (changed ? 12u : 6u)) send_token(path, now);
            }
            break;
        case 'H':
            if (via_relay) send_relay(path, reply('A'), 2);
            else {
                uint8_t a[6] = {'A', (uint8_t)slot};
                int al = 2;
                if (tokens_on()) { put32(a + 2, my_tok); al = 6; }
                out_udp(addr, a, al);
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
            uint8_t q[10] = {'Q', (uint8_t)slot};
            memcpy(q + 2, d + 2, 4);
            if (via_relay) send_relay(path, q, 6);
            else if (tokens_on()) { put32(q + 6, my_tok); out_udp(addr, q, 10); }
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
                        if (to == 0xFF && ((spec_mask | bcast_excl) >> s & 1u)) continue;
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
            if (rc.active()) net_log("Session %d: lobby socket back after %u ms", slot, now - rc.blip_ms());
            rc.welcomed();
            int s = (int)j.i64("slot", slot);
            if (s != slot) net_log("Session: server gave slot %d (wanted %d)", s, slot);
            slot = s;
            net_log("Session %d: welcome room %s", slot, j.str("room").c_str());
            if (!ident.empty()) { ws.send_text(ident); net_log("Session %d: sent account proof", slot); }   // strict rooms: every seat proves its account (no pairing)
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
                if (seen[s]) peers[s].ws_gone_at = 0;
                if (seen[s] && !peers[s].present) {
                    peers[s].present = true;
                    bye_from[s] = false;   // a new occupant of the slot (drop-in): an old bye does not count
                    peers[s].since = now;
                    net_log("Session %d: slot %d joined", slot, s);
                } else if (!seen[s] && peers[s].present) {
                    /* Gone from the roster. A socket blip looks the same: keep the peer while its direct UDP path is alive. */
                    if (ready_ever && direct_fresh(peers[s], now)) {
                        if (!peers[s].ws_gone_at) { peers[s].ws_gone_at = now ? now : 1; net_log("Session %d: slot %d left the room but its UDP is alive; keeping it", slot, s); }
                    } else peer_left(s);
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
        } else if (type == "relay_denied") {
            /* The server did not forward a relayed frame of ours. "server-busy": its free daily budget is used up (a room's first
             * relayed frame from 50%, every frame from 90%): the relay is off for everybody until it resets, so tell the players
             * that instead of letting the timeouts say "Connection lost". Anything else (rate-limited): this room or this PC sent
             * too much for a moment - transient, and the server tells again at most every 10 s. */
            std::string reason = j.str("reason", "server-busy");
            net_log("Session %d: relay denied by the server (%s): %s", slot, reason.c_str(), j.str("detail").c_str());
            if (reason == "server-busy") {
                if (!relay_busy) relay_busy_at = now;
                relay_busy = true;   // update() ends a session that still depends on the relay kRelayBusyGraceMs later (a direct path may yet come up)
                if (!failed) status = kRelayBusyInfo;
            }
        } else if (type == "error") {
            std::string reason = j.str("reason", "error");
            if (ready_ever && rc.active() && WsReconnect::retryable_reason(reason)) {   // the server still holds our old seat: the close that follows triggers the next try
                net_log("Session %d: reconnect refused (%s), will retry", slot, reason.c_str());
                return;
            }
            if ((reason == "no-such-room" || reason == "host-reconnecting") && !is_host()) {
                ws_retrying = true;
                ws_retry_at = now + 700;
                status = "waiting for host";
                return;
            }
            if (reason == "ident-required") { fail(server_reason_text(reason)); return; }   // the room needs a RetroHub account: never retried
            if (reason == "server-busy" && !ident.empty() && !ready_ever && welcomed && ident_busy_tries < 6) {   // account check refused for now (4014): temporary, never a lock; back off and try again
                ++ident_busy_tries;
                ws_retrying = true;
                ws_retry_at = now + 3000u * (uint32_t)ident_busy_tries;
                status = "Server busy, try again shortly";
                return;
            }
            if (reason == "room-in-use") fail("Room code already in use");
            else if (reason == "room-full") fail("Room is full or slot taken");
            else if (reason == "bad-slot") fail("That player slot is already taken");
            else if (reason == "spectators-full") fail("This room already has the maximum number of spectators");
            else if (reason == "spectate-busy") fail("Spectating is paused: the server is busy, try again later");
            else if (reason == "no-free-port") fail("That game has no free controller port");
            else if (reason == "host-left-game") fail("The host left - game over");
            else {   // server-busy, rate-limited, no-such-game, host-too-old ...: plain words (lobby.cpp); a sentence we don't know is shown as it is
                std::string t = server_reason_text(reason);
                fail(t.empty() ? reason : t);
            }
        }
    }

    void peer_left(int s)
    {
        net_log("Session %d: slot %d left", slot, s);
        peers[s].reset();
        peers[s].present = false;
        peers[s].ws_gone_at = 0;
        if (!is_host() && s == 0) departed(0, "The host left");
    }
    /* A peer that left the roster only counts as gone once its UDP went quiet too. */
    void check_gone_peers(uint32_t now)
    {
        for (int s = 0; s < kMaxSlots; s++)
            if (s != slot && peers[s].present && peers[s].ws_gone_at && member_departed(true, direct_fresh(peers[s], now))) peer_left(s);
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
            } else if (code == 4001 && !(ready_ever && direct_fresh(peers[0], now))) {
                departed(0, "The host left");
            } else if (code == 1008) {   // the server cut this socket off for sending far more than a game ever does (relay or text flood)
                if (!ready_ever) fail(kRelayCut);
                else { net_log("Session %d: %s", slot, kRelayCut); welcomed = false; }
            } else if (!ready_ever) {
                fail(err.empty() ? "Lost connection to the lobby server" : err);
            } else if (WsReconnect::reconnectable_close(code)) {
                if (!rc.active()) {
                    net_log("Session %d: lobby socket lost after ready (code %d, %s); reconnecting, UDP play continues", slot, code, err.c_str());
                    welcomed = false;
                }
                rc.blip(now);
                if (rc.should_retry(now)) { ws.close(); connect_ws(); rc.attempted(now); }
                else if (rc.gave_up() && welcomed == false && ws.state() == WsState::Failed) {
                    static thread_local int logged = 0;
                    if (logged++ < 3) net_log("Session %d: lobby socket not recovered; relay unavailable", slot);
                    welcomed = false;
                }
            } else if (welcomed) {
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
                uint8_t h[6] = {'H', (uint8_t)slot};
                int hl = 2;
                if (tokens_on()) { put32(h + 2, my_tok); hl = 6; }
                if (p.pub.valid()) out_udp(p.pub, h, hl);
                if (p.loc.valid() && !(p.loc == p.pub)) out_udp(p.loc, h, hl);
            }
        }
        // relay handshake
        if (p.relay_active && !relay_fresh(p, now) && welcomed && now - p.last_relay_hello >= 250) {
            p.last_relay_hello = now;
            uint8_t h[2] = {'H', (uint8_t)slot};
            send_relay(s, h, 2);
        }
        // private token for the direct-path proof (relay only; a few tries, then it is up to the peer to answer)
        if (tokens_on() && welcomed && ws.state() == WsState::Open && !p.tok_known && p.tok_sends < 6 && now - p.last_tok_send >= 1000) send_token(s, now);
        // ping
        if ((direct_ok || p.relay_active) && now - p.last_ping >= ping_every) {
            p.last_ping = now;
            uint8_t t[8];
            put32(t, now);
            int tl = 4;
            if (tokens_on()) { put32(t + 4, my_tok); tl = 8; }
            send_ctl(s, 'P', now, t, tl);
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
        check_gone_peers(now);
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
            const bool no_relay_now = relay_busy && depends_on_relay(now);   // someone is only reachable through the relay, and the server says it is off
            if (!is_host() && now - peers[0].last_rx > (cfg.silence_fail_ms ? cfg.silence_fail_ms : kSilenceFailMs))
                fail(no_relay_now ? kRelayBusyLost : "Lost connection to the host");
            status = no_relay_now ? kRelayBusyInfo : "connected";
        } else {
            /* The server said the relay is off and somebody can only be reached through it: no point waiting out the 15 s. */
            if (relay_busy && now - relay_busy_at >= kRelayBusyGraceMs && depends_on_relay(now)) fail(kRelayBusyFail);
            else if (now - start_ms > (cfg.ready_timeout_ms ? cfg.ready_timeout_ms : kNotReadyFailMs)) {
                int absent = 0, present_n = 0;
                if (is_host()) {
                    for (int s = 1; s < kMaxSlots; s++) if (peers[s].present) present_n++;
                    absent = cfg.expected - 1 - present_n;
                }
                if (relay_busy) fail(kRelayBusyFail);
                else if (!is_host() && !peers[0].present) fail("The host never opened this game session (their RetroHub did not start it)");
                else if (is_host() && absent > 0)
                    fail(std::to_string(absent) + " player(s) never joined the game session (their RetroHub did not start it or lost the room connection)");
                else fail("Couldn't connect to the other players (the game room is reachable, but neither direct UDP nor the relay got through)");
            } else if (!welcomed) {
                if (!ws_retrying) status = "contacting server";
            } else {
                status = relay_busy ? kRelayBusyInfo : "connecting to players";
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
    d.ident = !cfg.ident.empty() ? cfg.ident : process_ident_frame(d.cfg.server);
    if (d.ident.empty()) d.ident = ident_frame_from_env(d.cfg.server, getenv(kIdentEnv));
    d.ident_busy_tries = 0;
    if (cfg.spectate && cfg.slot == 0) return false;
    if (cfg.slot < 0 || cfg.slot >= kMaxSlots || cfg.expected < 1 || cfg.expected > kMaxSlots) return false;
    d.slot = cfg.slot;
    d.spectator = cfg.spectate && cfg.slot != 0;
    d.spec_mask = 0;
    d.rc.reset();
    d.failed = d.ready_ever = d.welcomed = d.endpoint_sent = d.ws_retrying = d.relay_busy = false;
    d.inbox.clear();
    d.verdict_done = d.verdict_left = false;
    d.ended = d.departing = false;   // a restarted session (the drop-in joiner tries the next slot) starts clean
    for (bool& b : d.bye_from) b = false;
    for (Peer& p : d.peers) p = Peer();
    d.roster_mask = 0;
    d.ping_every = kPingMs;
    const char* fr = getenv("HUBNET_FORCE_RELAY");
    d.force_relay = fr && fr[0] && fr[0] != '0' && !d.no_relay;
    d.sim_init(d.slot);
    d.spoof_dropped = d.unproven_seen = 0;
    d.my_tok = std::random_device()();
    if (!d.my_tok) d.my_tok = 0x5eed0001u;
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
bool Session::await_host_verdict(uint32_t max_ms)
{
    Impl& d = *impl_;
    if (d.is_host() || !d.started) return false;
    if (d.status.find("host left") != std::string::npos || d.status.find("partner ended") != std::string::npos) return true;
    if (d.verdict_done) return d.verdict_left;
    VerdictInput in;
    in.max_ms = max_ms;
    uint32_t t0 = now_ms();
    if (d.ws.state() == WsState::Open) d.ws.send_text("{\"type\":\"keepalive\"}");
    HostVerdict v = HostVerdict::Wait;
    auto scan = [&](const std::string& text) {
        in.any_text_seen = true;
        Json j;
        if (!json_parse(text, &j) || j.type != Json::Object) return;
        std::string type = j.str("type");
        if (type == "error" && j.str("reason") == "host-left-game") in.host_left_signal = true;
        else if (type == "members") {
            bool has0 = false;
            const Json* a = j.get("members");
            if (a) for (const Json& m : a->a) if (m.i64("slot", -1) == 0) has0 = true;
            if (a && !has0) in.host_left_signal = true;
        }
    };
    for (;;) {
        std::string text;
        while (d.ws.poll_text(&text)) scan(text);
        std::vector<uint8_t> bin;
        while (d.ws.poll_binary(&bin)) {}   // relayed game frames: not needed any more
        if (d.ws.state() == WsState::Failed) {
            while (d.ws.poll_text(&text)) scan(text);   // a verdict that came right before the close
            if (d.ws.close_code() == 4001) in.host_left_signal = true; else in.socket_dead_other = true;
        }
        in.elapsed_ms = now_ms() - t0;
        v = host_verdict(in);
        if (v != HostVerdict::Wait) break;
        Sleep(10);
    }
    d.verdict_done = true;
    d.verdict_left = (v == HostVerdict::HostLeft);
    net_log("Session %d: host verdict: %s (%u ms)", d.slot, d.verdict_left ? "the host left" : "no word", now_ms() - t0);
    if (d.verdict_left) d.status = "The host left";
    return d.verdict_left;
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
void Session::set_broadcast_exclude(uint32_t mask) { impl_->bcast_excl = mask; }
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
void Session::set_wake_event(void* event) {
    impl_->ws.set_wake(event);
    if (impl_->udp.is_open()) WSAEventSelect((SOCKET)impl_->udp.handle(), (WSAEVENT)event, event ? FD_READ : 0);
}
uint32_t Session::spoofed_frames() const { return impl_->spoof_dropped; }
uint32_t Session::unproven_frames() const { return impl_->unproven_seen; }
bool Session::qa_offline(const SessionConfig& cfg)
{
    stop();
    Impl& d = *impl_;
    d.cfg = cfg;
    if (cfg.slot < 0 || cfg.slot >= kMaxSlots) return false;
    d.slot = cfg.slot;
    d.spectator = false;
    d.spec_mask = 0;
    d.failed = d.ended = d.departing = false;
    d.verdict_done = d.verdict_left = false;
    d.ready_ever = true;
    d.inbox.clear();
    for (bool& b : d.bye_from) b = false;
    for (Peer& p : d.peers) p = Peer();
    d.spoof_dropped = d.unproven_seen = 0;
    d.no_relay = false;
    d.my_tok = 0xA5A5A5A5u;
    d.started = true;   // never update()d: only qa_inject feeds it
    return true;
}
void Session::qa_peer(int slot, bool present, const std::string& pub, const std::string& loc, bool spectator)
{
    Impl& d = *impl_;
    if (slot < 0 || slot >= kMaxSlots) return;
    Peer& p = d.peers[slot];
    p.present = present;
    Addr a;
    p.pub = parse_addr(pub, &a) ? a : Addr();
    p.loc = parse_addr(loc, &a) ? a : Addr();
    p.has_cand = p.pub.valid() || p.loc.valid();
    if (spectator) d.spec_mask |= 1u << slot; else d.spec_mask &= ~(1u << slot);
}
void Session::qa_inject(bool via_relay, int relay_from, const std::string& from_addr, const void* data, int len)
{
    Addr a;
    if (!via_relay) parse_addr(from_addr, &a);
    impl_->handle_packet(via_relay, relay_from, a, (const uint8_t*)data, len, now_ms());
}
void Session::qa_drop_ws() { impl_->ws.drop_abruptly(); }
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
                if (!d.peers[s].present || ((d.spec_mask | d.bcast_excl) >> s & 1u)) continue;
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
            if (to_slot < 0 && ((d.spec_mask | d.bcast_excl) >> s & 1u)) continue;
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
