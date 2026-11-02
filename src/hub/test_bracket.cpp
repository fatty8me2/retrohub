// Console test: brackets (byes, single/double elimination, undo, serialization, host rev rules, seeding) and the friends-ladder Elo.
#include <cmath>
#include <cstdio>

#include "bracket_logic.h"
#include "elo_logic.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubui::bracket;

static std::vector<Entrant> ents(int n) {
    std::vector<Entrant> v;
    for (int i = 0; i < n; ++i) { Entrant e; e.name = "P" + std::to_string(i + 1); e.rating = 1500 - i * 10; v.push_back(e); }
    return v;
}
/* Plays every match; `pick_b` makes the second-listed seat win instead of the lower seed. Returns matches played. */
static int play_all(Bracket& b, bool lb_wins_gf = false) {
    int played = 0;
    for (int guard = 0; guard < 100 && b.current() >= 0; ++guard) {
        int c = b.current();
        const MState& m = b.state(c);
        int w = m.a.seat < m.b.seat ? m.a.seat : m.b.seat;
        if (lb_wins_gf && b.nodes()[(size_t)c].br == 'G') w = m.a.seat < m.b.seat ? m.b.seat : m.a.seat;   // the losers-bracket (higher seed number) player takes both finals
        CHECK(b.report(c, w));
        ++played;
    }
    return played;
}
static hubui::rivals::Match mk(const char* id, int64_t at, int myrank, const char* uid, int rank) {
    hubui::rivals::Match m; m.id = id; m.game = "smash64"; m.at = at;
    hubui::rivals::Player a; a.me = true; a.name = "Me"; a.rank = myrank;
    hubui::rivals::Player b; b.uid = uid; b.name = uid; b.rank = rank;
    m.players = {a, b};
    return m;
}

int main() {
    // sizes and seed positions
    CHECK(bracket_size(1) == 0 && bracket_size(2) == 4 && bracket_size(4) == 4 && bracket_size(5) == 8 && bracket_size(9) == 16 && bracket_size(17) == 0);
    CHECK((seed_positions(8) == std::vector<int>{0, 7, 3, 4, 1, 6, 2, 5}));
    { Bracket b; CHECK(!b.init(Format::Single, true, ents(1)) && !b.valid() && !b.init(Format::Single, true, ents(17))); }

    // single elimination, 8 players, seed 1 always wins: 7 matches, champion P1
    { Bracket b; CHECK(b.init(Format::Single, true, ents(8)));
      CHECK(b.nodes().size() == 7 && b.current() == 0 && b.next() == 1);
      CHECK(b.match_label(6) == "Final" && b.match_label(4) == "Semifinal" && b.match_label(0) == "Round 1");
      CHECK(play_all(b) == 7 && b.finished() && b.champion() == 0 && b.current() == -1 && b.next() == -1);
      CHECK(!b.report(0, 0)); }
    // byes: 5 players in 8 -> 3 byes (seeds 1-3 skip round 1), 4 real matches
    { Bracket b; CHECK(b.init(Format::Single, true, ents(5)) && b.size() == 8);
      int byes = 0; for (size_t i = 0; i < 4; ++i) if (b.state((int)i).bye) ++byes;
      CHECK(byes == 3);
      CHECK(b.seat_name(-1) == "BYE");
      CHECK(play_all(b) == 4 && b.champion() == 0); }
    // 2 players in a 4 bracket: one real match
    { Bracket b; CHECK(b.init(Format::Single, true, ents(2)));
      CHECK(play_all(b) == 1 && b.champion() == 0); }
    // upset: seed 8 beats seed 1 in round 1
    { Bracket b; b.init(Format::Single, true, ents(8));
      int m = b.find_match(0, 7); CHECK(m == 0);
      CHECK(!b.report(m, 3));   // not a player of that match
      CHECK(b.report(m, 7));
      CHECK(b.state(4).a.seat == 7 || b.state(4).b.seat == 7); }

    // undo restores state and bumps rev
    { Bracket b; b.init(Format::Single, true, ents(4));
      int r0 = b.rev();
      Bracket c; c.init(Format::Single, true, ents(4));
      CHECK(!c.can_undo() && !c.undo());
      CHECK(c.report(0, 0) && c.report(1, 1) && c.state(2).a.seat == 0 && c.state(2).b.seat == 1);
      CHECK(c.undo() && c.state(2).b.known == false && c.current() == 1 && c.reports().size() == 1);
      CHECK(c.undo() && c.current() == 0 && c.rev() > r0 + 2);
      CHECK(c.report(0, 0)); }

    // double elimination, 4 players: WB 3 + LB 2 + GF = 6 matches; WB champion wins both -> no reset
    { Bracket b; CHECK(b.init(Format::Double, true, ents(4)));
      CHECK(b.nodes().size() == 7);   // incl. the (unused) reset final
      CHECK(b.match_label(5) == "Grand final" && b.match_label(6) == "Grand final reset" && b.match_label(2) == "Losers R1");
      CHECK(play_all(b) == 6 && b.champion() == 0 && b.state(6).skipped); }
    // reset played when the losers-bracket player wins the first final (7 matches); with reset off it ends after the first
    { Bracket b; b.init(Format::Double, true, ents(4));
      CHECK(play_all(b, true) == 7 && b.finished() && b.champion() != 0);
      Bracket n; n.init(Format::Double, false, ents(4));
      CHECK(play_all(n, true) == 6 && n.finished() && n.state(6).skipped); }
    // 8 players double: WB 7 + LB 6 + GF = 14
    { Bracket b; CHECK(b.init(Format::Double, true, ents(8)));
      CHECK(b.nodes().size() == 15 && play_all(b) == 14 && b.champion() == 0);
      // a loser drops to the losers bracket: after P2 loses WB R1 vs P7 ... first LB match pairs two WB R1 losers
      Bracket c; c.init(Format::Double, true, ents(8));
      c.report(0, 0); c.report(1, 3); c.report(2, 1); c.report(3, 2);
      int lb = -1; for (size_t i = 0; i < c.nodes().size(); ++i) if (c.nodes()[i].br == 'L' && c.nodes()[i].round == 1 && c.nodes()[i].idx == 0) lb = (int)i;
      CHECK(lb >= 0 && c.playable(lb) && c.state(lb).a.seat >= 4 && c.state(lb).b.seat >= 4); }
    // 16 players double with byes (11 players): finishes, never stalls
    { Bracket b; CHECK(b.init(Format::Double, true, ents(11)) && b.size() == 16);
      int p = play_all(b); CHECK(b.finished() && p > 10 && b.champion() == 0); }
    // every size/count combination finishes with exactly one champion
    for (int n = 2; n <= 16; ++n)
        for (int f = 0; f < 2; ++f) {
            Bracket b; b.init(f ? Format::Double : Format::Single, true, ents(n));
            play_all(b);
            CHECK(b.finished() && b.current() == -1);
            if (f == 0) { Bracket c; c.init(Format::Single, true, ents(n)); CHECK(play_all(c) == n - 1); }
            else { Bracket c; c.init(Format::Double, true, ents(n)); int pl = play_all(c); CHECK(pl >= 2 * (n - 1) - 1 && pl <= 2 * (n - 1)); }
        }

    // results -> winner
    { hubui::results::RoundResult r; r.ranks = {{3}, {1, 2}};
      CHECK(winner_of_result(r, 3, 1) == 0 && winner_of_result(r, 1, 3) == 1 && winner_of_result(r, 1, 2) == -1 && winner_of_result(r, 1, 7) == -1);
      Bracket b; b.init(Format::Single, true, ents(4));
      CHECK(b.report_result(r, 3, 1) && b.state(0).winner == 0);   // slot 3 = seat 0 (first listed) won
      hubui::results::RoundResult t; t.ranks = {{0, 1}};
      CHECK(!b.report_result(t, 0, 1)); }

    // serialization: round trip, size cap with 16 long names + uids, corrupt input
    { std::vector<Entrant> big;
      for (int i = 0; i < 16; ++i) { Entrant e; e.name = std::string(40, 'x') + std::to_string(i); e.uid = std::string(36, 'u'); e.rating = 1000 + i; big.push_back(e); }
      Bracket b; b.init(Format::Double, true, big);
      play_all(b, true);
      std::string j = b.to_json();
      CHECK(!j.empty() && j.size() < 4096);
      Bracket r; CHECK(Bracket::from_json(j, &r));
      CHECK(r.champion() == b.champion() && r.rev() == b.rev() && r.reports().size() == b.reports().size() && r.seat(0).name.size() == kMaxName);
      CHECK(!Bracket::from_json("nope", &r) && !Bracket::from_json("{\"kind\":\"bk_state\",\"seats\":[],\"rep\":[]}", &r));
      std::string bad = j; size_t p = bad.find("\"rep\":["); bad.replace(p, 8, "\"rep\":[9,9,");
      CHECK(!Bracket::from_json(bad, &r)); }

    // host-authoritative rev rules
    { Bracket host; host.init(Format::Single, true, ents(4));
      std::string s1 = host.to_json();
      Bracket g;
      CHECK(!g.accept_remote(1, s1));            // only slot 0
      CHECK(g.accept_remote(0, s1) && g.valid());
      CHECK(!g.accept_remote(0, s1));            // same rev: ignored
      host.report(0, 0);
      std::string s2 = host.to_json();
      CHECK(g.accept_remote(0, s2) && g.reports().size() == 1);
      CHECK(!g.accept_remote(0, s1));            // older: ignored
      // host change: the new host (a guest copy) bumps and rebroadcasts; the other guests accept
      Bracket other = g;
      g.bump_for_host_change();
      CHECK(other.accept_remote(0, g.to_json()) && other.rev() == g.rev()); }

    // seeding
    { auto e = ents(4); std::swap(e[0], e[3]);   // P4(1470) first
      auto r = seed_entrants(e, Seeding::Rating);
      CHECK(r[0].name == "P1" && r[3].name == "P4");
      auto m = seed_entrants(e, Seeding::Manual); CHECK(m[0].name == "P4");
      auto a = seed_entrants(ents(8), Seeding::Random, 7), b2 = seed_entrants(ents(8), Seeding::Random, 7), c = seed_entrants(ents(8), Seeding::Random, 8);
      bool same = true, diff = false;
      for (size_t i = 0; i < 8; ++i) { same &= a[i].name == b2[i].name; diff |= a[i].name != c[i].name; }
      CHECK(same && diff);
      std::vector<Entrant> tie = ents(3); for (auto& t : tie) t.rating = 1000;
      CHECK(seed_entrants(tie, Seeding::Rating)[0].name == "P1"); }

    // Elo golden values
    { using namespace hubui::elo;
      double a = 1000, b = 1000;
      apply(&a, &b, 1.0);
      CHECK(std::fabs(a - 1016.0) < 1e-9 && std::fabs(b - 984.0) < 1e-9);
      apply(&a, &b, 1.0);
      CHECK(std::lround(a) == 1031 && std::lround(b) == 969);
      double x = 1000, y = 1000; apply(&x, &y, 0.5);
      CHECK(x == 1000 && y == 1000);   // equal players drawing: nothing moves
      double hi = 1200, lo = 1000; apply(&hi, &lo, 0.5);
      CHECK(std::lround(hi) == 1192 && std::lround(lo) == 1008);
      CHECK(std::fabs(expected(1000, 1000) - 0.5) < 1e-12 && std::fabs(expected(1400, 1000) - 0.9090909) < 1e-6);
      // ladder from stored matches (newest first): Me beat Mia twice, lost to Kai once, drew Kai; a stranger changes nobody
      std::vector<hubui::rivals::Match> ms = {mk("e", 5, 1, "", 2), mk("d", 4, 1, "kai", 2), mk("c", 3, 2, "kai", 1), mk("b", 2, 1, "mia", 2), mk("a", 1, 1, "mia", 2)};
      auto l = ladder(ms);
      CHECK(l.size() == 3);   // stranger (empty uid) not rated; me + mia + kai
      CHECK(l[0].me && l[0].rating > 1000);
      const Entry* mia = find(l, "mia"); const Entry* kai = find(l, "kai");
      CHECK(mia && kai && mia->rating < 1000 && mia->games == 2 && place_of(l, "mia") == 3);
      CHECK(rating_of(l, "nobody") == 1000 && place_of(l, "nobody") == 0);
      CHECK(rating_text(l, "mia").find("among friends") != std::string::npos && rating_text(l, "nobody").empty());
      CHECK(ladder({}).size() == 1 && ladder({})[0].me && ladder({})[0].rating == 1000);
      // 3-player match moves no rating by more than K
      hubui::rivals::Match m3; m3.id = "t"; m3.at = 1;
      hubui::rivals::Player p0; p0.me = true; p0.rank = 1; hubui::rivals::Player p1; p1.uid = "u1"; p1.name = "U1"; p1.rank = 2; hubui::rivals::Player p2; p2.uid = "u2"; p2.name = "U2"; p2.rank = 3;
      m3.players = {p0, p1, p2};
      auto l3 = ladder({m3});
      CHECK(l3[0].me && l3[0].rating <= 1032 && l3[0].rating > 1000); }

    std::printf(fails ? "hub_bracket_test: %d FAILED\n" : "hub_bracket_test: ok\n", fails);
    return fails ? 1 : 0;
}
