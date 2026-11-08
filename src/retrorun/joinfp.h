/* Join fingerprint (netcore-H4): a drop-in joiner or a spectator says which game, which core build and which netcode it runs, and the host
 * compares that with its own before sending a snapshot. A friend with another dump (USA vs Japan, a hack), another core or another
 * RetroHub would otherwise load the host's state, drift apart and force a resync for the whole room.
 *
 * Wire: a trailer on the kPktJoin knock, after the existing fields (plain spectator knock: offset 3; drop-in knock: after the name):
 *   [kMagic, kTrailerVer, content u32 LE, core u32 LE, proto u8]   = kLen bytes
 * A knock without the trailer comes from an older RetroHub: refused like an old peer (kRefuseOldPeer), never let in to desync. */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace joinfp {

constexpr uint8_t kMagic = 0xF9;
constexpr uint8_t kTrailerVer = 1;
constexpr uint8_t kProto = 1;         // bump when the netcode / snapshot format changes in a way that old and new cannot play together
constexpr size_t kLen = 11;
constexpr uint8_t kRefuseOldPeer = 5;    // same values as main.cpp's drop-in refusals (spectate::JoinRefuse extensions)
constexpr uint8_t kRefuseMismatch = 8;   // the joiner's game file differs from the host's
constexpr uint8_t kRefuseCore = 9;       // the joiner's emulator core differs from the host's

struct Fp {
    uint32_t content = 0;   // the game (joinid.h; 0 = could not be worked out: only equal to another 0, so a known id never joins an unknown one)
    uint32_t core = 0;      // core name + version
    uint8_t proto = kProto;
};

enum class Verdict { Ok, Missing, Proto, Rom, Core };

inline uint64_t fnv(uint64_t h, const void* p, size_t n) {
    const unsigned char* c = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ c[i]) * 0x100000001B3ull;
    return h;
}
inline uint32_t fold(uint64_t h) { return (uint32_t)(h ^ (h >> 32)); }
inline uint32_t core_id(const std::string& name, const std::string& version) {
    uint64_t h = fnv(0xCBF29CE484222325ull, name.data(), name.size());
    h = fnv(h ^ 0xFF, version.data(), version.size());
    return fold(h);
}

/* Text index files point at the real tracks and differ between PCs without the game differing: not worth comparing. */
inline bool is_index_file(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e;
    for (size_t i = dot + 1; i < path.size(); i++) e += (char)((path[i] >= 'A' && path[i] <= 'Z') ? path[i] + 32 : path[i]);
    return e == "cue" || e == "m3u" || e == "gdi" || e == "ccd" || e == "toc";
}

inline void append(std::vector<uint8_t>& v, const Fp& f) {
    v.push_back(kMagic);
    v.push_back(kTrailerVer);
    for (int i = 0; i < 4; i++) v.push_back((uint8_t)(f.content >> (8 * i)));
    for (int i = 0; i < 4; i++) v.push_back((uint8_t)(f.core >> (8 * i)));
    v.push_back(f.proto);
}

/* The trailer at `off` of a knock; false when the knock has none (an older RetroHub). */
inline bool parse(const uint8_t* d, size_t n, size_t off, Fp* out) {
    if (off > n || n - off < kLen || d[off] != kMagic || d[off + 1] != kTrailerVer) return false;
    uint32_t c = 0, k = 0;
    for (int i = 0; i < 4; i++) { c |= (uint32_t)d[off + 2 + i] << (8 * i); k |= (uint32_t)d[off + 6 + i] << (8 * i); }
    out->content = c; out->core = k; out->proto = d[off + 10];
    return true;
}

/* The host's verdict on a joiner. `have` = the knock carried a trailer. */
inline Verdict check(const Fp& host, bool have, const Fp& theirs) {
    if (!have) return Verdict::Missing;
    if (theirs.proto != host.proto) return Verdict::Proto;
    if (theirs.core != host.core) return Verdict::Core;
    if (host.content != theirs.content) return Verdict::Rom;   // 0 vs a real id differs too: same core means the same system, so one side knows what the other cannot
    return Verdict::Ok;
}

/* kPktNoLive reason for a verdict: an older / different RetroHub keeps the existing old-version refusal. */
inline uint8_t refuse_code(Verdict v) { return v == Verdict::Rom ? kRefuseMismatch : v == Verdict::Core ? kRefuseCore : kRefuseOldPeer; }

/* Where the trailer sits in a knock: after the name of a drop-in knock ([kPktJoin, slot, 2, caps, name_len, name]), else right after a plain spectator knock. */
inline size_t trailer_offset(const std::vector<uint8_t>& m) { return m.size() >= 5 && m[2] == 2 ? 5u + m[4] : 3u; }

/* The host's whole decision for one knock (what Frontend::join_gate does before it answers): pass, or the kPktNoLive reason to send. */
struct Gate { bool pass = false; Verdict v = Verdict::Missing; uint8_t reason = kRefuseOldPeer; };
inline Gate gate(const Fp& host, const std::vector<uint8_t>& knock) {
    Fp theirs;
    const bool have = parse(knock.data(), knock.size(), trailer_offset(knock), &theirs);
    Gate g;
    g.v = check(host, have, theirs);
    g.pass = g.v == Verdict::Ok;
    g.reason = g.pass ? 0 : refuse_code(g.v);
    return g;
}

inline const char* verdict_name(Verdict v) {
    switch (v) { case Verdict::Ok: return "ok"; case Verdict::Missing: return "no fingerprint (older RetroHub)"; case Verdict::Proto: return "different netcode version";
                 case Verdict::Rom: return "different game file"; case Verdict::Core: return "different emulator core"; }
    return "?";
}

/* What a spectator / drop-in joiner shows when the host refuses with kRefuseMismatch. */
inline const char* mismatch_text() { return "Your copy of this game is different from the host's, so you can't join this match."; }
inline const char* core_text() { return "You and the host are using different emulator cores for this game, so you can't join this match. Update RetroHub on both PCs."; }
inline const char* old_text() { return "The host's RetroHub is a different version from yours, so you can't join this match. Update RetroHub on both PCs."; }

}  // namespace joinfp
