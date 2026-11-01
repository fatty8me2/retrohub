/* patcher_test: the ROM patch engine (IPS, UPS, BPS, VCDIFF / xdelta3), console test.
 *
 *  - small encoders for IPS, UPS, BPS and VCDIFF live in this file and round-trip random games and targets;
 *  - hand-built VCDIFF windows follow RFC 3284 (its section 3 example, every address mode, VCD_TARGET, checksums);
 *  - src/common/testdata/patcher holds patches made by the real xdelta3 v3.2.2 (plain, DJW, multi-window, ...) and one
 *    with its sections re-wrapped as an xz / LZMA2 stream (see testdata/patcher/README.txt);
 *  - a fuzz pass truncates and bit-flips every fixture thousands of times (exact-size heap buffers, so an
 *    over-read faults under the debug CRT or AddressSanitizer);
 *  - apply_file is exercised on UTF-8 paths with spaces, atomic replace, cancel, and the same-file guard.
 *
 * usage: patcher_test [--fuzz N] [--big] [testdata dir]
 *   --fuzz N  mutations per fixture (default 2500)
 *   --big     also patch a 1.5 GB sparse game file and check that memory stays small (writes 1.5 GB to the temp dir) */

#include "patcher.h"
#include "patcher_internal.h"
#include "winutil.h"

#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace hub::patch;
using Bytes = std::vector<uint8_t>;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
#define CHECK_ERR(r, e) do { if ((r).error != (e)) { std::printf("FAIL %s:%d: expected error %d, got %d (%s | %s)\n", __FILE__, __LINE__, (int)(e), (int)(r).error, (r).message.c_str(), (r).detail.c_str()); ++g_fail; } } while (0)
#define CHECK_OK(r) do { if (!(r).ok()) { std::printf("FAIL %s:%d: %s | %s\n", __FILE__, __LINE__, (r).message.c_str(), (r).detail.c_str()); ++g_fail; } } while (0)

#ifndef PATCHER_TESTDATA_DIR
#define PATCHER_TESTDATA_DIR "testdata/patcher"
#endif
static std::string g_data_dir = PATCHER_TESTDATA_DIR;
static int g_fuzz_iters = 2500;
static bool g_big = false;

/* ======================================================================================== helpers */

static Bytes B(const char* s) { return Bytes(reinterpret_cast<const uint8_t*>(s), reinterpret_cast<const uint8_t*>(s) + std::strlen(s)); }
static void add(Bytes& o, const Bytes& x) { o.insert(o.end(), x.begin(), x.end()); }
static void put_le32(Bytes& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back((uint8_t)(v >> (8 * i))); }
static void put_be32(Bytes& o, uint32_t v) { for (int i = 3; i >= 0; --i) o.push_back((uint8_t)(v >> (8 * i))); }
static uint32_t crc_of(const Bytes& b) { return crc32(b.data(), b.size()); }

/* byuu's variable-length number (UPS, BPS) */
static void put_vlq(Bytes& o, uint64_t v) {
    for (;;) {
        const uint8_t x = v & 0x7F;
        v >>= 7;
        if (v == 0) { o.push_back(0x80 | x); return; }
        o.push_back(x);
        --v;
    }
}
/* RFC 3284 section 2: big-endian base-128 */
static void put_varint(Bytes& o, uint64_t v) {
    uint8_t tmp[10];
    int n = 0;
    tmp[n++] = v & 0x7F;
    v >>= 7;
    while (v) { tmp[n++] = 0x80 | (v & 0x7F); v >>= 7; }
    while (n) o.push_back(tmp[--n]);
}
/* UPS / BPS footer: the patch's own CRC over everything before it */
static void seal(Bytes& o) { put_le32(o, crc32(o.data(), o.size())); }

static Bytes fixture(const char* name) {
    Bytes b;
    const std::string p = g_data_dir + "/" + name;
    if (!winutil::read_file(p, &b)) { std::printf("FAIL: cannot read fixture %s\n", p.c_str()); ++g_fail; }
    return b;
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint64_t next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 0x2545F4914F6CDD1Dull; }
    uint32_t below(uint32_t n) { return n ? (uint32_t)(next() % n) : 0; }
    uint8_t byte() { return (uint8_t)(next() >> 56); }
};

/* A game: noise, runs, repeats of earlier parts, two-byte patterns. */
static Bytes make_game(Rng& r, size_t n) {
    Bytes g;
    g.reserve(n);
    while (g.size() < n) {
        const size_t len = 1 + r.below(300);
        switch (r.below(4)) {
            case 0: for (size_t i = 0; i < len; ++i) g.push_back(r.byte()); break;
            case 1: g.insert(g.end(), len, r.byte()); break;
            case 2:
                if (g.size() > 64) {
                    const size_t from = r.below((uint32_t)g.size() - 32);
                    const size_t l2 = std::min<size_t>(len, g.size() - from);
                    for (size_t i = 0; i < l2; ++i) g.push_back(g[from + i]);
                }
                break;
            default: {
                const uint8_t a = r.byte(), b = r.byte();
                for (size_t i = 0; i < len; ++i) g.push_back(i & 1 ? b : a);
            }
        }
    }
    g.resize(n);
    return g;
}

/* The game with a few edits: overwrites, fills, insertions, deletions, copies of other parts. The size may change. */
static Bytes make_target(Rng& r, const Bytes& game) {
    Bytes t = game;
    const int edits = 1 + (int)r.below(12);
    for (int e = 0; e < edits; ++e) {
        const size_t len = 1 + r.below(200);
        switch (r.below(6)) {
            case 0: if (!t.empty()) { const size_t at = r.below((uint32_t)t.size()); for (size_t i = 0; i < len && at + i < t.size(); ++i) t[at + i] = r.byte(); } break;
            case 1: if (!t.empty()) { const size_t at = r.below((uint32_t)t.size()); const uint8_t v = r.byte(); for (size_t i = 0; i < len && at + i < t.size(); ++i) t[at + i] = v; } break;
            case 2: { const size_t at = r.below((uint32_t)t.size() + 1); Bytes ins; for (size_t i = 0; i < len; ++i) ins.push_back(r.byte()); t.insert(t.begin() + at, ins.begin(), ins.end()); break; }
            case 3: if (t.size() > len) { const size_t at = r.below((uint32_t)(t.size() - len)); t.erase(t.begin() + at, t.begin() + at + len); } break;
            case 4: if (t.size() > len) { const size_t from = r.below((uint32_t)(t.size() - len)); Bytes chunk(t.begin() + from, t.begin() + from + len); const size_t at = r.below((uint32_t)t.size() + 1); t.insert(t.begin() + at, chunk.begin(), chunk.end()); } break;
            default: if (t.size() > 2 * len) { const size_t a = r.below((uint32_t)(t.size() - len)), b = r.below((uint32_t)(t.size() - len)); for (size_t i = 0; i < len; ++i) std::swap(t[a + i], t[b + i]); }
        }
    }
    return t;
}

/* ======================================================================================== encoders */

static Bytes ips_encode(const Bytes& src, const Bytes& tgt) {
    Bytes o = B("PATCH");
    const size_t n = tgt.size();
    auto changed = [&](size_t i) { return i >= src.size() || tgt[i] != src[i]; };
    size_t i = 0;
    while (i < n) {
        if (!changed(i)) { ++i; continue; }
        size_t last = i, j = i;
        while (j < n && j - last < 8) { if (changed(j)) last = j; ++j; }
        const size_t end = last + 1;
        size_t p = i;
        while (p < end) {
            size_t start = p;
            if (start == 0x454F46 && start > 0) --start;              // an offset that spells "EOF" cannot be written
            const size_t len = std::min<size_t>(end - start, 0xFFFF);
            bool rle = len >= 6;
            for (size_t k = 1; rle && k < len; ++k) {
                if (tgt[start + k] != tgt[start]) rle = false;
            }
            o.push_back((uint8_t)(start >> 16)); o.push_back((uint8_t)(start >> 8)); o.push_back((uint8_t)start);
            if (rle) {
                o.push_back(0); o.push_back(0); o.push_back((uint8_t)(len >> 8)); o.push_back((uint8_t)len); o.push_back(tgt[start]);
            } else {
                o.push_back((uint8_t)(len >> 8)); o.push_back((uint8_t)len);
                o.insert(o.end(), tgt.begin() + start, tgt.begin() + start + len);
            }
            p = start + len;
        }
        i = end;
    }
    o.push_back('E'); o.push_back('O'); o.push_back('F');
    if (tgt.size() < src.size()) { o.push_back((uint8_t)(tgt.size() >> 16)); o.push_back((uint8_t)(tgt.size() >> 8)); o.push_back((uint8_t)tgt.size()); }
    return o;
}

static Bytes ups_encode(const Bytes& src, const Bytes& tgt) {
    Bytes o = B("UPS1");
    put_vlq(o, src.size());
    put_vlq(o, tgt.size());
    auto sb = [&](size_t i) -> uint8_t { return i < src.size() ? src[i] : 0; };
    const size_t n = tgt.size();
    size_t pos = 0, last_end = 0;
    while (pos < n) {
        if (tgt[pos] == sb(pos)) { ++pos; continue; }
        put_vlq(o, pos - last_end);
        while (pos < n && (tgt[pos] ^ sb(pos)) != 0) { o.push_back((uint8_t)(tgt[pos] ^ sb(pos))); ++pos; }
        o.push_back(0);                                              // end byte: it covers the unchanged byte at pos
        if (pos < n) ++pos;
        last_end = pos;
    }
    put_le32(o, crc_of(src));
    put_le32(o, crc_of(tgt));
    seal(o);
    return o;
}

static uint32_t gram4(const Bytes& b, size_t i) { return (uint32_t)b[i] | ((uint32_t)b[i + 1] << 8) | ((uint32_t)b[i + 2] << 16) | ((uint32_t)b[i + 3] << 24); }

static Bytes bps_encode(const Bytes& src, const Bytes& tgt, const std::string& meta = std::string()) {
    Bytes o = B("BPS1");
    put_vlq(o, src.size());
    put_vlq(o, tgt.size());
    put_vlq(o, meta.size());
    o.insert(o.end(), meta.begin(), meta.end());
    std::unordered_map<uint32_t, std::vector<uint32_t>> sidx, tidx;
    for (size_t i = 0; i + 4 <= src.size(); ++i) {
        auto& v = sidx[gram4(src, i)];
        if (v.size() < 8) v.push_back((uint32_t)i);
    }
    size_t out = 0, indexed = 0;
    int64_t src_rel = 0, tgt_rel = 0;
    Bytes pending;
    auto flush_pending = [&]() {
        if (pending.empty()) return;
        put_vlq(o, ((uint64_t)(pending.size() - 1) << 2) | 1);
        add(o, pending);
        pending.clear();
    };
    auto match = [&](const Bytes& a, size_t ai, size_t bi) {
        size_t l = 0;
        while (bi + l < tgt.size() && ai + l < a.size() && a[ai + l] == tgt[bi + l]) ++l;
        return l;
    };
    while (out < tgt.size()) {
        size_t best = 0, from = 0;
        unsigned kind = 0;
        size_t l = 0;
        while (out + l < tgt.size() && out + l < src.size() && tgt[out + l] == src[out + l]) ++l;
        if (l >= 4) { best = l; kind = 0; }
        if (out + 4 <= tgt.size()) {
            const uint32_t g = gram4(tgt, out);
            auto it = sidx.find(g);
            if (it != sidx.end()) {
                for (uint32_t c : it->second) { const size_t m = match(src, c, out); if (m > best) { best = m; kind = 2; from = c; } }
            }
            auto jt = tidx.find(g);
            if (jt != tidx.end()) {
                for (uint32_t c : jt->second) { const size_t m = match(tgt, c, out); if (m > best) { best = m; kind = 3; from = c; } }
            }
        }
        if (best >= 4) {
            flush_pending();
            put_vlq(o, ((uint64_t)(best - 1) << 2) | kind);
            if (kind == 2) {
                const int64_t d = (int64_t)from - src_rel;
                put_vlq(o, ((uint64_t)(d < 0 ? -d : d) << 1) | (d < 0 ? 1u : 0u));
                src_rel = (int64_t)(from + best);
            } else if (kind == 3) {
                const int64_t d = (int64_t)from - tgt_rel;
                put_vlq(o, ((uint64_t)(d < 0 ? -d : d) << 1) | (d < 0 ? 1u : 0u));
                tgt_rel = (int64_t)(from + best);
            }
            out += best;
        } else {
            pending.push_back(tgt[out]);
            ++out;
        }
        while (indexed + 4 <= out) {
            auto& v = tidx[gram4(tgt, indexed)];
            if (v.size() < 8) v.push_back((uint32_t)indexed);
            ++indexed;
        }
    }
    flush_pending();
    put_le32(o, crc_of(src));
    put_le32(o, crc_of(tgt));
    seal(o);
    return o;
}

/* Cheap BPS for big games: the game unchanged (SourceRead) with `edits` bytes replaced at the given offsets. */
struct Edit { uint64_t at; uint8_t value; };
static Bytes bps_sparse(uint64_t size, const std::vector<Edit>& edits, uint32_t src_crc, uint32_t tgt_crc) {
    Bytes o = B("BPS1");
    put_vlq(o, size); put_vlq(o, size); put_vlq(o, 0);
    uint64_t pos = 0;
    for (const Edit& e : edits) {
        if (e.at > pos) { put_vlq(o, ((e.at - pos - 1) << 2) | 0); }          // SourceRead
        put_vlq(o, (0ull << 2) | 1);                                          // TargetRead of one byte
        o.push_back(e.value);
        pos = e.at + 1;
    }
    if (pos < size) put_vlq(o, ((size - pos - 1) << 2) | 0);
    put_le32(o, src_crc); put_le32(o, tgt_crc);
    seal(o);
    return o;
}

/* ---- VCDIFF: a small encoder that follows RFC 3284 section 5 (default code table, address caches) ---- */
static uint32_t adler32_ref(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) { a = (a + p[i]) % 65521u; b = (b + a) % 65521u; }
    return (b << 16) | a;
}

struct AddrEnc {
    uint64_t near_[4] = {}, same_[768] = {};
    unsigned next = 0;
    void update(uint64_t a) { near_[next] = a; next = (next + 1) % 4; same_[a % 768] = a; }
    /* Appends the encoded address to `out`, returns the mode (0 SELF, 1 HERE, 2..5 near, 6..8 same). */
    unsigned encode(uint64_t addr, uint64_t here, Bytes& out) {
        uint64_t best = addr;
        unsigned mode = 0;
        if (here - addr < best) { best = here - addr; mode = 1; }
        for (unsigned i = 0; i < 4; ++i) {
            if (addr >= near_[i] && addr - near_[i] < best) { best = addr - near_[i]; mode = 2 + i; }
        }
        if (same_[addr % 768] == addr) {
            out.push_back((uint8_t)(addr % 256));
            mode = 6 + (unsigned)((addr % 768) / 256);
        } else {
            put_varint(out, best);
        }
        update(addr);
        return mode;
    }
};

struct VInst { int type; uint64_t size; unsigned mode; };       // 1 ADD, 2 RUN, 3 COPY

/* One window: indicator byte, copy segment, then the delta encoding with its three sections. */
static void vcd_window(Bytes& out, unsigned win_ind, uint64_t cpylen, uint64_t cpyoff, uint64_t tgtlen, unsigned del_ind,
                       const Bytes& data, const Bytes& inst, const Bytes& addr, bool has_sum, uint32_t sum) {
    Bytes body;
    put_varint(body, tgtlen);
    body.push_back((uint8_t)del_ind);
    put_varint(body, data.size()); put_varint(body, inst.size()); put_varint(body, addr.size());
    if (has_sum) put_be32(body, sum);
    add(body, data); add(body, inst); add(body, addr);
    out.push_back((uint8_t)(win_ind | (has_sum ? 4 : 0)));
    if (win_ind & 3) { put_varint(out, cpylen); put_varint(out, cpyoff); }
    put_varint(out, body.size());
    add(out, body);
}

static Bytes vcd_header(unsigned hdr_ind = 0, int sec_id = -1, const std::string& app = std::string()) {
    Bytes o = {0xD6, 0xC3, 0xC4, 0x00};
    o.push_back((uint8_t)(hdr_ind | (sec_id >= 0 ? 1 : 0) | (!app.empty() ? 4 : 0)));
    if (sec_id >= 0) o.push_back((uint8_t)sec_id);
    if (!app.empty()) { put_varint(o, app.size()); o.insert(o.end(), app.begin(), app.end()); }
    return o;
}

struct VcdOpts {
    size_t window = 1u << 30;     // target bytes per window
    bool adler = true;
    bool segmented = false;       // a source segment per window (offset and length differ) instead of the whole game
    bool pairs = true;            // ADD+COPY and COPY+ADD double opcodes
    std::string app;              // application header
};

static Bytes vcd_encode(const Bytes& src, const Bytes& tgt, const VcdOpts& op) {
    Bytes out = vcd_header(0, -1, op.app);
    size_t ws = 0;
    do {
        const size_t we = std::min(tgt.size(), ws + op.window);
        uint64_t cpyoff = 0, cpylen = 0;
        if (!src.empty()) {
            if (op.segmented) { cpyoff = std::min(ws, src.size() - 1); cpylen = std::min<size_t>(src.size() - cpyoff, op.window + 777); }
            else { cpyoff = 0; cpylen = src.size(); }
        }
        std::unordered_map<uint32_t, std::vector<uint32_t>> sidx;
        for (uint64_t i = 0; i + 4 <= cpylen; ++i) {
            auto& v = sidx[gram4(src, (size_t)(cpyoff + i))];
            if (v.size() < 8) v.push_back((uint32_t)i);
        }
        // choose instructions
        std::vector<VInst> insts;
        Bytes data, addrs;
        AddrEnc ac;
        size_t p = ws;
        Bytes pending;
        auto flush_add = [&]() { if (!pending.empty()) { insts.push_back({1, pending.size(), 0}); add(data, pending); pending.clear(); } };
        std::unordered_map<uint32_t, std::vector<uint32_t>> widx;
        size_t windexed = ws;
        while (p < we) {
            size_t run = 1;
            while (p + run < we && tgt[p + run] == tgt[p]) ++run;
            size_t best = 0, from = 0;
            bool from_src = false;
            if (p + 4 <= we) {
                const uint32_t g = gram4(tgt, p);
                auto it = sidx.find(g);
                if (it != sidx.end()) {
                    for (uint32_t c : it->second) {
                        size_t l = 0;
                        while (p + l < we && c + l < cpylen && src[(size_t)(cpyoff + c + l)] == tgt[p + l]) ++l;
                        if (l > best) { best = l; from = c; from_src = true; }
                    }
                }
                auto jt = widx.find(g);
                if (jt != widx.end()) {
                    for (uint32_t c : jt->second) {
                        size_t l = 0;
                        while (p + l < we && tgt[c + l] == tgt[p + l]) ++l;
                        if (l > best) { best = l; from = c - ws; from_src = false; }
                    }
                }
            }
            if (run >= 8 && run >= best) {
                flush_add();
                insts.push_back({2, run, 0});
                data.push_back(tgt[p]);
                p += run;
            } else if (best >= 4) {
                flush_add();
                const uint64_t addr = from_src ? from : cpylen + from;
                const uint64_t here = cpylen + (p - ws);
                const unsigned mode = ac.encode(addr, here, addrs);
                insts.push_back({3, best, mode});
                p += best;
            } else {
                pending.push_back(tgt[p]);
                ++p;
            }
            while (windexed + 4 <= p) {
                auto& v = widx[gram4(tgt, windexed)];
                if (v.size() < 8) v.push_back((uint32_t)windexed);
                ++windexed;
            }
        }
        flush_add();
        // opcodes
        Bytes inst;
        for (size_t i = 0; i < insts.size();) {
            const VInst& a = insts[i];
            if (op.pairs && i + 1 < insts.size()) {
                const VInst& b = insts[i + 1];
                if (a.type == 1 && a.size >= 1 && a.size <= 4 && b.type == 3) {
                    if (b.mode <= 5 && b.size >= 4 && b.size <= 6) { inst.push_back((uint8_t)(163 + b.mode * 12 + (a.size - 1) * 3 + (b.size - 4))); i += 2; continue; }
                    if (b.mode >= 6 && b.size == 4) { inst.push_back((uint8_t)(235 + (b.mode - 6) * 4 + (a.size - 1))); i += 2; continue; }
                }
                if (a.type == 3 && a.size == 4 && b.type == 1 && b.size == 1) { inst.push_back((uint8_t)(247 + a.mode)); i += 2; continue; }
            }
            if (a.type == 1) {
                if (a.size <= 17) inst.push_back((uint8_t)(1 + a.size));
                else { inst.push_back(1); put_varint(inst, a.size); }
            } else if (a.type == 2) {
                inst.push_back(0); put_varint(inst, a.size);
            } else {
                const unsigned base = 19 + 16 * a.mode;
                if (a.size >= 4 && a.size <= 18) inst.push_back((uint8_t)(base + (a.size - 3)));
                else { inst.push_back((uint8_t)base); put_varint(inst, a.size); }
            }
            ++i;
        }
        const unsigned win_ind = cpylen ? 1 : 0;
        vcd_window(out, win_ind, cpylen, cpyoff, we - ws, 0, data, inst, addrs, op.adler, adler32_ref(tgt.data() + ws, we - ws));
        ws = we;
    } while (ws < tgt.size());
    return out;
}

/* ======================================================================================== tests */

static void expect_roundtrip(const char* what, const Bytes& game, const Bytes& patch, const Bytes& target) {
    Bytes out;
    const Result r = apply(game, patch.data(), patch.size(), &out, 0);
    if (!r.ok() || out != target) {
        std::printf("FAIL round trip (%s): game %zu, target %zu, patch %zu: %s | %s%s\n", what, game.size(), target.size(), patch.size(),
                    r.message.c_str(), r.detail.c_str(), r.ok() ? " [output differs]" : "");
        ++g_fail;
    }
}

static uint32_t crc_bitwise(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
    }
    return ~c;
}

static void test_crc32() {
    CHECK(crc32(nullptr, 0) == 0);
    CHECK(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u);
    const Bytes fox = B("The quick brown fox jumps over the lazy dog");
    CHECK(crc_of(fox) == 0x414FA339u);
    Rng r(1);
    for (int i = 0; i < 300; ++i) {
        const size_t n = r.below(2000);
        Bytes b;
        for (size_t k = 0; k < n; ++k) b.push_back(r.byte());
        const size_t off = r.below(8) < (uint32_t)n ? r.below(8) : 0;       // unaligned starts too
        CHECK(crc32(b.data() + off, n - off) == crc_bitwise(b.data() + off, n - off));
        const size_t cut = n ? r.below((uint32_t)n) : 0;                    // chaining: crc(b) == crc(b[cut:], crc(b[:cut]))
        CHECK(crc32(b.data() + cut, n - cut, crc32(b.data(), cut)) == crc32(b.data(), n));
    }
}

static void test_detect_and_ext() {
    const uint8_t ips[] = {'P', 'A', 'T', 'C', 'H', 'E', 'O', 'F'};
    const uint8_t ups[] = {'U', 'P', 'S', '1'};
    const uint8_t bps[] = {'B', 'P', 'S', '1'};
    const uint8_t vcd[] = {0xD6, 0xC3, 0xC4, 0x00, 0x00};
    const uint8_t vcd1[] = {0xD6, 0xC3, 0xC4, 0x01, 0x00};
    const uint8_t ppf[] = {'P', 'P', 'F', '3', '0'};
    CHECK(detect(ips, sizeof ips) == Format::Ips);
    CHECK(detect(ips, 4) == Format::Unknown);                 // "PATC"
    CHECK(detect(ups, 4) == Format::Ups && detect(ups, 3) == Format::Unknown);
    CHECK(detect(bps, 4) == Format::Bps);
    CHECK(detect(vcd, sizeof vcd) == Format::Vcdiff && detect(vcd1, sizeof vcd1) == Format::Unknown);
    CHECK(detect(ppf, sizeof ppf) == Format::Unknown);
    CHECK(detect(nullptr, 10) == Format::Unknown && detect(ips, 0) == Format::Unknown);

    CHECK(format_from_ext("hack.IPS") == Format::Ips);
    CHECK(format_from_ext("C:\\x y\\hack.ups") == Format::Ups);
    CHECK(format_from_ext("/a/b/Hack.Bps") == Format::Bps);
    CHECK(format_from_ext("a.xdelta") == Format::Vcdiff && format_from_ext("a.VCDIFF") == Format::Vcdiff && format_from_ext("a.delta") == Format::Vcdiff);
    CHECK(format_from_ext("a.ppf") == Format::Unknown && format_from_ext("noext") == Format::Unknown);
    CHECK(format_from_ext("dir.ips/file") == Format::Unknown && format_from_ext("") == Format::Unknown && format_from_ext("x.") == Format::Unknown);
    CHECK(std::string(format_name(Format::Vcdiff)) == "xdelta" && std::string(format_name(Format::Unknown)) == "unknown");

    // PPF is refused in plain words, not treated as damaged
    Bytes game(100, 1), out;
    const Bytes ppf_patch = B("PPF30\x02 whatever");
    Result r = apply(game, ppf_patch.data(), ppf_patch.size(), &out, 0);
    CHECK_ERR(r, Error::Unsupported);
    CHECK(r.message == "PPF patches aren't supported yet.");
    r = apply(game, ips, 3, &out, 0);                         // not a patch at all
    CHECK_ERR(r, Error::BadPatch);
    r = apply(game, nullptr, 0, &out, 0);
    CHECK_ERR(r, Error::BadPatch);
}

/* ---- IPS ---- */
static void test_ips() {
    // hand-made: a 3-byte record, an RLE record, EOF
    {
        Bytes patch = B("PATCH");
        const uint8_t recs[] = {0x00, 0x00, 0x02, 0x00, 0x03, 0xAA, 0xBB, 0xCC,          // offset 2, 3 bytes
                                0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x05, 0xEE};          // offset 10, RLE 5 x EE
        patch.insert(patch.end(), recs, recs + sizeof recs);
        for (char ch : std::string("EOF")) patch.push_back((uint8_t)ch);
        Bytes game(12, 0x11), out;
        Result r = apply(game, patch.data(), patch.size(), &out, 0);
        CHECK_OK(r);
        Bytes want = game;
        want[2] = 0xAA; want[3] = 0xBB; want[4] = 0xCC;
        want.resize(15);
        for (int i = 10; i < 15; ++i) want[i] = 0xEE;
        CHECK(out == want);                                        // grown by the RLE record that runs past the end
        // the optional truncation size after EOF
        Bytes t = patch;
        t.push_back(0x00); t.push_back(0x00); t.push_back(0x06);
        r = apply(game, t.data(), t.size(), &out, 0);
        CHECK_OK(r);
        want.resize(6);
        CHECK(out == want);
        // a gap between the game's end and a record reads as zeros
        Bytes g2 = {1, 2, 3}, p2 = B("PATCH");
        const uint8_t gap_rec[] = {0x00, 0x00, 0x08, 0x00, 0x01, 0x77};
        p2.insert(p2.end(), gap_rec, gap_rec + sizeof gap_rec);
        for (char ch : std::string("EOF")) p2.push_back((uint8_t)ch);
        r = apply(g2, p2.data(), p2.size(), &out, 0);
        CHECK_OK(r);
        CHECK((out == Bytes{1, 2, 3, 0, 0, 0, 0, 0, 0x77}));
        // truncation to zero
        Bytes p3 = B("PATCHEOF");
        p3.push_back(0); p3.push_back(0); p3.push_back(0);
        r = apply(g2, p3.data(), p3.size(), &out, 0);
        CHECK_OK(r);
        CHECK(out.empty());
        // missing EOF / cut in the middle of a record / cut in the truncation field
        for (size_t cut : {5u, 6u, 8u, 12u, 16u}) {
            if (cut >= patch.size() - 3) continue;
            r = apply(game, patch.data(), cut, &out, 0);
            CHECK_ERR(r, Error::BadPatch);
            CHECK(out.empty());
        }
        Bytes bad = B("PATCHEOF");
        bad.push_back(1);
        r = apply(game, bad.data(), bad.size(), &out, 0);
        CHECK_ERR(r, Error::BadPatch);
        // an empty patch (just the header and EOF) leaves the game alone
        Bytes empty = B("PATCHEOF");
        r = apply(game, empty.data(), empty.size(), &out, 0);
        CHECK_OK(r);
        CHECK(out == game);
    }
    // a record that starts at 0x454F46 ("EOF") is not the end marker when more data follows
    {
        Bytes game(0x454F46 + 100, 0x42), out;
        Bytes patch = B("PATCH");
        const uint8_t rec[] = {0x45, 0x4F, 0x46, 0x00, 0x02, 0xDE, 0xAD};
        patch.insert(patch.end(), rec, rec + sizeof rec);
        for (char ch : std::string("EOF")) patch.push_back((uint8_t)ch);
        const Result r = apply(game, patch.data(), patch.size(), &out, 0);
        CHECK_OK(r);
        CHECK(out.size() == game.size() && out[0x454F46] == 0xDE && out[0x454F47] == 0xAD && out[0x454F48] == 0x42);
    }
    // round trips: random games and targets, longer, shorter and equal
    Rng r(7);
    for (int i = 0; i < 300; ++i) {
        const size_t n = i < 3 ? (size_t)i : r.below(6000);
        const Bytes game = make_game(r, n);
        const Bytes tgt = i % 7 == 0 ? Bytes() : make_target(r, game);
        expect_roundtrip("ips", game, ips_encode(game, tgt), tgt);
    }
    // a collision with the EOF offset in a bigger game, written by the encoder one byte early
    {
        Rng q(99);
        Bytes game = make_game(q, 0x454F46 + 5000);
        Bytes tgt = game;
        for (int k = 0; k < 20; ++k) tgt[0x454F46 + k] ^= 0x5A;
        const Bytes p = ips_encode(game, tgt);
        expect_roundtrip("ips eof offset", game, p, tgt);
    }
}

/* ---- UPS ---- */
static void test_ups() {
    Rng r(11);
    for (int i = 0; i < 300; ++i) {
        const size_t n = i < 3 ? (size_t)i : r.below(6000);
        const Bytes game = make_game(r, n);
        const Bytes tgt = i % 9 == 0 ? Bytes() : make_target(r, game);
        expect_roundtrip("ups", game, ups_encode(game, tgt), tgt);
    }
    const Bytes game = make_game(r, 3000);
    Bytes tgt = make_target(r, game);
    tgt.push_back(0x55);
    const Bytes patch = ups_encode(game, tgt);
    Bytes out;
    // wrong source: another version of the game, in plain words
    Bytes other = game;
    other[1500] ^= 0x40;
    Result res = apply(other, patch.data(), patch.size(), &out, 0);
    CHECK_ERR(res, Error::WrongSource);
    CHECK(res.message == "This patch is for a different version of the game.");
    CHECK(out.empty());
    // wrong size
    Bytes shorter(game.begin(), game.end() - 1);
    res = apply(shorter, patch.data(), patch.size(), &out, 0);
    CHECK_ERR(res, Error::WrongSource);
    // already patched
    res = apply(tgt, patch.data(), patch.size(), &out, 0);
    CHECK_ERR(res, Error::WrongSource);
    CHECK(res.message == "This game already has this patch applied.");
    // a damaged patch fails its own CRC
    Bytes bad = patch;
    bad[bad.size() / 2] ^= 1;
    res = apply(game, bad.data(), bad.size(), &out, 0);
    CHECK_ERR(res, Error::BadPatch);
    CHECK(res.message == "This patch file is damaged or incomplete.");
    // intact patch, wrong result CRC in the footer (patch CRC recomputed): the result is thrown away
    Bytes wrong = patch;
    wrong[wrong.size() - 8] ^= 0x01;
    wrong.resize(wrong.size() - 4);
    seal(wrong);
    res = apply(game, wrong.data(), wrong.size(), &out, 0);
    CHECK_ERR(res, Error::BadOutput);
    CHECK(out.empty());
    // every proper prefix is refused
    for (size_t cut = 0; cut < patch.size(); cut += 1 + patch.size() / 120) {
        res = apply(game, patch.data(), cut, &out, 0);
        CHECK(!res.ok());
    }
    // a block that ends exactly at the end of the result, and the end byte one past it
    {
        Bytes g = {1, 2, 3, 4}, t = {1, 2, 9, 9};
        expect_roundtrip("ups tail", g, ups_encode(g, t), t);
        Bytes g2 = {1, 2}, t2 = {1, 2, 7, 7, 7};                      // grows: XOR against zeros
        expect_roundtrip("ups grow", g2, ups_encode(g2, t2), t2);
    }
}

/* ---- BPS ---- */
static void test_bps() {
    Rng r(13);
    for (int i = 0; i < 300; ++i) {
        const size_t n = i < 3 ? (size_t)i : r.below(8000);
        const Bytes game = make_game(r, n);
        const Bytes tgt = i % 9 == 0 ? Bytes() : make_target(r, game);
        expect_roundtrip("bps", game, bps_encode(game, tgt), tgt);
    }
    const Bytes game = make_game(r, 5000);
    Bytes tgt = make_target(r, game);
    // a long run: TargetCopy that overlaps what it writes (distance 1 and 3)
    tgt.insert(tgt.begin() + 100, 4000, 0x61);
    for (int i = 0; i < 3000; ++i) tgt.push_back("xyz"[i % 3]);
    const Bytes patch = bps_encode(game, tgt, "<bps>hello metadata</bps>");
    expect_roundtrip("bps overlap", game, patch, tgt);

    Info info;
    std::string err;
    CHECK(inspect(patch.data(), patch.size(), &info, &err));
    CHECK(info.format == Format::Bps && info.has_source_crc && info.has_target_crc);
    CHECK(info.source_crc == crc_of(game) && info.target_crc == crc_of(tgt));
    CHECK(info.source_size == game.size() && info.target_size == tgt.size());
    CHECK(info.metadata == "<bps>hello metadata</bps>");

    Bytes out;
    Bytes other = game;
    other[2500] ^= 0x10;
    Result res = apply(other, patch.data(), patch.size(), &out, 0);
    CHECK_ERR(res, Error::WrongSource);
    CHECK(res.message == "This patch is for a different version of the game.");
    CHECK(res.detail.find("game CRC") != std::string::npos);
    res = apply(tgt, patch.data(), patch.size(), &out, 0);
    CHECK_ERR(res, Error::WrongSource);
    CHECK(res.message == "This game already has this patch applied.");
    Bytes bad = patch;
    bad[20] ^= 1;
    res = apply(game, bad.data(), bad.size(), &out, 0);
    CHECK_ERR(res, Error::BadPatch);
    Bytes wrong = patch;
    wrong[wrong.size() - 8] ^= 0x01;                               // result CRC, with the patch CRC fixed up
    wrong.resize(wrong.size() - 4);
    seal(wrong);
    res = apply(game, wrong.data(), wrong.size(), &out, 0);
    CHECK_ERR(res, Error::BadOutput);
    for (size_t cut = 0; cut < patch.size(); cut += 1 + patch.size() / 150) {
        res = apply(game, patch.data(), cut, &out, 0);
        CHECK(!res.ok());
    }
    // structural errors behind a valid CRC: an action past the end, a copy from nothing, unused bytes
    auto craft = [&](const Bytes& body_after_sizes, uint64_t src_size, uint64_t tgt_size, const Bytes& g, const Bytes& t) {
        Bytes p = B("BPS1");
        put_vlq(p, src_size); put_vlq(p, tgt_size); put_vlq(p, 0);
        add(p, body_after_sizes);
        put_le32(p, crc_of(g)); put_le32(p, crc_of(t));
        seal(p);
        return apply(g, p.data(), p.size(), &out, 0);
    };
    {
        const Bytes g = {1, 2, 3, 4}, t = {1, 2, 3, 4};
        Bytes ok_body; put_vlq(ok_body, (3ull << 2) | 0);                              // SourceRead 4
        CHECK_OK(craft(ok_body, 4, 4, g, t));
        Bytes over; put_vlq(over, (4ull << 2) | 0);                                    // SourceRead 5 > target
        CHECK_ERR(craft(over, 4, 4, g, t), Error::BadPatch);
        Bytes extra = ok_body; put_vlq(extra, (0ull << 2) | 0);                        // an action after the target is full
        CHECK_ERR(craft(extra, 4, 4, g, t), Error::BadPatch);
        Bytes tc; put_vlq(tc, (3ull << 2) | 3); put_vlq(tc, 0);                        // TargetCopy from output that does not exist
        CHECK_ERR(craft(tc, 4, 4, g, t), Error::BadPatch);
        Bytes sc; put_vlq(sc, (3ull << 2) | 2); put_vlq(sc, (3ull << 1) | 0);          // SourceCopy 4 bytes from offset 3 > size
        CHECK_ERR(craft(sc, 4, 4, g, t), Error::BadPatch);
        Bytes neg; put_vlq(neg, (3ull << 2) | 2); put_vlq(neg, (1ull << 1) | 1);       // SourceCopy to a negative offset
        CHECK_ERR(craft(neg, 4, 4, g, t), Error::BadPatch);
        Bytes tr; put_vlq(tr, (9ull << 2) | 1); add(tr, B("ab"));                      // TargetRead 10 with only 2 bytes left
        CHECK_ERR(craft(tr, 4, 4, g, t), Error::BadPatch);
        const Bytes g2 = {5, 6}, t2 = {5, 6, 5, 6, 5, 6, 5, 6};                       // SourceRead 2 then TargetCopy 6 at distance 2
        Bytes ov; put_vlq(ov, (1ull << 2) | 0); put_vlq(ov, (5ull << 2) | 3); put_vlq(ov, 0);
        CHECK_OK(craft(ov, 2, 8, g2, t2));
    }
}

/* ---- the default VCDIFF code table against RFC 3284 section 5.6 (written out from the xdelta3 source) ---- */
static const uint8_t kRfcTable[256][6] = {
    {2,0,0,0,0,0}, {1,0,0,0,0,0}, {1,1,0,0,0,0}, {1,2,0,0,0,0}, {1,3,0,0,0,0}, {1,4,0,0,0,0}, {1,5,0,0,0,0},
    {1,6,0,0,0,0}, {1,7,0,0,0,0}, {1,8,0,0,0,0}, {1,9,0,0,0,0}, {1,10,0,0,0,0}, {1,11,0,0,0,0}, {1,12,0,0,0,0},
    {1,13,0,0,0,0}, {1,14,0,0,0,0}, {1,15,0,0,0,0}, {1,16,0,0,0,0}, {1,17,0,0,0,0}, {3,0,0,0,0,0}, {3,4,0,0,0,0},
    {3,5,0,0,0,0}, {3,6,0,0,0,0}, {3,7,0,0,0,0}, {3,8,0,0,0,0}, {3,9,0,0,0,0}, {3,10,0,0,0,0}, {3,11,0,0,0,0},
    {3,12,0,0,0,0}, {3,13,0,0,0,0}, {3,14,0,0,0,0}, {3,15,0,0,0,0}, {3,16,0,0,0,0}, {3,17,0,0,0,0}, {3,18,0,0,0,0},
    {3,0,1,0,0,0}, {3,4,1,0,0,0}, {3,5,1,0,0,0}, {3,6,1,0,0,0}, {3,7,1,0,0,0}, {3,8,1,0,0,0}, {3,9,1,0,0,0},
    {3,10,1,0,0,0}, {3,11,1,0,0,0}, {3,12,1,0,0,0}, {3,13,1,0,0,0}, {3,14,1,0,0,0}, {3,15,1,0,0,0}, {3,16,1,0,0,0},
    {3,17,1,0,0,0}, {3,18,1,0,0,0}, {3,0,2,0,0,0}, {3,4,2,0,0,0}, {3,5,2,0,0,0}, {3,6,2,0,0,0}, {3,7,2,0,0,0},
    {3,8,2,0,0,0}, {3,9,2,0,0,0}, {3,10,2,0,0,0}, {3,11,2,0,0,0}, {3,12,2,0,0,0}, {3,13,2,0,0,0}, {3,14,2,0,0,0},
    {3,15,2,0,0,0}, {3,16,2,0,0,0}, {3,17,2,0,0,0}, {3,18,2,0,0,0}, {3,0,3,0,0,0}, {3,4,3,0,0,0}, {3,5,3,0,0,0},
    {3,6,3,0,0,0}, {3,7,3,0,0,0}, {3,8,3,0,0,0}, {3,9,3,0,0,0}, {3,10,3,0,0,0}, {3,11,3,0,0,0}, {3,12,3,0,0,0},
    {3,13,3,0,0,0}, {3,14,3,0,0,0}, {3,15,3,0,0,0}, {3,16,3,0,0,0}, {3,17,3,0,0,0}, {3,18,3,0,0,0}, {3,0,4,0,0,0},
    {3,4,4,0,0,0}, {3,5,4,0,0,0}, {3,6,4,0,0,0}, {3,7,4,0,0,0}, {3,8,4,0,0,0}, {3,9,4,0,0,0}, {3,10,4,0,0,0},
    {3,11,4,0,0,0}, {3,12,4,0,0,0}, {3,13,4,0,0,0}, {3,14,4,0,0,0}, {3,15,4,0,0,0}, {3,16,4,0,0,0}, {3,17,4,0,0,0},
    {3,18,4,0,0,0}, {3,0,5,0,0,0}, {3,4,5,0,0,0}, {3,5,5,0,0,0}, {3,6,5,0,0,0}, {3,7,5,0,0,0}, {3,8,5,0,0,0},
    {3,9,5,0,0,0}, {3,10,5,0,0,0}, {3,11,5,0,0,0}, {3,12,5,0,0,0}, {3,13,5,0,0,0}, {3,14,5,0,0,0}, {3,15,5,0,0,0},
    {3,16,5,0,0,0}, {3,17,5,0,0,0}, {3,18,5,0,0,0}, {3,0,6,0,0,0}, {3,4,6,0,0,0}, {3,5,6,0,0,0}, {3,6,6,0,0,0},
    {3,7,6,0,0,0}, {3,8,6,0,0,0}, {3,9,6,0,0,0}, {3,10,6,0,0,0}, {3,11,6,0,0,0}, {3,12,6,0,0,0}, {3,13,6,0,0,0},
    {3,14,6,0,0,0}, {3,15,6,0,0,0}, {3,16,6,0,0,0}, {3,17,6,0,0,0}, {3,18,6,0,0,0}, {3,0,7,0,0,0}, {3,4,7,0,0,0},
    {3,5,7,0,0,0}, {3,6,7,0,0,0}, {3,7,7,0,0,0}, {3,8,7,0,0,0}, {3,9,7,0,0,0}, {3,10,7,0,0,0}, {3,11,7,0,0,0},
    {3,12,7,0,0,0}, {3,13,7,0,0,0}, {3,14,7,0,0,0}, {3,15,7,0,0,0}, {3,16,7,0,0,0}, {3,17,7,0,0,0}, {3,18,7,0,0,0},
    {3,0,8,0,0,0}, {3,4,8,0,0,0}, {3,5,8,0,0,0}, {3,6,8,0,0,0}, {3,7,8,0,0,0}, {3,8,8,0,0,0}, {3,9,8,0,0,0},
    {3,10,8,0,0,0}, {3,11,8,0,0,0}, {3,12,8,0,0,0}, {3,13,8,0,0,0}, {3,14,8,0,0,0}, {3,15,8,0,0,0}, {3,16,8,0,0,0},
    {3,17,8,0,0,0}, {3,18,8,0,0,0}, {1,1,0,3,4,0}, {1,1,0,3,5,0}, {1,1,0,3,6,0}, {1,2,0,3,4,0}, {1,2,0,3,5,0},
    {1,2,0,3,6,0}, {1,3,0,3,4,0}, {1,3,0,3,5,0}, {1,3,0,3,6,0}, {1,4,0,3,4,0}, {1,4,0,3,5,0}, {1,4,0,3,6,0},
    {1,1,0,3,4,1}, {1,1,0,3,5,1}, {1,1,0,3,6,1}, {1,2,0,3,4,1}, {1,2,0,3,5,1}, {1,2,0,3,6,1}, {1,3,0,3,4,1},
    {1,3,0,3,5,1}, {1,3,0,3,6,1}, {1,4,0,3,4,1}, {1,4,0,3,5,1}, {1,4,0,3,6,1}, {1,1,0,3,4,2}, {1,1,0,3,5,2},
    {1,1,0,3,6,2}, {1,2,0,3,4,2}, {1,2,0,3,5,2}, {1,2,0,3,6,2}, {1,3,0,3,4,2}, {1,3,0,3,5,2}, {1,3,0,3,6,2},
    {1,4,0,3,4,2}, {1,4,0,3,5,2}, {1,4,0,3,6,2}, {1,1,0,3,4,3}, {1,1,0,3,5,3}, {1,1,0,3,6,3}, {1,2,0,3,4,3},
    {1,2,0,3,5,3}, {1,2,0,3,6,3}, {1,3,0,3,4,3}, {1,3,0,3,5,3}, {1,3,0,3,6,3}, {1,4,0,3,4,3}, {1,4,0,3,5,3},
    {1,4,0,3,6,3}, {1,1,0,3,4,4}, {1,1,0,3,5,4}, {1,1,0,3,6,4}, {1,2,0,3,4,4}, {1,2,0,3,5,4}, {1,2,0,3,6,4},
    {1,3,0,3,4,4}, {1,3,0,3,5,4}, {1,3,0,3,6,4}, {1,4,0,3,4,4}, {1,4,0,3,5,4}, {1,4,0,3,6,4}, {1,1,0,3,4,5},
    {1,1,0,3,5,5}, {1,1,0,3,6,5}, {1,2,0,3,4,5}, {1,2,0,3,5,5}, {1,2,0,3,6,5}, {1,3,0,3,4,5}, {1,3,0,3,5,5},
    {1,3,0,3,6,5}, {1,4,0,3,4,5}, {1,4,0,3,5,5}, {1,4,0,3,6,5}, {1,1,0,3,4,6}, {1,2,0,3,4,6}, {1,3,0,3,4,6},
    {1,4,0,3,4,6}, {1,1,0,3,4,7}, {1,2,0,3,4,7}, {1,3,0,3,4,7}, {1,4,0,3,4,7}, {1,1,0,3,4,8}, {1,2,0,3,4,8},
    {1,3,0,3,4,8}, {1,4,0,3,4,8}, {3,4,0,1,1,0}, {3,4,1,1,1,0}, {3,4,2,1,1,0}, {3,4,3,1,1,0}, {3,4,4,1,1,0},
    {3,4,5,1,1,0}, {3,4,6,1,1,0}, {3,4,7,1,1,0}, {3,4,8,1,1,0}};

static void test_vcdiff_table() {
    const hub::patch::detail::VcdCode* t = hub::patch::detail::vcd_default_table();
    int bad = 0;
    for (int i = 0; i < 256; ++i) {
        const uint8_t got[6] = {t[i].type1, t[i].size1, t[i].mode1, t[i].type2, t[i].size2, t[i].mode2};
        if (std::memcmp(got, kRfcTable[i], 6) != 0) { std::printf("table entry %d differs\n", i); ++bad; }
    }
    CHECK(bad == 0);
}

static Bytes B2(std::initializer_list<int> v) { Bytes b; for (int x : v) b.push_back((uint8_t)x); return b; }

/* ---- VCDIFF built by hand ---- */
static void test_vcdiff_handbuilt() {
    Bytes out;
    // RFC 3284 section 3: source "abcdefghijklmnop", target "abcdwxyzefghefghefghefghzzzz"
    const Bytes src = B("abcdefghijklmnop");
    const Bytes tgt = B("abcdwxyzefghefghefghefghzzzz");
    {
        Bytes p = vcd_header();
        // COPY 4 @0 | ADD 4 "wxyz" | COPY 4 @4 | COPY 12 @24 (inside the window, overlapping itself) | RUN 4 'z'
        const Bytes inst = B2({20, 5, 20, 28, 0, 4});
        const Bytes data = B("wxyzz");
        const Bytes addr = B2({0, 4, 24});
        vcd_window(p, 1, src.size(), 0, tgt.size(), 0, data, inst, addr, true, adler32_ref(tgt.data(), tgt.size()));
        Result r = apply(src, p.data(), p.size(), &out, 0);
        CHECK_OK(r);
        CHECK(out == tgt);
        // without the checksum, and with an application header
        Bytes q = vcd_header(0, -1, "from.bin/to.bin/");
        vcd_window(q, 1, src.size(), 0, tgt.size(), 0, data, inst, addr, false, 0);
        r = apply(src, q.data(), q.size(), &out, 0);
        CHECK_OK(r);
        CHECK(out == tgt);
        Info info;
        std::string err;
        CHECK(inspect(q.data(), q.size(), &info, &err) && info.format == Format::Vcdiff && info.metadata == "from.bin/to.bin/");
        // wrong source (a changed byte inside the copied part): the window's Adler-32 notices
        Bytes other = src;
        other[1] = 'X';
        r = apply(other, p.data(), p.size(), &out, 0);
        CHECK_ERR(r, Error::WrongSource);
        CHECK(r.message == "This patch is for a different version of the game.");
        // a window that never reads the source but has a wrong checksum is a bad result, not a wrong game
        Bytes s;
        vcd_window(s, 0, 0, 0, 4, 0, B("abcd"), B2({5}), Bytes(), true, 0x12345678u);
        Bytes s2 = vcd_header();
        add(s2, s);
        r = apply(Bytes(), s2.data(), s2.size(), &out, 0);
        CHECK_ERR(r, Error::BadOutput);
    }
    // every address mode (RFC 3284 section 5.1 / 5.3): copies of 4 bytes whose address bytes are written out by hand
    //   mode 0 SELF, 1 HERE, 2..5 the four "near" slots, 6..8 the three "same" groups
    {
        Rng rg(3);
        Bytes g;
        for (int i = 0; i < 1000; ++i) g.push_back(rg.byte());
        Bytes data, inst, addr, want;
        uint64_t here = g.size();                                       // size of the source segment + bytes built so far
        auto copy = [&](unsigned mode, uint64_t start, const Bytes& addr_bytes) {
            inst.push_back((uint8_t)(19 + 16 * mode + 1));              // COPY of size 4 in this mode
            add(addr, addr_bytes);
            for (uint64_t i = 0; i < 4; ++i) want.push_back(g[start + i]);
            here += 4;
        };
        Bytes a;
        put_varint(a, 100);  copy(0, 100, a);                           // SELF 100                near: [100 - - -]
        copy(2, 104, B2({4}));                                          // near 0 + 4 = 104        near: [100 104 - -]
        a.clear(); put_varint(a, here - 65);  copy(1, 65, a);           // HERE: here - 943 = 65   near: [100 104 65 -]
        copy(3, 114, B2({10}));                                         // near 1 + 10 = 114       near: [100 104 65 114]
        copy(6, 100, B2({100}));                                        // same group 0, byte 100  near: [100 104 65 114] (slot 0 rewritten)
        a.clear(); put_varint(a, 300);  copy(0, 300, a);                // SELF 300
        copy(7, 300, B2({44}));                                         // same group 1: 300 % 256 = 44
        a.clear(); put_varint(a, 600);  copy(0, 600, a);                // SELF 600
        copy(8, 600, B2({88}));                                         // same group 2: 600 % 256 = 88
        copy(2, 604, B2({4}));                                          // near 0 (now 600) + 4
        Bytes p = vcd_header();
        vcd_window(p, 1, g.size(), 0, want.size(), 0, data, inst, addr, true, adler32_ref(want.data(), want.size()));
        const Result r = apply(g, p.data(), p.size(), &out, 0);
        CHECK_OK(r);
        CHECK(out == want);
    }
    // double opcodes: ADD+COPY (163, 175, 235) and COPY+ADD (247)
    {
        const Bytes g = B("0123456789abcdefghijklmnopqrstuvwxyz");
        Bytes data, inst, addr, want;
        // opcode 163: ADD 1, COPY 4 mode 0 -> 'Q' then g[10..14)
        inst.push_back(163); data.push_back('Q'); addr.push_back(10);
        want.push_back('Q'); for (int i = 10; i < 14; ++i) want.push_back(g[i]);
        // opcode 163 + 12*1 + 3*1 + 2 = 180: ADD 2, COPY 6 mode 1 (HERE). here = 36 + 7 = 43 when the copy starts; address 11 = 43 - 32
        inst.push_back(180); add(data, B("RS")); addr.push_back(32);
        want.push_back('R'); want.push_back('S'); for (int i = 0; i < 6; ++i) want.push_back(g[11 + i]);
        // opcode 235 + 4*(m-6) + (add-1) for the same-cache modes: m = 6, ADD 3 -> 237. The cache holds address 10 in slot 10.
        inst.push_back(237); add(data, B("TUV")); addr.push_back(10);
        add(want, B("TUV")); for (int i = 10; i < 14; ++i) want.push_back(g[i]);
        // opcode 247 + 0: COPY 4 mode 0 then ADD 1: copy g[20..24), add 'W'
        inst.push_back(247); data.push_back('W'); addr.push_back(20);
        for (int i = 20; i < 24; ++i) want.push_back(g[i]);
        want.push_back('W');
        Bytes p = vcd_header();
        vcd_window(p, 1, g.size(), 0, want.size(), 0, data, inst, addr, true, adler32_ref(want.data(), want.size()));
        const Result r = apply(g, p.data(), p.size(), &out, 0);
        CHECK_OK(r);
        CHECK(out == want);
    }
    // VCD_TARGET: a second window copies from what the first one wrote; a source segment that is not the whole game
    {
        const Bytes g = B("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
        const Bytes w1 = B("HIJKLMNOPQ");                 // window 1: COPY 10 from g[7..17)
        Bytes p = vcd_header();
        vcd_window(p, 1, 10, 7, w1.size(), 0, Bytes(), B2({19 + 7}), B2({0}), true, adler32_ref(w1.data(), w1.size()));
        // window 2 (VCD_TARGET, segment = output[3..9) = "KLMNOP"): COPY 6 @0 | ADD 1 '!' | COPY 4 @2 (= "MNOP")
        // opcodes: COPY 6 mode 0 = 19 + 3 = 22; ADD 1 = 2; COPY 4 mode 0 = 20
        const Bytes data = B("!"), inst = B2({22, 2, 20}), addr = B2({0, 2});
        const Bytes want2 = B("KLMNOP!MNOP");
        vcd_window(p, 2, 6, 3, want2.size(), 0, data, inst, addr, true, adler32_ref(want2.data(), want2.size()));
        const Result r = apply(g, p.data(), p.size(), &out, 0);
        CHECK_OK(r);
        CHECK(out == [&] { Bytes e = w1; add(e, want2); return e; }());
        // a target segment that reaches past what exists is damage
        Bytes q = vcd_header();
        vcd_window(q, 2, 4, 0, 4, 0, B("abcd"), B2({6}), Bytes(), false, 0);
        const Result bad = apply(g, q.data(), q.size(), &out, 0);
        CHECK_ERR(bad, Error::BadPatch);
    }
    // header variants
    {
        const Bytes g = B("xyz");
        const Bytes p0 = [&] { Bytes p = vcd_header(); vcd_window(p, 0, 0, 0, 3, 0, B("abc"), B2({4}), Bytes(), false, 0); return p; }();
        CHECK_OK(apply(g, p0.data(), p0.size(), &out, 0));
        CHECK(out == B("abc"));
        Bytes none = vcd_header();                                   // no windows at all: an empty result
        CHECK_OK(apply(g, none.data(), none.size(), &out, 0));
        CHECK(out.empty());
        Bytes v1 = p0; v1[3] = 1;
        CHECK_ERR(apply(g, v1.data(), v1.size(), &out, 0), Error::BadPatch);         // magic no longer matches: not a patch
        Bytes table = p0; table[4] |= 2;
        CHECK_ERR(apply(g, table.data(), table.size(), &out, 0), Error::Unsupported); // custom code table
        Bytes unk = p0; unk[4] |= 0x80;
        CHECK_ERR(apply(g, unk.data(), unk.size(), &out, 0), Error::BadPatch);        // unknown indicator bit
        // a compressed section without a compressor named
        Bytes nodelta = vcd_header();
        vcd_window(nodelta, 0, 0, 0, 3, 1, B("abc"), B2({4}), Bytes(), false, 0);
        CHECK_ERR(apply(g, nodelta.data(), nodelta.size(), &out, 0), Error::BadPatch);
        // a compressor the engine does not know, named and used
        Bytes odd = vcd_header(0, 77);
        vcd_window(odd, 0, 0, 0, 3, 1, B("\x03zzz"), B2({4}), Bytes(), false, 0);
        CHECK_ERR(apply(g, odd.data(), odd.size(), &out, 0), Error::Unsupported);
        // a compressor that is named but never used is fine
        Bytes unused = vcd_header(0, 99);
        vcd_window(unused, 0, 0, 0, 3, 0, B("abc"), B2({4}), Bytes(), false, 0);
        CHECK_OK(apply(g, unused.data(), unused.size(), &out, 0));
        // lengths that do not add up
        Bytes lenbad = p0;
        lenbad[6]++;                                                 // 4 magic bytes, header indicator, window indicator, then the delta length
        CHECK_ERR(apply(g, lenbad.data(), lenbad.size(), &out, 0), Error::BadPatch);
        // instruction errors: past the end of the window, address too big, data underflow, leftovers
        auto one = [&](const Bytes& data, const Bytes& inst, const Bytes& addr, uint64_t tgtlen, uint64_t cpylen) {
            Bytes p = vcd_header();
            vcd_window(p, cpylen ? 1 : 0, cpylen, 0, tgtlen, 0, data, inst, addr, false, 0);
            return apply(g, p.data(), p.size(), &out, 0);
        };
        CHECK_OK(one(B("abc"), B2({4}), Bytes(), 3, 0));                                 // ADD 3
        CHECK_ERR(one(B("abc"), B2({5}), Bytes(), 3, 0), Error::BadPatch);               // ADD 4 into a window of 3
        CHECK_ERR(one(B("ab"), B2({4}), Bytes(), 3, 0), Error::BadPatch);                // data underflow
        CHECK_ERR(one(B("abcd"), B2({4}), Bytes(), 3, 0), Error::BadPatch);              // unused data left over
        CHECK_ERR(one(B("ab"), B2({4}), Bytes(), 4, 0), Error::BadPatch);                // window shorter than it says
        CHECK_ERR(one(Bytes(), B2({20}), B2({0}), 4, 0), Error::BadPatch);               // COPY from address 0 before anything exists
        CHECK_ERR(one(Bytes(), B2({20}), B2({3}), 4, 3), Error::BadPatch);               // address 3 = here (nothing there yet)
        CHECK_ERR(one(Bytes(), B2({20}), Bytes(), 4, 3), Error::BadPatch);               // address underflow
        CHECK_OK(one(Bytes(), B2({20}), B2({0}), 4, 3));                                 // COPY 4 @0 from a 3-byte source segment? crosses into the window
    }
    // a game that is too short for what the patch copies
    {
        Bytes p = vcd_header();
        vcd_window(p, 1, 8, 0, 4, 0, Bytes(), B2({20}), B2({4}), false, 0);          // COPY 4 @4 of an 8-byte segment
        const Bytes tiny = B("abcd");
        const Result r = apply(tiny, p.data(), p.size(), &out, 0);
        CHECK_ERR(r, Error::WrongSource);
    }
    // LZMA: a hand-made xz stream of stored LZMA2 chunks (the compressed-chunk path is covered by the z_xz fixture)
    {
        auto xz_first = [&](const Bytes& data) {
            Bytes o;
            const uint8_t sh[] = {0xFD, '7', 'z', 'X', 'Z', 0x00, 0x00, 0x00};
            o.insert(o.end(), sh, sh + sizeof sh);
            put_le32(o, crc32(sh + 6, 2));
            Bytes bh = {0x02, 0x00, 0x21, 0x01, 0x16, 0x00, 0x00, 0x00};          // size byte, flags, LZMA2 filter, dict prop, padding
            put_le32(bh, crc32(bh.data(), bh.size()));
            add(o, bh);
            for (size_t i = 0; i < data.size();) {
                const size_t n = std::min<size_t>(data.size() - i, 0x10000);
                o.push_back(i == 0 ? 0x01 : 0x02);
                o.push_back((uint8_t)((n - 1) >> 8)); o.push_back((uint8_t)(n - 1));
                o.insert(o.end(), data.begin() + i, data.begin() + i + n);
                i += n;
            }
            return o;
        };
        auto xz_next = [&](const Bytes& data) {
            Bytes o;
            for (size_t i = 0; i < data.size();) {
                const size_t n = std::min<size_t>(data.size() - i, 0x10000);
                o.push_back(0x02);
                o.push_back((uint8_t)((n - 1) >> 8)); o.push_back((uint8_t)(n - 1));
                o.insert(o.end(), data.begin() + i, data.begin() + i + n);
                i += n;
            }
            return o;
        };
        Rng r(5);
        Bytes big;
        for (int i = 0; i < 150000; ++i) big.push_back(r.byte());          // > 2 stored chunks in one section
        Bytes p = vcd_header(0, 2);
        // window 1: ADD of 150000 bytes, window 2: ADD 3 more; the data sections form one continuing stream
        Bytes d1 = xz_first(big), i1, i2;
        Bytes dd1; put_varint(dd1, big.size()); add(dd1, d1);
        i1.push_back(1); put_varint(i1, big.size());
        vcd_window(p, 0, 0, 0, big.size(), 1, dd1, i1, Bytes(), false, 0);
        const Bytes more = B("tail");
        Bytes dd2; put_varint(dd2, more.size()); add(dd2, xz_next(more));
        i2.push_back(1); put_varint(i2, more.size());
        vcd_window(p, 0, 0, 0, more.size(), 1, dd2, i2, Bytes(), false, 0);
        const Result res = apply(Bytes(), p.data(), p.size(), &out, 0);
        CHECK_OK(res);
        Bytes want = big;
        add(want, more);
        CHECK(out == want);
        // a corrupt xz header
        Bytes bad = p;
        for (size_t i = 0; i + 6 < bad.size(); ++i) {
            if (bad[i] == 0xFD && bad[i + 1] == '7') { bad[i + 8] ^= 1; break; }
        }
        CHECK_ERR(apply(Bytes(), bad.data(), bad.size(), &out, 0), Error::BadPatch);
    }
}

/* ---- the encoder in this file against the decoder, many shapes ---- */
static void test_vcdiff_roundtrip() {
    Rng r(17);
    for (int i = 0; i < 200; ++i) {
        const size_t n = i < 3 ? (size_t)i : r.below(9000);
        const Bytes game = make_game(r, n);
        Bytes tgt = i % 11 == 0 ? Bytes() : make_target(r, game);
        if (i % 5 == 0) tgt.insert(tgt.end(), 300, 0x7E);                         // a RUN
        if (i % 6 == 0 && tgt.size() > 40) { for (int k = 0; k < 200; ++k) tgt.push_back(tgt[tgt.size() - 3]); }   // overlapping copies
        VcdOpts o;
        o.window = i % 3 == 0 ? 1u << 30 : 400 + r.below(3000);
        o.adler = i % 4 != 0;
        o.segmented = i % 2 == 1;
        o.pairs = i % 5 != 1;
        if (i % 7 == 0) o.app = "a.bin/b.bin/";
        expect_roundtrip("vcdiff", game, vcd_encode(game, tgt, o), tgt);
    }
}

/* ---- patches made by the real xdelta3 ---- */
static const uint32_t kBaseCrc = 0x54a8982au, kTargetCrc = 0xb43de862u;
static const size_t kBaseSize = 56000, kTargetSize = 72104;

static void test_vcdiff_fixtures() {
    const Bytes base = fixture("base.bin");
    CHECK(base.size() == kBaseSize && crc_of(base) == kBaseCrc);
    struct Fx { const char* file; const char* what; };
    const Fx ok[] = {
        {"x_plain.xdelta", "no secondary compression, application header, one window"},
        {"x_djw.xdelta", "DJW (Huffman) secondary compression"},
        {"x_djw9.xdelta", "DJW level 9"},
        {"x_multi_plain.xdelta", "several windows with their own source segments"},
        {"x_multi_djw9.xdelta", "several windows, DJW"},
        {"x_noadler.xdelta", "no checksums"},
        {"x_noapp.xdelta", "no application header"},
        {"z_xz.xdelta", "LZMA (xz stream per section type), several windows"},
    };
    for (const Fx& f : ok) {
        const Bytes patch = fixture(f.file);
        Bytes out;
        const Result r = apply(base, patch.data(), patch.size(), &out, 0);
        if (!r.ok() || out.size() != kTargetSize || crc_of(out) != kTargetCrc) {
            std::printf("FAIL fixture %s (%s): %s | %s\n", f.file, f.what, r.message.c_str(), r.detail.c_str());
            ++g_fail;
        }
        Info info;
        std::string err;
        CHECK(inspect(patch.data(), patch.size(), &info, &err));
        CHECK(info.format == Format::Vcdiff && !info.has_source_crc && !info.has_target_crc);
    }
    {   // no source at all
        const Bytes patch = fixture("x_nosrc.xdelta");
        Bytes out;
        const Result r = apply(Bytes(), patch.data(), patch.size(), &out, 0);
        CHECK_OK(r);
        CHECK(out.size() == kTargetSize && crc_of(out) == kTargetCrc);
    }
    {   // the application header is the file names
        const Bytes patch = fixture("x_plain.xdelta");
        Info info;
        std::string err;
        CHECK(inspect(patch.data(), patch.size(), &info, &err));
        CHECK(info.metadata == "target.bin//base.bin/");
        const Bytes noapp = fixture("x_noapp.xdelta");
        CHECK(inspect(noapp.data(), noapp.size(), &info, &err) && info.metadata.empty());
    }
    {   // FGK (adaptive Huffman): one window, and four windows whose model carries over from one to the next
        const Bytes one = fixture("x_fgk.xdelta");
        Bytes out;
        Result r = apply(Bytes(), one.data(), one.size(), &out, 0);
        CHECK_OK(r);
        CHECK(out.size() == 9000 && crc_of(out) == 0x00687cdbu);
        CHECK(out == Bytes(base.begin(), base.begin() + 9000));
        const Bytes four = fixture("x_fgk_multi.xdelta");
        r = apply(Bytes(), four.data(), four.size(), &out, 0);
        CHECK_OK(r);
        CHECK(out.size() == 52000 && crc_of(out) == 0xd4a6be79u);
        // the model is stateful: the same patch with its first window cut out must not decode to the same thing
        // (a window starts with the header, so cut at the second window's start by re-applying only the tail is not
        // possible; instead flip a bit in the first window and the later windows go wrong or fail)
        Bytes bad = four;
        bad[four.size() / 4] ^= 0x10;
        r = apply(Bytes(), bad.data(), bad.size(), &out, 0);
        CHECK(!r.ok() || crc_of(out) != 0xd4a6be79u);
    }
    {   // another version of the game: the window checksums say so, for plain, DJW and LZMA alike
        Bytes other = base;
        other[30000] ^= 0x20;
        for (const char* f : {"x_plain.xdelta", "x_djw9.xdelta", "x_multi_djw9.xdelta", "z_xz.xdelta"}) {
            const Bytes patch = fixture(f);
            Bytes out;
            const Result r = apply(other, patch.data(), patch.size(), &out, 0);
            CHECK_ERR(r, Error::WrongSource);
            CHECK(r.message == "This patch is for a different version of the game.");
            CHECK(out.empty());
        }
        const Bytes shorter(base.begin(), base.begin() + 20000);
        const Bytes patch = fixture("x_plain.xdelta");
        Bytes out;
        CHECK_ERR(apply(shorter, patch.data(), patch.size(), &out, 0), Error::WrongSource);
    }
    {   // the output cap, and the output of a failed run is empty
        const Bytes patch = fixture("x_multi_djw9.xdelta");
        Bytes out;
        CHECK_OK(apply(base, patch.data(), patch.size(), &out, kTargetSize));
        CHECK_ERR(apply(base, patch.data(), patch.size(), &out, kTargetSize - 1), Error::TooBig);
        CHECK(out.empty());
        CHECK_ERR(apply(base, patch.data(), patch.size(), &out, 1000), Error::TooBig);
    }
}

/* ---- inspect() and friends ---- */
static void test_inspect_and_limits() {
    Rng r(23);
    const Bytes game = make_game(r, 4000);
    const Bytes tgt = make_target(r, game);
    const Bytes ups = ups_encode(game, tgt);
    const Bytes ips = ips_encode(game, tgt);
    Info info;
    std::string err;
    CHECK(inspect(ups.data(), ups.size(), &info, &err));
    CHECK(info.format == Format::Ups && info.has_source_crc && info.source_crc == crc_of(game) && info.target_crc == crc_of(tgt));
    CHECK(info.source_size == game.size() && info.target_size == tgt.size());
    CHECK(inspect(ips.data(), ips.size(), &info, &err));
    CHECK(info.format == Format::Ips && !info.has_source_crc && !info.has_target_crc && info.source_size == 0);
    CHECK(!inspect(nullptr, 0, &info, &err) && !err.empty());
    const Bytes junk = B("this is not a patch");
    CHECK(!inspect(junk.data(), junk.size(), &info, &err) && err == "This file isn't a patch the hub can read.");
    CHECK(inspect(ups.data(), ups.size(), nullptr, nullptr));
    CHECK(!inspect(ups.data(), 10, &info, &err));                                   // header without footer
    const Bytes ppf = B("PPF30 and so on");
    CHECK(!inspect(ppf.data(), ppf.size(), &info, &err) && err == "PPF patches aren't supported yet.");

    // limits on every format: exactly enough works, one byte less is Error::TooBig
    // (IPS starts from a copy of the game, so for it the game's size counts as well)
    const Bytes bps = bps_encode(game, tgt);
    const Bytes vcd = vcd_encode(game, tgt, VcdOpts());
    const struct { const Bytes* p; size_t need; } formats[] = {{&ips, std::max(tgt.size(), game.size())}, {&ups, tgt.size()}, {&bps, tgt.size()}, {&vcd, tgt.size()}};
    for (const auto& f : formats) {
        const Bytes* p = f.p;
        Bytes out;
        Result res = apply(game, p->data(), p->size(), &out, f.need);
        CHECK_OK(res);
        CHECK(out == tgt);
        res = apply(game, p->data(), p->size(), &out, f.need - 1);
        CHECK_ERR(res, Error::TooBig);
        CHECK(out.empty());
        // the same vector as source and output
        Bytes both = game;
        res = apply(both, p->data(), p->size(), &both, 0);
        CHECK_OK(res);
        CHECK(both == tgt);
        // a null output only checks
        CHECK_OK(apply(game, p->data(), p->size(), nullptr, 0));
    }
}

/* ======================================================================================== files */

static std::wstring g_dir;     // scratch folder: spaces and non-ASCII on purpose

static std::string u8(const std::wstring& w) { return winutil::to_utf8(w); }
static bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool put_file(const std::wstring& p, const Bytes& b) { return winutil::write_file(u8(p), b.data(), b.size()); }
static Bytes get_file(const std::wstring& p) { Bytes b; winutil::read_file(u8(p), &b); return b; }

static void test_apply_file() {
    Rng r(31);
    const std::wstring game_p = g_dir + L"\\game \u00e9.bin", patch_p = g_dir + L"\\patch \u65e5\u672c.bin", out_p = g_dir + L"\\new folder\\sub \u00e9\\hack \u00e9.out";
    const Bytes game = make_game(r, 200000);
    const Bytes tgt = make_target(r, game);
    CHECK(put_file(game_p, game));

    struct Case { const char* name; Bytes patch; };
    const Case cases[] = {{"ips", ips_encode(game, tgt)}, {"ups", ups_encode(game, tgt)}, {"bps", bps_encode(game, tgt, "meta")},
                          {"xdelta", vcd_encode(game, tgt, [] { VcdOpts o; o.window = 30000; o.app = "a/b/"; return o; }())}};
    for (const Case& c : cases) {
        CHECK(put_file(patch_p, c.patch));
        DeleteFileW(out_p.c_str());
        std::vector<double> seen;
        const Result res = apply_file(u8(game_p), u8(patch_p), u8(out_p), 0, [&](double f) { seen.push_back(f); return true; });
        if (!res.ok()) std::printf("apply_file %s: %s | %s\n", c.name, res.message.c_str(), res.detail.c_str());
        CHECK_OK(res);
        CHECK(get_file(out_p) == tgt);
        CHECK(!exists(out_p + L".tmp"));
        CHECK(!seen.empty() && seen.back() == 1.0 && seen.front() == 0.0);
        bool mono = true;
        for (size_t i = 1; i < seen.size(); ++i) mono = mono && seen[i] >= seen[i - 1] && seen[i] >= 0.0 && seen[i] <= 1.0;
        CHECK(mono);
        // the same patch with the default progress (empty callback)
        CHECK_OK(apply_file(u8(game_p), u8(patch_p), u8(out_p), 0, std::function<bool(double)>()));
        // a limit that is too small leaves the existing result alone
        CHECK_ERR(apply_file(u8(game_p), u8(patch_p), u8(out_p), tgt.size() / 2, nullptr), Error::TooBig);
        CHECK(get_file(out_p) == tgt);
        CHECK(!exists(out_p + L".tmp"));
    }

    // a wrong game: an existing result is untouched, nothing is left behind
    {
        const Bytes patch = bps_encode(game, tgt);
        CHECK(put_file(patch_p, patch));
        Bytes other = game;
        other[1234] ^= 1;
        const std::wstring other_p = g_dir + L"\\other game.bin";
        CHECK(put_file(other_p, other));
        const Bytes sentinel = B("previous result");
        const std::wstring fresh = g_dir + L"\\keep me.out";
        CHECK(put_file(fresh, sentinel));
        Result res = apply_file(u8(other_p), u8(patch_p), u8(fresh), 0, nullptr);
        CHECK_ERR(res, Error::WrongSource);
        CHECK(res.message == "This patch is for a different version of the game.");
        CHECK(get_file(fresh) == sentinel && !exists(fresh + L".tmp"));
        const std::wstring never = g_dir + L"\\never made.out";
        res = apply_file(u8(other_p), u8(patch_p), u8(never), 0, nullptr);
        CHECK_ERR(res, Error::WrongSource);
        CHECK(!exists(never) && !exists(never + L".tmp"));
        DeleteFileW(other_p.c_str());
        DeleteFileW(fresh.c_str());
    }

    // cancel half way through a game big enough for a progress bar
    {
        Bytes big(6u << 20, 0x5A);
        for (size_t i = 0; i < big.size(); i += 4099) big[i] = (uint8_t)(i >> 3);
        Bytes big2 = big;
        big2[3u << 20] ^= 0xFF;
        big2[5u << 20] ^= 0xFF;
        const std::wstring bg = g_dir + L"\\big game.bin", bp = g_dir + L"\\big patch.bps", bo = g_dir + L"\\big out.bin";
        CHECK(put_file(bg, big));
        CHECK(put_file(bp, bps_sparse(big.size(), {{3u << 20, (uint8_t)(big[3u << 20] ^ 0xFF)}, {5u << 20, (uint8_t)(big[5u << 20] ^ 0xFF)}}, crc_of(big), crc_of(big2))));
        int calls = 0;
        const Result res = apply_file(u8(bg), u8(bp), u8(bo), 0, [&](double f) { ++calls; return f < 0.5; });
        CHECK_ERR(res, Error::Cancelled);
        CHECK(calls > 3 && !exists(bo) && !exists(bo + L".tmp"));
        CHECK_OK(apply_file(u8(bg), u8(bp), u8(bo), 0, nullptr));
        CHECK(get_file(bo) == big2);
        // the result of crc32_file matches
        uint32_t crc = 0;
        std::string err;
        CHECK(crc32_file(u8(bo), &crc, &err) && crc == crc_of(big2));
        double last = -1;
        CHECK(crc32_file(u8(bo), &crc, &err, [&](double f) { last = f; return true; }) && last == 1.0);
        CHECK(!crc32_file(u8(bo), &crc, &err, [](double) { return false; }) && err == "Patching was cancelled.");
        CHECK(!crc32_file(u8(g_dir + L"\\nope.bin"), &crc, &err));
        DeleteFileW(bg.c_str()); DeleteFileW(bp.c_str()); DeleteFileW(bo.c_str());
    }

    // the output may not be the game or the patch
    {
        const Bytes patch = ups_encode(game, tgt);
        CHECK(put_file(patch_p, patch));
        Result res = apply_file(u8(game_p), u8(patch_p), u8(game_p), 0, nullptr);
        CHECK_ERR(res, Error::Io);
        CHECK(get_file(game_p) == game);
        // the same file spelled differently (forward slashes, other case)
        std::string alias = u8(game_p);
        for (char& ch : alias) { if (ch == '\\') ch = '/'; }
        for (char& ch : alias) { if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A'); }
        res = apply_file(u8(game_p), u8(patch_p), alias, 0, nullptr);
        CHECK_ERR(res, Error::Io);
        CHECK(get_file(game_p) == game);
        res = apply_file(u8(game_p), u8(patch_p), u8(patch_p), 0, nullptr);
        CHECK_ERR(res, Error::Io);
        CHECK(get_file(patch_p) == patch);
    }

    // missing and broken inputs
    {
        const std::wstring none = g_dir + L"\\does not exist.bin", outp = g_dir + L"\\o.out";
        CHECK(put_file(patch_p, ups_encode(game, tgt)));
        CHECK_ERR(apply_file(u8(none), u8(patch_p), u8(outp), 0, nullptr), Error::Io);
        CHECK_ERR(apply_file(u8(game_p), u8(none), u8(outp), 0, nullptr), Error::Io);
        CHECK(put_file(patch_p, Bytes()));
        CHECK_ERR(apply_file(u8(game_p), u8(patch_p), u8(outp), 0, nullptr), Error::BadPatch);
        CHECK(put_file(patch_p, B("not a patch at all, just text")));
        CHECK_ERR(apply_file(u8(game_p), u8(patch_p), u8(outp), 0, nullptr), Error::BadPatch);
        CHECK(put_file(patch_p, B("PPF30xxxxxxxxxxxx")));
        CHECK_ERR(apply_file(u8(game_p), u8(patch_p), u8(outp), 0, nullptr), Error::Unsupported);
        CHECK(!exists(outp) && !exists(outp + L".tmp"));
        // inspect_file
        Info info;
        std::string err;
        const Bytes b = bps_encode(game, tgt, "hello");
        CHECK(put_file(patch_p, b));
        CHECK(inspect_file(u8(patch_p), &info, &err) && info.format == Format::Bps && info.source_crc == crc_of(game) && info.metadata == "hello");
        CHECK(!inspect_file(u8(none), &info, &err));
        CHECK(put_file(patch_p, Bytes()));
        CHECK(!inspect_file(u8(patch_p), &info, &err));
    }

    // a game and an output deep enough for a path past MAX_PATH (the \\?\ prefix is added for you)
    {
        std::wstring deep = g_dir;
        while (deep.size() < 290) {
            deep += L"\\level-with-a-fairly-long-name-";
            deep += std::to_wstring(deep.size());
            CreateDirectoryW((L"\\\\?\\" + deep).c_str(), nullptr);
        }
        const std::wstring dg = deep + L"\\game.bin", dp = deep + L"\\patch.ups", dout = deep + L"\\out.bin";
        const Bytes deep_patch = ups_encode(game, tgt);
        const bool wrote = winutil::write_file(u8(L"\\\\?\\" + dg), game.data(), game.size()) &&
                           winutil::write_file(u8(L"\\\\?\\" + dp), deep_patch.data(), deep_patch.size());
        CHECK(wrote);
        if (wrote) {
            CHECK_OK(apply_file(u8(dg), u8(dp), u8(dout), 0, nullptr));
            CHECK(exists(L"\\\\?\\" + dout));
        }
        DeleteFileW((L"\\\\?\\" + dg).c_str()); DeleteFileW((L"\\\\?\\" + dp).c_str()); DeleteFileW((L"\\\\?\\" + dout).c_str());
        for (std::wstring d = deep; d.size() > g_dir.size(); d.resize(d.find_last_of(L'\\'))) RemoveDirectoryW((L"\\\\?\\" + d).c_str());
    }
    DeleteFileW(game_p.c_str());
    DeleteFileW(patch_p.c_str());
    DeleteFileW(out_p.c_str());
}

/* ======================================================================================== fuzz */

static Bytes mutate(const Bytes& p, Rng& r) {
    Bytes m = p;
    switch (r.below(8)) {
        case 0: m.resize(r.below((uint32_t)m.size() + 1)); break;                          // truncate
        case 1: { const int n = 1 + (int)r.below(8); for (int i = 0; i < n && !m.empty(); ++i) m[r.below((uint32_t)m.size())] ^= (uint8_t)(1u << r.below(8)); break; }
        case 2: { const int n = 1 + (int)r.below(4); for (int i = 0; i < n && !m.empty(); ++i) m[r.below((uint32_t)m.size())] = r.byte(); break; }
        case 3: if (m.size() > 2) { const size_t at = r.below((uint32_t)m.size() - 1), n = 1 + r.below((uint32_t)std::min<size_t>(64, m.size() - at - 1)); m.erase(m.begin() + at, m.begin() + at + n); } break;
        case 4: { const size_t at = r.below((uint32_t)m.size() + 1); const int n = 1 + (int)r.below(16); Bytes ins; for (int i = 0; i < n; ++i) ins.push_back(r.byte()); m.insert(m.begin() + at, ins.begin(), ins.end()); break; }
        case 5: if (m.size() > 8) { const size_t a = r.below((uint32_t)m.size() - 4), b = r.below((uint32_t)m.size() - 4); std::memmove(&m[a], &m[b], 4); } break;
        case 6: { const int n = 1 + (int)r.below(3); static const uint8_t ext[] = {0x00, 0x7F, 0x80, 0xFF, 0xFE, 0x01}; for (int i = 0; i < n && !m.empty(); ++i) m[r.below((uint32_t)m.size())] = ext[r.below(6)]; break; }
        default: if (m.size() > 12) { const size_t at = r.below(16); if (at < m.size()) m[at] ^= (uint8_t)(1u << r.below(8)); } break;   // the header
    }
    return m;
}

/* UPS and BPS check the patch's own CRC first; recompute it now and then so the mutations reach the decoder. */
static void refresh_crc(Bytes& m) {
    if (m.size() >= 16 && (std::memcmp(m.data(), "UPS1", 4) == 0 || std::memcmp(m.data(), "BPS1", 4) == 0)) {
        const uint32_t c = crc32(m.data(), m.size() - 4);
        for (int i = 0; i < 4; ++i) m[m.size() - 4 + i] = (uint8_t)(c >> (8 * i));
    }
}

struct Seed { std::string name; Bytes game, patch; };

static void fuzz_seed(const Seed& s, int iters, uint64_t salt) {
    std::printf("  fuzz %-28s %d mutations", s.name.c_str(), iters);
    std::fflush(stdout);
    const ULONGLONG t0 = GetTickCount64();
    Rng r(salt);
    Bytes out;
    int ok = 0;
    for (int it = 0; it < iters; ++it) {
        Bytes m = mutate(s.patch, r);
        if (r.below(10) < 7) refresh_crc(m);
        std::unique_ptr<uint8_t[]> exact(new uint8_t[m.size() ? m.size() : 1]);
        if (!m.empty()) std::memcpy(exact.get(), m.data(), m.size());
        Bytes game(s.game.begin(), s.game.end());
        if (r.below(10) == 0 && !game.empty()) game[r.below((uint32_t)game.size())] ^= (uint8_t)(1u << r.below(8));
        if (r.below(20) == 0 && !game.empty()) game.resize(r.below((uint32_t)game.size()));
        const Result res = apply(game, exact.get(), m.size(), &out, 4u << 20);
        if (res.ok()) ++ok;
        else if (!out.empty() || res.message.empty()) { std::printf("FAIL fuzz %s #%d: failed run left output or no message\n", s.name.c_str(), it); ++g_fail; }
        Info info;
        std::string err;
        inspect(exact.get(), m.size(), &info, &err);
        detect(exact.get(), m.size());
    }
    std::printf("  (%llu ms, %d still applied)\n", (unsigned long long)(GetTickCount64() - t0), ok);
}

static void test_fuzz() {
    std::vector<Seed> seeds;
    Rng r(41);
    {
        const Bytes game = make_game(r, 3000);
        const Bytes tgt = make_target(r, game);
        seeds.push_back({"ips", game, ips_encode(game, tgt)});
        seeds.push_back({"ups", game, ups_encode(game, tgt)});
        seeds.push_back({"bps", game, bps_encode(game, tgt, "meta data")});
        VcdOpts o;
        o.window = 700;
        o.segmented = true;
        o.app = "x/y/";
        seeds.push_back({"vcdiff (own encoder)", game, vcd_encode(game, tgt, o)});
        o.window = 1u << 30;
        o.pairs = false;
        o.segmented = false;
        o.adler = false;
        seeds.push_back({"vcdiff (no checksum)", game, vcd_encode(game, tgt, o)});
    }
    const Bytes base = fixture("base.bin");
    for (const char* f : {"x_plain.xdelta", "x_djw.xdelta", "x_djw9.xdelta", "x_multi_plain.xdelta", "x_multi_djw9.xdelta", "x_noadler.xdelta", "x_noapp.xdelta", "z_xz.xdelta"})
        seeds.push_back({f, base, fixture(f)});
    for (const char* f : {"x_nosrc.xdelta", "x_fgk.xdelta", "x_fgk_multi.xdelta"}) seeds.push_back({f, Bytes(), fixture(f)});
    uint64_t salt = 1000;
    for (const Seed& s : seeds) {
        const bool slow = s.name.find("fgk") != std::string::npos || s.name == "x_nosrc.xdelta";     // 30-50 KB of bit-by-bit decoding each
        fuzz_seed(s, slow ? std::max(1, g_fuzz_iters / 4) : g_fuzz_iters, ++salt);
    }

    // every proper prefix of every seed (all lengths; the big x_nosrc one in steps of 8, with its first and last 200 bytes covered fully)
    for (const Seed& s : seeds) {
        Bytes out;
        const size_t step = s.patch.size() > 8000 ? 8 : 1;
        for (size_t cut = 0; cut < s.patch.size(); cut += (cut < 200 || cut + 200 > s.patch.size()) ? 1 : step) {
            std::unique_ptr<uint8_t[]> exact(new uint8_t[cut ? cut : 1]);
            if (cut) std::memcpy(exact.get(), s.patch.data(), cut);
            const Result res = apply(s.game, exact.get(), cut, &out, 4u << 20);
            if (!res.ok() && !out.empty()) { std::printf("FAIL prefix %s %zu left output\n", s.name.c_str(), cut); ++g_fail; }
            if (s.name == "bps" || s.name == "ups") CHECK(!res.ok());
        }
    }

    // the same through files (FileSource / FileSink / the atomic rename): a few dozen mutated patches per seed
    const ULONGLONG file_t0 = GetTickCount64();
    const std::wstring gp = g_dir + L"\\fuzz game.bin", pp = g_dir + L"\\fuzz patch.bin", op = g_dir + L"\\fuzz out.bin";
    for (const Seed& s : seeds) {
        if (!put_file(gp, s.game)) { ++g_fail; continue; }
        Rng q(77);
        for (int it = 0; it < g_fuzz_iters / 100 + 12; ++it) {
            Bytes m = mutate(s.patch, q);
            if (q.below(10) < 7) refresh_crc(m);
            put_file(pp, m);
            DeleteFileW(op.c_str());
            const Result res = apply_file(u8(gp), u8(pp), u8(op), 4u << 20, nullptr);
            if (!res.ok() && (exists(op) || exists(op + L".tmp"))) { std::printf("FAIL fuzz file %s #%d: failed run left a file\n", s.name.c_str(), it); ++g_fail; break; }
            if (res.ok() && !exists(op)) { std::printf("FAIL fuzz file %s #%d: ok but no output\n", s.name.c_str(), it); ++g_fail; break; }
        }
    }
    DeleteFileW(gp.c_str()); DeleteFileW(pp.c_str()); DeleteFileW(op.c_str());
    std::printf("  fuzz through files: %llu ms\n", (unsigned long long)(GetTickCount64() - file_t0));
}

/* ======================================================================================== big */

static void test_big() {
    std::printf("big: 1.5 GB sparse game...\n");
    std::fflush(stdout);
    const uint64_t size = 1500ull << 20;
    const std::wstring gp = g_dir + L"\\huge game.iso", pp = g_dir + L"\\huge patch.bps", op = g_dir + L"\\huge out.iso";
    {
        HANDLE h = CreateFileW(gp.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD ret = 0;
        DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr);
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)size;
        CHECK(SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && SetEndOfFile(h));
        CloseHandle(h);
    }
    // the game is all zeros; the result differs at three places
    const std::vector<Edit> edits = {{12345, 0x11}, {size / 2, 0x22}, {size - 1, 0x33}};
    Bytes zeros(1u << 20, 0);
    uint32_t src_crc = 0, tgt_crc = 0;
    for (uint64_t done = 0; done < size; done += zeros.size()) src_crc = crc32(zeros.data(), (size_t)std::min<uint64_t>(zeros.size(), size - done), src_crc);
    for (uint64_t done = 0; done < size;) {
        const size_t n = (size_t)std::min<uint64_t>(zeros.size(), size - done);
        Bytes chunk(n, 0);
        for (const Edit& e : edits) { if (e.at >= done && e.at < done + n) chunk[(size_t)(e.at - done)] = e.value; }
        tgt_crc = crc32(chunk.data(), n, tgt_crc);
        done += n;
    }
    CHECK(put_file(pp, bps_sparse(size, edits, src_crc, tgt_crc)));
    PROCESS_MEMORY_COUNTERS before = {}, after = {};
    GetProcessMemoryInfo(GetCurrentProcess(), &before, sizeof before);
    int calls = 0;
    const Result res = apply_file(u8(gp), u8(pp), u8(op), 0, [&](double) { ++calls; return true; });
    GetProcessMemoryInfo(GetCurrentProcess(), &after, sizeof after);
    CHECK_OK(res);
    const double grew = ((double)after.PeakWorkingSetSize - (double)before.PeakWorkingSetSize) / (1024.0 * 1024.0);
    const double page = ((double)after.PeakPagefileUsage - (double)before.PeakPagefileUsage) / (1024.0 * 1024.0);
    std::printf("big: %d progress calls, peak working set grew %.1f MB, peak commit grew %.1f MB\n", calls, grew, page);
    CHECK(page < 200.0);
    uint32_t got = 0;
    std::string err;
    CHECK(crc32_file(u8(op), &got, &err) && got == tgt_crc);
    DeleteFileW(gp.c_str()); DeleteFileW(pp.c_str()); DeleteFileW(op.c_str());
}

/* ======================================================================================== main */

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--big") g_big = true;
        else if (a == "--fuzz" && i + 1 < argc) g_fuzz_iters = std::max(1, std::atoi(argv[++i]));
        else g_data_dir = a;
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_dir = std::wstring(tmp) + L"patcher test \u00e9 \u65e5\u672c " + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_dir.c_str(), nullptr);
    try {
        struct Step { const char* name; void (*fn)(); };
        const Step steps[] = {{"crc32", test_crc32}, {"detect", test_detect_and_ext}, {"ips", test_ips}, {"ups", test_ups},
                              {"bps", test_bps}, {"vcdiff table", test_vcdiff_table}, {"vcdiff by hand", test_vcdiff_handbuilt},
                              {"vcdiff round trips", test_vcdiff_roundtrip}, {"xdelta3 fixtures", test_vcdiff_fixtures},
                              {"inspect, limits", test_inspect_and_limits}, {"apply_file", test_apply_file}};
        for (const Step& s : steps) {
            const ULONGLONG t0 = GetTickCount64();
            s.fn();
            std::printf("%-20s %6llu ms\n", s.name, (unsigned long long)(GetTickCount64() - t0));
        }
        std::printf("fuzz: %d mutations per seed\n", g_fuzz_iters);
        const ULONGLONG t0 = GetTickCount64();
        test_fuzz();
        std::printf("%-20s %6llu ms\n", "fuzz (all)", (unsigned long long)(GetTickCount64() - t0));
        if (g_big) test_big();
    } catch (const std::exception& e) {
        std::printf("FAIL: exception escaped: %s\n", e.what());
        ++g_fail;
    }
    // best-effort cleanup of the scratch folder
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((g_dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring n = fd.cFileName;
                if (n == L"." || n == L"..") continue;
                const std::wstring p = g_dir + L"\\" + n;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    WIN32_FIND_DATAW f2;
                    HANDLE h2 = FindFirstFileW((p + L"\\*").c_str(), &f2);
                    if (h2 != INVALID_HANDLE_VALUE) {
                        do {
                            const std::wstring n2 = f2.cFileName;
                            if (n2 == L"." || n2 == L"..") continue;
                            const std::wstring p2 = p + L"\\" + n2;
                            if (f2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p2.c_str());
                            else DeleteFileW(p2.c_str());
                        } while (FindNextFileW(h2, &f2));
                        FindClose(h2);
                    }
                    RemoveDirectoryW(p.c_str());
                } else {
                    DeleteFileW(p.c_str());
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(g_dir.c_str());
    }
    if (g_fail) { std::printf("patcher_test: %d FAILED\n", g_fail); return 1; }
    std::printf("patcher_test: all passed\n");
    return 0;
}
