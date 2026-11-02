// Which games run GLideN64 without framebuffer emulation / RDRAM readbacks when played online (pure logic).
// Those games keep CPU-visible framebuffers: GLideN64's GPU->RDRAM copies are timing dependent, so peers' RDRAM (and sometimes game
// state) diverged. Offline play keeps the effects; online is plain HLE rendering, which is deterministic.
#pragma once
#include <cctype>
#include <string>

namespace n64run {

// game: --game id; header_name: ROM header name (any case). Online only: the caller applies it when a session is on.
inline bool det_gfx_game(const std::string& game, const std::string& header_name) {
    static const char* const kIds[] = {"mp1", "mp2", "mp3", "mariogolf", "mariotennis", "pokestadium", "pokestadium2"};
    for (const char* id : kIds) if (game == id) return true;
    std::string up;
    for (char c : header_name) up += (char)std::toupper((unsigned char)c);
    static const char* const kHdr[] = {"MARIOPARTY", "MARIOGOLF", "MARIOTENNIS", "POKEMON STADIUM"};
    for (const char* h : kHdr) if (up.find(h) != std::string::npos) return true;
    return false;
}

}  // namespace n64run
