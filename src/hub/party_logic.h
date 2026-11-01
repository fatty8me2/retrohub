#pragma once
/* Party mode (Plan 4 P1-P3): pure, host-authoritative state. No ImGui, no network, no files. Unit tested in test_party.cpp.
 * The party lane (L1) owns this file and party_ui.cpp; this header is the contract the other lanes rely on.
 *
 * Lobby messages (room `msg` bodies, kinds starting "pt_"; lobby.cpp routes every "pt_" kind to party_on_msg). Each body < 4 KB.
 * Older RetroHubs ignore unknown kinds, so a party room still works for them as an ordinary room.
 *   pt_state   host -> all   the whole State (encode_state); sent on every change and when membership changes. Guests never edit
 *                            the State; host migration: the new host (slot 0) re-broadcasts the State it last received.
 *   pt_vote    any  -> host  {kind, round, what:"next"|"winner", pick:<game id> | <slot>}; the host folds votes into the State.
 * Round results do not travel as messages: every PC reads its own game's result file (results.h), and the host's State is the
 * one that counts.
 *
 * Flow:
 * 1. Phase::Lobby: the host builds the playlist (games everyone owns, or shuffle) and the number of rounds.
 * 2. start_party() -> Phase::Intermission with round 0: the first game is preselected and everyone readies (auto-ready).
 * 3. The host launches the game the normal way (the room's launch message); the PC that sees it start calls next_round():
 *    Phase::Playing, round N.
 * 4. The round ends with the game's result (Smash), the round timer, or when the host's game closes: Phase::Intermission again,
 *    with the standings and the NEXT game preselected in `cur` ("" after the last round). A round that ended without a result
 *    (timer / game closed) opens the "who won?" poll in the same phase.
 * 5. next_round() after the last round -> Phase::Done: the podium, saved to history (party_history.h; feeds Wrapped).
 *
 * Votes (P2, any room, party on or off): open_poll("next", shortlist) -> members vote (on_vote) -> resolve_next(). A tie is
 * broken by a seeded pick (the same on every PC), shown as the Surprise-me style reel.
 *
 * Couch party (no room, P3): the same reducer driven locally (State::couch); slots are controller ports. */
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "json.h"
#include "results.h"

namespace hubui {
namespace party {

enum class Phase { Off, Lobby, Playing, Intermission, Done };
enum class Scoring { Auto /* match results where the game reports them, else a vote */, Vote, None /* co-op: no points */ };

constexpr int kStateV = 1;                 // State::v written by this build; decode_state refuses anything newer
constexpr int kMaxRounds = 10;
constexpr int kMaxList = 10;               // playlist entries
constexpr int kMaxCand = 8;                // poll options
constexpr size_t kMaxId = 96;              // a game id longer than this never enters a State
constexpr size_t kMaxName = 24;
constexpr size_t kMaxBody = 3900;          // encode_state output stays under this (the lobby server drops text frames above 4096 bytes)
constexpr int kGhostBase = 4;              // slots >= 4 hold the points of players who left (their names stay in the standings)
constexpr int kRevLeap = 16;               // a new host re-broadcasts with rev + this, so a guest that saw one more update still accepts it
constexpr const char* kMinApp = "1.30.0";  // first RetroHub release with Party mode

struct State {
    int v = kStateV;
    int rev = 0;                        // bumped by the host on every change; guests apply only newer revs
    Phase phase = Phase::Off;
    int round = 0, rounds = 3;          // round is 1-based while playing (0 = the first game has not started yet)
    bool shuffle = false;
    uint32_t seed = 0;                  // host-picked; shuffle order is a pure function of (seed, pool); also the night's id
    std::vector<std::string> list;      // game ids (room ids) in play order; round r plays list[(r-1) % list.size()]
    std::string cur;                    // Playing: this round's game id. Intermission: the NEXT round's game ("" after the last round)
    Scoring scoring = Scoring::Auto;
    int round_secs = 0;                 // 0 = no timer (the round ends with the game's result or when the game closes)
    bool auto_ready = true;
    std::map<int, int> points;          // slot -> total points
    std::map<int, std::string> names;   // slot -> name at the time it scored (standings survive a leave)
    std::map<int, std::string> votes;   // the open poll's votes: slot -> pick (game id or slot number as text)
    /* Plan 4 additions (older revs decode with these defaults): */
    bool couch = false;                 // couch party: slots are controller ports and only local results count
    std::string poll;                   // "" none | "next" (vote for the next game) | "winner" ("who won?")
    std::vector<std::string> cand;      // the open poll's options: game ids ("next") or slot numbers as text ("winner")
    std::vector<int> players;           // slots in this round (the "who won?" candidates)
    std::map<int, int> firsts;          // slot -> rounds won
    std::map<int, int> gain;            // slot -> points the LAST round gave (cleared when the next round starts)
    std::vector<std::string> played;    // game ids of the finished rounds
    std::string last_id;                // id of the last match counted (a result is counted once)
    std::string end;                    // how the last round ended: "result" | "timer" | "closed" | "vote" | "skip" | ""
    std::string pick;                   // the last "next" poll's winner ("" none)
    std::vector<std::string> tied;      // options that tied in that poll (empty = no tie); the reel spins over them
};

/* Points by finishing position: 1st 3, 2nd 2, 3rd 1, 4th 0 (and nothing below).
 * COMPETITION RANKING: a group's position is 1 + the number of players in the better groups, and every member of a tied group
 * gets the points of that position. [[a],[b,c],[d]]: a is 1st (3), b and c tie for 2nd (2 each), d is 4th (0) - not 3rd.
 * [[a,b],[c],[d]]: a and b tie for 1st (3 each), c is 3rd (1), d is 4th (0).
 * Every ranked slot is in the result, also with 0 points. A slot listed twice keeps its first group. */
std::map<int, int> points_for(const std::vector<std::vector<int>>& ranks);

/* pt_state body ({"kind":"pt_state",...}); always <= kMaxBody bytes (long lists / polls are trimmed, never the standings). */
std::string encode_state(const State& s);
bool decode_state(const std::string& body, State* out);     // false = malformed / too new
bool decode_state(const json::Value& body, State* out);

/* Shuffle: deterministic from (seed, pool); every PC that sees the same State picks the same order. `count` entries; when the
 * pool is smaller the games come round again in a fresh order (never the same game twice in a row). Empty pool -> empty. */
std::vector<std::string> shuffled(const std::vector<std::string>& pool, uint32_t seed, int count);

/* pt_vote body ({kind:"pt_vote", round, what:"next"|"winner", pick}) and its validated parse. */
struct VoteMsg { int round = 0; std::string what, pick; };
std::string encode_vote(const VoteMsg& v);
bool decode_vote(const json::Value& body, VoteMsg* out);

/* Host reducer. Pure; every function returns the state unchanged (same rev) when it does nothing, else with rev + 1. */
State on_result(const State& s, const results::RoundResult& r);           // a round's match result arrived
State on_result(const State& s, const results::RoundResult& r, const std::map<int, std::string>& names);   // + who sat where
/* A vote counts only for the poll it was cast in: `round` must be the State's round and `what` the open poll's kind ("next" |
 * "winner"); a late vote for an earlier round or another kind of poll is ignored (the State comes back unchanged). */
State on_vote(const State& s, int from_slot, int round, const std::string& what, const std::string& pick);
State on_vote(const State& s, int from_slot, const VoteMsg& v);
State on_round_end(const State& s);                                       // timer ran out / game closed without a result
State on_round_end(const State& s, const std::string& how);               // how = "timer" | "closed"
State next_round(const State& s);                                         // Intermission -> Playing (or Done after the last round)
State next_round(const State& s, const std::vector<int>& players);        // + who plays this round

/* ---- setup (Lobby) ---- */
State make_lobby(uint32_t seed, int rev = 0);                             // Off -> Lobby with the defaults (the host keeps counting revs)
State configure(const State& s, int rounds, Scoring scoring, int round_secs, bool auto_ready);   // clamps; Lobby only
State set_playlist(const State& s, const std::vector<std::string>& list, bool shuffle, uint32_t seed);   // Lobby only
State start_party(const State& s, const std::vector<int>& players, const std::map<int, std::string>& names);   // Lobby -> Intermission (round 0)
State end_party(const State& s);                                          // any phase -> Off
std::string game_for_round(const State& s, int round);                    // 1-based; "" when the playlist is empty
State set_next(const State& s, const std::string& game);                  // Intermission: the host (or a vote) chose the next round's game

/* ---- polls ---- */
State open_poll(const State& s, const std::string& what, const std::vector<std::string>& cand);   // "next" (any phase but Playing) or "winner"
State cancel_poll(const State& s);
struct Tally {
    std::vector<std::pair<std::string, int>> rows;   // option -> votes, most votes first (ties keep the option order)
    int voters = 0;                                  // valid votes cast
    std::vector<std::string> leaders;                // every option with the top count (empty = nobody voted)
    bool tied() const { return leaders.size() > 1; }
};
Tally tally(const State& s);
bool all_voted(const State& s, const std::vector<int>& member_slots);     // every listed member has a valid vote
/* The "next" poll's winner; a tie is broken by `tiebreak` (same on every PC); *tied tells whether it was needed. "" = nobody voted. */
std::string leading_pick(const State& s, uint32_t tiebreak, bool* tied = nullptr);
State resolve_next(const State& s, uint32_t tiebreak);                    // close the "next" poll: pick/tied set, set_next() in Intermission
State resolve_winner(const State& s, const std::vector<int>& winners);    // close the "who won?" poll; empty winners = no points; several = shared win
State resolve_winner(const State& s, const std::vector<int>& winners, const std::map<int, std::string>& names);

/* ---- standings ---- */
struct Standing { int slot = 0; std::string name; int points = 0, firsts = 0, gain = 0, rank = 0; bool ghost = false; };
std::vector<Standing> standings(const State& s);                          // best first (points, then rounds won); equal -> equal rank
std::vector<Standing> podium(const State& s);                             // the same list, for the night's end
/* Texts for the running game's OSD (ASCII; the corner slot is 60 characters, a toast 120 bytes). */
std::string osd_standings(const State& s, size_t max_len = 60);          // "R2/5 1 Mia 8  2 Jake 5  3 Sam 2"
std::string osd_round_over(const State& s);                              // "Round 2 over! Mia wins (+3)"
std::string osd_timer(int secs_left, int round, int rounds);             // "R2/5  4 min left"
std::string clock_text(int secs);                                        // "9:41"

/* ---- room membership / host migration ---- */
/* Players who left keep their points under ghost slots; a new player in a vacated slot starts at 0; a player who came back in
 * another slot gets their points moved there (matched by name). `members`: slot -> name of everyone in the room now. */
State reconcile(const State& s, const std::map<int, std::string>& members);
/* The server renumbered member `promoted_from_slot` to slot 0 (the old host left): moves every per-slot field the same way. */
State on_host_change(const State& s, int promoted_from_slot);
State rebroadcast(const State& last);                                    // the new host re-sends the State it last received (rev + kRevLeap)
/* Guest side: apply `incoming` over `cur`? Newer rev of the same night, or a different night (seed). */
bool accept(const State& cur, const State& incoming);
/* The whole policy for an incoming pt_state: only the host speaks for the party (sender slot 0), the host itself never takes a
 * State from the room, and a guest applies it only when accept() says it is newer. */
bool accept_state_from(int from_slot, bool i_am_host, const State& cur, const State& incoming);
/* A pt_vote is for the host to fold in: only a seat that is in the room can vote (member_slots = everyone in the room now). */
bool accept_vote_from(int from_slot, bool i_am_host, const std::vector<int>& member_slots);

/* Mixed versions: older RetroHubs ignore pt_*. A peer can do Party mode when its hello `app` is at least kMinApp (or at least this
 * build's version, for builds before the release bump). */
bool peer_can_party(const std::string& peer_app, const std::string& my_app);
std::string needs_update_text(const std::vector<std::string>& names);   // "Mickey needs to update for Party mode" ("" = nobody)

/* Small helpers the UI shares. */
std::string phase_name(Phase p);
std::string scoring_name(Scoring s);
int clamp_rounds(int r);
int clamp_round_secs(int secs);

}  // namespace party
}  // namespace hubui
