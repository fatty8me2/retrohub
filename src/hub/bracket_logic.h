#pragma once
/* Brackets (Plan 4 P7): pure logic, no ImGui, no network. Unit tested in test_bracket.cpp.
 *
 * A bracket has 4, 8 or 16 positions; 2..16 entrants fill them in seed order and the empty positions are byes. Single or double
 * elimination (winners bracket, losers bracket, grand final, optional reset when the losers-bracket player wins the first final).
 * The state is just: format + seeded entrants + the ordered list of reported winners. Everything else (who plays whom, byes,
 * champion) is recomputed by replaying that list, so Undo is "drop the last report".
 *
 * Network: the HOST owns the bracket and broadcasts `bk_state` (to_json, always under kMaxJson bytes) on change only. A guest
 * accepts a state only from slot 0 and only with a newer `rev` (accept_remote). After a host change the new host calls
 * bump_for_host_change() and re-broadcasts, so every guest takes it. */
#include <cstdint>
#include <string>
#include <vector>

#include "results.h"

namespace hubui {
namespace bracket {

constexpr size_t kMaxJson = 4000;   // lobby messages stay under 4 KB
constexpr int kMaxEntrants = 16;
constexpr size_t kMaxName = 20;

enum class Format { Single, Double };
enum class Seeding { Rating, Random, Manual };

struct Entrant {
    std::string name, uid;   // uid empty = typed / couch name
    int rating = 1000;       // seeding input (ladder rating), shown on the page
};

/* Where a match slot comes from. */
struct Src {
    enum K { Seat, Win, Lose, SideA, SideB } k = Seat;
    int v = 0;   // Seat: seed index (0-based); Win/Lose/SideA/SideB: match id
};
struct Node {
    int id = 0;
    char br = 'W';   // 'W' winners, 'L' losers, 'G' grand final (round 1 = final, round 2 = reset)
    int round = 1, idx = 0;
    Src a, b;
};
struct Slot { bool known = false; int seat = -1; };   // known && seat < 0: nobody will ever arrive (bye)
struct MState {
    Slot a, b;
    bool done = false, bye = false, skipped = false;   // bye: a lone entrant moved on; skipped: the reset final that is not needed
    int winner = -1, loser = -1;                         // seats
};
struct Report { int match = 0, winner = -1; };

/* Orders entrants by seeding mode. Rating: highest first (ties keep the given order). Random: shuffled with `seed`. Manual: as given. */
std::vector<Entrant> seed_entrants(std::vector<Entrant> in, Seeding mode, uint32_t seed = 1);
/* Standard bracket order: positions[i] = 0-based seed index for position i (1 v N, 2 v N-1 ...). */
std::vector<int> seed_positions(int size);
int bracket_size(int entrants);   // 4, 8 or 16 (0 when out of range 2..16)

/* Winner of a finished match between two lobby slots (or controller ports when r.local): 0 = slot_a, 1 = slot_b, -1 = tie or
 * one of them is not in the result. */
int winner_of_result(const results::RoundResult& r, int slot_a, int slot_b);

class Bracket {
  public:
    bool init(Format f, bool reset, const std::vector<Entrant>& seeded);   // false: fewer than 2 or more than 16 entrants
    bool valid() const { return !seats_.empty(); }
    Format format() const { return fmt_; }
    bool reset_enabled() const { return reset_; }
    int size() const { return size_; }
    int players() const { return (int)seats_.size(); }
    const Entrant& seat(int i) const { return seats_[(size_t)i]; }
    const std::vector<Entrant>& seats() const { return seats_; }
    const std::vector<Node>& nodes() const { return nodes_; }
    const MState& state(int match) const { return st_[(size_t)match]; }
    const std::vector<Report>& reports() const { return reps_; }
    int rev() const { return rev_; }

    bool playable(int match) const;
    int current() const;                 // match id to play now (-1 = none / finished)
    int next() const;                    // the one after it
    /* Manual winner pick (also the path for results): the seat must be one of the two players. */
    bool report(int match, int winner_seat);
    /* A finished match between two seats: fills the current match when both seats are its players and the result is not a tie. */
    bool report_result(const results::RoundResult& r, int slot_of_seat_a_in_match, int slot_of_seat_b_in_match);
    bool can_undo() const { return !reps_.empty(); }
    bool undo();
    int champion() const;                // seat, -1 until decided
    bool finished() const { return champion() >= 0; }
    int find_match(int seat_x, int seat_y) const;   // playable match between these two seats, -1 if none
    std::string match_label(int match) const;       // "Winners R2", "Losers R1", "Grand final", "Grand final reset"
    std::string seat_name(int s) const;              // "BYE" for -1

    /* host-authoritative sync */
    std::string to_json() const;                      // {"kind":"bk_state",...}; "" if it would exceed kMaxJson (never for 16 entrants)
    static bool from_json(const std::string& text, Bracket* out);
    /* Guest side: take `text` only from slot 0 and only when newer than what we hold. Returns true when adopted. */
    bool accept_remote(int from_slot, const std::string& text);
    void bump_for_host_change() { ++rev_; }
    void set_rev(int r) { rev_ = r; }

  private:
    void build();
    void recompute();
    Format fmt_ = Format::Single;
    bool reset_ = true;
    int size_ = 0, rev_ = 0;
    std::vector<Entrant> seats_;
    std::vector<Node> nodes_;
    std::vector<MState> st_;
    std::vector<Report> reps_;
};

}  // namespace bracket
}  // namespace hubui
