#pragma once
/* Deleted save states (saves-F15).
 *
 * Cloud saves v2 has no per-file delete (only "remove the whole game"), so a state the player deleted would be pulled straight back on the next launch.
 * Instead this PC remembers WHAT it deleted: the file's identity (size + SHA-256 of the content). The engine (cloud_engine.cpp evaluate) then hides only a
 * cloud file with exactly that identity (no download, nothing to upload). A different file at the same path - a new state saved here, or one another PC
 * uploaded later - has a different identity, so it is never hidden and the tombstone is dropped. No clocks are involved.
 *
 * Scope is this PC only: other PCs keep their own copy (no server change; the Worker has no per-file delete).
 * The list is <vault>\.sync\state-tombstones.txt (the .sync folder never syncs). One line per file: "<unix>\t<size>\t<sha256>\t<key>",
 * key = "<system folder>/<game folder>/states/<file>". Every read-modify-write holds one process-wide mutex and replaces the file atomically (temp + rename).
 * Header only and dependency free so the hub's pure state logic and the engine share one definition. */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace hub { namespace saves { namespace tomb {

constexpr size_t kMax = 10000;   // entries are tiny; the oldest are dropped beyond this (and the drop is logged next to the list)

struct Entry { int64_t when = 0; int64_t size = 0; std::string sha; };

inline std::mutex& mu() { static std::mutex m; return m; }

namespace detail {
/* Incremental SHA-256: files are hashed in chunks, never held whole in memory. */
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char buf[64];
    size_t fill = 0;
    uint64_t total = 0;
    static uint32_t rotr(uint32_t x, int c) { return (x >> c) | (x << (32 - c)); }
    void block(const unsigned char* m) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = ((uint32_t)m[i * 4] << 24) | ((uint32_t)m[i * 4 + 1] << 16) | ((uint32_t)m[i * 4 + 2] << 8) | m[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const unsigned char* d, size_t n) {
        total += n;
        while (n) {
            const size_t take = std::min(n, sizeof buf - fill);
            std::memcpy(buf + fill, d, take);
            fill += take; d += take; n -= take;
            if (fill == sizeof buf) { block(buf); fill = 0; }
        }
    }
    std::string hex() {
        const uint64_t bits = total * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char zero = 0;
        while (fill != 56) update(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; ++i) len[i] = (unsigned char)(bits >> ((7 - i) * 8));
        update(len, 8);
        char out[65];
        for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
        return std::string(out, 64);
    }
};
}  // namespace detail

inline std::string file_for(const std::string& vault) {
    return (std::filesystem::u8path(vault) / ".sync" / "state-tombstones.txt").u8string();
}

/* identity of a file on disk: size + SHA-256, read in 64 KB chunks; false when it cannot be read */
inline bool identity_of_file(const std::string& path_utf8, Entry* out) {
    std::ifstream f(std::filesystem::u8path(path_utf8), std::ios::binary);
    if (!f) return false;
    detail::Sha256 sha;
    std::vector<unsigned char> chunk(64 * 1024);
    int64_t size = 0;
    while (f) {
        f.read((char*)chunk.data(), (std::streamsize)chunk.size());
        const std::streamsize got = f.gcount();
        if (got <= 0) break;
        sha.update(chunk.data(), (size_t)got);
        size += got;
    }
    if (f.bad()) return false;
    out->size = size;
    out->sha = sha.hex();
    return true;
}

inline std::map<std::string, Entry> load_unlocked(const std::string& vault) {
    std::map<std::string, Entry> m;
    std::ifstream f(std::filesystem::u8path(file_for(vault)), std::ios::binary);
    std::string line;
    while (f && std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1), c = b == std::string::npos ? b : line.find('\t', b + 1);
        if (c == std::string::npos || c + 1 >= line.size() || c - b - 1 != 64) continue;
        Entry e;
        e.when = std::strtoll(line.substr(0, a).c_str(), nullptr, 10);
        e.size = std::strtoll(line.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
        e.sha = line.substr(b + 1, 64);
        m[line.substr(c + 1)] = e;
    }
    return m;
}

inline bool store_unlocked(const std::string& vault, std::map<std::string, Entry> m) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::u8path(file_for(vault)), tmp = p;
    tmp += ".hubtmp";
    std::filesystem::create_directories(p.parent_path(), ec);
    if (m.size() > kMax) {   // drop the oldest, and say so (never silently: a dropped tombstone lets that state come back)
        std::vector<std::pair<int64_t, std::string>> v;
        for (auto& kv : m) v.push_back({kv.second.when, kv.first});
        std::sort(v.begin(), v.end());
        const size_t drop = m.size() - kMax;
        for (size_t i = 0; i < drop; ++i) m.erase(v[i].second);
        std::ofstream lg(p.parent_path() / "state-tombstones.log", std::ios::app);
        lg << (long long)std::time(nullptr) << "\tpruned " << drop << " oldest deleted-state records (limit " << kMax << ")\n";
    }
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        for (auto& kv : m) f << kv.second.when << '\t' << kv.second.size << '\t' << kv.second.sha << '\t' << kv.first << '\n';
        f.flush();
        if (!f) return false;
    }
    std::filesystem::rename(tmp, p, ec);   // replaces the old list in one step on Windows
    if (ec) { std::error_code e2; std::filesystem::remove(tmp, e2); return false; }
    return true;
}

inline std::map<std::string, Entry> load(const std::string& vault) {
    std::lock_guard<std::mutex> lk(mu());
    return load_unlocked(vault);
}

inline bool add(const std::string& vault, const std::string& key, const Entry& e) {
    std::lock_guard<std::mutex> lk(mu());
    auto m = load_unlocked(vault);
    m[key] = e;
    return store_unlocked(vault, m);
}

inline bool clear(const std::string& vault, const std::string& key) {
    std::lock_guard<std::mutex> lk(mu());
    auto m = load_unlocked(vault);
    if (!m.erase(key)) return true;
    return store_unlocked(vault, m);
}

/* <vault>\<system>\<game>\states (any slash style, optional trailing slash) + a file name -> vault folder and tombstone key.
 * False when the folder is not shaped like a vault game's states folder (a test folder, say): nothing is remembered then. */
inline bool key_for_state_file(const std::string& state_dir, const std::string& file_name, std::string* vault, std::string* key) {
    std::string d = state_dir;
    for (auto& c : d) if (c == '\\') c = '/';
    while (!d.empty() && d.back() == '/') d.pop_back();
    std::vector<std::string> seg;
    size_t i = 0;
    while (i <= d.size()) {
        size_t j = d.find('/', i);
        if (j == std::string::npos) j = d.size();
        seg.push_back(d.substr(i, j - i));
        i = j + 1;
    }
    if (seg.size() < 4 || seg.back() != "states" || seg[seg.size() - 2].empty() || seg[seg.size() - 3].empty()) return false;
    std::string v;
    for (size_t k = 0; k + 3 < seg.size(); ++k) v += (k ? "/" : "") + seg[k];
    if (v.empty() && !(d.size() && d[0] == '/')) return false;
    *vault = v.empty() ? std::string("/") : v;
    *key = seg[seg.size() - 3] + "/" + seg[seg.size() - 2] + "/states/" + file_name;
    return true;
}

}}}  // namespace hub::saves::tomb
