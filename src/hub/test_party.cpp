// Console test: party mode pure logic (party_logic.h): points_for ties, encode/decode round trip and the size cap, deterministic
// shuffle, the host reducer (results, votes, timer, game closed, last round -> Done), polls, standings, host migration, a guest
// ignoring an older rev, reconnects (reconcile) and the mixed-version check.
#include "party_closer.h"
#include "party_history.h"
#include "party_logic.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubui::party;
using hubui::results::RoundResult;
using Ranks = std::vector<std::vector<int>>;
using Names = std::map<int, std::string>;

static RoundResult mk(const std::string& id, const Ranks& ranks, bool local = false) {
    RoundResult r;
    r.id = id;
    r.game = "smash64";
    r.source = "battleship";
    r.local = local;
    r.ranks = ranks;
    return r;
}

/* A vote cast in the poll that is open now (what a client sends: the State's round and the poll's kind). */
static State vote(const State& s, int slot, const std::string& pick) { return on_vote(s, slot, s.round, s.poll, pick); }

/* A party with three games and two rounds, started with Mia (0), Jake (1), Sam (2). */
static State started(Scoring sc = Scoring::Auto, int rounds = 3) {
    State s = make_lobby(777, 4);
    s = configure(s, rounds, sc, 600, true);
    s = set_playlist(s, {"smash64", "mk64", "gba:kirby-1234"}, false, 777);
    return start_party(s, {0, 1, 2}, Names{{0, "Mia"}, {1, "Jake"}, {2, "Sam"}});
}

static void test_points() {
    auto p = points_for({{1}, {2}, {3}, {0}});
    CHECK(p.size() == 4 && p[1] == 3 && p[2] == 2 && p[3] == 1 && p[0] == 0);
    p = points_for({{1, 2}, {3}, {0}});                    // a tie for first shares first-place points; the next is third
    CHECK(p[1] == 3 && p[2] == 3 && p[3] == 1 && p[0] == 0);
    p = points_for({{1}, {2, 3}, {0}});                    // a tie for second shares second-place points
    CHECK(p[1] == 3 && p[2] == 2 && p[3] == 2 && p[0] == 0);
    /* competition ranking: a group's position is 1 + the players in the better groups; a tie gives every member that position's points */
    p = points_for({{10}, {11, 12}, {13}});                // a wins; b and c tie for 2nd (2 each); d is 4th - not 3rd - and gets 0
    CHECK(p[10] == 3 && p[11] == 2 && p[12] == 2 && p[13] == 0);
    p = points_for({{10, 11}, {12}, {13}});                // two tie for 1st (3 each); the next one is 3rd (1), the last 4th (0)
    CHECK(p[10] == 3 && p[11] == 3 && p[12] == 1 && p[13] == 0);
    p = points_for({{10}, {11}, {12, 13}});                // a tie for 3rd: 1 each
    CHECK(p[10] == 3 && p[11] == 2 && p[12] == 1 && p[13] == 1);
    p = points_for({{10}, {11, 12, 13}});                  // everybody behind the winner tied for 2nd
    CHECK(p[10] == 3 && p[11] == 2 && p[12] == 2 && p[13] == 2);
    p = points_for({{0, 1, 2, 3}});                        // everyone tied
    CHECK(p.size() == 4 && p[0] == 3 && p[1] == 3 && p[2] == 3 && p[3] == 3);
    p = points_for({{5}, {6}, {7}, {8}, {9}});             // a fifth place is worth nothing
    CHECK(p[5] == 3 && p[8] == 0 && p[9] == 0);
    p = points_for({{4}, {}, {4, 6}});                     // empty groups are skipped; a slot keeps its first group
    CHECK(p[4] == 3 && p[6] == 2 && p.size() == 2);
    CHECK(points_for({}).empty());
}

static void test_shuffle() {
    const std::vector<std::string> pool = {"smash64", "mk64", "gba:kirby-1234", "nes:abcd1234", "snes:ff00ff00", "gc:gm4e01-0"};
    auto a = shuffled(pool, 12345, 6), b = shuffled(pool, 12345, 6);
    CHECK(a == b && a.size() == 6);                        // deterministic
    CHECK(std::set<std::string>(a.begin(), a.end()) == std::set<std::string>(pool.begin(), pool.end()));   // a permutation of the pool
    std::vector<std::string> rev(pool.rbegin(), pool.rend());
    CHECK(shuffled(rev, 12345, 6) == a);                   // the order the pool was collected in does not matter
    auto dup = pool;
    dup.push_back("mk64");
    dup.push_back("");
    CHECK(shuffled(dup, 12345, 6) == a);                   // duplicates and empty ids are dropped
    bool differs = false;
    for (uint32_t seed = 1; seed <= 8; ++seed) if (shuffled(pool, seed, 6) != a) differs = true;
    CHECK(differs);                                        // the seed changes the order
    auto three = shuffled(pool, 99, 3);
    CHECK(three.size() == 3 && std::set<std::string>(three.begin(), three.end()).size() == 3);
    auto longer = shuffled(pool, 99, 15);                  // more rounds than games: they come round again, never twice in a row
    CHECK(longer.size() == 15);
    for (size_t i = 1; i < longer.size(); ++i) CHECK(longer[i] != longer[i - 1]);
    auto one = shuffled({"only"}, 1, 4);                   // a pool of one repeats
    CHECK(one.size() == 4 && one[0] == "only" && one[3] == "only");
    CHECK(shuffled({}, 1, 5).empty() && shuffled(pool, 1, 0).empty() && shuffled(pool, 1, -2).empty());
    /* golden: the algorithm must not change between releases or two PCs on different builds would disagree */
    auto g = shuffled({"a", "b", "c", "d"}, 2024, 4);
    CHECK(g == (std::vector<std::string>{"d", "b", "a", "c"}));
    CHECK(shuffled({"a", "b", "c", "d"}, 0, 4).size() == 4 && shuffled({"a", "b", "c", "d"}, 0xFFFFFFFFu, 4).size() == 4);   // any seed works, also 0
}

static void test_wire() {
    State s = started();
    s = next_round(s);
    RoundResult r = mk("AB12-9", {{2}, {0, 1}});
    s = on_result(s, r, Names{{0, "Mia"}, {1, "Jake"}, {2, "Sam"}});
    s = open_poll(s, "next", {"mk64", "smash64", "gba:kirby-1234"});
    s = vote(s, 1, "mk64");
    s = vote(s, 0, "nes:newgame-00");                  // a proposal outside the shortlist
    const std::string body = encode_state(s);
    CHECK(body.size() < 4096 && body.size() <= kMaxBody);
    State d;
    CHECK(decode_state(body, &d));
    CHECK(d.rev == s.rev && d.phase == s.phase && d.round == s.round && d.rounds == s.rounds && d.seed == s.seed && d.list == s.list);
    CHECK(d.cur == s.cur && d.scoring == s.scoring && d.round_secs == s.round_secs && d.auto_ready == s.auto_ready);
    CHECK(d.points == s.points && d.names == s.names && d.votes == s.votes && d.firsts == s.firsts && d.gain == s.gain);
    CHECK(d.poll == "next" && d.cand == s.cand && d.played == s.played && d.last_id == "AB12-9" && d.end == "result" && d.players == s.players);
    CHECK(encode_state(d) == body);                        // stable
    json::Value j;
    CHECK(json::parse(body, &j) && j.str("kind") == "pt_state");
    CHECK(decode_state(j, &d));

    /* the size cap: ids at the limit everywhere must still fit one frame, and the standings survive the trimming */
    State big = s;
    const std::string longid(kMaxId, 'x');
    big.list.clear();
    big.cand.clear();
    big.played.clear();
    big.tied.clear();
    for (int i = 0; i < 12; ++i) {
        big.list.push_back(longid);
        big.cand.push_back(longid.substr(0, kMaxId - 2) + std::to_string(10 + i));
        big.played.push_back(longid);
        big.tied.push_back(longid.substr(0, kMaxId - 2) + std::to_string(10 + i));
    }
    big.pick = longid;
    for (int i = 0; i < 4; ++i) { big.names[i] = std::string(60, 'N'); big.votes[i] = longid; big.points[i] = 100 + i; }
    const std::string bb = encode_state(big);
    CHECK(bb.size() <= kMaxBody && bb.size() < 4096);
    CHECK(decode_state(bb, &d) && d.points == big.points && d.names.at(0).size() <= kMaxName);

    /* malformed / too new */
    CHECK(!decode_state("", &d) && !decode_state("{}", &d) && !decode_state("not json", &d));
    CHECK(!decode_state("{\"kind\":\"pt_vote\",\"v\":1,\"ph\":1}", &d));
    CHECK(!decode_state("{\"kind\":\"pt_state\",\"v\":2,\"rev\":1,\"ph\":1}", &d));          // newer than this build
    CHECK(!decode_state("{\"kind\":\"pt_state\",\"v\":0,\"rev\":1,\"ph\":1}", &d));
    CHECK(!decode_state("{\"kind\":\"pt_state\",\"v\":1,\"rev\":1}", &d));                    // no phase
    CHECK(!decode_state("{\"kind\":\"pt_state\",\"v\":1,\"rev\":1,\"ph\":9}", &d));
    CHECK(!decode_state("{\"kind\":\"pt_state\",\"v\":1,\"rev\":1,\"ph\":1,\"pts\":{\"x\":1}}", &d));
    CHECK(!decode_state("{\"kind\":\"pt_state\",\"v\":1,\"rev\":1,\"ph\":1,\"list\":[1,2]}", &d));
    CHECK(!decode_state(std::string(9000, ' ') + body, &d));
    /* an older rev of this build (fewer fields) and fields from the future decode fine */
    CHECK(decode_state("{\"kind\":\"pt_state\",\"v\":1,\"rev\":3,\"ph\":2,\"rd\":1,\"rds\":3,\"seed\":5,\"list\":[\"a\"],\"cur\":\"a\",\"future\":[1,2,3]}", &d));
    CHECK(d.rev == 3 && d.phase == Phase::Playing && d.list.size() == 1 && d.poll.empty() && !d.couch && d.points.empty() && d.auto_ready);
    CHECK(decode_state("{\"kind\":\"pt_state\",\"v\":1,\"rev\":4,\"ph\":1,\"rds\":99,\"secs\":99999,\"poll\":\"bogus\",\"vt\":{\"0\":\"x\"}}", &d));
    CHECK(d.rounds == kMaxRounds && d.round_secs == 3600 && d.poll.empty() && d.votes.empty());   // clamped, a bogus poll is dropped

    VoteMsg v{3, "winner", "2"}, vd;
    json::Value vj;
    CHECK(json::parse(encode_vote(v), &vj) && decode_vote(vj, &vd) && vd.round == 3 && vd.what == "winner" && vd.pick == "2");
    CHECK(!(json::parse("{\"kind\":\"pt_vote\",\"round\":1,\"what\":\"x\",\"pick\":\"2\"}", &vj) && decode_vote(vj, &vd)));
    CHECK(!(json::parse("{\"kind\":\"pt_vote\",\"round\":1,\"what\":\"next\",\"pick\":\"\"}", &vj) && decode_vote(vj, &vd)));
    CHECK(!(json::parse("{\"kind\":\"pt_state\",\"round\":1,\"what\":\"next\",\"pick\":\"a\"}", &vj) && decode_vote(vj, &vd)));
}

static void test_setup_and_rounds() {
    State s = make_lobby(31, 10);
    CHECK(s.phase == Phase::Lobby && s.rev == 11 && s.rounds == 5 && s.shuffle && s.auto_ready);
    CHECK(start_party(s, {0, 1}, Names{}).phase == Phase::Lobby);                    // no playlist yet: nothing starts
    CHECK(next_round(s).phase == Phase::Lobby);                                      // next_round only runs from Intermission
    State c = configure(s, 7, Scoring::Vote, 900, false);
    CHECK(c.rev == s.rev + 1 && c.rounds == 7 && c.scoring == Scoring::Vote && c.round_secs == 900 && !c.auto_ready);
    CHECK(configure(c, 7, Scoring::Vote, 900, false).rev == c.rev);                  // unchanged: no new rev
    CHECK(configure(s, 99, Scoring::Auto, -5, true).rounds == kMaxRounds && configure(s, 0, Scoring::Auto, 99999, true).rounds == 1);
    CHECK(configure(s, 0, Scoring::Auto, 99999, true).round_secs == 3600);
    State p = set_playlist(c, {"a", "", "b", std::string(kMaxId + 1, 'z'), "c"}, false, 5);
    CHECK(p.list == (std::vector<std::string>{"a", "b", "c"}) && !p.shuffle && p.seed == 5);
    CHECK(game_for_round(p, 1) == "a" && game_for_round(p, 3) == "c" && game_for_round(p, 4) == "a" && game_for_round(p, 0).empty());   // cycles
    CHECK(game_for_round(State(), 1).empty());

    State q = start_party(p, {0, 1, 2}, Names{{0, "Mia"}, {1, "Jake"}});
    CHECK(q.phase == Phase::Intermission && q.round == 0 && q.cur == "a" && q.points.size() == 3 && q.points.at(2) == 0);
    CHECK(q.names.at(0) == "Mia" && !q.names.count(2) && q.rev == p.rev + 1);
    State r1 = next_round(q);
    CHECK(r1.phase == Phase::Playing && r1.round == 1 && r1.cur == "a" && r1.rev == q.rev + 1);
    CHECK(next_round(r1).rev == r1.rev);                                              // already playing: no-op
    CHECK(on_result(q, mk("m0", {{0}, {1}})).rev == q.rev);                           // a result outside Playing is ignored
    CHECK(on_round_end(q).rev == q.rev);

    /* a full night: 3 rounds of results, then the podium */
    State s2 = started(Scoring::Auto, 3);
    s2 = next_round(s2);
    s2 = on_result(s2, mk("m1", {{0}, {1}, {2}}), Names{{0, "Mia"}, {1, "Jake"}, {2, "Sam"}});
    CHECK(s2.phase == Phase::Intermission && s2.round == 1 && s2.points.at(0) == 3 && s2.points.at(1) == 2 && s2.points.at(2) == 1);
    CHECK(s2.cur == "mk64" && s2.played == std::vector<std::string>{"smash64"} && s2.end == "result" && s2.firsts.at(0) == 1 && s2.gain.at(0) == 3);
    CHECK(s2.poll.empty() && s2.last_id == "m1");
    CHECK(on_result(s2, mk("m2", {{1}, {0}})).rev == s2.rev);                         // Intermission: ignored
    s2 = next_round(s2);
    CHECK(s2.phase == Phase::Playing && s2.round == 2 && s2.cur == "mk64" && s2.gain.empty() && s2.end.empty());
    s2 = on_result(s2, mk("m2", {{1}, {0}, {2}}));
    CHECK(s2.points.at(0) == 5 && s2.points.at(1) == 5 && s2.points.at(2) == 2 && s2.cur == "gba:kirby-1234");
    s2 = next_round(s2);
    s2 = on_result(s2, mk("m3", {{1}, {0, 2}}));
    CHECK(s2.round == 3 && s2.phase == Phase::Intermission && s2.cur.empty());       // the last round: nothing left to preselect
    CHECK(s2.points.at(1) == 8 && s2.points.at(0) == 7 && s2.points.at(2) == 4 && s2.played.size() == 3);
    State done = next_round(s2);
    CHECK(done.phase == Phase::Done && done.rev == s2.rev + 1 && done.cur.empty());
    CHECK(next_round(done).rev == done.rev);
    auto pod = podium(done);
    CHECK(pod.size() == 3 && pod[0].name == "Jake" && pod[0].points == 8 && pod[0].rank == 1 && pod[1].name == "Mia" && pod[2].name == "Sam");
    CHECK(end_party(done).phase == Phase::Off && end_party(done).rev == done.rev + 1);

    /* a result counts once; Vote and None scoring ignore game results; couch and room results do not mix */
    State t = next_round(started());
    State t1 = on_result(t, mk("same", {{0}, {1}}));
    CHECK(t1.rev == t.rev + 1);
    State back = t1;
    back.phase = Phase::Playing;                                                      // pretend the round restarted
    CHECK(on_result(back, mk("same", {{1}, {0}})).rev == back.rev);                   // same match id: counted once
    CHECK(on_result(t, mk("loc", {{0}, {1}}, true)).rev == t.rev);                    // a local match is not a room result
    State couch = t;
    couch.couch = true;
    CHECK(on_result(couch, mk("net", {{0}, {1}}, false)).rev == couch.rev && on_result(couch, mk("loc", {{0}, {1}}, true)).rev == couch.rev + 1);
    State v = next_round(started(Scoring::Vote));
    CHECK(on_result(v, mk("x", {{0}, {1}})).rev == v.rev);
    State n = next_round(started(Scoring::None));
    CHECK(on_result(n, mk("x", {{0}, {1}})).rev == n.rev);
    CHECK(on_result(t, mk("e", {})).rev == t.rev);
}

static void test_round_end_and_winner_vote() {
    State s = next_round(started(Scoring::Auto, 3));
    State e = on_round_end(s, "timer");                                               // time ran out, nobody reported a winner
    CHECK(e.phase == Phase::Intermission && e.end == "timer" && e.poll == "winner" && e.cand == (std::vector<std::string>{"0", "1", "2"}));
    CHECK(e.cur == "mk64" && e.played == std::vector<std::string>{"smash64"} && e.points == s.points);
    CHECK(on_round_end(e).rev == e.rev);                                              // not playing any more
    CHECK(on_round_end(s).end == "closed");
    CHECK(on_round_end(started(Scoring::None)).rev == started(Scoring::None).rev);
    State none = on_round_end(next_round(started(Scoring::None)), "timer");           // co-op: no points, no vote
    CHECK(none.phase == Phase::Intermission && none.poll.empty() && none.points.at(0) == 0);

    State v = vote(e, 0, "1");
    v = vote(v, 1, "1");
    CHECK(v.rev == e.rev + 2 && v.votes.size() == 2);
    CHECK(vote(v, 1, "1").rev == v.rev);                                           // the same vote again changes nothing
    CHECK(vote(v, 2, "9").rev == v.rev);                                           // not a candidate in a "who won?" poll
    CHECK(vote(started(), 0, "1").rev == started().rev);                              // no poll open
    /* a vote counts only in the poll it was cast in: the same round and the same kind of poll */
    CHECK(on_vote(e, 0, e.round - 1, "winner", "1").rev == e.rev);                    // a late vote for an earlier round
    CHECK(on_vote(e, 0, e.round + 1, "winner", "1").rev == e.rev);
    CHECK(on_vote(e, 0, e.round, "next", "1").rev == e.rev);                          // another kind of poll
    CHECK(on_vote(e, 0, e.round, "winner", "1").rev == e.rev + 1);
    CHECK(on_vote(e, 1, VoteMsg{e.round, "winner", "1"}).rev == e.rev + 1 && on_vote(e, 1, VoteMsg{e.round - 1, "winner", "1"}).rev == e.rev);
    Tally t = tally(v);
    CHECK(t.voters == 2 && t.rows[0].first == "1" && t.rows[0].second == 2 && t.leaders == std::vector<std::string>{"1"} && !t.tied());
    CHECK(!all_voted(v, {0, 1, 2}) && all_voted(v, {0, 1}));
    v = vote(v, 2, "0");
    CHECK(all_voted(v, {0, 1, 2}));
    State w = resolve_winner(v, {1}, Names{{1, "Jake"}});
    CHECK(w.poll.empty() && w.cand.empty() && w.votes.empty() && w.points.at(1) == 3 && w.points.at(0) == 0 && w.end == "vote" && w.firsts.at(1) == 1);
    CHECK(resolve_winner(w, {1}).rev == w.rev);                                       // poll already closed
    State s2 = resolve_winner(e, {}), s3 = resolve_winner(e, {0, 2});
    CHECK(s2.end == "skip" && s2.points == e.points && s2.poll.empty());             // skipped: no points
    CHECK(s3.points.at(0) == 3 && s3.points.at(2) == 3 && s3.points.at(1) == 0 && s3.firsts.at(0) == 1 && s3.firsts.at(2) == 1);   // a shared win
    /* a tie in the vote: both leaders; the host decides */
    State tv = vote(vote(e, 0, "0"), 1, "1");
    Tally tt = tally(tv);
    CHECK(tt.tied() && tt.leaders.size() == 2);
    /* going on without answering counts as skipped */
    State go = next_round(e);
    CHECK(go.phase == Phase::Playing && go.round == 2 && go.poll.empty() && go.points == e.points);
    /* a single player has nobody to ask */
    State solo = start_party(set_playlist(make_lobby(1), {"a"}, false, 1), {0}, Names{{0, "Mia"}});
    CHECK(on_round_end(next_round(solo), "timer").poll.empty());
}

static void test_next_poll() {
    State s = started();                                                              // Intermission, round 0, cur = smash64
    State p = open_poll(s, "next", {"mk64", "smash64", "mk64", "", "gba:kirby-1234"});
    CHECK(p.poll == "next" && p.cand == (std::vector<std::string>{"mk64", "smash64", "gba:kirby-1234"}) && p.rev == s.rev + 1);
    CHECK(open_poll(s, "winner", {"0"}).poll == "winner");                            // allowed in Intermission
    CHECK(open_poll(next_round(s), "next", {"a"}).rev == next_round(s).rev);          // not while a round is running
    CHECK(open_poll(s, "bogus", {"a"}).rev == s.rev && open_poll(s, "next", {}).rev == s.rev);
    State v = vote(p, 0, "mk64");
    v = vote(v, 1, "gba:kirby-1234");
    v = vote(v, 2, "snes:fresh-0000");                                             // any game everyone can play joins the poll
    CHECK(v.cand.size() == 4 && v.cand.back() == "snes:fresh-0000");
    Tally t = tally(v);
    CHECK(t.voters == 3 && t.tied() && t.leaders.size() == 3);                        // 1-1-1
    bool tied = false;
    const std::string a = leading_pick(v, 1001, &tied), b = leading_pick(v, 1001, nullptr);
    CHECK(tied && a == b && !a.empty());                                              // the same tiebreak gives the same pick on every PC
    std::set<std::string> seen;
    for (uint32_t tb = 0; tb < 9; ++tb) seen.insert(leading_pick(v, tb, nullptr));
    CHECK(seen.size() == 3);                                                          // every tied option can win
    v = vote(v, 2, "mk64");                                                        // changing a vote replaces it
    CHECK(tally(v).leaders == std::vector<std::string>{"mk64"} && tally(v).voters == 3);
    CHECK(leading_pick(v, 5, &tied) == "mk64" && !tied);
    State r = resolve_next(v, 7);
    CHECK(r.poll.empty() && r.cand.empty() && r.votes.empty() && r.pick == "mk64" && r.tied.empty() && r.cur == "mk64" && r.list[0] == "mk64");
    State rt = resolve_next(vote(vote(p, 0, "mk64"), 1, "smash64"), 4);          // a tie: the seeded pick, and the reel gets the tied options
    CHECK(rt.tied == (std::vector<std::string>{"mk64", "smash64"}) && (rt.pick == "mk64" || rt.pick == "smash64") && rt.cur == rt.pick);
    CHECK(resolve_next(rt, 1).rev == rt.rev);
    State nobody = resolve_next(p, 1);                                                // nobody voted: the preselected game stays
    CHECK(nobody.pick.empty() && nobody.cur == "smash64" && nobody.poll.empty());
    /* in a room without Party mode the vote works the same and only reports the winner */
    State off = open_poll(State(), "next", {"x", "y"});
    CHECK(off.phase == Phase::Off && off.poll == "next");
    State offr = resolve_next(vote(off, 0, "y"), 1);
    CHECK(offr.pick == "y" && offr.phase == Phase::Off && offr.cur.empty());
    CHECK(cancel_poll(p).poll.empty() && cancel_poll(s).rev == s.rev);
    /* set_next: the host overrides the preselected game */
    State sn = set_next(s, "gc:abc-0");
    CHECK(sn.cur == "gc:abc-0" && sn.list[0] == "gc:abc-0" && sn.rev == s.rev + 1 && set_next(sn, "gc:abc-0").rev == sn.rev);
    CHECK(set_next(next_round(s), "x").rev == next_round(s).rev);
}

static void test_standings_and_osd() {
    State s = next_round(started());
    s = on_result(s, mk("a", {{1}, {0, 2}}), Names{{0, "Mia"}, {1, "Jake"}, {2, "Sam"}});
    auto st = standings(s);
    CHECK(st.size() == 3 && st[0].name == "Jake" && st[0].rank == 1 && st[0].points == 3 && st[0].gain == 3);
    CHECK(st[1].name == "Mia" && st[1].rank == 2 && st[2].name == "Sam" && st[2].rank == 2);     // equal points and rounds won share a rank
    CHECK(osd_standings(s) == "R1/3 1 Jake 3  2 Mia 2  2 Sam 2");
    CHECK(osd_round_over(s) == "Round 1 over! Jake wins (+3)");
    State tie = next_round(s);
    tie = on_result(tie, mk("b", {{0, 2}, {1}}));
    CHECK(osd_round_over(tie) == "Round 2 over! Mia and Sam tie (+3)");
    State longn = s;
    longn.names[0] = "Maximilian the Magnificent"; longn.names[1] = "Bartholomew the Brave"; longn.names[2] = "Wolfeschlegelstein";
    longn.points[3] = 1; longn.names[3] = "Zaphod Beeblebrox";
    CHECK(osd_standings(longn).size() <= 60 && !osd_standings(longn).empty());
    CHECK(osd_standings(longn, 20).size() <= 20 && !osd_standings(longn, 20).empty());
    CHECK(osd_standings(State()).empty());
    State vp = on_round_end(next_round(started()), "timer");
    CHECK(osd_round_over(vp) == "Round 1 over! Who won? Vote in RetroHub");
    CHECK(osd_round_over(vp).size() < 120);
    CHECK(osd_timer(600, 2, 5) == "R2/5  10 min left" && osd_timer(95, 2, 5) == "R2/5  2 min left" && osd_timer(45, 2, 5) == "R2/5  45 s left" && osd_timer(0, 2, 5) == "R2/5  time is up!");
    CHECK(osd_timer(30, 0, 0) == "30 s left");
    CHECK(clock_text(581) == "9:41" && clock_text(-4) == "0:00" && clock_text(3600) == "60:00");
    /* the round count shown for a night that is not running yet */
    CHECK(osd_standings(started()) == "1 Mia 0  1 Jake 0  1 Sam 0");
    /* ghost slots (players who left) stay in the standings */
    State g = s;
    g.points[5] = 9; g.names[5] = "Kai";
    auto gs = standings(g);
    CHECK(gs[0].name == "Kai" && gs[0].ghost);
}

static void test_migration() {
    /* a 3-player night; the host (slot 0, Mia) leaves, the server promotes slot 1 (Jake) to 0; Sam stays slot 2 */
    State s = next_round(started());
    s = on_result(s, mk("a", {{1}, {0}, {2}}), Names{{0, "Mia"}, {1, "Jake"}, {2, "Sam"}});
    State pw = on_round_end(next_round(s), "timer");                                   // a "who won?" poll between slots 0..2
    pw = vote(pw, 0, "1");                                                          // Mia votes for Jake
    pw = vote(pw, 1, "2");                                                          // Jake votes for Sam
    pw = vote(pw, 2, "1");                                                          // Sam votes for Jake
    State m = on_host_change(pw, 1);
    CHECK(m.rev == pw.rev);                                                            // the renumbering itself is not a new revision
    CHECK(m.points.at(0) == 3 && m.points.at(2) == 1 && m.names.at(0) == "Jake" && m.names.at(2) == "Sam");
    CHECK(m.points.at(4) == 2 && m.names.at(4) == "Mia" && m.firsts.at(0) == 1);        // Mia's points live on under a ghost slot
    CHECK(m.votes.size() == 2 && m.votes.at(0) == "2" && m.votes.at(2) == "0");        // Mia's vote is gone; Jake (now 0) keeps his; Sam's vote for Jake follows him to slot 0
    CHECK(m.players == (std::vector<int>{0, 2}));
    CHECK(std::find(m.cand.begin(), m.cand.end(), "0") != m.cand.end() && m.cand.size() == 2);   // the winner poll offers the people who are still here
    auto st = standings(m);
    CHECK(st.size() == 3 && st[0].name == "Jake" && st[1].name == "Mia" && st[1].ghost && st[2].name == "Sam");
    CHECK(on_host_change(pw, 0).rev == pw.rev && on_host_change(pw, 0).points == pw.points);

    /* the new host re-broadcasts what it last received; guests that saw one update more still accept it */
    State last_seen_by_new_host = s;                                                   // rev R
    State guest_ahead = vote(on_round_end(next_round(s)), 2, "1");                  // the old host got further (rev R+...) before dropping
    CHECK(guest_ahead.rev > last_seen_by_new_host.rev);
    State re = rebroadcast(on_host_change(last_seen_by_new_host, 1));
    CHECK(re.rev == last_seen_by_new_host.rev + kRevLeap);
    CHECK(accept(on_host_change(guest_ahead, 1), re));                                 // a guest one or two revs ahead takes it
    CHECK(re.points == on_host_change(last_seen_by_new_host, 1).points && re.names == on_host_change(last_seen_by_new_host, 1).names);
    /* guests apply only newer revs of the same night */
    State cur = s;
    State older = s;
    older.rev = cur.rev - 1;
    State newer = s;
    newer.rev = cur.rev + 1;
    CHECK(!accept(cur, older) && !accept(cur, cur) && accept(cur, newer));
    State other = s;
    other.seed = cur.seed + 1;
    other.rev = 1;
    CHECK(accept(cur, other));                                                         // a different night (a new party) always starts fresh
    CHECK(accept(State(), cur));                                                       // a guest that knows nothing yet takes the first State
    State future = s;
    future.v = kStateV + 1;
    CHECK(!accept(cur, future));
    /* who may speak: only slot 0 (the host); the host never takes a State from the room; a guest applies only a newer rev */
    CHECK(accept_state_from(0, false, cur, newer));
    CHECK(!accept_state_from(1, false, cur, newer) && !accept_state_from(2, false, cur, newer) && !accept_state_from(3, false, cur, newer));
    CHECK(!accept_state_from(0, true, cur, newer));
    CHECK(!accept_state_from(0, false, cur, older) && !accept_state_from(0, false, cur, cur));
    CHECK(!accept_state_from(-1, false, cur, newer) && !accept_state_from(4, false, cur, newer));
    /* votes are for the host, from seats that are in the room */
    CHECK(accept_vote_from(1, true, {0, 1, 2}) && accept_vote_from(0, true, {0, 1, 2}));
    CHECK(!accept_vote_from(1, false, {0, 1, 2}) && !accept_vote_from(3, true, {0, 1, 2}) && !accept_vote_from(-1, true, {0, 1, 2}) && !accept_vote_from(1, true, {}));
    State freshhost = make_lobby(55, 0);
    CHECK(accept(cur, freshhost));
}

static void test_reconcile() {
    State s = next_round(started());
    s = on_result(s, mk("a", {{1}, {0}, {2}}), Names{{0, "Mia"}, {1, "Jake"}, {2, "Sam"}});
    Names all = {{0, "Mia"}, {1, "Jake"}, {2, "Sam"}};
    CHECK(reconcile(s, all).rev == s.rev);                                             // nothing changed
    /* Jake leaves: his score moves to a ghost slot */
    Names two = {{0, "Mia"}, {2, "Sam"}};
    State r1 = reconcile(s, two);
    CHECK(r1.rev == s.rev + 1 && !r1.points.count(1) && r1.points.at(4) == 3 && r1.names.at(4) == "Jake");
    /* Kai takes the free seat: starts at 0, Jake's score is untouched */
    Names withkai = {{0, "Mia"}, {1, "Kai"}, {2, "Sam"}};
    State r2 = reconcile(r1, withkai);
    CHECK(r2.points.at(1) == 0 && r2.names.at(1) == "Kai" && r2.points.at(4) == 3);
    /* Jake comes back into a different seat: Kai has nothing, so Jake gets his score back (matched by name) */
    State r3 = reconcile(r2, Names{{0, "Mia"}, {1, "Jake"}, {2, "Sam"}});
    CHECK(r3.points.at(1) == 3 && r3.names.at(1) == "Jake" && !r3.points.count(4));
    /* leave and a newcomer in the same update */
    State r4 = reconcile(s, Names{{0, "Mia"}, {1, "Kai"}, {2, "Sam"}});
    CHECK(r4.points.at(1) == 0 && r4.names.at(1) == "Kai" && r4.points.at(4) == 3 && r4.names.at(4) == "Jake");
    /* Jake returns into his own seat while Kai (with a real score) holds nothing */
    State r5 = reconcile(r1, Names{{0, "Mia"}, {1, "Jake"}, {2, "Sam"}});
    CHECK(r5.points.at(1) == 3 && !r5.points.count(4));
    /* lobby / off: untouched */
    CHECK(reconcile(make_lobby(1), all).rev == make_lobby(1).rev && reconcile(State(), all).rev == 0);
}

static void test_versions() {
    CHECK(peer_can_party("1.30.0", "1.30.0") && peer_can_party("v1.30.2", "1.30.2") && peer_can_party("1.31.0", "1.30.0"));
    CHECK(!peer_can_party("1.29.24", "1.30.0") && !peer_can_party("", "1.30.0") && !peer_can_party("1.9.1", "1.30.1"));
    /* a development build older than the release: peers on the same build count */
    CHECK(peer_can_party("1.29.24", "1.29.24") && !peer_can_party("1.29.23", "1.29.24"));
    CHECK(needs_update_text({}).empty());
    CHECK(needs_update_text({"Mickey"}) == "Mickey needs to update for Party mode");
    CHECK(needs_update_text({"Mickey", "Kai"}) == "Mickey and Kai need to update for Party mode");
}

/* ------------------------------------------------------------------ the close-the-game sequence */
static void test_closer() {
    using hubui::party::AckInfo;
    using hubui::party::CloseCmd;
    using hubui::party::Closer;
    Closer::Plan plan;
    plan.has_ctl = true;
    plan.msg = "Round 2 over! Mia wins (+3)";
    plan.corner = "R2/5 1 Mia 8  2 Jake 5";
    plan.fallback = "Standings: R2/5 1 Mia 8  2 Jake 5";
    {   // a runner that knows every command: msg, corner, hold, quit - in that order, each after the previous one's ack
        Closer c;
        uint64_t t = 1000;
        c.start(t, plan);
        CHECK(c.active());
        CloseCmd a = c.step(t, AckInfo());
        CHECK(a.kind == CloseCmd::Msg && a.text == plan.msg);
        c.sent(11);
        CHECK(c.step(t + 100, AckInfo()).kind == CloseCmd::None);                         // waits for the ack: the file holds ONE command
        CloseCmd b = c.step(t + 300, AckInfo{11, true, ""});
        CHECK(b.kind == CloseCmd::None);                                                   // the ack moves it on; the corner goes out next tick
        b = c.step(t + 320, AckInfo{11, true, ""});
        CHECK(b.kind == CloseCmd::Corner && b.text == plan.corner && b.corner == 1 && b.keep_secs >= 6);
        c.sent(12);
        CHECK(c.step(t + 400, AckInfo{11, true, ""}).kind == CloseCmd::None);
        CHECK(c.step(t + 700, AckInfo{12, true, ""}).kind == CloseCmd::None);              // corner acked ok: now hold
        CHECK(c.step(t + 5000, AckInfo{12, true, ""}).kind == CloseCmd::None);             // still holding (6 s from the start)
        CloseCmd q = c.step(t + 6100, AckInfo{12, true, ""});
        CHECK(q.kind == CloseCmd::None);
        q = c.step(t + 6120, AckInfo{12, true, ""});
        CHECK(q.kind == CloseCmd::Quit);
        c.sent(13);
        CHECK(c.step(t + 9000, AckInfo{13, true, ""}).kind == CloseCmd::None);             // Quit is waiting to be honoured
        CHECK(c.step(t + 6120 + 11000, AckInfo{13, true, ""}).kind == CloseCmd::None);
        CloseCmd w = c.step(t + 6120 + 12100, AckInfo{13, true, ""});
        CHECK(w.kind == CloseCmd::None);
        w = c.step(t + 6120 + 12200, AckInfo{13, true, ""});
        CHECK(w.kind == CloseCmd::WindowClose);                                            // the fallback: ask the window to close
        CHECK(c.step(t + 6120 + 12300, AckInfo{13, true, ""}).kind == CloseCmd::None);
        c.step(t + 6120 + 12200 + 30000, AckInfo());
        CHECK(!c.active());                                                                // gives up quietly; never a kill
    }
    {   // a runner that does not know OsdCorner: the standings go out as a second toast
        Closer c;
        uint64_t t = 0;
        c.start(t, plan);
        c.step(t, AckInfo());
        c.sent(1);
        c.step(t + 1000, AckInfo());                                                       // no ack in 900 ms: carry on anyway
        CloseCmd k = c.step(t + 1010, AckInfo());
        CHECK(k.kind == CloseCmd::Corner);
        c.sent(2);
        CHECK(c.step(t + 1300, AckInfo{2, false, "unsupported"}).kind == CloseCmd::None);
        CHECK(c.corner_unsupported());
        CloseCmd f = c.step(t + 1310, AckInfo{2, false, "unsupported"});
        CHECK(f.kind == CloseCmd::Msg && f.text == plan.fallback);
        c.sent(3);
        bool quit = false;
        for (uint64_t u = t + 1400; u < t + 9000 && !quit; u += 100) quit = c.step(u, AckInfo{3, true, ""}).kind == CloseCmd::Quit;
        CHECK(quit);
    }
    {   // no control channel (native Smash): no OSD at all, a short pause, then the window is asked to close
        Closer c;
        Closer::Plan np = plan;
        np.has_ctl = false;
        c.start(0, np);
        CHECK(c.step(10, AckInfo()).kind == CloseCmd::None && c.step(3900, AckInfo()).kind == CloseCmd::None);
        CHECK(c.step(4100, AckInfo()).kind == CloseCmd::None);
        CHECK(c.step(4200, AckInfo()).kind == CloseCmd::WindowClose);
    }
    {   // the game ended by itself: nothing more is sent
        Closer c;
        c.start(0, plan);
        c.step(0, AckInfo());
        c.finish();
        CHECK(!c.active() && c.step(10000, AckInfo()).kind == CloseCmd::None);
    }
    {   // a command that could not be written (sent(0)) does not stall the sequence
        Closer c;
        c.start(0, plan);
        c.step(0, AckInfo());
        c.sent(0);
        c.step(5, AckInfo());
        CHECK(c.step(10, AckInfo()).kind == CloseCmd::Corner);
    }
}

/* ------------------------------------------------------------------ party nights history */
static void test_history() {
    namespace fs = std::filesystem;
    State s = next_round(started());
    s = on_result(s, mk("a", {{1}, {0, 2}}), Names{{0, "Mia"}, {1, "Jake"}, {2, "Sam"}});
    s = next_round(s);
    s = on_result(s, mk("b", {{1}, {2}, {0}}));
    s = next_round(s);
    s = on_result(s, mk("c", {{0}, {1}, {2}}));
    s = next_round(s);
    CHECK(s.phase == Phase::Done);
    CHECK(hubui::party::night_id("K7M2QX", 777, false) == "K7M2QX-777" && hubui::party::night_id("", 5, false) == "couch-5" && hubui::party::night_id("K7M2QX", 5, true) == "couch-5");
    hubui::party::Night n = hubui::party::night_from_state(s, "K7M2QX-777", 1700000000000ll, 2);
    CHECK(n.rows.size() == 3 && n.played == 3 && n.rounds == 3 && n.games.size() == 3 && n.rows[0].name == "Jake" && n.rows[0].rank == 1);
    CHECK(n.rows[2].me && n.rows[2].name == "Sam");                          // my row (slot 2)
    CHECK(hubui::party::night_worth_saving(n));
    hubui::party::Night none = n;
    none.played = 0;
    CHECK(!hubui::party::night_worth_saving(none));
    hubui::party::Night back;
    CHECK(hubui::party::night_from_json(hubui::party::night_to_json(n), &back) && back.id == n.id && back.at == n.at && back.rows.size() == 3 &&
          back.rows[0].points == n.rows[0].points && back.games == n.games && back.rows[2].me);
    CHECK(!hubui::party::night_from_json("", &back) && !hubui::party::night_from_json("{\"id\":\"x\",\"at\":1}", &back));
    fs::path d = fs::temp_directory_path() / "rh_party_test";
    fs::remove_all(d);
    fs::create_directories(d);
    const std::string ud = (d / "user").u8string(), vd = (d / "vault").u8string();
    CHECK(hubui::party::save_night(n, ud, vd));
    CHECK(fs::exists(d / "user" / "party" / "K7M2QX-777.json") && fs::exists(d / "vault" / "_account" / "party" / "K7M2QX-777.json"));
    CHECK(hubui::party::save_night(n, ud, vd));                               // saving twice keeps one file
    hubui::party::Night older = n;
    older.id = "ZZZZ-1";
    older.at = 1600000000000ll;
    CHECK(hubui::party::save_night(older, ud, ""));
    auto all = hubui::party::load_nights(ud, vd);
    CHECK(all.size() == 2 && all[0].id == "K7M2QX-777" && all[1].id == "ZZZZ-1");
    CHECK(hubui::party::load_nights("", "").empty());
    CHECK(hubui::party::count_nights(all, 1650000000, 1750000000) == 1 && hubui::party::count_nights(all, 0, 2000000000) == 2);
    CHECK(hubui::party::my_wins(all, 0, 2000000000) == 0);                    // Sam did not win
    hubui::party::Night mine = n;
    mine.id = "MINE-1";
    mine.rows[0].me = true;
    CHECK(hubui::party::save_night(mine, ud, vd));
    CHECK(hubui::party::my_wins(hubui::party::load_nights(ud, vd), 0, 2000000000) == 1);
    for (int i = 0; i < 5; ++i) { hubui::party::Night x = n; x.id = "N" + std::to_string(i); x.at += i; hubui::party::save_night(x, ud, vd); }
    hubui::party::prune_nights(ud, false, 3);
    size_t files = 0;
    for (auto& e : fs::directory_iterator(d / "user" / "party")) { (void)e; ++files; }
    CHECK(files == 3);
    fs::remove_all(d);
}

int main() {
    test_points();
    test_shuffle();
    test_wire();
    test_setup_and_rounds();
    test_round_end_and_winner_vote();
    test_next_poll();
    test_standings_and_osd();
    test_migration();
    test_reconcile();
    test_versions();
    test_closer();
    test_history();
    if (!fails) std::printf("hub_party_test: all passed\n");
    return fails ? 1 : 0;
}
