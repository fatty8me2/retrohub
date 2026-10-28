#pragma once
/* Framebuffer-masked RDRAM hash for the lockstep sync checks (pure logic, unit tested in test_resync.cpp).
 *
 * GLideN64 copies the frame into RDRAM; that copy depends on the plugin's own state, which a savestate does not capture, so a peer that loaded a
 * state (desync recovery, live spectate join) differs from the others in the framebuffers for a while although the game state is identical.
 * The sync hash therefore skips the framebuffers. They are found from the VI registers (game state, identical on every peer at the same
 * lead-port poll): Tracker::sample() is called at every lead poll with VI_STATUS / VI_ORIGIN / VI_WIDTH and remembers the last kKeep distinct
 * origins (a game triple-buffers). With no origin seen the hash is the plain one (hash_block). Hash version 2 = this hash (capability bit
 * kCapMaskedHash); a peer without it compares the plain hash, so a session uses the masked one only when everybody has it. */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace n64run {
namespace fbmask {

constexpr int kKeep = 4;
constexpr uint32_t kMargin = 0x800;   // bytes skipped around each framebuffer

struct Range { uint32_t lo, hi; };   // [lo, hi) in bytes, multiples of 8

class Tracker {
  public:
    void reset() { n_ = 0; }
    int count() const { return n_; }
    /* VI_STATUS, VI_ORIGIN, VI_WIDTH as the game set them */
    void sample(uint32_t status, uint32_t origin, uint32_t width) {
        const uint32_t type = status & 3u;   // 0/1 blank/reserved, 2 = 16 bit, 3 = 32 bit
        origin &= 0x00FFFFFFu;
        if (type < 2 || !origin || !width || width > 1024) return;
        const uint32_t h = width > 400 ? 480u : 240u;
        const uint32_t bytes = width * h * (type == 3 ? 4u : 2u);
        for (int i = 0; i < n_; i++)
            if (e_[i].origin == origin) { e_[i].bytes = bytes; return; }   // seen: the set is unchanged
        if (n_ == kKeep) { for (int i = 1; i < kKeep; i++) e_[i - 1] = e_[i]; n_--; }
        e_[n_].origin = origin; e_[n_].bytes = bytes; n_++;
    }
    /* sorted, merged, clipped to [0, ram_bytes) */
    std::vector<Range> ranges(uint32_t ram_bytes) const {
        std::vector<Range> r;
        for (int i = 0; i < n_; i++) {
            uint64_t lo = e_[i].origin > kMargin ? e_[i].origin - kMargin : 0, hi = (uint64_t)e_[i].origin + e_[i].bytes + kMargin;
            lo &= ~7ull; hi = (hi + 7) & ~7ull;
            if (hi > ram_bytes) hi = ram_bytes;
            if (lo < hi) r.push_back({(uint32_t)lo, (uint32_t)hi});
        }
        std::sort(r.begin(), r.end(), [](const Range& a, const Range& b) { return a.lo < b.lo; });
        std::vector<Range> m;
        for (const Range& x : r) { if (!m.empty() && x.lo <= m.back().hi) m.back().hi = std::max(m.back().hi, x.hi); else m.push_back(x); }
        return m;
    }
  private:
    struct E { uint32_t origin = 0, bytes = 0; } e_[kKeep];
    int n_ = 0;
};

/* Same mixing as hash_block (lockstep.cpp): with no ranges the result is identical to it. */
inline uint64_t hash(const void* data, size_t bytes, const std::vector<Range>& skip) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xCBF29CE484222325ull;
    const size_t words = bytes / 8;
    size_t ri = 0;
    for (size_t i = 0; i < words; i++) {
        const uint64_t off = (uint64_t)i * 8;
        while (ri < skip.size() && skip[ri].hi <= off) ri++;
        if (ri < skip.size() && off >= skip[ri].lo) continue;
        uint64_t w;
        std::memcpy(&w, p + i * 8, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    return h;
}

}  // namespace fbmask
}  // namespace n64run
