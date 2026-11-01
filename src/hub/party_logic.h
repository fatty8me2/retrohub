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
 * 2. Phase::Playing: the host launches round N's game the normal way (the room's launch message).
 * 3. Phase::Intermission: the game ended (result, round timer, or "who won?" vote). Standings are shown, and the next game is
 *    preselected. Auto-ready (default on) readies everyone, because players can't press Ready while the hub is behind a game.
 * 4. Phase::Done: the podium, saved to history (feeds Wrapped).
 *
 * Couch party (no room, P3): the same reducer driven locally (transport = loopback); slots are controller ports. */
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "results.h"

namespace hubui {
namespace party {

enum class Phase { Off, Lobby, Playing, Intermission, Done };
enum class Scoring { Auto /* match results where the game reports them, else a vote */, Vote, None /* co-op: no points */ };

struct State {
    int v = 1;
    int rev = 0;                        // bumped by the host on every change; guests apply only newer revs
    Phase phase = Phase::Off;
    int round = 0, rounds = 3;          // round is 1-based while playing
    bool shuffle = false;
    uint32_t seed = 0;                  // host-picked; shuffle order is a pure function of (seed, pool)
    std::vector<std::string> list;      // game ids (room ids) in play order
    std::string cur;                    // this round's game id
    Scoring scoring = Scoring::Auto;
    int round_secs = 0;                 // 0 = no timer (the round ends with the game's result or when the game closes)
    bool auto_ready = true;
    std::map<int, int> points;          // slot -> total points
    std::map<int, std::string> names;   // slot -> name at the time it scored (standings survive a leave)
    std::map<int, std::string> votes;   // this round's votes: slot -> pick (game id or slot number as text)
};

/* 1st 3 points, 2nd 2, 3rd 1, 4th 0; a tie shares the better position's points. */
std::map<int, int> points_for(const std::vector<std::vector<int>>& ranks);

std::string encode_state(const State& s);                   // pt_state body
bool decode_state(const std::string& body, State* out);     // false = malformed / too new

/* Shuffle: deterministic from (seed, pool); every PC that sees the same State picks the same order. */
std::vector<std::string> shuffled(const std::vector<std::string>& pool, uint32_t seed, int count);

/* Host reducer. Pure. */
State on_result(const State& s, const results::RoundResult& r);           // a round's match result arrived
State on_vote(const State& s, int from_slot, const std::string& pick);
State on_round_end(const State& s);                                       // timer ran out / game closed without a result
State next_round(const State& s);                                         // Intermission -> Playing (or Done after the last round)

}  // namespace party
}  // namespace hubui
