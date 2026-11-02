#pragma once
/* Friend challenges: pure logic (no UI, no network). "Challenge a friend from here" saves a state + screenshot, sends a CHALLENGE card as a DM
 * (the state and the picture ride as DM attachments, see attach_xfer.h), the friend plays from that state and posts a RESULT card back.
 *
 * Wire: a card travels in Message::card as "RHC1:" + base64url(compact JSON), at most hubnet::kCardMax (1536) characters in total. The text of the DM
 * (what an old client shows) is built by card_fallback_text(). decode_card never trusts its input: garbage, truncation and oversize give false. */
#include "friends.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hub::challenge {

constexpr const char* kCardPrefix = "RHC1:";
constexpr int64_t kMaxStateBytes = 8 * 1024 * 1024;   // the attachment cap; DS and PS1 states are bigger and are not offered
constexpr size_t kBoardRows = 8;

enum class GoalKind { FastestTime, JustBeatIt, Score };
struct Goal {
    GoalKind kind = GoalKind::JustBeatIt;
    int64_t target = 0;   // Score: the number the sender typed (to beat); 0 = none
};

enum class CardKind { Challenge, Result };
enum class Outcome { Done, GaveUp };

struct Card {
    CardKind kind = CardKind::Challenge;
    std::string id;                         // challenge id (16 hex), shared by the challenge and every result
    // Challenge
    std::string game_key, game_title, system, rom_key;
    std::string note;                       // optional, <= 80 chars
    std::string sender;                     // sender's display name (informational; the DM's from_name is authoritative)
    Goal goal;
    hubnet::Attach state;                   // the save state
    bool has_shot = false;
    hubnet::Attach shot;                    // the picture (optional)
    // Result
    std::string player;                     // who finished (display name)
    Outcome outcome = Outcome::Done;
    int64_t time_ms = -1;                   // FastestTime: elapsed; -1 = unknown
    int64_t score = -1;                     // Score: typed by the player; -1 = unknown
    int64_t at = 0;                         // ms since epoch
};

/* "RHC1:..." or "" when the card cannot be made to fit kCardMax (long titles/notes/names are trimmed first, the picture is dropped last). */
std::string encode_card(const Card& c);
bool decode_card(const std::string& text, Card* out);
bool looks_like_card(const std::string& text);   // cheap prefix test, no decode
/* What an old client (or a notification) shows for the DM text: "Challenge: Fastest time in Super Mario 64" / "Result: 1:23.45". */
std::string card_fallback_text(const Card& c);
std::string goal_text(const Goal& g);            // "Fastest time" | "Just beat it" | "Score 12,000"

std::string new_challenge_id();                  // 16 lowercase hex, random

/* ---- leaderboard ---- */
struct Row {
    int rank = 0;               // 1-based (ties share a rank); 0 for "gave up" rows
    std::string player;
    std::string text;           // "1:23.45", "12,000", "Done", "Gave up"
    bool gave_up = false;
    int64_t at = 0;
};
/* Rows for challenge `id` from every result card (cards of other challenges are ignored). One row per player (case-insensitive), their best
 * (a Done beats a GaveUp; FastestTime lower time, Score higher score, JustBeatIt the earliest). Order: finished by the goal, then gave-up
 * players. At most `cap` rows. */
std::vector<Row> leaderboard(const Goal& goal, const std::string& id, const std::vector<Card>& results, size_t cap = kBoardRows);

/* ---- text ---- */
std::string format_time(int64_t ms);                // "1:23.45", "0:05.07", "1:02:03.45"
std::string format_score(int64_t score);            // "12,000"
std::string osd_timer_text(int64_t elapsed_ms);     // runctl osd_corner text, tenths: "0:05.3" (hours past 59:59)
std::string result_text(const Card& result, const Goal& goal);   // the line a result shows: time / score / "Done" / "Gave up"

/* ---- rules ---- */
enum class RomCheck { Same, Different };
/* Keys compare case-insensitively; an empty key on either side is Different (we cannot prove it is the same game). */
RomCheck rom_check(const std::string& mine, const std::string& theirs);
std::string rom_check_message(const std::string& game_title);   // "You need the same game (<title>) to take this challenge."

/* Built-in runners only: false for Dolphin / Cemu / Citra / PCSX2 families and the native port. `system` is the library system short code. */
bool runner_supports_challenge(const std::string& system);
/* false (with a plain reason) when the saved state is over 8 MB (DS, PS1) or empty. */
bool challenge_state_ok(int64_t state_bytes, std::string* why = nullptr);
/* One call for the game-menu row: runner + size. why = text shown on the disabled row. */
bool challenge_eligible(const std::string& system, int64_t state_bytes, std::string* why = nullptr);
/* Playing a challenge turns RetroAchievements hardcore off for that run (loading a state breaks hardcore). */
constexpr bool kChallengeDisablesHardcore = true;
std::string hardcore_note();
/* Challenges are played offline only. */
std::string offline_only_note();

/* ---- scratch states ---- */
/* Received states live in <data_dir>\challenge\ and sender states in the same folder, never in the user's save slots. */
std::string scratch_dir(const std::string& data_dir);
std::string scratch_state_name(const std::string& challenge_id, bool received);   // "ch-<id>-recv.state" / "ch-<id>-send.state"
std::string scratch_shot_name(const std::string& challenge_id);                    // "ch-<id>.png"
std::string scratch_path(const std::string& data_dir, const std::string& file_name);

} // namespace hub::challenge
