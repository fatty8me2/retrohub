#pragma once
/* ROM hacks and translations (Plan 4 H2-H4): the identity of a hack entry. Pure; no ImGui, no file system.
 *
 * A hack entry is a patch file (IPS / UPS / BPS / VCDIFF, common/patcher.h) applied to a base game the player owns. It is its own
 * library entry with its own saves, title and cover:
 *   key        = <base game key> + ".hack-" + <CRC32 of the PATCHED image, 8 lowercase hex digits>
 *                e.g. "gba:bpre-1a2b3c4d.hack-9f8e7d6c". Same alphabet as every key ([a-z0-9._:-]); the suffix never occurs in a base key.
 *   catalog_id = "" always. So room_id() is the key and the room hello compares the exact content: a friend's stock ROM never
 *                matches a hack (GBA / GB / NDS ids alone would, they come from the header serial).
 *   path       = the BASE game's file (rom_missing() checks the base); hack_ensure_rom() prepares the patched image in
 *                rom-cache\hacks\ and play_entry / launch_online launch that copy.
 * Stored per hack in the vault (synced like a save): <vault>/<system>/<hack key folder>/patch/hack.json + the patch file next to it.
 *
 * Known limits (by design, documented in docs/PATCHES.md): cheats, RetroAchievements and cover matching key off the catalog, so a
 * hack starts without them (the cover is the base game's with a HACK ribbon unless the player picks one). GB/GBC hacks that already
 * sit in a library as plain files keep their header key (title + checksum, often unchanged by the patch). */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace hubui {
namespace hacks {

constexpr const char* kKeySep = ".hack-";

struct HackDef {
    std::string key;             // make_key(base_key, out_crc)
    std::string base_key;        // the base game's library key
    std::string system;          // system short ("gba", "snes", ...), the base game's
    std::string title;           // shown title (player can rename it through prefs `renames` like any game)
    std::string kind = "hack";   // "hack" | "translation"
    std::string lang;            // translation language tag ("en", "es", "pt-br"...); "" for hacks
    std::string patch_file;      // file name inside the patch folder
    std::string format;          // "ips" | "ups" | "bps" | "vcdiff"
    uint32_t patch_crc = 0;      // CRC32 of the patch file
    uint32_t out_crc = 0;        // CRC32 of the patched image (the key's suffix)
    uint64_t out_size = 0;
    int64_t added = 0;           // unix seconds
};

inline std::string hex8(uint32_t v) {
    char b[9];
    std::snprintf(b, sizeof b, "%08x", (unsigned)v);
    return b;
}

inline std::string make_key(const std::string& base_key, uint32_t out_crc) { return base_key + kKeySep + hex8(out_crc); }

inline bool is_hack_key(const std::string& key) {
    size_t p = key.rfind(kKeySep);
    if (p == std::string::npos || p == 0) return false;
    std::string h = key.substr(p + 6);
    if (h.size() != 8) return false;
    for (char c : h) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

/* "gba:bpre-1a2b3c4d.hack-9f8e7d6c" -> base "gba:bpre-1a2b3c4d", crc 0x9f8e7d6c. False if not a hack key. */
inline bool parse_key(const std::string& key, std::string* base_key, uint32_t* out_crc) {
    if (!is_hack_key(key)) return false;
    size_t p = key.rfind(kKeySep);
    if (base_key) *base_key = key.substr(0, p);
    if (out_crc) *out_crc = (uint32_t)std::strtoul(key.substr(p + 6).c_str(), nullptr, 16);
    return true;
}

/* hack.json text <-> HackDef: implemented by the hacks lane (L7) next to its tests. */
std::string encode(const HackDef& d);
bool decode(const std::string& text, HackDef* out);

}  // namespace hacks
}  // namespace hubui
