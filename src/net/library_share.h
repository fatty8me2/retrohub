#pragma once
/* Friend game libraries (metadata only, never files): what a client publishes about the games it owns, and the pure
 * helpers around it. Header-only so the console tests need no library. See docs/FRIENDS.md "Game libraries". */

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace hubnet {

constexpr size_t kMaxLibGames = 2000;   // the server keeps at most this many per account

/* One owned game. sys = system short code ("n64"); key = the library's stable game key; cid = catalog id ("" if none);
 * title = display title. Field limits match the server: sys <= 8, key <= 64, cid <= 32, title <= 80 chars. */
struct LibGame {
    std::string sys, key, cid, title;
    bool operator==(const LibGame& o) const { return sys == o.sys && key == o.key && cid == o.cid && title == o.title; }
};

inline std::string lib_clip(const std::string& s, size_t max)
{
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // do not cut a UTF-8 sequence
    return s.substr(0, n);
}

/* Trims every field to the server limits, drops entries without sys/key, de-duplicates on sys+key, sorts (so the hash is
 * order independent) and caps at kMaxLibGames. */
inline std::vector<LibGame> lib_normalize(std::vector<LibGame> g)
{
    std::vector<LibGame> out;
    out.reserve(g.size());
    for (LibGame& x : g) {
        x.sys = lib_clip(x.sys, 8); x.key = lib_clip(x.key, 64); x.cid = lib_clip(x.cid, 32); x.title = lib_clip(x.title, 80);
        if (x.sys.empty() || x.key.empty()) continue;
        out.push_back(std::move(x));
    }
    std::sort(out.begin(), out.end(), [](const LibGame& a, const LibGame& b) { return a.sys != b.sys ? a.sys < b.sys : a.key < b.key; });
    out.erase(std::unique(out.begin(), out.end(), [](const LibGame& a, const LibGame& b) { return a.sys == b.sys && a.key == b.key; }), out.end());
    if (out.size() > kMaxLibGames) out.resize(kMaxLibGames);
    return out;
}

/* 64-bit FNV-1a over the share flag and the normalized list, as 16 hex chars. Sent as `h`; the server stores it and
 * echoes it in the welcome, so a reconnecting client only re-sends its list when something really changed. */
inline std::string lib_hash(bool share, const std::vector<LibGame>& normalized)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const std::string& s) {
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        h ^= 0x1f; h *= 1099511628211ull;
    };
    mix(share ? "1" : "0");
    if (share) for (const LibGame& g : normalized) { mix(g.sys); mix(g.key); mix(g.cid); mix(g.title); }
    static const char hex[] = "0123456789abcdef";
    std::string o(16, '0');
    for (int i = 15; i >= 0; --i) { o[i] = hex[h & 15]; h >>= 4; }
    return o;
}

} // namespace hubnet
