/* hubreconnect_test: the WebSocket reconnect state machine (offline, no server). */
#include "reconnect.h"
#include "session.h"

#include <vector>

#include <cstdio>

using namespace hubnet;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* Session sender checks and the bye rule (offline: frames are injected, no sockets). */
static bool got(Session& s, int* from)
{
    std::vector<uint8_t> d;
    return s.recv(from, &d);
}
namespace hubnet { std::string server_reason_text(const std::string& reason); }
static void caps_checks()
{
    CHECK(game_room_caps(false) == "&caps=reasons");                       // no account proof: unchanged
    CHECK(game_room_caps(true) == "&caps=reasons,gameauth");               // older servers split on "," and still see "reasons"
    const char* codes[] = {"not-in-lobby", "wrong-slot", "slot-not-yours", "ident-required"};
    for (const char* c : codes) {
        std::string t = hubnet::server_reason_text(c);
        CHECK(t.size() > 20 && t.find(c) == std::string::npos && t.find(' ') != std::string::npos);
    }
}
static void session_checks()
{
    const char* H0 = "127.0.0.1:5000";
    const char* H1 = "127.0.0.1:5001";
    const char* H2 = "127.0.0.1:5002";
    const char* EVIL = "9.9.9.9:4000";
    const uint8_t d01[] = {'D', 1, 0, 42};      // guest 1 -> host
    const uint8_t d02[] = {'D', 2, 0, 42};      // claims to be guest 2 -> host
    const uint8_t dh[] = {'D', 0, 0xFF, 42};    // claims to be the host
    const uint8_t fwd2[] = {'D', 2, 0xFF, 42};  // host-forwarded data from guest 2
    int from = -1;

    SessionConfig hc; hc.slot = 0; hc.expected = 3;
    /* Direct UDP is proven by a private token: sent over the relay (the server stamps the sender), then shown in the direct hello / ack / ping /
     * pong frames. Peers here sit on different public IPs, so address guessing could not tell them apart. */
    auto tokf = [](int slot, uint32_t tok) { std::vector<uint8_t> v = {'T', (uint8_t)slot, 0, 0, 0, 0}; for (int i = 0; i < 4; i++) v[2 + i] = (uint8_t)(tok >> (8 * i)); return v; };
    auto hsf = [](char type, int slot, uint32_t tok) {   // H / A: [type, from, tok]; P / Q: [type, from, ts, tok]
        std::vector<uint8_t> v = {(uint8_t)type, (uint8_t)slot};
        if (type == 'P' || type == 'Q') v.insert(v.end(), 4, 0);
        for (int i = 0; i < 4; i++) v.push_back((uint8_t)(tok >> (8 * i)));
        return v;
    };
    const char* A1 = "1.2.3.4:5000";
    const char* A2 = "5.6.7.8:5001";
    {   // host, direct UDP, distinct public IPs, announced endpoints "-" (STUN never answered)
        Session h;
        CHECK(h.qa_offline(hc));
        h.qa_peer(1, true, "-", "-"); h.qa_peer(2, true, "-", "-");
        auto t1 = tokf(1, 0x11111111u), t2 = tokf(2, 0x22222222u);
        h.qa_inject(true, 1, "", t1.data(), (int)t1.size());
        h.qa_inject(true, 2, "", t2.data(), (int)t2.size());
        h.qa_inject(false, -1, A1, d01, sizeof d01);        // data before any proof: not from a proven address
        CHECK(!got(h, &from) && h.spoofed_frames() == 1);
        auto h1 = hsf('H', 1, 0x11111111u);
        h.qa_inject(false, -1, A1, h1.data(), (int)h1.size());   // the direct path forms through the token
        h.qa_inject(false, -1, A1, d01, sizeof d01);
        CHECK(got(h, &from) && from == 1);
        CHECK(h.spoofed_frames() == 1);
        h.qa_inject(false, -1, "1.2.3.4:6000", d02, sizeof d02);    // same IP, other port, poses as guest 2
        CHECK(!got(h, &from) && h.spoofed_frames() == 2);
        h.qa_inject(false, -1, "1.2.3.4:6000", d01, sizeof d01);    // ... or as guest 1 from a port it has not proven
        CHECK(!got(h, &from) && h.spoofed_frames() == 3);
        auto h2w = hsf('H', 2, 0x11111111u);                 // guest 1's token shown as guest 2
        h.qa_inject(false, -1, A1, h2w.data(), (int)h2w.size());
        CHECK(h.spoofed_frames() == 4);
        auto pbad = hsf('P', 1, 0xDEADBEEFu);                // wrong token cannot rebind guest 1
        h.qa_inject(false, -1, "9.9.9.9:4000", pbad.data(), (int)pbad.size());
        h.qa_inject(false, -1, "9.9.9.9:4000", d01, sizeof d01);
        CHECK(!got(h, &from) && h.spoofed_frames() == 6);
        auto p2 = hsf('P', 2, 0x22222222u);
        h.qa_inject(false, -1, A2, p2.data(), (int)p2.size());
        h.qa_inject(false, -1, A2, d02, sizeof d02);
        CHECK(got(h, &from) && from == 2);
        h.qa_inject(false, -1, A2, d01, sizeof d01);         // guest 2's address posing as guest 1
        CHECK(!got(h, &from) && h.spoofed_frames() == 7);
        const uint8_t b1[] = {'B', 1};
        h.qa_inject(false, -1, A2, b1, sizeof b1);           // a fake bye cannot end the session
        CHECK(!h.ended() && h.spoofed_frames() == 8);
        // NAT rebinding: a fresh valid token moves guest 1 to its new address
        auto p1 = hsf('P', 1, 0x11111111u);
        h.qa_inject(false, -1, "1.2.3.4:7000", p1.data(), (int)p1.size());
        h.qa_inject(false, -1, "1.2.3.4:7000", d01, sizeof d01);
        CHECK(got(h, &from) && from == 1);
        h.qa_inject(false, -1, A1, d01, sizeof d01);         // the old address no longer speaks as guest 1
        CHECK(!got(h, &from));
        // the token itself is only accepted over the relay
        auto t1x = tokf(1, 0x33333333u);
        h.qa_inject(false, -1, A1, t1x.data(), (int)t1x.size());
        h.qa_inject(false, -1, "1.2.3.4:7000", d01, sizeof d01);
        CHECK(got(h, &from) && from == 1);
    }
    {   // two guests behind one public IP: told apart by token, not by address
        Session h;
        CHECK(h.qa_offline(hc));
        h.qa_peer(1, true, "7.7.7.7:1000", "-"); h.qa_peer(2, true, "7.7.7.7:2000", "-");
        auto t1 = tokf(1, 0x11111111u), t2 = tokf(2, 0x22222222u);
        h.qa_inject(true, 1, "", t1.data(), (int)t1.size());
        h.qa_inject(true, 2, "", t2.data(), (int)t2.size());
        auto h1 = hsf('H', 1, 0x11111111u), h2 = hsf('H', 2, 0x22222222u);
        h.qa_inject(false, -1, "7.7.7.7:1000", h1.data(), (int)h1.size());
        h.qa_inject(false, -1, "7.7.7.7:2002", h2.data(), (int)h2.size());   // a symmetric NAT picked another port: still proven
        h.qa_inject(false, -1, "7.7.7.7:1000", d01, sizeof d01);
        CHECK(got(h, &from) && from == 1);
        h.qa_inject(false, -1, "7.7.7.7:2002", d02, sizeof d02);
        CHECK(got(h, &from) && from == 2);
        CHECK(h.spoofed_frames() == 0);
        h.qa_inject(false, -1, "7.7.7.7:2002", d01, sizeof d01);   // guest 2 poses as guest 1
        CHECK(!got(h, &from) && h.spoofed_frames() == 1);
    }
    {   // a legacy peer (no token) keeps the old rules next to a peer that sends one
        Session h;
        CHECK(h.qa_offline(hc));
        h.qa_peer(1, true, "-", "-"); h.qa_peer(2, true, "-", "-");
        auto t2 = tokf(2, 0x22222222u);
        h.qa_inject(true, 2, "", t2.data(), (int)t2.size());
        const uint8_t h1p[] = {'H', 1};
        h.qa_inject(false, -1, A1, h1p, sizeof h1p);         // plain 2-byte hello from the legacy guest: fine
        h.qa_inject(false, -1, A1, d01, sizeof d01);
        CHECK(got(h, &from) && from == 1);
        CHECK(h.spoofed_frames() == 0 && h.unproven_frames() == 2);
        h.qa_inject(false, -1, A1, d02, sizeof d02);         // but guest 2 has a token: that claim is refused
        CHECK(!got(h, &from) && h.spoofed_frames() == 1);
    }
    {   // host, relay
        Session h;
        CHECK(h.qa_offline(hc));
        h.qa_peer(1, true, "-", "-"); h.qa_peer(2, true, "-", "-");
        h.qa_inject(true, 1, "", d01, sizeof d01);
        CHECK(got(h, &from) && from == 1);
        h.qa_inject(true, 1, "", d02, sizeof d02);         // guest 1 poses as guest 2
        CHECK(!got(h, &from) && h.spoofed_frames() == 1);
        h.qa_inject(true, 2, "", dh, sizeof dh);           // guest 2 poses as the host
        CHECK(!got(h, &from));
        h.qa_inject(true, 2, "", d02, sizeof d02);
        CHECK(got(h, &from) && from == 2);
    }
    {   // guest 1: only the host is heard; a guest posing as the host is dropped
        SessionConfig gc; gc.slot = 1; gc.expected = 3;
        Session g;
        CHECK(g.qa_offline(gc));
        g.qa_peer(0, true, "-", H0); g.qa_peer(2, true, "-", H2);
        auto th = tokf(0, 0xAAAA0000u), tg = tokf(2, 0x22222222u);
        g.qa_inject(true, 0, "", th.data(), (int)th.size());
        g.qa_inject(true, 2, "", tg.data(), (int)tg.size());   // guests do not talk to each other: ignored
        auto hh = hsf('A', 0, 0xAAAA0000u);
        g.qa_inject(false, -1, H0, hh.data(), (int)hh.size());
        g.qa_inject(false, -1, H0, dh, sizeof dh);
        CHECK(got(g, &from) && from == 0);
        g.qa_inject(false, -1, H2, dh, sizeof dh);         // guest 2 poses as the host
        CHECK(!got(g, &from) && g.spoofed_frames() == 1);
        auto hx = hsf('P', 0, 0x12345678u);                // ... even with a guessed token
        g.qa_inject(false, -1, H2, hx.data(), (int)hx.size());
        CHECK(g.spoofed_frames() == 2);
        g.qa_inject(false, -1, H0, fwd2, sizeof fwd2);     // host forwards guest 2's data: fine
        CHECK(got(g, &from) && from == 2);
        g.qa_inject(false, -1, H2, fwd2, sizeof fwd2);     // guest 2 sends it itself: not the host's address
        CHECK(!got(g, &from));
        g.qa_inject(true, 0, "", fwd2, sizeof fwd2);       // relayed by the host: fine
        CHECK(got(g, &from) && from == 2);
        g.qa_inject(true, 2, "", dh, sizeof dh);           // relayed from guest 2 posing as the host
        CHECK(!got(g, &from));
        g.qa_inject(true, 2, "", fwd2, sizeof fwd2);       // guest 2 straight over the relay: guests never hear guests (not counted: not a partner)
        CHECK(!got(g, &from));
        const uint8_t r[] = {'R', 0, 0x07};
        g.qa_inject(true, 2, "", r, sizeof r);             // fake roster
        CHECK(g.spoofed_frames() == 3);
    }
    {   // bye: in a 3-player room the first guest leaving does not end the host's session
        Session h;
        CHECK(h.qa_offline(hc));
        h.qa_peer(1, true, "-", "-"); h.qa_peer(2, true, "-", "-");
        const uint8_t b1[] = {'B', 1}, b2[] = {'B', 2};
        h.qa_inject(true, 1, "", b1, sizeof b1);
        CHECK(!h.ended());
        h.qa_inject(true, 2, "", dh, sizeof dh);
        h.qa_inject(true, 2, "", d02, sizeof d02);
        CHECK(got(h, &from) && from == 2);                  // still hearing guest 2
        h.qa_peer(1, false, "-", "-");
        h.qa_inject(true, 2, "", b2, sizeof b2);            // the last one leaves
        CHECK(h.ended());
        // a lone guest's bye still ends a 2-player session at once
        SessionConfig c2; c2.slot = 0; c2.expected = 2;
        Session h2;
        CHECK(h2.qa_offline(c2));
        h2.qa_peer(1, true, "-", "-");
        h2.qa_inject(true, 1, "", b1, sizeof b1);
        CHECK(h2.ended());
        // a spectator in the room does not keep the session alive when the last player leaves; keep_on_guest_bye rooms keep going
        Session h3;
        CHECK(h3.qa_offline(hc));
        h3.qa_peer(1, true, "-", "-"); h3.qa_peer(2, true, "-", "-", true);
        h3.qa_inject(true, 1, "", b1, sizeof b1);
        CHECK(h3.ended());
        SessionConfig k = hc; k.keep_on_guest_bye = true;
        Session h4;
        CHECK(h4.qa_offline(k));
        h4.qa_peer(1, true, "-", "-");
        h4.qa_inject(true, 1, "", b1, sizeof b1);
        CHECK(!h4.ended());
    }
}

int main()
{
    caps_checks();
    session_checks();
    WsReconnect r;
    CHECK(!r.active());
    CHECK(!r.should_retry(1000));

    r.blip(1000);
    CHECK(r.active());
    CHECK(!r.should_retry(1100));                       // first try waits kFirstDelayMs
    CHECK(r.should_retry(1000 + WsReconnect::kFirstDelayMs));
    r.attempted(1400);
    CHECK(!r.should_retry(1500));                       // backoff
    r.blip(1450);                                       // idempotent: a second failure does not restart the window
    CHECK(r.blip_ms() == 1000);

    uint32_t t = 1400, last_gap = 0;
    for (int i = 0; i < 6; i++) {                       // delays grow but stay capped
        uint32_t before = t;
        while (!r.should_retry(t)) t += 10;
        last_gap = t - before;
        r.attempted(t);
    }
    CHECK(last_gap <= WsReconnect::kMaxDelayMs + 10);
    CHECK(last_gap >= WsReconnect::kMaxDelayMs - 10);

    CHECK(!r.expired(1000 + WsReconnect::kWindowMs - 1));
    CHECK(r.expired(1000 + WsReconnect::kWindowMs));
    CHECK(!r.should_retry(1000 + WsReconnect::kWindowMs + 5000));
    CHECK(r.gave_up());

    r.welcomed();
    CHECK(!r.active() && !r.gave_up() && r.attempts() == 0);

    // a later blip starts a fresh window
    r.blip(100000);
    CHECK(r.active() && r.blip_ms() == 100000);
    CHECK(r.should_retry(100000 + WsReconnect::kFirstDelayMs));

    // wrap-around safe
    WsReconnect w;
    w.blip(0xFFFFFF00u);
    CHECK(!w.expired(0xFFFFFF00u + 1000));
    CHECK(w.should_retry(0xFFFFFF00u + 500));
    CHECK(w.expired(0xFFFFFF00u + WsReconnect::kWindowMs));

    // reasons / codes
    CHECK(WsReconnect::retryable_reason("room-in-use"));
    CHECK(WsReconnect::retryable_reason("bad-slot"));
    CHECK(WsReconnect::retryable_reason("host-reconnecting"));
    CHECK(!WsReconnect::retryable_reason("spectators-full"));
    CHECK(!WsReconnect::retryable_reason("server-busy"));
    CHECK(WsReconnect::reconnectable_close(1006));
    CHECK(WsReconnect::reconnectable_close(4000));
    CHECK(!WsReconnect::reconnectable_close(1008));

    // departed rule: gone from the roster but UDP alive -> still a member
    CHECK(!member_departed(true, true));
    CHECK(member_departed(true, false));
    CHECK(!member_departed(false, false));

    printf(fails ? "hubreconnect_test: %d FAILED\n" : "hubreconnect_test: all ok\n", fails);
    return fails ? 1 : 0;
}
