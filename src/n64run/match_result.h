#pragma once
/* Smash Remix / Smash 64 ROM VS results -> the match-result JSON the hub reads (src/hub/results.h, rivals_logic.h v1).
 * Plan 4 P1/P7. Implemented by match_result.cpp, unit tested in test_match_result.cpp (RDRAM fixtures taken from real matches).
 *
 * --result-file <path>: the hub passes it for Remix and the Smash 64 ROM, online and offline. When the scene byte (0x800A4AD0) turns into
 * the VS results screen (0x18, remix::screen::kVsResults) right after a VS battle (0x16), the frontend reads the player blocks from
 * RDRAM ONCE per match and writes the file atomically (tmp + rename), replacing the previous match. The game keeps running; the hub
 * polls the file. A "No Contest" (A+B+R+Z reset) is not a result and is skipped.
 *
 * RDRAM layout (decomp SCBattleState / SCPlayerData, docs/REMIX.md section 3; VERIFIED on Smash Remix 2.0.0 and the vanilla US ROM):
 * the VS block is at 0x800A4D08 (game_rules +0x03, is_team_battle +0x02), player block n (n = controller port 0-3) at
 * 0x800A4D28 + n * 0x74: pkind +0x02 (0 human, 1 CPU, 2 none), fkind +0x03, stock_count +0x0B (s8, -1 = out of stocks), place +0x0D
 * (0 = winner), falls +0x10 (s32), score +0x14 (s32, the KOs). The scene block is at 0x800A4AD0 (scene_curr +0x00, is_suddendeath
 * +0x10, is_reset +0x12).
 *
 * "place": 0 = winner, larger = knocked out earlier, ties share a number. The game fills the player blocks' place only for stock-only
 * rules; every other rule set (time, or both) is ranked by the results screen itself from score - falls, so read_results() ranks those
 * the same way (mnVSResultsSetRoyalPlace / SetTeamPlaceAll, including the sudden-death order) and the hub always gets a usable place.
 *
 * Online, every peer writes the same match id (same room, same match counter, a signature of the final blocks), so the hub counts one
 * match once. */
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace n64match {

constexpr uint32_t kSceneBase = 0x800A4AD0;       // SCCommonData
constexpr uint32_t kSceneSuddenDeath = 0x10;      // u8 bool: the battle went to sudden death
constexpr uint32_t kSceneIsReset = 0x12;          // u8 bool: A+B+R+Z "No Contest"
constexpr uint32_t kVsBlock = 0x800A4D08;         // gSCManagerTransferBattleState (Remix Global.vs)
constexpr uint32_t kSuddenDeathBlock = 0x800A4EF8;// gSCManagerVSBattleState: the sudden-death round's own battle state
constexpr uint32_t kPlayersOffset = 0x20;
constexpr uint32_t kPlayerStride = 0x74;
constexpr int kPlayers = 4;

struct PlayerResult {
    int port = -1;        // controller port 0-3
    bool present = false; // a fighter was in this match (human or CPU)
    bool cpu = false;     // a computer player: never reported to the hub
    int ch = -1;          // fighter id (remix_data.h)
    int place = 0;        // 0 = winner, larger = knocked out earlier
    int stocks = 0, kos = 0, falls = 0;
};

/* What the rest of the match looked like (for the id and the log). */
struct MatchInfo {
    int game_rules = 0;       // bit 1 = time, 2 = stock
    int stage = 0;
    bool team = false;
    bool sudden_death = false;
    bool no_contest = false;  // the match was abandoned with A+B+R+Z
    uint32_t signature = 0;   // FNV-1a over the stable result fields: the last part of the match id
};

/* Pure: reads the four player blocks from an RDRAM image in N64 (big-endian) byte order, the way n64run's --dump-ram writes it (index =
 * physical address). False when the blocks look invalid (fewer than two present players, out-of-range values) or the image is too small. */
bool read_results(const uint8_t* rdram, size_t rdram_size, std::vector<PlayerResult>* out, MatchInfo* info = nullptr);

/* Same for the live core's RDRAM, which mupen64plus keeps as host-endian 32-bit words (byte index ^ 3). */
bool read_results_host(const uint8_t* rdram, size_t rdram_size, std::vector<PlayerResult>* out, MatchInfo* info = nullptr);

/* Fires once per VS match: the first VI the scene is the results screen after the VS battle scene. Feed it the scene byte every VI. */
class Trigger {
  public:
    bool step(uint8_t scene);
    unsigned matches() const { return n_; }   // results screens seen after a battle so far
  private:
    int prev_ = -1;
    bool battle_ = false;
    unsigned n_ = 0;
};

/* The match id every peer derives alike. Online (room_code non-empty): "<room>-<launch>-m<counter>-<signature>" where `room` is the
 * --room value ("ABC234-1"); offline: "local-<unix time>-m<counter>-<signature>". */
std::string match_id(const std::string& room, unsigned counter, uint32_t signature, long long unix_time);

/* The rivals JSON v1 the hub parses, plus "source":"n64run" and "local". port_to_slot maps a controller port to the lobby slot that
 * owns it (online; -1 = CPU / nobody); offline (local = true) the port number is used as the slot. CPUs and ports nobody owns are left
 * out. Returns "" when fewer than two reportable players remain (a match against CPUs is not a result). */
std::string result_json(const std::string& game, const std::string& match_id, const std::string& room_code, bool local,
                        const std::vector<PlayerResult>& players, const std::vector<int>& port_to_slot);

}  // namespace n64match
