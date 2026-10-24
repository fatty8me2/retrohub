#include "winutil.h"
#include "json.h"
#include "miniz.h"
#include "library.h"
#include "emu_formats.h"
#include "games.h"
#include "ps2.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
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

std::wstring to_wide(const std::string& s) { return winutil::to_wide(s); }

std::string to_utf8(const std::wstring& s) { return winutil::to_utf8(s); }

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

bool id_n64(const fs::path& p, const fs::path& name, GameEntry* e) {
    std::vector<uint8_t> img;
    if (!load_rom(path_utf8(p), &img) || img.size() < 0x40) return false;
    std::string md5 = md5_hex(img.data(), img.size());
    if (md5.empty()) return false;
    e->system = System::N64;
    e->key = "n64:" + ascii_lower(md5);
    e->title = pretty_title(header_text(img.data() + 0x20, 0x14), name);
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

bool id_gba(const fs::path& p, const fs::path& name, uint64_t size, GameEntry* e) {
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
    e->title = pretty_title(header_text(&d[0xA0], 12), name);
    if (const GameInfo* g = find_game_for("gba", code, std::string())) {
        e->catalog_id = g->id;
        e->title = g->title;   // key above is unchanged
    }
    return true;
}

/* Nintendo DS: 0x200-byte header. Title 0x00 (12), game code 0x0C (4), ROM version 0x1E, header CRC16 0x15E
 * (CRC-16/MODBUS of bytes 0..0x15D). The key avoids hashing the (up to 512 MB) image: code + version + header CRC
 * identify one release identically on every PC, trimmed or padded dump alike. */
uint16_t nds_crc16(const uint8_t* p, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

bool id_nds(const fs::path& p, const fs::path& name, uint64_t size, GameEntry* e) {
    if (size < 0x4000) return false;
    std::vector<uint8_t> h;
    if (!read_at(p, 0, 0x200, &h) || h.size() < 0x200) return false;
    /* Nintendo logo CRC16 (0x15C) is constant 0xCF56 on every licensed cartridge/dump. */
    uint16_t logo = (uint16_t)(h[0x15C] | (h[0x15D] << 8));
    if (logo != 0xCF56) return false;
    std::string code((const char*)&h[0x0C], 4);
    for (char c : code)
        if (!is_alnum(c)) return false;
    unsigned want = (unsigned)(h[0x15E] | (h[0x15F] << 8));
    unsigned have = nds_crc16(h.data(), 0x15E);
    if (want != have) {   // tolerate patched headers only if the unit code/size fields are sane
        if (h[0x12] > 3) return false;
    }
    char tail[16];
    std::snprintf(tail, sizeof tail, "%02x%04x", (unsigned)h[0x1E], want);
    e->system = System::NDS;
    e->serial = code;
    e->key = "nds:" + sanitize_key(code) + "-" + tail;
    e->title = pretty_title(header_text(h.data(), 12), name);
    if (const GameInfo* g = find_game_for("nds", code, std::string())) {
        e->catalog_id = g->id;
        e->title = g->title;
    }
    return true;
}

bool id_gb(const fs::path& p, const fs::path& name, uint64_t size, GameEntry* e) {
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
    e->title = pretty_title(t, name);
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
    std::string title_serial = t;
    while (!title_serial.empty() && title_serial.back() == ' ') title_serial.pop_back();
    for (auto& c : title_serial)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    const char* sys_short = is_cgb ? "gbc" : "gb";
    e->serial = code.size() == 4 ? code : title_serial;
    const GameInfo* g = find_game_for(sys_short, e->serial, std::string());
    if (!g && code.size() == 4) {
        /* Code is valid but not in the catalog (or is junk): fall back to the header title. */
        g = find_game_for(sys_short, title_serial, std::string());
    }
    if (!g) {   /* header titles use '_' or ' ' interchangeably ("POKEMON_GLD" vs "POKEMON GLD") */
        std::string alt = title_serial;
        for (auto& c : alt) if (c == '_') c = ' ';
        g = find_game_for(sys_short, alt, std::string());
        if (!g) { for (auto& c : alt) if (c == ' ') c = '_'; g = find_game_for(sys_short, alt, std::string()); }
    }
    if (g) {   /* catalog title wins over the raw header name (the save key above is unchanged) */
        e->catalog_id = g->id;
        e->title = g->title;
    }
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
    if (const GameInfo* gi = find_game_for("gc", id, std::string())) { e->catalog_id = gi->id; }   // loose discs are catalog games too (online needs it)
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

/* ---- Nintendo 3DS (Azahar) / Wii U (Cemu) ---- */

/* Up to n bytes from the start of a file (short files are fine). */
bool read_prefix(const fs::path& p, size_t n, std::vector<uint8_t>* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign(n, 0);
    f.read(reinterpret_cast<char*>(out->data()), (std::streamsize)n);
    out->resize((size_t)f.gcount());
    return !out->empty();
}

void fill_3ds(const N3dsInfo& i, const fs::path& name_src, GameEntry* e) {
    e->system = System::N3DS;
    e->serial = i.serial;
    std::string k = n3ds_key(i);
    e->title = title_from_filename(name_src);
    e->key = k.empty() ? "3ds:file-" + sanitize_key(e->title) : k;
    if (const GameInfo* g = find_game_for("3ds", e->serial, std::string())) { e->catalog_id = g->id; e->title = g->title; }
    if ((i.container == "ncsd" || i.container == "ncch") && !i.decrypted) e->issue = "encrypted";   // Azahar needs AES keys
}

bool id_3ds(const fs::path& p, const std::string& ext, GameEntry* e) {
    std::vector<uint8_t> h;
    if (ext == ".zcci" || ext == ".zcia" || ext == ".zcxi" || ext == ".z3dsx") {   // Azahar's own compressed formats: no cheap header
        if (!read_prefix(p, 4, &h) || h.size() < 4 || std::memcmp(h.data(), "Z3DS", 4) != 0) return false;
        fill_3ds(N3dsInfo{}, p, e);
        return true;
    }
    if (!read_prefix(p, kN3dsHeaderBytes, &h)) return false;
    N3dsInfo i;
    if (!parse_3ds_header(h.data(), h.size(), &i)) return false;
    if (i.container != "3dsx" && !i.is_application) return false;   // updates / DLC / system titles are not games
    fill_3ds(i, p, e);
    return true;
}

/* A zip holding one 3DS image: only the first 17 KB of the entry is inflated. Azahar cannot open zips, so the
 * entry is flagged "extract" and keeps the zip as its path. Nothing is extracted. */
struct ZipPrefixSink { std::vector<uint8_t> buf; };
size_t zip_prefix_cb(void* user, mz_uint64 /*ofs*/, const void* data, size_t len) {
    ZipPrefixSink* s = static_cast<ZipPrefixSink*>(user);
    size_t room = kN3dsHeaderBytes > s->buf.size() ? kN3dsHeaderBytes - s->buf.size() : 0;
    size_t take = len < room ? len : room;
    s->buf.insert(s->buf.end(), (const uint8_t*)data, (const uint8_t*)data + take);
    return take < len ? 0 : len;   // a short return aborts the inflate once we have enough
}

bool id_3ds_zip(const fs::path& zip, GameEntry* e) {
    mz_zip_archive za;
    std::memset(&za, 0, sizeof(za));
    FILE* f = _wfopen(zip.wstring().c_str(), L"rbN");
    if (!f) return false;
    if (!mz_zip_reader_init_cfile(&za, f, 0, 0)) { std::fclose(f); return false; }
    bool ok = false;
    mz_uint n = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < n && !ok; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st) || st.m_is_directory) continue;
        std::string ext = ascii_lower(path_utf8(utf8_path(st.m_filename).extension()));
        if (!is_3ds_zip_entry(ext) || st.m_uncomp_size < kN3dsHeaderBytes) continue;
        ZipPrefixSink sink;
        mz_zip_reader_extract_to_callback(&za, i, zip_prefix_cb, &sink, 0);   // reports failure by design after the prefix
        N3dsInfo info;
        if (!parse_3ds_header(sink.buf.data(), sink.buf.size(), &info)) continue;
        if (info.container != "3dsx" && !info.is_application) continue;
        fill_3ds(info, zip, e);
        e->issue = "extract";
        ok = true;
    }
    mz_zip_reader_end(&za);
    std::fclose(f);
    return ok;
}

bool id_wiiu(const fs::path& p, const std::string& ext, uint64_t size, GameEntry* e) {
    WiiuInfo w;
    auto reader = [&](uint64_t off, void* buf, size_t n) -> bool {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        f.seekg((std::streamoff)off, std::ios::beg);
        if (!f) return false;
        f.read(static_cast<char*>(buf), (std::streamsize)n);
        return f.gcount() == (std::streamsize)n;
    };
    bool ok = false;
    std::string name_stem;   // the game's display name when the header has none
    fs::path name_src = p;
    if (ext == ".wua") {
        ok = parse_wua(reader, size, &w);
        if (!ok) { w = WiiuInfo{}; w.container = "wua"; ok = true; }   // unknown layout: still launchable, keyed by file name
    } else if (ext == ".wud") {
        std::vector<uint8_t> h;
        ok = read_prefix(p, 16, &h) && parse_wud_header(h.data(), h.size(), &w);
    } else if (ext == ".wux") {
        ok = parse_wux(reader, size, &w);
    } else if (ext == ".rpx") {
        /* loadiine folder: <game>/code/<x>.rpx + <game>/meta/meta.xml; only base games (title id 00050000-...) */
        fs::path game_dir = p.parent_path().parent_path();
        std::ifstream f(game_dir / L"meta" / L"meta.xml", std::ios::binary);
        if (!f) return false;
        std::string xml((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        uint64_t tid = 0;
        if (!parse_meta_xml_title_id(xml, &tid) || (tid >> 32) != 0x00050000u) return false;
        w.container = "rpx";
        w.title_id = tid;
        w.serial = hex16(tid);
        for (char& c : w.serial) if (c >= 'a' && c <= 'f') c = char(c - 32);
        name_src = fs::path(game_dir.filename().wstring() + L".rpx");   // the folder name is the game name
        ok = true;
    }
    if (!ok) return false;
    e->system = System::WiiU;
    e->serial = w.serial;
    e->title = title_from_filename(name_src);
    std::string k = wiiu_key(w);
    e->key = k.empty() ? "wiiu:file-" + sanitize_key(e->title) : k;
    if (const GameInfo* g = find_game_for("wiiu", e->serial, std::string())) { e->catalog_id = g->id; e->title = g->title; }
    if (w.container == "wud" || w.container == "wux") e->issue = "keys";   // needs Cemu's keys.txt (disc key)
    return true;
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
    case System::N3DS: return "Nintendo 3DS";
    case System::WiiU: return "Wii U";
    case System::NDS: return "Nintendo DS";
    case System::PS2: return "PlayStation 2";
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
    case System::N3DS: return "3ds";
    case System::WiiU: return "wiiu";
    case System::NDS: return "nds";
    case System::PS2: return "ps2";
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
    if (l == "3ds") return System::N3DS;
    if (l == "wiiu") return System::WiiU;
    if (l == "nds") return System::NDS;
    if (l == "ps2") return System::PS2;
    return System::Unknown;
}

uint64_t library_identify_count() { return g_identify_calls.load(); }

namespace {
fs::path lib_dir();   // defined below, in this file's unnamed namespace

/* The \\?\ form of an absolute path, used only when the plain one is near MAX_PATH (short paths stay as they are). */
fs::path long_path(const fs::path& p) {
    std::wstring w = p.wstring();
    if (w.size() < 200 || w.compare(0, 4, L"\\\\?\\") == 0) return p;
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    return ec ? p : fs::path(L"\\\\?\\" + abs.wstring());
}
}

/* Zipped cartridge ROMs (N64/GBA/GB/GBC): the first ROM inside the zip is
 * extracted once to data_dir()/rom-cache/<zip name>-<entry crc>/ and that file
 * is what gets identified and launched. Disc images are never extracted. */
static bool extract_zipped_rom(const fs::path& zip, fs::path* out_rom, fs::path* inner_name) {
    mz_zip_archive za;
    std::memset(&za, 0, sizeof(za));
    std::string zpath = path_utf8(zip);
    FILE* f = _wfopen(zip.wstring().c_str(), L"rbN");
    if (!f) return false;
    if (!mz_zip_reader_init_cfile(&za, f, 0, 0)) { std::fclose(f); return false; }
    bool ok = false;
    mz_uint n = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < n && !ok; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st) || st.m_is_directory) continue;
        std::string name = st.m_filename;
        std::string ext = ascii_lower(path_utf8(utf8_path(name).extension()));
        if (ext != ".z64" && ext != ".v64" && ext != ".n64" && ext != ".gba" && ext != ".gb" && ext != ".gbc" && ext != ".nds") continue;
        /* DS cartridges go up to 512 MB (Black/White 2); the other cartridge systems stay under 128 MB. */
        if (st.m_uncomp_size < 16 * 1024 || st.m_uncomp_size > (ext == ".nds" ? 1024ull : 128ull) * 1024 * 1024) continue;
        char crc[16];
        std::snprintf(crc, sizeof crc, "%08x", (unsigned)st.m_crc32);
        /* Short names on purpose: "<zip stem>-<crc>\<inner name>" under a deep data dir passes MAX_PATH and extraction
         * fails. The save name is not taken from this file name: identify_game passes the inner name on (save_stem). */
        fs::path dir = lib_dir() / L"rom-cache" / utf8_path(crc);
        fs::path wdir = long_path(dir);   // \\?\ form when the data dir is deep (MAX_PATH)
        fs::path rom = wdir / (L"rom" + utf8_path(ext).wstring());
        std::error_code ec;
        if (!fs::exists(rom, ec) || fs::file_size(rom, ec) != st.m_uncomp_size) {
            fs::create_directories(wdir, ec);
            fs::path tmp = rom; tmp += L".part";
            bool done = false;
            if (FILE* out = _wfopen(tmp.c_str(), L"wbN")) {   // wide path: works past MAX_PATH
                done = mz_zip_reader_extract_to_cfile(&za, i, out, 0) != 0;
                done = (std::fclose(out) == 0) && done;
            }
            if (done) { fs::rename(tmp, rom, ec); done = !ec; }
            if (!done) {   // never leave a half-written .part or an empty folder behind
                std::error_code rec;
                fs::remove(tmp, rec);
                fs::remove(wdir, rec);   // only succeeds when the folder is empty
                continue;
            }
        }
        *out_rom = rom;
        *inner_name = utf8_path(name);
        ok = true;
        /* Drop stale extractions of the same zip (<stem>-<crc8>) left by an older version of the file. */
        {
            std::error_code pec;
            const std::wstring stem = zip.stem().wstring() + L"-";   // legacy folders: "<zip stem>-<crc>"
            const std::wstring keep = wdir.filename().wstring();
            for (fs::directory_iterator it(wdir.parent_path(), pec), end; !pec && it != end; it.increment(pec)) {
                std::wstring n = it->path().filename().wstring();
                if (n == keep || n.size() != stem.size() + 8 || n.compare(0, stem.size(), stem) != 0) continue;
                bool hex = true;
                for (size_t k = stem.size(); k < n.size(); ++k)
                    if (!iswxdigit(n[k])) hex = false;
                if (hex && it->is_directory(pec)) { std::error_code rec; fs::remove_all(it->path(), rec); }
            }
        }
    }
    mz_zip_reader_end(&za);
    std::fclose(f);
    return ok;
}

/* ------------------------------------------------------------------ */
/* GameCube disc images inside .7z / .zip archives                     */
/*                                                                     */
/* Dolphin cannot open archives. The library lists them (issue =       */
/* "extract") and Play unpacks the one disc image into                 */
/* rom-cache\gc\<key>\disc.<ext>. All reading goes through Windows'    */
/* own bsdtar (%SystemRoot%\System32\tar.exe, Windows 10 1803+), which */
/* handles 7z and zip; nothing is bundled. Entries are streamed with   */
/* "tar -xOf" straight into a file we name ourselves, so an archive    */
/* entry name can never choose where data is written.                  */
/* ------------------------------------------------------------------ */

bool archive_entry_safe(const std::string& n) {
    if (n.empty() || n.size() > 260) return false;
    if (n[0] == '/' || n[0] == '\\') return false;
    if (n.size() >= 2 && n[1] == ':') return false;
    for (unsigned char c : n) if (c < 0x20 || c == '"') return false;
    size_t i = 0;
    while (i <= n.size()) {
        size_t j = n.find_first_of("/\\", i);
        if (j == std::string::npos) j = n.size();
        if (n.compare(i, j - i, "..") == 0) return false;
        i = j + 1;
    }
    return true;
}

bool is_7z_file(const std::string& path) {
    std::vector<uint8_t> h;
    if (!read_at(utf8_path(path), 0, 6, &h) || h.size() < 6) return false;
    static const uint8_t magic[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
    return std::memcmp(h.data(), magic, 6) == 0;
}

std::vector<ArchiveEntry> parse_tar_listing(const std::string& text) {
    std::vector<ArchiveEntry> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        if (line.empty() || line[0] != '-') continue;   // regular files only
        /* "-rw-r--r--  0 0      0  1088455232 Oct 20  2022 name": 8 whitespace separated fields, then the name */
        size_t p = 0;
        uint64_t size = 0;
        bool ok = true;
        for (int f = 0; f < 8 && ok; ++f) {
            while (p < line.size() && line[p] == ' ') ++p;
            size_t s = p;
            while (p < line.size() && line[p] != ' ') ++p;
            if (p == s) ok = false;
            if (f == 4 && ok) {
                char* end = nullptr;
                std::string tok = line.substr(s, p - s);
                size = std::strtoull(tok.c_str(), &end, 10);
                if (!end || *end) ok = false;
            }
        }
        if (!ok || p >= line.size()) continue;
        ++p;   // exactly one separator, the name may itself start with a space
        out.push_back({line.substr(p), size});
    }
    return out;
}

namespace {

bool is_gc_image_ext(const std::string& ext) {
    return ext == ".iso" || ext == ".gcm" || ext == ".rvz" || ext == ".ciso" || ext == ".gcz" || ext == ".wia";
}

fs::path tar_exe() {
    wchar_t buf[MAX_PATH];
    UINT n = GetSystemDirectoryW(buf, MAX_PATH);
    return n ? fs::path(buf) / L"tar.exe" : fs::path(L"tar.exe");
}

/* Runs tar.exe with stdout = `out` (a pipe end or a file), stderr/stdin = NUL. Returns the process handle or null. */
HANDLE tar_spawn(const std::wstring& args, HANDLE out) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) return nullptr;
    /* Inherit ONLY the three std handles. A plain bInheritHandles=TRUE would also hand tar every file another scan
     * thread has open at that moment (e.g. a half-extracted ROM's .part), which then can't be renamed or deleted. */
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = nul;
    HANDLE inherit[2] = {nul, out};
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &asz);
    std::vector<char> abuf(asz);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)abuf.data();
    bool have_list = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &asz) &&
                     UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);
    if (!have_list) { CloseHandle(nul); return nullptr; }
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + tar_exe().wstring() + L"\" " + args;
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(nul);
    if (!ok) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

/* Captures up to max_bytes of tar's stdout (kills tar once it has enough, or after timeout_ms). */
bool tar_capture(const std::wstring& args, size_t max_bytes, unsigned timeout_ms, std::string* out, bool* complete = nullptr) {
    out->clear();
    if (complete) *complete = false;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE proc = tar_spawn(args, wr);
    CloseHandle(wr);
    if (!proc) { CloseHandle(rd); return false; }
    std::atomic<bool> finished{false};
    std::thread dog([&] {
        for (unsigned t = 0; t < timeout_ms && !finished.load(); t += 50) Sleep(50);
        if (!finished.load()) TerminateProcess(proc, 1);
    });
    char buf[1 << 15];
    bool eof = false;
    while (out->size() < max_bytes) {
        DWORD got = 0;
        if (!ReadFile(rd, buf, (DWORD)std::min<size_t>(sizeof buf, max_bytes - out->size()), &got, nullptr) || got == 0) { eof = true; break; }
        out->append(buf, got);
    }
    if (!eof) TerminateProcess(proc, 1);   // had enough
    DWORD code = 1;
    WaitForSingleObject(proc, 5000);
    GetExitCodeProcess(proc, &code);
    finished = true;
    dog.join();
    CloseHandle(rd);
    CloseHandle(proc);
    if (complete) *complete = eof && code == 0;
    return eof ? code == 0 : !out->empty();
}

/* Extraction pattern for one entry name: glob characters escaped, non-ASCII runs become '*'. */
std::wstring tar_member_arg(const std::string& name) {
    std::wstring w = L"\"";
    bool star = false;
    for (unsigned char c : name) {
        if (c >= 0x80) { if (!star) w += L'*'; star = true; continue; }
        star = false;
        if (c == '*' || c == '?' || c == '[' || c == ']' || c == '\\') w += L'\\';
        w += (wchar_t)c;
    }
    return w + L"\"";
}

std::wstring tar_archive_arg(const std::string& archive) { return L"\"" + utf8_path(archive).wstring() + L"\""; }

/* Files inside an archive: zip through miniz (central directory), 7z through tar. */
bool list_archive(const std::string& archive, std::vector<ArchiveEntry>* out) {
    out->clear();
    std::string ext = ascii_lower(path_utf8(utf8_path(archive).extension()));
    if (ext == ".zip") {
        mz_zip_archive za;
        std::memset(&za, 0, sizeof za);
        FILE* f = _wfopen(utf8_path(archive).wstring().c_str(), L"rbN");
        if (!f) return false;
        if (mz_zip_reader_init_cfile(&za, f, 0, 0)) {
            mz_uint n = mz_zip_reader_get_num_files(&za);
            for (mz_uint i = 0; i < n; ++i) {
                mz_zip_archive_file_stat st;
                if (!mz_zip_reader_file_stat(&za, i, &st) || st.m_is_directory) continue;
                out->push_back({st.m_filename, st.m_uncomp_size});
            }
            mz_zip_reader_end(&za);
            std::fclose(f);
            return true;
        }
        std::fclose(f);   // miniz cannot read it (zip64, odd method): let bsdtar try
    }
    std::string text;
    if (!tar_capture(L"-tvf " + tar_archive_arg(archive), 16u << 20, 120000, &text)) return false;
    *out = parse_tar_listing(text);
    return true;
}

/* The disc image inside: first entry with a GameCube image extension and a safe name. */
bool glob_star(const std::string& pat, size_t pi, const std::string& s, size_t si) {   // '*' only, everything else literal
    while (pi < pat.size()) {
        if (pat[pi] == '*') {
            for (size_t k = si; k <= s.size(); ++k) if (glob_star(pat, pi + 1, s, k)) return true;
            return false;
        }
        if (si >= s.size() || pat[pi] != s[si]) return false;
        ++pi; ++si;
    }
    return si == s.size();
}

/* What tar_member_arg() asks tar for: the name with every non-ASCII run turned into '*'. */
std::string member_skeleton(const std::string& name) {
    std::string r;
    bool star = false;
    for (unsigned char c : name) {
        if (c >= 0x80) { if (!star) r += '*'; star = true; } else { r += (char)c; star = false; }
    }
    return r;
}

/* The member must be unambiguous: exactly one entry in the archive matches the pattern we hand to tar. */
bool is_ps2_image_ext(const std::string& ext) { return ext == ".iso" || ext == ".chd" || ext == ".cso"; }

bool find_gc_entry(const std::vector<ArchiveEntry>& l, ArchiveEntry* out, bool ps2 = false) {
    for (const auto& a : l) {
        if (!archive_entry_safe(a.name) || a.size < 0x1000) continue;
        { std::string xe = ascii_lower(path_utf8(utf8_path(a.name).extension())); if (!(ps2 ? is_ps2_image_ext(xe) : is_gc_image_ext(xe))) continue; }
        const std::string pat = member_skeleton(a.name);
        int matches = 0;
        for (const auto& b : l) if (glob_star(pat, 0, b.name, 0)) ++matches;
        if (matches != 1) return false;
        *out = a;
        return true;
    }
    return false;
}

fs::path gc_unpack_dir(const GameEntry& e) { return lib_dir() / L"rom-cache" / (e.system == System::PS2 ? L"ps2" : L"gc") / utf8_path(sanitize_key(e.key)); }

} // namespace

/* Catalog serial for a No-Intro style file name ("Mario Kart - Double Dash!! (USA)"), "" if unknown. */
std::string gc_serial_for_name(const std::string& name) {
    std::string stem = name;
    size_t slash = stem.find_last_of("/\\");
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    for (const char* x : {".nkit.iso", ".nkit.gcz", ".iso", ".gcm", ".rvz", ".ciso", ".gcz", ".wia", ".7z", ".zip"}) {
        size_t n = std::strlen(x);
        if (stem.size() > n && ascii_lower(stem.substr(stem.size() - n)) == x) { stem.resize(stem.size() - n); break; }
    }
    const std::string want = ascii_lower(stem);
    for (const auto& g : catalog()) {
        if (g.system != "gc" || g.serials.empty()) continue;
        if (ascii_lower(g.nointro) == want) return g.serials[0];
    }
    return "";
}

static bool id_gc_archive(const fs::path& arch, GameEntry* e) {
    std::string ext = ascii_lower(path_utf8(arch.extension()));
    if (ext == ".7z" && !is_7z_file(path_utf8(arch))) return false;
    std::vector<ArchiveEntry> l;
    ArchiveEntry ent;
    if (!list_archive(path_utf8(arch), &l) || !find_gc_entry(l, &ent)) return false;
    const std::string inner_ext = ascii_lower(path_utf8(utf8_path(ent.name).extension()));
    /* header: stream the first bytes of the entry into a temp file and reuse the normal identification */
    GameEntry g;
    bool ok = false;
    std::string head;
    if (tar_capture(L"-xOf " + tar_archive_arg(path_utf8(arch)) + L" " + tar_member_arg(ent.name), 0x9000, 60000, &head) && head.size() >= 0x440) {
        static std::atomic<unsigned> seq{0};
        fs::path probe_dir = lib_dir() / L"rom-cache" / L"_probe";
        std::error_code ec;
        fs::create_directories(probe_dir, ec);
        fs::path probe = probe_dir / (L"p" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(seq++) + utf8_path(inner_ext).wstring());
        {
            std::ofstream o(probe, std::ios::binary);
            o.write(head.data(), (std::streamsize)head.size());
        }
        ok = id_gc(probe, inner_ext, ent.size, &g) && !g.serial.empty();
        fs::remove(probe, ec);
    }
    if (!ok) {   /* header unreadable: fall back to the No-Intro style name */
        std::string serial = gc_serial_for_name(path_utf8(arch.filename()));
        if (serial.empty()) serial = gc_serial_for_name(ent.name);
        if (serial.empty()) return false;
        g = GameEntry();
        g.system = System::GC;
        g.serial = serial;
        g.key = "gc:" + sanitize_key(serial) + "-0";
    }
    if (const GameInfo* gi = find_game_for("gc", g.serial, std::string())) { g.catalog_id = gi->id; g.title = gi->title; }
    else {
        std::string t = path_utf8(utf8_path(ent.name).filename());
        size_t dot = t.rfind('.');
        if (dot != std::string::npos) t.resize(dot);
        if (t.size() > 5 && ascii_lower(t.substr(t.size() - 5)) == ".nkit") t.resize(t.size() - 5);
        g.title = title_from_filename(utf8_path(t + ".iso"));
    }
    g.path = path_utf8(arch);
    g.issue = "extract";
    g.unpacked = ent.size;
    *e = std::move(g);
    return true;
}

static void fill_ps2(const hub::ps2::Ident& id, const fs::path& name_src, GameEntry* e) {
    e->system = System::PS2;
    e->serial = id.serial;
    e->key = hub::ps2::key_for(id.serial, sanitize_key(title_from_filename(name_src)));
    e->title = !id.title.empty() ? id.title : title_from_filename(name_src);
}

static bool id_ps2_file(const fs::path& p, GameEntry* e) {
    hub::ps2::Ident id;
    if (!hub::ps2::identify_file(path_utf8(p), &id)) return false;
    fill_ps2(id, p, e);
    return true;
}

/* A PS2 image inside a .zip/.7z: the first 4 MB are streamed to find SYSTEM.CNF (plain .iso); .chd/.cso use the serial in the name. */
static bool id_ps2_archive(const fs::path& arch, GameEntry* e) {
    std::string ext = ascii_lower(path_utf8(arch.extension()));
    if (ext == ".7z" && !is_7z_file(path_utf8(arch))) return false;
    std::vector<ArchiveEntry> l;
    ArchiveEntry ent;
    if (!list_archive(path_utf8(arch), &l) || !find_gc_entry(l, &ent, true)) return false;
    const std::string inner_ext = ascii_lower(path_utf8(utf8_path(ent.name).extension()));
    hub::ps2::Ident id;
    bool ok = false;
    if (inner_ext == ".iso") {
        auto head = std::make_shared<std::string>();
        if (tar_capture(L"-xOf " + tar_archive_arg(path_utf8(arch)) + L" " + tar_member_arg(ent.name), 4u << 20, 60000, head.get()) && head->size() > 0x10000)
            ok = hub::ps2::identify_buffer(head, &id);
        if (!ok && id.ps1) return false;
    }
    if (!ok) {
        std::string s = hub::ps2::serial_from_filename(path_utf8(arch.filename()));
        if (s.empty()) s = hub::ps2::serial_from_filename(ent.name);
        if (s.empty() || hub::ps2::title_for(s).empty()) return false;
        id = hub::ps2::Ident();
        id.serial = s;
        id.title = hub::ps2::title_for(s);
    }
    GameEntry g;
    fill_ps2(id, utf8_path(ent.name), &g);
    g.path = path_utf8(arch);
    g.issue = "extract";
    g.unpacked = ent.size;
    *e = std::move(g);
    return true;
}

/* Stamp written next to the unpacked image: archive size + mtime + image size + entry name. A changed or replaced archive
 * (or a truncated image) no longer matches, so it is unpacked again. */
static std::string gc_stamp(const GameEntry& e, uint64_t image_size, const std::string& entry) {
    std::error_code ec;
    fs::path a = utf8_path(e.path);
    uintmax_t sz = fs::file_size(a, ec);
    auto mt = fs::last_write_time(a, ec).time_since_epoch().count();
    return std::to_string(sz) + "|" + std::to_string((long long)mt) + "|" + std::to_string(image_size) + "|" + entry;
}

static std::string gc_extracted_path(const GameEntry& e) {
    std::error_code ec;
    fs::path dir = gc_unpack_dir(e);
    std::string stamp;
    {
        std::ifstream in(dir / L"stamp.txt", std::ios::binary);
        if (!in) return "";
        std::getline(in, stamp);
    }
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::wstring n = it->path().filename().wstring();
        if (n.rfind(L"disc.", 0) != 0 || n.find(L".part") != std::wstring::npos) continue;
        std::vector<std::string> parts;   // size|mtime|image size|entry
        size_t b = 0;
        for (int k = 0; k < 3; ++k) { size_t x = stamp.find('|', b); if (x == std::string::npos) return ""; parts.push_back(stamp.substr(b, x - b)); b = x + 1; }
        uint64_t img = std::strtoull(parts[2].c_str(), nullptr, 10);
        if (fs::file_size(it->path(), ec) != img || ec) return "";
        if (gc_stamp(e, img, stamp.substr(b)) != stamp) return "";
        return path_utf8(it->path());
    }
    return "";
}

static uint64_t gc_unpacked_size(const GameEntry& e) {
    if (e.unpacked) return e.unpacked;
    std::vector<ArchiveEntry> l;
    ArchiveEntry ent;
    return list_archive(e.path, &l) && find_gc_entry(l, &ent, e.system == System::PS2) ? ent.size : 0;
}

static bool gc_extract(const GameEntry& e, const std::function<bool(uint64_t, uint64_t)>& progress, std::string* out_path, std::string* err) {
    std::vector<ArchiveEntry> l;
    ArchiveEntry ent;
    if (!fs::exists(tar_exe())) { *err = "Windows' tar.exe was not found (needs Windows 10 version 1803 or newer)"; return false; }
    if (!list_archive(e.path, &l)) { *err = "cannot read the archive"; return false; }
    if (!find_gc_entry(l, &ent, e.system == System::PS2)) { *err = "no disc image inside the archive"; return false; }
    fs::path dir = gc_unpack_dir(e);
    std::error_code ec;
    fs::remove_all(dir, ec);   // a stale or half-done extraction of the same key
    fs::create_directories(dir, ec);
    ULARGE_INTEGER free_bytes{};
    if (GetDiskFreeSpaceExW(dir.wstring().c_str(), &free_bytes, nullptr, nullptr) && free_bytes.QuadPart < ent.size + (64ull << 20)) {
        *err = "not enough free disk space (" + std::to_string((ent.size >> 20) + 64) + " MB needed)";
        return false;
    }
    std::string ext = ascii_lower(path_utf8(utf8_path(ent.name).extension()));
    fs::path final_path = dir / (L"disc" + utf8_path(ext).wstring());
    fs::path part = dir / (L"disc" + utf8_path(ext).wstring() + L".part");
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE f = CreateFileW(part.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { *err = "cannot write " + path_utf8(part); return false; }
    HANDLE proc = tar_spawn(L"-xOf " + tar_archive_arg(e.path) + L" " + tar_member_arg(ent.name), f);
    if (!proc) { CloseHandle(f); fs::remove(part, ec); *err = "cannot start tar.exe"; return false; }
    bool cancelled = false, stalled = false;
    uint64_t last_size = 0;
    ULONGLONG last_move = GetTickCount64();
    for (;;) {
        DWORD w = WaitForSingleObject(proc, 150);
        LARGE_INTEGER sz{};
        GetFileSizeEx(f, &sz);
        if ((uint64_t)sz.QuadPart != last_size) { last_size = (uint64_t)sz.QuadPart; last_move = GetTickCount64(); }
        else if (w == WAIT_TIMEOUT && GetTickCount64() - last_move > 180000) { stalled = true; TerminateProcess(proc, 1); WaitForSingleObject(proc, 5000); break; }
        if (progress && !progress((uint64_t)sz.QuadPart, ent.size)) { cancelled = true; TerminateProcess(proc, 1); WaitForSingleObject(proc, 5000); break; }
        if (w != WAIT_TIMEOUT) break;
    }
    DWORD code = 1;
    GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    FlushFileBuffers(f);
    CloseHandle(f);
    if (cancelled || code != 0 || (uint64_t)sz.QuadPart != ent.size) {
        fs::remove(part, ec);
        *err = cancelled ? "cancelled" : stalled ? "extraction stalled (no data for 3 minutes); try again" : "extraction failed (disk full or damaged archive)";
        return false;
    }
    fs::rename(part, final_path, ec);
    if (ec) { *err = "cannot move the unpacked image into place"; return false; }
    {
        std::ofstream st(dir / L"stamp.txt", std::ios::binary | std::ios::trunc);
        st << gc_stamp(e, ent.size, ent.name) << '\n';
        if (!st) { fs::remove(final_path, ec); *err = "cannot write the unpack record"; return false; }
    }
    if (out_path) *out_path = path_utf8(final_path);
    return true;
}

static bool identify_rom(const fs::path& p, const fs::path& name_src, GameEntry* out);

bool identify_game(const std::string& path, GameEntry* out) {
    if (!out) return false;
    std::error_code ec;
    fs::path p = fs::absolute(utf8_path(path), ec);
    if (ec) p = utf8_path(path);
    uintmax_t size = fs::file_size(p, ec);
    if (ec) return false;
    std::string ext = ascii_lower(path_utf8(p.extension()));
    if (ext == ".zip") {
        {   /* a zipped 3DS image is only listed (header read), never extracted behind the player's back */
            GameEntry z;
            if (id_3ds_zip(p, &z)) {
                z.path = path_utf8(p);
                z.size = size;
                g_identify_calls.fetch_add(1);
                *out = std::move(z);
                return true;
            }
        }
        {   /* a zip holding a GameCube disc image: listed, unpacked on Play */
            GameEntry g;
            if (id_gc_archive(p, &g) || id_ps2_archive(p, &g)) { g.size = size; g_identify_calls.fetch_add(1); *out = std::move(g); return true; }
        }
        fs::path rom, inner;
        if (!extract_zipped_rom(p, &rom, &inner)) return false;
        if (!identify_rom(rom, inner, out)) return false;
        out->save_stem = path_utf8(inner.stem());   // per-game files (DS .srm) keep the inner name's stem
        return true;
    }
    if (ext == ".7z") {
        GameEntry g;
        if (!id_gc_archive(p, &g) && !id_ps2_archive(p, &g)) return false;
        g.size = size;
        g_identify_calls.fetch_add(1);
        *out = std::move(g);
        return true;
    }
    return identify_rom(p, p, out);
}

/* name_src: what a title falls back to (the zip's inner name for an extracted ROM, the file itself otherwise). */
static bool identify_rom(const fs::path& p, const fs::path& name_src, GameEntry* out) {
    std::error_code ec;
    uintmax_t size = fs::file_size(p, ec);
    if (ec) return false;
    std::string ext = ascii_lower(path_utf8(p.extension()));
    g_identify_calls.fetch_add(1);

    GameEntry e;
    bool ok = false;
    if (ext == ".z64" || ext == ".v64" || ext == ".n64") ok = id_n64(p, name_src, &e);
    else if (ext == ".gba") ok = id_gba(p, name_src, size, &e);
    else if (ext == ".gb" || ext == ".gbc") ok = id_gb(p, name_src, size, &e);
    else if (ext == ".nds") ok = id_nds(p, name_src, size, &e);
    else if (ext == ".chd" || ext == ".cso" || ext == ".cue" || ext == ".bin") ok = id_ps2_file(p, &e);
    else if (ext == ".iso" || ext == ".gcm" || ext == ".rvz" || ext == ".wia" || ext == ".ciso" || ext == ".gcz")
        ok = id_gc(p, ext, size, &e);
    else if (is_3ds_extension(ext)) ok = id_3ds(p, ext, &e);
    else if (ext == ".wua" || ext == ".wud" || ext == ".wux" || ext == ".rpx") ok = id_wiiu(p, ext, size, &e);
    if (!ok && ext == ".iso") { e = GameEntry(); ok = id_ps2_file(p, &e); }
    if (!ok) return false;
    e.path = path_utf8(p);
    e.size = size;
    *out = std::move(e);
    return true;
}

/* ------------------------------------------------------------------ */
/* explicit unzip of a 3DS image                                       */
/* ------------------------------------------------------------------ */

namespace {
struct ExtractCtx {
    FILE* f = nullptr;
    uint64_t done = 0, total = 0;
    const std::function<bool(uint64_t, uint64_t)>* cb = nullptr;
    bool cancelled = false;
};
size_t extract_write_cb(void* user, mz_uint64 /*ofs*/, const void* data, size_t n) {
    ExtractCtx* c = static_cast<ExtractCtx*>(user);
    if (std::fwrite(data, 1, n, c->f) != n) return 0;
    c->done += n;
    if (c->cb && *c->cb && !(*c->cb)(c->done, c->total)) { c->cancelled = true; return 0; }
    return n;
}
} // namespace

std::string extract_3ds_dir(const GameEntry& e) {
    if (e.system == System::GC || e.system == System::PS2) return path_utf8(gc_unpack_dir(e));
    return path_utf8(utf8_path(data_dir()) / L"rom-cache" / L"3ds" / utf8_path(sanitize_key(e.key)));
}

uint64_t zip_3ds_unpacked_size(const GameEntry& e) {
    if (e.system == System::GC || e.system == System::PS2) return gc_unpacked_size(e);
    mz_zip_archive za;
    std::memset(&za, 0, sizeof(za));
    FILE* zf = _wfopen(utf8_path(e.path).wstring().c_str(), L"rbN");
    if (!zf) return 0;
    if (!mz_zip_reader_init_cfile(&za, zf, 0, 0)) { std::fclose(zf); return 0; }
    uint64_t size = 0;
    mz_uint n = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < n && !size; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st) || st.m_is_directory) continue;
        if (is_3ds_zip_entry(ascii_lower(path_utf8(utf8_path(st.m_filename).extension())))) size = st.m_uncomp_size;
    }
    mz_zip_reader_end(&za);
    std::fclose(zf);
    return size;
}

std::string extracted_3ds_path(const GameEntry& e) {
    if (e.system == System::GC || e.system == System::PS2) return gc_extracted_path(e);
    std::error_code ec;
    fs::path dir = utf8_path(extract_3ds_dir(e));
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string ext = ascii_lower(path_utf8(it->path().extension()));
        if (is_3ds_zip_entry(ext) && it->path().filename().wstring().find(L".part") == std::wstring::npos) return path_utf8(it->path());
    }
    return "";
}

bool extract_3ds_zip(const GameEntry& e, const std::function<bool(uint64_t, uint64_t)>& progress,
                     std::string* out_path, std::string* err) {
    std::string dummy;
    if (!err) err = &dummy;
    if (e.system == System::GC || e.system == System::PS2) return gc_extract(e, progress, out_path, err);
    fs::path zip = utf8_path(e.path);
    mz_zip_archive za;
    std::memset(&za, 0, sizeof(za));
    FILE* zf = _wfopen(zip.wstring().c_str(), L"rbN");
    if (!zf) { *err = "cannot open the zip"; return false; }
    if (!mz_zip_reader_init_cfile(&za, zf, 0, 0)) { std::fclose(zf); *err = "not a valid zip"; return false; }
    bool ok = false;
    mz_uint n = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < n && !ok; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st) || st.m_is_directory) continue;
        fs::path name = utf8_path(st.m_filename).filename();
        if (!is_3ds_zip_entry(ascii_lower(path_utf8(name.extension())))) continue;
        fs::path dir = utf8_path(extract_3ds_dir(e));
        std::error_code ec;
        fs::create_directories(dir, ec);
        ULARGE_INTEGER free_bytes{};
        if (GetDiskFreeSpaceExW(dir.wstring().c_str(), &free_bytes, nullptr, nullptr) &&
            free_bytes.QuadPart < st.m_uncomp_size + (64ull << 20)) {
            *err = "not enough free disk space (" + std::to_string((st.m_uncomp_size >> 20) + 64) + " MB needed)";
            break;
        }
        fs::path final_path = dir / name, part = dir / (name.wstring() + L".part");
        ExtractCtx ctx;
        ctx.f = _wfopen(part.wstring().c_str(), L"wbN");
        if (!ctx.f) { *err = "cannot write " + path_utf8(part); break; }
        ctx.total = st.m_uncomp_size;
        ctx.cb = &progress;
        mz_bool r = mz_zip_reader_extract_to_callback(&za, i, extract_write_cb, &ctx, 0);
        bool wrote = std::fflush(ctx.f) == 0;
        std::fclose(ctx.f);
        if (!r || !wrote || ctx.done != st.m_uncomp_size) {
            fs::remove(part, ec);
            *err = ctx.cancelled ? "cancelled" : "extraction failed (disk full or damaged zip)";
            break;
        }
        fs::remove(final_path, ec);
        fs::rename(part, final_path, ec);
        if (ec) { *err = "cannot move the unpacked image into place"; break; }
        if (out_path) *out_path = path_utf8(final_path);
        ok = true;
    }
    mz_zip_reader_end(&za);
    std::fclose(zf);
    if (!ok && err->empty()) *err = "no 3DS image inside the zip";
    return ok;
}

/* ------------------------------------------------------------------ */
/* JSON: the shared reader/writer (json.h)                             */
/* ------------------------------------------------------------------ */

namespace {

typedef json::Value JV;
bool json_parse(const std::string& text, JV* out) { return json::parse(text, out, json::ParseOptions{64, true}); }
bool jbool(const JV& o, const char* k) {   // true/false, or a number (!= 0)
    const JV* v = o.get(k);
    if (!v) return false;
    if (v->type == JV::Bool) return v->b;
    if (v->type == JV::Number) return v->i != 0;
    return false;
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
    if (!json_parse(text, &root) || root.type != JV::Object) return;
    const JV* games = root.get("games");
    if (!games || games->type != JV::Object) return;
    for (const auto& kv : games->o) {
        if (kv.second.type != JV::Object) continue;
        GameState gs;
        gs.favorite = jbool(kv.second, "favorite");
        gs.last_played = (uint64_t)std::max<long long>(0, kv.second.i64("last_played"));
        gs.play_seconds = (uint64_t)std::max<long long>(0, kv.second.i64("play_seconds"));
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
        o += json::quote(kv.first);
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

constexpr int kCacheVersion = 9;  // 2: serial matching + GBA catalog_id; 5: GB/GBC serials; 6: 3DS/Wii U/DS; 7: save_stem (short rom-cache names); 8: GB/GBC catalog titles; forces a rescan

std::unordered_map<std::string, CacheRec> cache_load() {
    std::unordered_map<std::string, CacheRec> m;
    std::string text;
    if (!read_text(lib_dir() / L"library_cache.json", &text)) return m;
    JV root;
    if (!json_parse(text, &root) || root.type != JV::Object) return m;
    if (root.i64("version") != kCacheVersion) return m;
    const JV* files = root.get("files");
    if (!files || files->type != JV::Array) return m;
    for (const JV& f : files->a) {
        if (f.type != JV::Object) continue;
        CacheRec r;
        std::string path = f.str("p");
        if (path.empty()) continue;
        r.size = (uint64_t)f.i64("s");
        r.mtime = f.i64("m");
        r.ok = jbool(f, "ok");
        if (r.ok) {
            r.e.key = f.str("k");
            r.e.system = system_from_short(f.str("sys"));
            r.e.title = f.str("t");
            r.e.serial = f.str("sn");
            r.e.catalog_id = f.str("c");
            r.e.issue = f.str("is");
            r.e.path = f.str("lp").empty() ? path : f.str("lp");
            r.e.save_stem = f.str("ss");
            r.e.unpacked = (uint64_t)f.i64("us");
            {   /* catalog can change between versions: re-match instead of trusting the cache */
                std::string md5 = (r.e.key.rfind("n64:", 0) == 0) ? r.e.key.substr(4) : std::string();
                if (const GameInfo* g = find_game_for(f.str("sys"), r.e.serial, md5)) {
                    r.e.catalog_id = g->id;
                    if (r.e.system == System::N64 || r.e.system == System::N3DS || r.e.system == System::WiiU ||
                        r.e.system == System::GB || r.e.system == System::GBC || r.e.system == System::GBA ||
                        r.e.system == System::NDS) r.e.title = g->title;
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
        o += json::quote(pr.first);
        o += ",\"s\":" + std::to_string(r.size) + ",\"m\":" + std::to_string(r.mtime);
        o += r.ok ? ",\"ok\":true" : ",\"ok\":false";
        if (r.ok) {
            o += ",\"k\":"; o += json::quote(r.e.key);
            o += ",\"sys\":"; o += json::quote(system_short(r.e.system));
            o += ",\"t\":"; o += json::quote(r.e.title);
            o += ",\"sn\":"; o += json::quote(r.e.serial);
            o += ",\"c\":"; o += json::quote(r.e.catalog_id);
            o += ",\"is\":"; o += json::quote(r.e.issue);
            o += ",\"lp\":"; o += json::quote(r.e.path);   // launch path (differs for zipped ROMs)
            if (!r.e.save_stem.empty()) { o += ",\"ss\":"; o += json::quote(r.e.save_stem); }
            if (r.e.unpacked) o += ",\"us\":" + std::to_string(r.e.unpacked);
        }
        o.push_back('}');
    }
    o += "]}\n";
    write_text_atomic(lib_dir() / L"library_cache.json", o);
}

bool is_archive_file(const std::string& path) {
    std::string ext = ascii_lower(path_utf8(fs::path(utf8_path(path)).extension()));
    return ext == ".zip" || ext == ".7z";
}

bool scan_extension(const std::string& ext) {
    static const char* const kExt[] = {".z64", ".v64", ".n64", ".gba", ".gb", ".gbc", ".iso", ".7z",
                                       ".gcm", ".rvz", ".wia", ".ciso", ".gcz", ".zip", ".nds",
                                       ".3ds", ".cci", ".cia", ".cxi", ".3dsx", ".zcci", ".zcia", ".zcxi", ".z3dsx",
                                       ".wua", ".wud", ".wux", ".rpx", ".chd", ".cso", ".cue"};
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
    {   /* once per process: stale "*.part" files from an interrupted unzip / extraction in the rom cache */
        static std::once_flag cleaned;
        std::call_once(cleaned, [] {
            std::error_code ec;
            fs::path rc = lib_dir() / L"rom-cache";
            fs::remove_all(rc / L"_probe", ec);
            for (fs::recursive_directory_iterator it(rc, ec), end; !ec && it != end; it.increment(ec))
                if (it->path().extension() == L".part") { std::error_code e2; fs::remove(it->path(), e2); }
        });
    }
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

    /* Dedupe by key (one card per game), then sort. Among copies of the same game the
     * unzipped one is listed (dedupe_prefers); otherwise the first found wins. */
    std::vector<GameEntry> out;
    std::map<std::string, size_t> by_key;   // key -> index in out
    std::vector<std::string> out_src;       // original file path of each out entry
    for (const Candidate& c : cands) {
        if (!c.rec.ok) continue;
        auto it = by_key.find(c.rec.e.key);
        if (it == by_key.end()) {
            by_key.emplace(c.rec.e.key, out.size());
            out.push_back(c.rec.e);
            out_src.push_back(c.path_utf8);
        } else if (dedupe_prefers(c.path_utf8, out_src[it->second])) {
            out[it->second] = c.rec.e;
            out_src[it->second] = c.path_utf8;
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const GameEntry& a, const GameEntry& b) {
        if (a.system != b.system) return (int)a.system < (int)b.system;
        std::string ta = ascii_lower(a.title), tb = ascii_lower(b.title);
        if (ta != tb) return ta < tb;
        return a.path < b.path;
    });
    return out;
}

bool dedupe_prefers(const std::string& challenger_path, const std::string& holder_path) {
    return is_archive_file(holder_path) && !is_archive_file(challenger_path);
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
