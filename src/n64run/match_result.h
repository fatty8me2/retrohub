#pragma once
/* Smash Remix / Smash 64 ROM VS results -> the match-result JSON the hub reads (src/hub/results.h, rivals_logic.h v1).
 * Plan 4 P1/P7. The n64run lane (L3) implements match_result.cpp and its test (RDRAM fixtures).
 *
 * --result-file <path>: the hub passes it for Remix and the Smash 64 ROM, online and offline. When the scene byte (0x800A4AD0) becomes
 * the VS results screen (0x18, remix::kVsResults), the frontend reads the player blocks from RDRAM ONCE per match and writes the
 * file atomically (tmp + rename), replacing the previous match. The game keeps running; the hub polls the file.
 *
 * RDRAM layout (from the decomp, docs/REMIX.md section 3; VERIFY on Remix and vanilla before relying on it): player block n
 * (n = controller port 0-3) at 0x800A4D28 + n * 0x74: stocks left +0x0B, place +0x0D, falls +0x10, score / KOs +0x14.
 * Online, every peer writes the same match (same id), so the hub's dedupe by id makes one match count once. */
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace n64match {

struct PlayerResult {
    int port = -1;        // controller port 0-3
    bool present = false; // a fighter was in this match
    int ch = -1;          // fighter id (remix_data.h)
    int place = 0;        // 0 = winner, larger = knocked out earlier
    int stocks = 0, kos = 0, falls = 0;
};

/* Pure: reads the four player blocks from an RDRAM image (N64 byte order as n64run's --dump-ram writes it). False when the blocks
 * look invalid (no or one present player, out-of-range values). */
bool read_results(const uint8_t* rdram, size_t rdram_size, std::vector<PlayerResult>* out);

/* The rivals JSON v1 the hub parses, plus "source":"n64run" and "local". port_to_slot maps a controller port to the lobby slot that
 * owns it (online; -1 = CPU / nobody); offline (local = true) the port number is used as the slot. */
std::string result_json(const std::string& game, const std::string& match_id, const std::string& room_code, bool local,
                        const std::vector<PlayerResult>& players, const std::vector<int>& port_to_slot);

}  // namespace n64match
