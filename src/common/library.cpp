#include "miniz.h"
#include "library.h"
#include "games.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace hub {
namespace {

namespace fs = std::filesystem;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

fs::path utf8_path(const std::string& s) { return fs::path(to_wide(s)); }
std::string path_utf8(const fs::path& p) { return to_utf8(p.native()); }

std::string ascii_lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string hex_lower(const std::string& upper_or_lower) { return ascii_lower(upper_or_lower); }

std::string hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08x", v);
    return b;
}

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint32_t le32(const uint8_t* p) {
    return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[0]);
}
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }

/* Read n bytes at offset; false if the file is too short. */
bool read_at(const fs::path& p, uint64_t offset, size_t n, std::vector<uint8_t>* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    if (offset) {
        f.seekg((std::streamoff)offset, std::ios::beg);
        if (!f) return false;
    }
    out->assign(n, 0);
    f.read(reinterpret_cast<char*>(out->data()), (std::streamsize)n);
    return f.gcount() == (std::streamsize)n;
}

/* ---- CRC32 (table, IEEE 802.3) ---- */
const uint32_t* crc_table() {
    static uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
    });
    return table;
}

uint32_t crc32_update(uint32_t crc, const uint8_t* d, size_t n) {
    const uint32_t* t = crc_table();
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = t[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- text helpers ---- */

/* [a-z0-9._-], lowercase, runs of '-' collapsed, trimmed. */
std::string sanitize_key(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        char r;
        if (c >= 'A' && c <= 'Z') r = char(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_') r = char(c);
        else r = '-';
        if (r == '-' && (o.empty() || o.back() == '-')) continue;
        o.push_back(r);
    }
    while (!o.empty() && (o.back() == '-' || o.back() == '.')) o.pop_back();
    if (o.empty()) o = "x";
    return o;
}

bool is_alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Raw header bytes -> printable text, or "" if it looks like garbage. Stops at first NUL. */
std::string header_text(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] == 0) break;
        s.push_back((char)p[i]);
    }
    for (unsigned char c : s) {
        if (c < 0x20 || c >= 0x7F) return std::string();
    }
    return s;
}

std::string collapse_spaces(const std::string& s) {
    std::string o;
    bool pend = false;
    for (char c : s) {
        if (c == ' ' || c == '\t') { pend = !o.empty(); continue; }
        if (pend) o.push_back(' ');
        pend = false;
        o.push_back(c);
    }
    return o;
}

bool is_roman(const std::string& w) {
    for (char c : w)
        if (c != 'I' && c != 'V' && c != 'X') return false;
    return !w.empty();
}

/* Title-case an ALL-CAPS string; short vowel-less words (NBA, WWF) and roman numerals stay upper. */
std::string title_case_if_caps(const std::string& s) {
    bool has_letter = false, has_lower = false;
    for (char c : s) {
        if (c >= 'a' && c <= 'z') has_lower = true;
        if (c >= 'A' && c <= 'Z') has_letter = true;
    }
    if (!has_letter || has_lower) return s;
    std::string o;
    size_t i = 0;
    while (i < s.size()) {
        if (!is_alnum(s[i])) { o.push_back(s[i++]); continue; }
        size_t j = i;
        while (j < s.size() && is_alnum(s[j])) ++j;
        std::string w = s.substr(i, j - i);
        bool letters_only = true, vowel = false;
        for (char c : w) {
            if (!(c >= 'A' && c <= 'Z')) letters_only = false;
            if (c == 'A' || c == 'E' || c == 'I' || c == 'O' || c == 'U' || c == 'Y') vowel = true;
        }
        if (!(letters_only && ((!vowel && w.size() <= 4) || is_roman(w)))) {
            for (size_t k = 1; k < w.size(); ++k)
                if (w[k] >= 'A' && w[k] <= 'Z') w[k] = char(w[k] - 'A' + 'a');
        }
        o += w;
        i = j;
    }
    return o;
}

/* File name without extension and without "(USA)" / "[!]" style tags. */
std::string title_from_filename(const fs::path& p) {
    std::string stem = path_utf8(p.stem());
    std::string o;
    int depth = 0;
    for (char c : stem) {
        if (c == '(' || c == '[') { ++depth; continue; }
        if (c == ')' || c == ']') { if (depth > 0) --depth; continue; }
        if (depth == 0) o.push_back(c == '_' ? ' ' : c);
    }
    o = collapse_spaces(o);
    if (o.empty()) o = collapse_spaces(stem);
    return o;
}

/* Prettify a header title; falls back to the file name when empty / garbage. */
std::string pretty_title(const std::string& raw, const fs::path& p) {
    std::string s = collapse_spaces(raw);
    int alnum = 0;
    for (char c : s)
        if (is_alnum(c)) ++alnum;
    if (alnum < 2) return title_from_filename(p);
    return title_case_if_caps(s);
}

/* ------------------------------------------------------------------ */
/* inflate (RFC 1951) for GCZ block 0; adapted from the puff design    */
/* ------------------------------------------------------------------ */

struct InflateDone {};
struct InflateErr {};

struct Huff {
    short count[16];
    short symbol[288];
};

struct InflState {
    const uint8_t* in;
    size_t inlen, incnt = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    uint8_t* out;
    size_t outlen, outcnt = 0;
};

int infl_bits(InflState& s, int need) {
    long val = s.bitbuf;
    while (s.bitcnt < need) {
        if (s.incnt >= s.inlen) throw InflateErr();
        val |= (long)s.in[s.incnt++] << s.bitcnt;
        s.bitcnt += 8;
    }
    s.bitbuf = (uint32_t)(val >> need);
    s.bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}

void infl_put(InflState& s, uint8_t b) {
    s.out[s.outcnt++] = b;
    if (s.outcnt >= s.outlen) throw InflateDone();  // we only need the first bytes
}

int infl_construct(Huff& h, const short* length, int n) {
    for (int len = 0; len <= 15; ++len) h.count[len] = 0;
    for (int sym = 0; sym < n; ++sym) h.count[length[sym]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = (short)(offs[len] + h.count[len]);
    for (int sym = 0; sym < n; ++sym)
        if (length[sym] != 0) h.symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

int infl_decode(InflState& s, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= infl_bits(s, 1);
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    throw InflateErr();
}

void infl_codes(InflState& s, const Huff& lencode, const Huff& distcode) {
    static const short lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                    6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int sym = infl_decode(s, lencode);
        if (sym < 256) {
            infl_put(s, (uint8_t)sym);
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) throw InflateErr();
            int len = lens[sym] + infl_bits(s, lext[sym]);
            int ds = infl_decode(s, distcode);
            if (ds >= 30) throw InflateErr();
            size_t dist = (size_t)dists[ds] + (size_t)infl_bits(s, dext[ds]);
            if (dist > s.outcnt) throw InflateErr();
            while (len--) infl_put(s, s.out[s.outcnt - dist]);
        }
    }
}

/* Raw deflate stream -> up to outlen bytes. Returns bytes produced (stops early when full). */
size_t inflate_prefix(const uint8_t* in, size_t inlen, uint8_t* out, size_t outlen) {
    InflState s;
    s.in = in; s.inlen = inlen; s.out = out; s.outlen = outlen;
    try {
        int last;
        do {
            last = infl_bits(s, 1);
            int type = infl_bits(s, 2);
            if (type == 0) {
                s.bitbuf = 0; s.bitcnt = 0;
                if (s.incnt + 4 > s.inlen) throw InflateErr();
                unsigned len = s.in[s.incnt] | (s.in[s.incnt + 1] << 8);
                unsigned nlen = s.in[s.incnt + 2] | (s.in[s.incnt + 3] << 8);
                s.incnt += 4;
                if (len != (~nlen & 0xFFFFu)) throw InflateErr();
                if (s.incnt + len > s.inlen) throw InflateErr();
                while (len--) infl_put(s, s.in[s.incnt++]);
            } else if (type == 1) {
                static Huff lencode, distcode;
                static std::once_flag once;
                std::call_once(once, [] {
                    short lengths[288];
                    int sym = 0;
                    for (; sym < 144; ++sym) lengths[sym] = 8;
                    for (; sym < 256; ++sym) lengths[sym] = 9;
                    for (; sym < 280; ++sym) lengths[sym] = 7;
                    for (; sym < 288; ++sym) lengths[sym] = 8;
                    infl_construct(lencode, lengths, 288);
                    for (sym = 0; sym < 30; ++sym) lengths[sym] = 5;
                    infl_construct(distcode, lengths, 30);
                });
                infl_codes(s, lencode, distcode);
            } else if (type == 2) {
                static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                short lengths[320];
                int nlen = infl_bits(s, 5) + 257;
                int ndist = infl_bits(s, 5) + 1;
                int ncode = infl_bits(s, 4) + 4;
                if (nlen > 286 || ndist > 30) throw InflateErr();
                int idx;
                for (idx = 0; idx < ncode; ++idx) lengths[order[idx]] = (short)infl_bits(s, 3);
                for (; idx < 19; ++idx) lengths[order[idx]] = 0;
                Huff lencode, distcode;
                if (infl_construct(lencode, lengths, 19) != 0) throw InflateErr();
                idx = 0;
                while (idx < nlen + ndist) {
                    int sym = infl_decode(s, lencode);
                    if (sym < 16) {
                        lengths[idx++] = (short)sym;
                    } else {
                        int len = 0, rep;
                        if (sym == 16) {
                            if (idx == 0) throw InflateErr();
                            len = lengths[idx - 1];
                            rep = 3 + infl_bits(s, 2);
                        } else if (sym == 17) {
                            rep = 3 + infl_bits(s, 3);
                        } else {
                            rep = 11 + infl_bits(s, 7);
                        }
                        if (idx + rep > nlen + ndist) throw InflateErr();
                        while (rep--) lengths[idx++] = (short)len;
                    }
                }
                if (lengths[256] == 0) throw InflateErr();
                int err = infl_construct(lencode, lengths, nlen);
                if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) throw InflateErr();
                err = infl_construct(distcode, lengths + nlen, ndist);
                if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) throw InflateErr();
                infl_codes(s, lencode, distcode);
            } else {
                throw InflateErr();
            }
        } while (!last);
    } catch (const InflateDone&) {
        return outlen;
    } catch (const InflateErr&) {
        return s.outcnt;
    }
    return s.outcnt;
}

/* ------------------------------------------------------------------ */
/* identification                                                      */
/* ------------------------------------------------------------------ */

std::atomic<uint64_t> g_identify_calls{0};

constexpr uint64_t kGbaMin = 64u * 1024u, kGbaMax = 32u * 1024u * 1024u;
constexpr uint64_t kGbMin = 32u * 1024u, kGbMax = 8u * 1024u * 1024u;
constexpr size_t kGcHeader = 0x440;

bool id_n64(const fs::path& p, GameEntry* e) {
    std::vector<uint8_t> img;
    if (!load_rom(path_utf8(p), &img) || img.size() < 0x40) return false;
    std::string md5 = md5_hex(img.data(), img.size());
    if (md5.empty()) return false;
    e->system = System::N64;
    e->key = "n64:" + ascii_lower(md5);
    e->title = pretty_title(header_text(img.data() + 0x20, 0x14), p);
    std::string ser(reinterpret_cast<const char*>(img.data() + 0x3B), 4);
    bool ok = true;
    for (char c : ser)
        if (!is_alnum(c)) ok = false;
    e->serial = ok ? ser : std::string();
    if (const GameInfo* g = find_game_for("n64", e->serial, md5)) {
        e->catalog_id = g->id;
        e->title = g->title;
    }
    return true;
}

bool id_gba(const fs::path& p, uint64_t size, GameEntry* e) {
    if (size < kGbaMin || size > kGbaMax) return false;
    std::vector<uint8_t> d;
    if (!read_at(p, 0, (size_t)size, &d)) return false;
    unsigned chk = 0;
    for (int i = 0xA0; i <= 0xBC; ++i) chk += d[i];
    uint8_t want = (uint8_t)(-(int)chk - 0x19);
    bool chk_ok = want == d[0xBD];
    if (!chk_ok && d[0xB2] != 0x96) return false;
    if (!chk_ok && d[0xB2] == 0x96) {
        /* Tolerate hacked headers only if the game code is sane. */
        for (int i = 0; i < 4; ++i)
            if (!is_alnum((char)d[0xAC + i])) return false;
    }
    std::string code((const char*)&d[0xAC], 4);
    bool code_ok = true;
    for (char c : code)
        if (!is_alnum(c)) code_ok = false;
    if (!code_ok) return false;
    uint32_t crc = crc32_update(0, d.data(), d.size());
    e->system = System::GBA;
    e->serial = code;
    e->key = "gba:" + sanitize_key(code) + "-" + hex32(crc);
    if (const GameInfo* g = find_game_for("gba", code, std::string())) e->catalog_id = g->id;
    e->title = pretty_title(header_text(&d[0xA0], 12), p);
    return true;
}

bool id_gb(const fs::path& p, uint64_t size, GameEntry* e) {
    if (size < kGbMin || size > kGbMax) return false;
    std::vector<uint8_t> h;
    if (!read_at(p, 0, 0x150, &h)) return false;
    uint8_t x = 0;
    for (int i = 0x134; i <= 0x14C; ++i) x = (uint8_t)(x - h[i] - 1);
    if (x != h[0x14D]) return false;
    uint8_t cgb = h[0x143];
    bool is_cgb = (cgb == 0x80 || cgb == 0xC0);
    size_t tlen = is_cgb ? 11 : 16;
    /* Non-printable bytes terminate the title (old carts reuse 0x143). */
    std::string t;
    for (size_t i = 0; i < tlen; ++i) {
        uint8_t c = h[0x134 + i];
        if (c < 0x20 || c >= 0x7F) break;
        t.push_back((char)c);
    }
    uint16_t gsum = (uint16_t)((h[0x14E] << 8) | h[0x14F]);
    e->system = is_cgb ? System::GBC : System::GB;
    char tail[8];
    std::snprintf(tail, sizeof(tail), "%04x", gsum);
    std::string base = sanitize_key(t);
    if (t.empty()) base = "untitled";
    e->key = std::string(is_cgb ? "gbc:" : "gb:") + base + "-" + tail;
    e->title = pretty_title(t, p);
    /* Catalog serial: the 4-char manufacturer/game code (0x13F-0x142) on CGB carts that have one,
     * otherwise the header title (uppercase, trailing spaces trimmed). games.cpp matches it. */
    std::string code;
    if (is_cgb) {
        for (int i = 0x13F; i < 0x143; ++i) {
            char c = (char)h[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) { code.clear(); break; }
            code.push_back(c);
        }
    }
    if (code.size() == 4) {
        e->serial = code;
    } else {
        std::string s = t;
        while (!s.empty() && s.back() == ' ') s.pop_back();
        for (auto& c : s)
            if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        e->serial = s;
    }
    if (const GameInfo* g = find_game_for(is_cgb ? "gbc" : "gb", e->serial, std::string())) e->catalog_id = g->id;
    return true;
}

/* 0x440-byte (or more) GC disc header -> entry. */
bool gc_from_header(const uint8_t* h, size_t n, const fs::path& p, GameEntry* e) {
    if (n < 0x80) return false;
    if (be32(h + 0x1C) != 0xC2339F3Du) return false;
    for (int i = 0; i < 6; ++i)
        if (!is_alnum((char)h[i])) return false;
    std::string id((const char*)h, 6);
    int disc = h[6];
    e->system = System::GC;
    e->serial = id;
    e->key = "gc:" + sanitize_key(id) + "-" + std::to_string(disc);
    size_t tl = std::min<size_t>(n > 0x20 ? n - 0x20 : 0, 0x3E0);
    e->title = pretty_title(header_text(h + 0x20, tl), p);
    return true;
}

bool id_gc_filename_only(const fs::path& p, GameEntry* e) {
    e->system = System::GC;
    e->serial.clear();
    std::string stem = sanitize_key(title_from_filename(p));
    e->key = "gc:file-" + stem;
    e->title = title_from_filename(p);
    return true;
}

bool id_gc(const fs::path& p, const std::string& ext, uint64_t size, GameEntry* e) {
    std::vector<uint8_t> h;
    if (ext == ".iso" || ext == ".gcm") {
        if (size < 0x1000) return false;
        if (!read_at(p, 0, kGcHeader, &h)) return false;
        return gc_from_header(h.data(), h.size(), p, e);
    }
    if (ext == ".rvz" || ext == ".wia") {
        /* wia_file_head_t is 0x48 bytes; wia_disc_t follows: type, compression,
         * compr_level, chunk_size (16 bytes), then dhead[0x80] = disc header start. */
        if (!read_at(p, 0, 0x58 + 0x80, &h)) return false;
        bool wia = std::memcmp(h.data(), "WIA\x01", 4) == 0;
        bool rvz = std::memcmp(h.data(), "RVZ\x01", 4) == 0;
        if (!wia && !rvz) return false;
        uint32_t disc_type = be32(h.data() + 0x48);
        if (disc_type != 1) return false;  // 1 = GameCube, 2 = Wii
        return gc_from_header(h.data() + 0x58, 0x80, p, e);
    }
    if (ext == ".ciso") {
        if (!read_at(p, 0, 0x8008, &h)) return false;
        if (std::memcmp(h.data(), "CISO", 4) != 0) return false;
        uint32_t block = le32(h.data() + 4);
        if (block < kGcHeader || h[8] != 1) return false;  // block 0 holds the disc header
        std::vector<uint8_t> d;
        if (!read_at(p, 0x8000, kGcHeader, &d)) return false;
        return gc_from_header(d.data(), d.size(), p, e);
    }
    if (ext == ".gcz") {
        if (!read_at(p, 0, 32, &h)) return false;
        if (le32(h.data()) != 0xB10BC001u) return false;
        uint64_t comp_size = le64(h.data() + 8);
        uint64_t data_size = le64(h.data() + 16);
        uint32_t block_size = le32(h.data() + 24);
        uint32_t num_blocks = le32(h.data() + 28);
        if (num_blocks == 0 || block_size == 0 || data_size < kGcHeader) return id_gc_filename_only(p, e);
        std::vector<uint8_t> ptr;
        if (!read_at(p, 32, num_blocks > 1 ? 16 : 8, &ptr)) return id_gc_filename_only(p, e);
        uint64_t p0 = le64(ptr.data());
        bool raw = (p0 >> 63) != 0;
        p0 &= ~(1ull << 63);
        uint64_t p1 = num_blocks > 1 ? (le64(ptr.data() + 8) & ~(1ull << 63)) : comp_size;
        if (p1 <= p0 || p1 - p0 > (1u << 20)) return id_gc_filename_only(p, e);
        uint64_t data_start = 32 + 12ull * num_blocks;
        std::vector<uint8_t> comp;
        if (!read_at(p, data_start + p0, (size_t)(p1 - p0), &comp)) return id_gc_filename_only(p, e);
        std::vector<uint8_t> disc(kGcHeader, 0);
        size_t got = 0;
        if (raw) {
            got = std::min(comp.size(), disc.size());
            std::memcpy(disc.data(), comp.data(), got);
        } else if (comp.size() > 2) {
            got = inflate_prefix(comp.data() + 2, comp.size() - 2, disc.data(), disc.size());  // skip zlib header
        }
        if (got >= 0x80 && gc_from_header(disc.data(), got, p, e)) return true;
        return id_gc_filename_only(p, e);
    }
    return false;
}

} // namespace

/* ------------------------------------------------------------------ */
/* system names                                                        */
/* ------------------------------------------------------------------ */

const char* system_name(System s) {
    switch (s) {
    case System::N64: return "Nintendo 64";
    case System::GBA: return "Game Boy Advance";
    case System::GB:  return "Game Boy";
    case System::GBC: return "Game Boy Color";
    case System::GC:  return "GameCube";
    default:          return "Unknown";
    }
}

const char* system_short(System s) {
    switch (s) {
    case System::N64: return "n64";
    case System::GBA: return "gba";
    case System::GB:  return "gb";
    case System::GBC: return "gbc";
    case System::GC:  return "gc";
    default:          return "";
    }
}

System system_from_short(const std::string& s) {
    std::string l = ascii_lower(s);
    if (l == "n64") return System::N64;
    if (l == "gba") return System::GBA;
    if (l == "gb") return System::GB;
    if (l == "gbc") return System::GBC;
    if (l == "gc") return System::GC;
    return System::Unknown;
}

uint64_t library_identify_count() { return g_identify_calls.load(); }

/* Zipped cartridge ROMs (N64/GBA/GB/GBC): the first ROM inside the zip is
 * extracted once to data_dir()/rom-cache/<zip name>-<entry crc>/ and that file
 * is what gets identified and launched. Disc images are never extracted. */
static bool extract_zipped_rom(const fs::path& zip, fs::path* out_rom) {
    mz_zip_archive za;
    std::memset(&za, 0, sizeof(za));
    std::string zpath = path_utf8(zip);
    FILE* f = _wfopen(zip.wstring().c_str(), L"rb");
    if (!f) return false;
    if (!mz_zip_reader_init_cfile(&za, f, 0, 0)) { std::fclose(f); return false; }
    bool ok = false;
    mz_uint n = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < n && !ok; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st) || st.m_is_directory) continue;
        std::string name = st.m_filename;
        std::string ext = ascii_lower(path_utf8(utf8_path(name).extension()));
        if (ext != ".z64" && ext != ".v64" && ext != ".n64" && ext != ".gba" && ext != ".gb" && ext != ".gbc") continue;
        if (st.m_uncomp_size < 16 * 1024 || st.m_uncomp_size > 128ull * 1024 * 1024) continue;
        char crc[16];
        std::snprintf(crc, sizeof crc, "%08x", (unsigned)st.m_crc32);
        fs::path dir = utf8_path(data_dir()) / L"rom-cache" /
                       (zip.stem().wstring() + L"-" + utf8_path(crc).wstring());
        fs::path rom = dir / utf8_path(name).filename();
        std::error_code ec;
        if (!fs::exists(rom, ec) || fs::file_size(rom, ec) != st.m_uncomp_size) {
            fs::create_directories(dir, ec);
            fs::path tmp = rom; tmp += L".part";
            if (!mz_zip_reader_extract_to_file(&za, i, path_utf8(tmp).c_str(), 0)) continue;
            fs::rename(tmp, rom, ec);
            if (ec) continue;
        }
        *out_rom = rom;
        ok = true;
    }
    mz_zip_reader_end(&za);
    std::fclose(f);
    return ok;
}

bool identify_game(const std::string& path, GameEntry* out) {
    if (!out) return false;
    std::error_code ec;
    fs::path p = fs::absolute(utf8_path(path), ec);
    if (ec) p = utf8_path(path);
    uintmax_t size = fs::file_size(p, ec);
    if (ec) return false;
    std::string ext = ascii_lower(path_utf8(p.extension()));
    if (ext == ".zip") {
        fs::path rom;
        if (!extract_zipped_rom(p, &rom)) return false;
        return identify_game(path_utf8(rom), out);
    }
    g_identify_calls.fetch_add(1);

    GameEntry e;
    bool ok = false;
    if (ext == ".z64" || ext == ".v64" || ext == ".n64") ok = id_n64(p, &e);
    else if (ext == ".gba") ok = id_gba(p, size, &e);
    else if (ext == ".gb" || ext == ".gbc") ok = id_gb(p, size, &e);
    else if (ext == ".iso" || ext == ".gcm" || ext == ".rvz" || ext == ".wia" || ext == ".ciso" || ext == ".gcz")
        ok = id_gc(p, ext, size, &e);
    if (!ok) return false;
    e.path = path_utf8(p);
    e.size = size;
    *out = std::move(e);
    return true;
}

/* ------------------------------------------------------------------ */
/* tiny JSON                                                           */
/* ------------------------------------------------------------------ */

namespace {

struct JV {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    long long i = 0;
    double d = 0;
    std::string s;
    std::vector<JV> a;
    std::vector<std::pair<std::string, JV>> o;

    const JV* get(const char* k) const {
        for (const auto& kv : o)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
    std::string str(const char* k) const {
        const JV* v = get(k);
        return (v && v->t == Str) ? v->s : std::string();
    }
    long long num(const char* k, long long def = 0) const {
        const JV* v = get(k);
        return (v && v->t == Num) ? v->i : def;
    }
    bool boolean(const char* k) const {
        const JV* v = get(k);
        if (!v) return false;
        if (v->t == Bool) return v->b;
        if (v->t == Num) return v->i != 0;
        return false;
    }
};

struct JParser {
    const char* p;
    const char* e;
    bool fail = false;

    void ws() {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    static void put_utf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) s.push_back((char)cp);
        else if (cp < 0x800) { s.push_back((char)(0xC0 | (cp >> 6))); s.push_back((char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) {
            s.push_back((char)(0xE0 | (cp >> 12)));
            s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            s.push_back((char)(0xF0 | (cp >> 18)));
            s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    bool hex4(uint32_t* v) {
        if (e - p < 4) return false;
        uint32_t r = 0;
        for (int k = 0; k < 4; ++k) {
            char c = *p++;
            r <<= 4;
            if (c >= '0' && c <= '9') r |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') r |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') r |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        *v = r;
        return true;
    }
    bool string(std::string* out) {
        if (p >= e || *p != '"') return false;
        ++p;
        while (p < e && *p != '"') {
            char c = *p++;
            if (c != '\\') { out->push_back(c); continue; }
            if (p >= e) return false;
            char x = *p++;
            switch (x) {
            case 'n': out->push_back('\n'); break;
            case 't': out->push_back('\t'); break;
            case 'r': out->push_back('\r'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'u': {
                uint32_t cp;
                if (!hex4(&cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    uint32_t lo;
                    if (!hex4(&lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                put_utf8(*out, cp);
                break;
            }
            default: out->push_back(x); break;  // " \ /
            }
        }
        if (p >= e) return false;
        ++p;
        return true;
    }
    bool value(JV* v, int depth) {
        if (depth > 32) return false;
        ws();
        if (p >= e) return false;
        char c = *p;
        if (c == '{') {
            ++p;
            v->t = JV::Obj;
            ws();
            if (p < e && *p == '}') { ++p; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!string(&k)) return false;
                ws();
                if (p >= e || *p++ != ':') return false;
                JV child;
                if (!value(&child, depth + 1)) return false;
                v->o.emplace_back(std::move(k), std::move(child));
                ws();
                if (p >= e) return false;
                if (*p == ',') { ++p; continue; }
                if (*p == '}') { ++p; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++p;
            v->t = JV::Arr;
            ws();
            if (p < e && *p == ']') { ++p; return true; }
            for (;;) {
                JV child;
                if (!value(&child, depth + 1)) return false;
                v->a.push_back(std::move(child));
                ws();
                if (p >= e) return false;
                if (*p == ',') { ++p; continue; }
                if (*p == ']') { ++p; return true; }
                return false;
            }
        }
        if (c == '"') { v->t = JV::Str; return string(&v->s); }
        if (e - p >= 4 && !std::strncmp(p, "true", 4)) { p += 4; v->t = JV::Bool; v->b = true; return true; }
        if (e - p >= 5 && !std::strncmp(p, "false", 5)) { p += 5; v->t = JV::Bool; v->b = false; return true; }
        if (e - p >= 4 && !std::strncmp(p, "null", 4)) { p += 4; v->t = JV::Null; return true; }
        const char* st = p;
        bool isf = false;
        while (p < e && (std::strchr("+-0123456789.eE", *p))) {
            if (*p == '.' || *p == 'e' || *p == 'E') isf = true;
            ++p;
        }
        if (p == st) return false;
        std::string tok(st, p);
        v->t = JV::Num;
        if (isf) { v->d = std::strtod(tok.c_str(), nullptr); v->i = (long long)v->d; }
        else { v->i = std::strtoll(tok.c_str(), nullptr, 10); v->d = (double)v->i; }
        return true;
    }
};

bool json_parse(const std::string& text, JV* out) {
    JParser ps{text.data(), text.data() + text.size()};
    if (!ps.value(out, 0)) return false;
    return true;
}

void jw_str(std::string& o, const std::string& s) {
    o.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
            else o.push_back((char)c);
        }
    }
    o.push_back('"');
}

bool read_text(const fs::path& p, std::string* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

/* Write via temp file + replace so a crash never leaves a truncated file. */
bool write_text_atomic(const fs::path& p, const std::string& text) {
    fs::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), (std::streamsize)text.size());
        if (!f) return false;
    }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* data dir override                                                   */
/* ------------------------------------------------------------------ */

std::mutex g_dir_mu;
std::string g_dir_override;

fs::path lib_dir() {
    std::string d;
    {
        std::lock_guard<std::mutex> lk(g_dir_mu);
        d = g_dir_override;
    }
    if (d.empty()) d = data_dir();
    std::error_code ec;
    fs::path p = utf8_path(d);
    fs::create_directories(p, ec);
    return p;
}

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

std::mutex g_state_mu;
bool g_state_loaded = false;
bool g_state_dirty = false;
std::map<std::string, GameState> g_state;

void state_load_locked() {
    if (g_state_loaded) return;
    g_state_loaded = true;
    g_state.clear();
    std::string text;
    if (!read_text(lib_dir() / L"library.json", &text)) return;
    JV root;
    if (!json_parse(text, &root) || root.t != JV::Obj) return;
    const JV* games = root.get("games");
    if (!games || games->t != JV::Obj) return;
    for (const auto& kv : games->o) {
        if (kv.second.t != JV::Obj) continue;
        GameState gs;
        gs.favorite = kv.second.boolean("favorite");
        gs.last_played = (uint64_t)std::max<long long>(0, kv.second.num("last_played"));
        gs.play_seconds = (uint64_t)std::max<long long>(0, kv.second.num("play_seconds"));
        g_state[kv.first] = gs;
    }
}

void state_flush_locked() {
    if (!g_state_dirty) return;
    std::string o = "{\"version\":1,\"games\":{";
    bool first = true;
    for (const auto& kv : g_state) {
        const GameState& g = kv.second;
        if (!g.favorite && g.last_played == 0 && g.play_seconds == 0) continue;
        if (!first) o.push_back(',');
        first = false;
        jw_str(o, kv.first);
        o += ":{\"favorite\":";
        o += g.favorite ? "true" : "false";
        o += ",\"last_played\":" + std::to_string(g.last_played);
        o += ",\"play_seconds\":" + std::to_string(g.play_seconds) + "}";
    }
    o += "}}\n";
    if (write_text_atomic(lib_dir() / L"library.json", o)) g_state_dirty = false;
}

/* ------------------------------------------------------------------ */
/* scan cache                                                          */
/* ------------------------------------------------------------------ */

struct CacheRec {
    uint64_t size = 0;
    long long mtime = 0;
    bool ok = false;
    GameEntry e;
};

constexpr int kCacheVersion = 5;  // 2: serial matching + GBA catalog_id; 5: GB/GBC serials; forces a rescan

std::unordered_map<std::string, CacheRec> cache_load() {
    std::unordered_map<std::string, CacheRec> m;
    std::string text;
    if (!read_text(lib_dir() / L"library_cache.json", &text)) return m;
    JV root;
    if (!json_parse(text, &root) || root.t != JV::Obj) return m;
    if (root.num("version") != kCacheVersion) return m;
    const JV* files = root.get("files");
    if (!files || files->t != JV::Arr) return m;
    for (const JV& f : files->a) {
        if (f.t != JV::Obj) continue;
        CacheRec r;
        std::string path = f.str("p");
        if (path.empty()) continue;
        r.size = (uint64_t)f.num("s");
        r.mtime = f.num("m");
        r.ok = f.boolean("ok");
        if (r.ok) {
            r.e.key = f.str("k");
            r.e.system = system_from_short(f.str("sys"));
            r.e.title = f.str("t");
            r.e.serial = f.str("sn");
            r.e.catalog_id = f.str("c");
            r.e.path = f.str("lp").empty() ? path : f.str("lp");
            {   /* catalog can change between versions: re-match instead of trusting the cache */
                std::string md5 = (r.e.key.rfind("n64:", 0) == 0) ? r.e.key.substr(4) : std::string();
                if (const GameInfo* g = find_game_for(f.str("sys"), r.e.serial, md5)) {
                    r.e.catalog_id = g->id;
                    if (r.e.system == System::N64) r.e.title = g->title;
                }
            }
            r.e.size = r.size;
            if (r.e.key.empty() || r.e.system == System::Unknown) continue;
        }
        m[ascii_lower(path)] = std::move(r);
    }
    return m;
}

void cache_save(const std::vector<std::pair<std::string, const CacheRec*>>& recs) {
    std::string o = "{\"version\":" + std::to_string(kCacheVersion) + ",\"files\":[";
    bool first = true;
    for (const auto& pr : recs) {
        const CacheRec& r = *pr.second;
        if (!first) o.push_back(',');
        first = false;
        o += "{\"p\":";
        jw_str(o, pr.first);
        o += ",\"s\":" + std::to_string(r.size) + ",\"m\":" + std::to_string(r.mtime);
        o += r.ok ? ",\"ok\":true" : ",\"ok\":false";
        if (r.ok) {
            o += ",\"k\":"; jw_str(o, r.e.key);
            o += ",\"sys\":"; jw_str(o, system_short(r.e.system));
            o += ",\"t\":"; jw_str(o, r.e.title);
            o += ",\"sn\":"; jw_str(o, r.e.serial);
            o += ",\"c\":"; jw_str(o, r.e.catalog_id);
            o += ",\"lp\":"; jw_str(o, r.e.path);   // launch path (differs for zipped ROMs)
        }
        o.push_back('}');
    }
    o += "]}\n";
    write_text_atomic(lib_dir() / L"library_cache.json", o);
}

bool scan_extension(const std::string& ext) {
    static const char* const kExt[] = {".z64", ".v64", ".n64", ".gba", ".gb", ".gbc", ".iso",
                                       ".gcm", ".rvz", ".wia", ".ciso", ".gcz", ".zip"};
    for (const char* e : kExt)
        if (ext == e) return true;
    return false;
}

struct Candidate {
    fs::path path;
    std::string path_utf8;
    uint64_t size = 0;
    long long mtime = 0;
    bool done = false;
    CacheRec rec;
};

/* Directory walk that skips unreadable dirs and never follows symlinks/junctions. */
void walk(const fs::path& root, std::vector<Candidate>& out, std::set<std::string>& seen) {
    std::vector<fs::path> stack{root};
    while (!stack.empty()) {
        fs::path dir = std::move(stack.back());
        stack.pop_back();
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) continue;
        std::vector<fs::path> subdirs;
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const fs::directory_entry& de = *it;
            std::error_code e1;
            if (de.is_symlink(e1)) continue;
            if (de.is_directory(e1)) { subdirs.push_back(de.path()); continue; }
            if (!de.is_regular_file(e1) || e1) continue;
            std::string ext = ascii_lower(path_utf8(de.path().extension()));
            if (!scan_extension(ext)) continue;
            Candidate c;
            c.path = de.path();
            c.path_utf8 = path_utf8(c.path);
            if (!seen.insert(ascii_lower(c.path_utf8)).second) continue;
            std::error_code e2, e3;
            c.size = de.file_size(e2);
            c.mtime = (long long)de.last_write_time(e3).time_since_epoch().count();
            if (e2) continue;
            out.push_back(std::move(c));
        }
        /* depth-first in sorted order for deterministic results */
        std::sort(subdirs.begin(), subdirs.end(), std::greater<fs::path>());
        for (auto& s : subdirs) stack.push_back(std::move(s));
    }
}

} // namespace

std::vector<GameEntry> scan_library(const std::vector<std::string>& folders,
                                    const std::function<void(int, int)>& progress) {
    std::vector<Candidate> cands;
    std::set<std::string> seen;
    std::vector<std::string> prefixes;
    for (const std::string& f : folders) {
        std::error_code ec;
        fs::path root = fs::absolute(utf8_path(f), ec);
        if (ec || !fs::is_directory(root, ec)) continue;
        std::string pre = ascii_lower(path_utf8(root));
        if (pre.empty() || (pre.back() != '\\' && pre.back() != '/')) pre.push_back('\\');
        prefixes.push_back(pre);
        walk(root, cands, seen);
    }

    std::unordered_map<std::string, CacheRec> cache = cache_load();

    /* Reuse unchanged cache entries; collect the rest as work. */
    std::vector<size_t> work;
    for (size_t i = 0; i < cands.size(); ++i) {
        Candidate& c = cands[i];
        auto it = cache.find(ascii_lower(c.path_utf8));
        if (it != cache.end() && it->second.size == c.size && it->second.mtime == c.mtime) {
            c.rec = it->second;
            if (c.rec.e.path.empty()) c.rec.e.path = c.path_utf8;   // zipped ROMs keep their extracted path
            c.done = true;
        } else {
            work.push_back(i);
        }
    }

    const int total = (int)cands.size();
    std::atomic<int> done{total - (int)work.size()};
    std::atomic<size_t> next{0};
    if (progress) progress(done.load(), total);

    auto worker = [&] {
        for (;;) {
            size_t k = next.fetch_add(1);
            if (k >= work.size()) return;
            Candidate& c = cands[work[k]];
            GameEntry e;
            bool ok = false;
            try { ok = identify_game(c.path_utf8, &e); } catch (...) { ok = false; }
            c.rec.size = c.size;
            c.rec.mtime = c.mtime;
            c.rec.ok = ok;
            if (ok) { if (e.path.empty()) e.path = c.path_utf8; e.size = c.size; c.rec.e = std::move(e); }
            c.done = true;
            done.fetch_add(1);
        }
    };

    if (!work.empty()) {
        unsigned hw = std::thread::hardware_concurrency();
        size_t nthreads = std::max<size_t>(1, std::min<size_t>(hw ? hw : 2, work.size()));
        std::vector<std::thread> pool;
        for (size_t i = 0; i < nthreads; ++i) pool.emplace_back(worker);
        int last = -1;
        while (done.load() < total) {
            int d = done.load();
            if (d != last) { last = d; if (progress) progress(d, total); }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        for (auto& t : pool) t.join();
        if (progress) progress(total, total);
    }

    /* Persist: new records, plus old ones outside the scanned folders. */
    {
        std::vector<std::pair<std::string, const CacheRec*>> recs;
        std::set<std::string> have;
        for (const Candidate& c : cands) {
            recs.emplace_back(c.path_utf8, &c.rec);
            have.insert(ascii_lower(c.path_utf8));
        }
        std::vector<std::string> old_keys;  // keep storage stable for pointers
        for (const auto& kv : cache) {
            if (have.count(kv.first)) continue;
            bool under = false;
            for (const auto& pre : prefixes)
                if (kv.first.compare(0, pre.size(), pre) == 0) { under = true; break; }
            if (under) continue;  // file vanished from a scanned folder
            recs.emplace_back(kv.second.ok ? kv.second.e.path : kv.first, &kv.second);
        }
        cache_save(recs);
    }

    /* Dedupe by key (first wins), then sort. */
    std::vector<GameEntry> out;
    std::set<std::string> keys;
    for (const Candidate& c : cands) {
        if (!c.rec.ok) continue;
        if (!keys.insert(c.rec.e.key).second) continue;
        out.push_back(c.rec.e);
    }
    std::stable_sort(out.begin(), out.end(), [](const GameEntry& a, const GameEntry& b) {
        if (a.system != b.system) return (int)a.system < (int)b.system;
        std::string ta = ascii_lower(a.title), tb = ascii_lower(b.title);
        if (ta != tb) return ta < tb;
        return a.path < b.path;
    });
    return out;
}

/* ------------------------------------------------------------------ */
/* state API                                                           */
/* ------------------------------------------------------------------ */

GameState game_state(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_state_mu);
    state_load_locked();
    auto it = g_state.find(key);
    return it == g_state.end() ? GameState() : it->second;
}

void set_favorite(const std::string& key, bool fav) {
    std::lock_guard<std::mutex> lk(g_state_mu);
    state_load_locked();
    GameState& g = g_state[key];
    if (g.favorite == fav) return;
    g.favorite = fav;
    g_state_dirty = true;
    state_flush_locked();
}

void record_play(const std::string& key, uint64_t started_unix, uint64_t seconds) {
    std::lock_guard<std::mutex> lk(g_state_mu);
    state_load_locked();
    GameState& g = g_state[key];
    g.last_played = std::max(g.last_played, started_unix);
    g.play_seconds += seconds;
    g_state_dirty = true;
    state_flush_locked();
}

void flush_library_state() {
    std::lock_guard<std::mutex> lk(g_state_mu);
    state_flush_locked();
}

void library_set_data_dir(const std::string& dir) {
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        if (g_state_loaded) state_flush_locked();
        g_state_loaded = false;
        g_state_dirty = false;
        g_state.clear();
    }
    std::lock_guard<std::mutex> lk(g_dir_mu);
    g_dir_override = dir;
}

} // namespace hub
