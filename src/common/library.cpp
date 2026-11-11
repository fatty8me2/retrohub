#include "winutil.h"
#include "json.h"
#include "miniz.h"
#include "library.h"
#include "library_scan_info.h"
#include "prefs.h"
#include "emu_formats.h"
#include "wiiu_meta.h"
#include "games.h"
#include "ps1.h"
#include "ps2.h"
#include "psp.h"
#include "nx.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <cstdio>
#include <cctype>
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

/* Files an archiver adds that are not part of the game: macOS resource forks ("__MACOSX/._Game.iso" has the game's extension and, at 4 KB,
 * the size of a small image), .DS_Store, Thumbs.db, desktop.ini. Never picked as the game, never counted. */
bool is_junk_entry(const std::string& n) {
    size_t i = 0;
    while (i <= n.size()) {
        size_t j = n.find_first_of("/\\", i);
        if (j == std::string::npos) j = n.size();
        const std::string part = n.substr(i, j - i);
        if (part == "__MACOSX" || part == ".DS_Store" || part == "Thumbs.db" || part == "desktop.ini" || part.compare(0, 2, "._") == 0) return true;
        i = j + 1;
    }
    return false;
}

/* A file/archive name that says nothing about the game: "game", "disc1", "rom", a disc serial ("SLUS_012.34"), a GameCube disc id ("GALE01").
 * Such a name never becomes a title when anything better is around (the archive name, the folder, the header). */
bool generic_stem(const std::string& stem) {
    std::string t, up;
    for (unsigned char c : stem) if (is_alnum((char)c)) { t.push_back((char)std::tolower(c)); up.push_back((char)c); }
    if (t.empty()) return true;
    size_t n = t.size();
    while (n > 0 && t[n - 1] >= '0' && t[n - 1] <= '9') --n;
    if (n == 0) return false;   // digits only: "1942" is a game, not a disc number
    const std::string w = t.substr(0, n);
    static const char* const kGeneric[] = {"game", "games", "disc", "disk", "cd", "dvd", "rom", "roms", "data", "main", "image", "iso", "boot", "dump", "cart", "cartridge", "backup",
                                           "unknown", "untitled", "track", "title", "default", "new", "file", "output", "extracted", "unpacked", "content", "install", "setup", "bin", "eboot"};
    for (const char* g : kGeneric) if (w == g) return true;
    if (t.size() == 9 && n == 4) return true;   // SLUS01234 / ULUS10041 / SCES12345: four letters, five digits
    if (up.size() == 6 && up == stem && t.size() == 6) {   // GALE01: an all-capital 6 character GameCube disc id
        bool caps = true;
        for (char c : up) if (c >= 'a' && c <= 'z') caps = false;
        if (caps && up[4] >= '0' && up[4] <= '9' && up[5] >= '0' && up[5] <= '9') return true;
    }
    return false;
}

/* Folder names that only say what kind of folder it is. */
bool noise_folder(const std::string& name) {
    static const char* const k[] = {"roms", "rom", "games", "game", "downloads", "download", "desktop", "documents", "iso", "isos", "discs", "backup", "gamecube", "nintendo gamecube", "gc", "wii", "wiiu",
                                    "wii u", "nintendo wii u", "ps1", "ps2", "ps3", "psx", "playstation", "playstation 1", "playstation 2", "sony playstation", "sony playstation 2", "psp", "playstation portable",
                                    "n64", "nintendo64", "nintendo 64", "nes", "nintendo", "famicom", "snes", "super nintendo", "super nintendo entertainment system", "super famicom", "gba", "game boy advance", "gameboy advance",
                                    "gb", "game boy", "gameboy", "gbc", "game boy color", "gameboy color", "nds", "nintendo ds", "ds", "3ds", "nintendo 3ds", "temp", "tmp", "new folder", "retrohub", "gamepack", "emulation"};
    std::string l = ascii_lower(name);
    for (const char* x : k) if (l == x) return true;
    return l.empty() || generic_stem(name);
}

/* Title of a game that sits in an archive or a folder: the container's name wins ("Pikmin 2.7z"); a name that says nothing ("disc1.7z") falls
 * back to the file inside, then to the folder around it. `generic` reports that nothing better than a generic name was found. */
std::string container_title(const fs::path& container, const std::string& inner_name, bool* generic = nullptr) {
    if (generic) *generic = false;
    std::string a = title_from_filename(container);
    if (!a.empty() && !generic_stem(a)) return a;
    if (!inner_name.empty()) {
        std::string i = title_from_filename(utf8_path(path_utf8(utf8_path(inner_name).filename())));
        if (!i.empty() && !generic_stem(i)) return i;
    }
    std::string f = path_utf8(container.parent_path().filename());
    if (!f.empty() && !noise_folder(f)) return collapse_spaces(f);
    if (generic) *generic = true;
    return a;
}

/* A header-less cartridge ("Mario.nes") in an archive ("NES Collection.zip"): the file inside names the game; only a generic file name
 * ("game.nes") falls back to the archive's name (then the folder). */
std::string cart_title(const fs::path& container, const std::string& inner_name) {
    std::string i = title_from_filename(utf8_path(path_utf8(utf8_path(inner_name).filename())));
    if (!i.empty() && !generic_stem(i)) return i;
    return container_title(container, inner_name);
}

/* The path whose stem is `title` (what the identify_* helpers take as their name source). */
fs::path title_src(const std::string& title) { return utf8_path(title + ".x"); }

/* The name a loose file is titled from: itself, or its folder when the file is called "game" / "disc" / a serial. */
fs::path loose_name_src(const fs::path& p) {
    std::string t = title_from_filename(p);
    if (!t.empty() && !generic_stem(t)) return p;
    std::string f = path_utf8(p.parent_path().filename());
    if (f.empty() || noise_folder(f)) return p;
    return p.parent_path() / utf8_path(collapse_spaces(f) + path_utf8(p.extension()));
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

} // namespace

bool nes_parse_header(const uint8_t* h, size_t n, uint64_t file_size, NesHeader* out) {
    if (n < 16 || h[0] != 'N' || h[1] != 'E' || h[2] != 'S' || h[3] != 0x1A) return false;
    NesHeader r;
    r.nes2 = (h[7] & 0x0C) == 0x08;
    r.battery = (h[6] & 2) != 0;
    r.trainer = (h[6] & 4) != 0;
    r.mapper = (unsigned)((h[6] >> 4) | (h[7] & 0xF0));
    uint64_t prg_units = h[4], chr_units = h[5];
    if (r.nes2) {
        r.mapper |= (unsigned)(h[8] & 0x0F) << 8;
        prg_units |= (uint64_t)(h[9] & 0x0F) << 8;
        chr_units |= (uint64_t)(h[9] >> 4) << 8;
    }
    r.prg = prg_units * 16384u;
    r.chr = chr_units * 8192u;
    r.data_off = 16 + (r.trainer ? 512 : 0);
    if (prg_units == 0) return false;
    if (r.nes2 && ((h[9] & 0x0F) == 0x0F || (h[9] >> 4) == 0x0F)) {   // exponent-multiplier sizes: trust the file
        *out = r;
        return file_size > r.data_off;
    }
    if (r.data_off + r.prg > file_size) return false;   // truncated / not an iNES image
    if (r.data_off + r.prg + r.chr > file_size) r.chr = file_size - r.data_off - r.prg;   // short CHR: dumps in the wild
    *out = r;
    return true;
}

size_t snes_copier_header_size(uint64_t file_size) { return (file_size % 1024u) == 512u ? 512u : 0u; }

SnesInfo snes_probe(const uint8_t* d, size_t n) {
    SnesInfo best;
    int best_score = -1;
    const size_t hdr = snes_copier_header_size(n);
    for (int hi = 0; hi < 2; ++hi) {
        size_t off = hdr + (hi ? 0xFFC0u : 0x7FC0u);
        if (off + 0x30 > n) continue;
        const uint8_t* h = d + off;
        unsigned comp = (unsigned)(h[0x1C] | (h[0x1D] << 8)), sum = (unsigned)(h[0x1E] | (h[0x1F] << 8));
        bool chk = (comp ^ sum) == 0xFFFFu;
        int printable = 0;
        for (int i = 0; i < 21; ++i) if (h[i] >= 0x20 && h[i] < 0x7F) ++printable;
        int score = (chk ? 100 : 0) + printable;
        unsigned mapmode = h[0x15] & 0x0F;   // 0 LoROM, 1 HiROM, 5 ExHiROM
        if ((mapmode == 1 || mapmode == 5) == (hi == 1)) score += 10;
        if ((h[0x15] & 0xE0) == 0x20) score += 5;   // 0x2x/0x3x map-mode byte
        if (printable < 12 && !chk) continue;
        if (score > best_score) {
            best_score = score;
            best.valid = true; best.hirom = hi == 1; best.checksum_ok = chk; best.header = hdr;
            std::string t((const char*)h, 21);
            for (auto& c : t) if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7F) c = ' ';
            while (!t.empty() && t.back() == ' ') t.pop_back();
            best.title = t;
        }
    }
    return best;
}

namespace {

bool id_nes(const fs::path& p, const fs::path& name, uint64_t size, GameEntry* e) {
    if (size < 16 + 16384 || size > 8u * 1024u * 1024u) return false;
    std::vector<uint8_t> d;
    if (!read_at(p, 0, (size_t)size, &d) || d.size() < size) return false;
    NesHeader h;
    if (!nes_parse_header(d.data(), d.size(), size, &h)) return false;
    uint64_t end = h.data_off + h.prg + h.chr;
    if (end > d.size()) end = d.size();
    uint32_t crc = crc32_update(0, d.data() + h.data_off, (size_t)(end - h.data_off));   // No-Intro (headerless) CRC32
    e->system = System::NES;
    e->key = "nes:" + hex32(crc);
    e->title = pretty_title(std::string(), name);
    return true;
}

bool id_snes(const fs::path& p, const fs::path& name, uint64_t size, GameEntry* e) {
    if (size < 32u * 1024u || size > 12u * 1024u * 1024u) return false;
    std::vector<uint8_t> d;
    if (!read_at(p, 0, (size_t)size, &d) || d.size() < size) return false;
    SnesInfo si = snes_probe(d.data(), d.size());
    std::string ext = ascii_lower(path_utf8(p.extension()));
    if (!si.valid && ext != ".sfc" && ext != ".smc") return false;   // an extension-less guess is not enough for other names
    size_t hdr = si.valid ? si.header : snes_copier_header_size(size);
    uint32_t crc = crc32_update(0, d.data() + hdr, d.size() - hdr);   // No-Intro (headerless) CRC32
    e->system = System::SNES;
    e->key = "snes:" + hex32(crc);
    e->title = pretty_title(std::string(), name);
    e->serial = si.title;
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

/* key_p: the file itself (KEYS NEVER CHANGE: they hold saves, favourites, play time); title_p: where the display title comes from (may be the folder). */
bool id_gc_filename_only(const fs::path& key_p, const fs::path& title_p, GameEntry* e) {
    e->system = System::GC;
    e->serial.clear();
    std::string stem = sanitize_key(title_from_filename(key_p));
    e->key = "gc:file-" + stem;
    e->title = title_from_filename(title_p);
    return true;
}

bool id_gc(const fs::path& p, const std::string& ext, uint64_t size, GameEntry* e, const fs::path& name = fs::path()) {
    std::vector<uint8_t> h;
    const fs::path& nm = name.empty() ? p : name;   // what a title the header cannot give falls back to
    if (ext == ".iso" || ext == ".gcm") {
        if (size < 0x1000) return false;
        if (!read_at(p, 0, kGcHeader, &h)) return false;
        return gc_from_header(h.data(), h.size(), nm, e);
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
        return gc_from_header(h.data() + 0x58, 0x80, nm, e);
    }
    if (ext == ".ciso") {
        if (!read_at(p, 0, 0x8008, &h)) return false;
        if (std::memcmp(h.data(), "CISO", 4) != 0) return false;
        uint32_t block = le32(h.data() + 4);
        if (block < kGcHeader || h[8] != 1) return false;  // block 0 holds the disc header
        std::vector<uint8_t> d;
        if (!read_at(p, 0x8000, kGcHeader, &d)) return false;
        return gc_from_header(d.data(), d.size(), nm, e);
    }
    if (ext == ".gcz") {
        if (!read_at(p, 0, 32, &h)) return false;
        if (le32(h.data()) != 0xB10BC001u) return false;
        uint64_t comp_size = le64(h.data() + 8);
        uint64_t data_size = le64(h.data() + 16);
        uint32_t block_size = le32(h.data() + 24);
        uint32_t num_blocks = le32(h.data() + 28);
        if (num_blocks == 0 || block_size == 0 || data_size < kGcHeader) return id_gc_filename_only(p, nm, e);
        std::vector<uint8_t> ptr;
        if (!read_at(p, 32, num_blocks > 1 ? 16 : 8, &ptr)) return id_gc_filename_only(p, nm, e);
        uint64_t p0 = le64(ptr.data());
        bool raw = (p0 >> 63) != 0;
        p0 &= ~(1ull << 63);
        uint64_t p1 = num_blocks > 1 ? (le64(ptr.data() + 8) & ~(1ull << 63)) : comp_size;
        if (p1 <= p0 || p1 - p0 > (1u << 20)) return id_gc_filename_only(p, nm, e);
        uint64_t data_start = 32 + 12ull * num_blocks;
        std::vector<uint8_t> comp;
        if (!read_at(p, data_start + p0, (size_t)(p1 - p0), &comp)) return id_gc_filename_only(p, nm, e);
        std::vector<uint8_t> disc(kGcHeader, 0);
        size_t got = 0;
        if (raw) {
            got = std::min(comp.size(), disc.size());
            std::memcpy(disc.data(), comp.data(), got);
        } else if (comp.size() > 2) {
            got = inflate_prefix(comp.data() + 2, comp.size() - 2, disc.data(), disc.size());  // skip zlib header
        }
        if (got >= 0x80 && gc_from_header(disc.data(), got, nm, e)) return true;
        return id_gc_filename_only(p, nm, e);
    }
    return false;
}


/* ---- PlayStation 1 ----
 * Key: ps1:<boot serial, lowercase> (Redump serial from SYSTEM.CNF / PBP DISC_ID) so every dump of a game (cue/bin, chd, pbp) shares
 * one save; homebrew without a serial: ps1:file-<name>. A multi-disc .m3u is keyed by disc 1. Title: the Redump name without its
 * region/language tags ("Crash Bash"), "(Disc N)" kept for single discs. */
std::string ps1_display_title(const std::string& redump, bool keep_disc) {
    std::string o;
    int depth = 0;
    std::string tag;
    std::string disc;
    for (char c : redump) {
        if (c == '(' || c == '[') { ++depth; tag.clear(); continue; }
        if (c == ')' || c == ']') {
            if (depth > 0) --depth;
            if (depth == 0 && ps1::disc_number("(" + tag + ")") > 0) disc = " (" + tag + ")";
            continue;
        }
        if (depth == 0) o.push_back(c);
        else tag.push_back(c);
    }
    o = collapse_spaces(o);
    if (keep_disc) o += disc;
    return o.empty() ? redump : o;
}

bool id_ps1(const fs::path& p, const std::string& ext, GameEntry* e, const fs::path& name = fs::path()) {
    ps1::DiscInfo di = ps1::probe_file(path_utf8(p));
    const fs::path& nm = name.empty() ? p : name;   // the TITLE comes from the name source (the folder when the file is called "disc"); the KEY never does
    std::string stem = path_utf8(p.stem());
    std::string serial = di.serial;
    if (!di.ok) {   // header unreadable (odd CHD, damaged image): trust a file name that is exactly a Redump title
        serial = ps1::serial_for_name(stem);
        if (serial.empty()) return false;
    }
    e->system = System::PS1;
    e->serial = serial;
    std::string redump = serial.empty() ? std::string() : ps1::title_for_serial(serial);
    if (!redump.empty()) e->title = ps1_display_title(redump, ext != ".m3u");
    else e->title = title_from_filename(nm);
    if (!serial.empty()) e->key = "ps1:" + sanitize_key(serial);
    else e->key = "ps1:file-" + sanitize_key(title_from_filename(p));
    return true;
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

/* key_src: the file / archive the key is derived from when the image has no title id (never changes); title_src: where the title comes from. */
void fill_3ds(const N3dsInfo& i, const fs::path& key_src, const fs::path& title_name, GameEntry* e) {
    e->system = System::N3DS;
    e->serial = i.serial;
    std::string k = n3ds_key(i);
    e->title = title_from_filename(title_name);
    e->key = k.empty() ? "3ds:file-" + sanitize_key(title_from_filename(key_src)) : k;
    if (const GameInfo* g = find_game_for("3ds", e->serial, std::string())) { e->catalog_id = g->id; e->title = g->title; }
    if ((i.container == "ncsd" || i.container == "ncch") && !i.decrypted) e->issue = "encrypted";   // Azahar needs AES keys
}

bool id_3ds(const fs::path& p, const std::string& ext, GameEntry* e, const fs::path& name = fs::path()) {
    std::vector<uint8_t> h;
    const fs::path& nm = name.empty() ? p : name;
    if (ext == ".zcci" || ext == ".zcia" || ext == ".zcxi" || ext == ".z3dsx") {   // Azahar's own compressed formats: no cheap header
        if (!read_prefix(p, 4, &h) || h.size() < 4 || std::memcmp(h.data(), "Z3DS", 4) != 0) return false;
        fill_3ds(N3dsInfo{}, p, nm, e);
        return true;
    }
    if (!read_prefix(p, kN3dsHeaderBytes, &h)) return false;
    N3dsInfo i;
    if (!parse_3ds_header(h.data(), h.size(), &i)) return false;
    if (i.container != "3dsx" && !i.is_application) return false;   // updates / DLC / system titles are not games
    fill_3ds(i, p, nm, e);
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
        if (!is_3ds_zip_entry(ext) || st.m_uncomp_size < kN3dsHeaderBytes || is_junk_entry(st.m_filename)) continue;
        ZipPrefixSink sink;
        mz_zip_reader_extract_to_callback(&za, i, zip_prefix_cb, &sink, 0);   // reports failure by design after the prefix
        N3dsInfo info;
        if (!parse_3ds_header(sink.buf.data(), sink.buf.size(), &info)) continue;
        if (info.container != "3dsx" && !info.is_application) continue;
        fill_3ds(info, zip, title_src(container_title(zip, st.m_filename)), e);
        e->issue = "extract";
        ok = true;
    }
    mz_zip_reader_end(&za);
    std::fclose(f);
    return ok;
}

bool id_wiiu(const fs::path& p, const std::string& ext, uint64_t size, GameEntry* e, const fs::path& name = fs::path()) {
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
    std::string meta_name;   // longname from meta.xml (.wua / loadiine), "" when unknown
    fs::path name_src = p;                              // a file-name key (no title id) is derived from this: never changes
    fs::path title_name = name.empty() ? p : name;      // the display title falls back to this (the folder when the file is called "game")
    if (ext == ".wua") {
        ok = parse_wua(reader, size, &w);
        if (!ok) { w = WiiuInfo{}; w.container = "wua"; ok = true; }   // unknown layout: still launchable, keyed by file name
        WiiuMeta wm;   // the game's real name (meta.xml longname) beats the file name; repacks whose folder is 0000000000000000_v0 carry the id only here
        if (wua_read_meta(reader, size, &wm)) {
            if (!wm.longname.empty()) meta_name = wm.longname;
            if (!w.title_id && wm.title_id) { w.title_id = wm.title_id; w.serial = hex16(wm.title_id); for (char& c : w.serial) if (c >= 'a' && c <= 'f') c = char(c - 32); }
        }
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
        name_src = title_name = fs::path(game_dir.filename().wstring() + L".rpx");   // the folder name is the game name
        WiiuMeta wm;
        if (parse_wiiu_meta_xml(xml, &wm) && !wm.longname.empty()) meta_name = wm.longname;
        ok = true;
    }
    if (!ok) return false;
    e->system = System::WiiU;
    e->serial = w.serial;
    e->title = meta_name.empty() ? title_from_filename(title_name) : meta_name;
    std::string k = wiiu_key(w);
    e->key = k.empty() ? "wiiu:file-" + sanitize_key(meta_name.empty() ? title_from_filename(name_src) : meta_name) : k;
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
    case System::PS1: return "PlayStation";
    case System::PS2: return "PlayStation 2";
    case System::NES: return "Nintendo Entertainment System";
    case System::SNES: return "Super Nintendo";
    case System::PSP: return "PlayStation Portable";
    case System::Switch: return "Nintendo Switch";
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
    case System::PS1: return "ps1";
    case System::PS2: return "ps2";
    case System::NES: return "nes";
    case System::SNES: return "snes";
    case System::PSP: return "psp";
    case System::Switch: return "switch";
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
    if (l == "ps1") return System::PS1;
    if (l == "ps2") return System::PS2;
    if (l == "nes") return System::NES;
    if (l == "snes") return System::SNES;
    if (l == "psp") return System::PSP;
    if (l == "switch") return System::Switch;
    return System::Unknown;
}

/* ---- consoles (Nintendo Switch and Switch 2 are two consoles; see library.h) ---- */
std::string console_id(System s, bool nx2) {
    if (s == System::Switch && nx2) return kConsoleSwitch2;
    return system_short(s);
}
std::string console_id(const GameEntry& e) { return console_id(e.system, e.nx2); }
System console_system(const std::string& id) {
    if (console_is_switch2(id)) return System::Switch;
    return system_from_short(id);
}
bool console_is_switch2(const std::string& id) { return ascii_lower(id) == kConsoleSwitch2; }
const char* console_name(const std::string& id) {
    if (console_is_switch2(id)) return "Nintendo Switch 2";
    System s = system_from_short(id);
    return s == System::Unknown ? "" : system_name(s);
}
const char* console_name(const GameEntry& e) { return e.system == System::Unknown ? "" : (e.system == System::Switch && e.nx2 ? "Nintendo Switch 2" : system_name(e.system)); }
const std::vector<std::string>& console_ids() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> r;
        for (int i = (int)System::Unknown + 1; i < 64; i++) {   // the whole enum: any System with a short code is listed (a future one is not silently dropped)
            System s = (System)i;
            if (!*system_short(s)) continue;
            r.push_back(system_short(s));
            if (s == System::Switch) r.push_back(kConsoleSwitch2);
        }
        return r;
    }();
    return v;
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
        if (ext != ".z64" && ext != ".v64" && ext != ".n64" && ext != ".gba" && ext != ".gb" && ext != ".gbc" && ext != ".nds" &&
            ext != ".nes" && ext != ".sfc" && ext != ".smc") continue;
        if (is_junk_entry(name)) continue;
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

static bool is_zip_sig_file(const std::string& path) {
    std::vector<uint8_t> h;
    if (!read_at(utf8_path(path), 0, 4, &h) || h.size() < 4) return false;
    return h[0] == 'P' && h[1] == 'K' && (h[2] == 3 || h[2] == 5) && (h[3] == 4 || h[3] == 6);
}

bool is_rar_file(const std::string& path) {
    std::vector<uint8_t> h;
    if (!read_at(utf8_path(path), 0, 7, &h) || h.size() < 7) return false;
    static const uint8_t rar4[7] = {'R', 'a', 'r', '!', 0x1A, 0x07, 0x00};   // RAR 1.5-4.x
    return std::memcmp(h.data(), rar4, 6) == 0 && (h[6] == 0x00 || h[6] == 0x01);   // 0x01 = RAR5
}

bool archive_sig_ok(const std::string& path) {
    std::string ext = ascii_lower(path_utf8(utf8_path(path).extension()));
    if (ext == ".7z" || ext == ".rar") return is_7z_file(path) || is_rar_file(path) || is_zip_sig_file(path);   // sniffed: a 7z named .rar (and the reverse, or a zip named .7z) is still read by bsdtar
    return true;
}

bool archive_truncated(const std::string& path) {
    std::vector<uint8_t> h;
    std::error_code ec;
    uintmax_t size = fs::file_size(utf8_path(path), ec);
    static const uint8_t magic[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
    if (ec || !read_at(utf8_path(path), 0, 32, &h) || h.size() < 32 || std::memcmp(h.data(), magic, 6) != 0) return false;
    uint64_t off = 0, len = 0;
    for (int i = 7; i >= 0; --i) { off = off << 8 | h[12 + i]; len = len << 8 | h[20 + i]; }
    return 32 + off + len > size;   // the end header lies beyond the end of the file: an unfinished copy or download
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

/* tar.exe gets its command line converted to the ANSI code page: an archive whose path holds a character outside it (a Japanese name on a
 * Western Windows, "Okami" with the macron, a user folder with such a name) cannot be opened by name ("Failed to open ... ????.7z"). Those
 * archives are handed to tar on stdin instead ("-f -" and an open file handle): same bytes, no path in the command line. */
bool acp_safe(const std::wstring& w) {
    if (w.empty() || GetACP() == CP_UTF8) return true;
    BOOL used = FALSE;
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), (int)w.size(), nullptr, 0, nullptr, &used);
    if (n <= 0) return false;
    std::string b((size_t)n, '\0');
    used = FALSE;
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), (int)w.size(), &b[0], n, nullptr, &used);
    return !used;
}

/* tar prints names in the ANSI code page too ("F\xE4ke" for "Fäke"); everything else here is UTF-8. A character the code page lacks arrives as '?'. */
std::string acp_to_utf8(const std::string& s) {
    if (s.empty() || GetACP() == CP_UTF8) return s;
    int n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return s;
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), &w[0], n);
    return to_utf8(w);
}

thread_local std::string t_tar_stdin;   // set by tar_archive_arg() for an archive that cannot go on the command line, consumed by the next tar_spawn() of this thread

/* Runs tar.exe with stdout = `out` (a pipe end or a file), stderr/stdin = NUL (stdin = the archive when tar_archive_arg() asked for that).
 * `cwd`: working directory (tar extracts there; a path with characters outside the ANSI code page cannot go in "-C"). Returns the process handle or null. */
HANDLE tar_spawn(const std::wstring& args, HANDLE out, const fs::path* cwd = nullptr) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    const std::string stdin_archive = std::move(t_tar_stdin);
    t_tar_stdin.clear();
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE in = nul, arc = INVALID_HANDLE_VALUE;
    if (!stdin_archive.empty()) {
        arc = CreateFileW(long_path(utf8_path(stdin_archive)).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (arc == INVALID_HANDLE_VALUE) { CloseHandle(nul); return nullptr; }
        in = arc;
    }
    /* Inherit ONLY the three std handles. A plain bInheritHandles=TRUE would also hand tar every file another scan
     * thread has open at that moment (e.g. a half-extracted ROM's .part), which then can't be renamed or deleted. */
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = nul;
    HANDLE inherit[3] = {nul, out, arc};
    DWORD n_inherit = arc == INVALID_HANDLE_VALUE ? 2 : 3;
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &asz);
    std::vector<char> abuf(asz);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)abuf.data();
    bool have_list = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &asz) &&
                     UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, n_inherit * sizeof(HANDLE), nullptr, nullptr);
    if (!have_list) { CloseHandle(nul); if (arc != INVALID_HANDLE_VALUE) CloseHandle(arc); return nullptr; }
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + tar_exe().wstring() + L"\" " + args;
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, cwd ? cwd->c_str() : nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(nul);
    if (arc != INVALID_HANDLE_VALUE) CloseHandle(arc);
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

/* Extraction pattern for one entry name: glob characters escaped, non-ASCII runs become '*'. A '?' in a listed name is a character tar could not
 * print in the ANSI code page (Windows file names cannot hold a real '?'), so it is part of such a run. */
std::wstring tar_member_arg(const std::string& name) {
    std::wstring w = L"\"";
    bool star = false;
    for (unsigned char c : name) {
        if (c >= 0x80 || c == '?') { if (!star) w += L'*'; star = true; continue; }
        star = false;
        if (c == '*' || c == '[' || c == ']' || c == '\\') w += L'\\';
        w += (wchar_t)c;
    }
    return w + L"\"";
}

std::wstring tar_archive_arg(const std::string& archive) {
    const fs::path p = utf8_path(archive);
    if (acp_safe(p.wstring())) { t_tar_stdin.clear(); return L"\"" + p.wstring() + L"\""; }
    t_tar_stdin = archive;   // the next tar_spawn() on this thread opens the file and gives it to tar as stdin
    return L"-";
}

/* Files inside an archive: zip through miniz (central directory), 7z through tar. */
struct ListMemo { std::string path; uint64_t size = 0; long long mtime = 0; bool ok = false; std::vector<ArchiveEntry> list; };
thread_local ListMemo g_list_memo;   // one archive is listed by several id_* probes in a row: list it once per thread
bool list_archive_raw(const std::string& archive, std::vector<ArchiveEntry>* out);
bool list_archive(const std::string& archive, std::vector<ArchiveEntry>* out) {
    std::error_code ec;
    fs::path ap = utf8_path(archive);
    uint64_t sz = (uint64_t)fs::file_size(ap, ec);
    long long mt = (long long)fs::last_write_time(ap, ec).time_since_epoch().count();
    if (!ec && g_list_memo.path == archive && g_list_memo.size == sz && g_list_memo.mtime == mt) {
        if (g_list_memo.ok) *out = g_list_memo.list; else out->clear();
        return g_list_memo.ok;
    }
    bool ok = list_archive_raw(archive, out);
    if (!ec) { g_list_memo.path = archive; g_list_memo.size = sz; g_list_memo.mtime = mt; g_list_memo.ok = ok; g_list_memo.list = ok ? *out : std::vector<ArchiveEntry>(); }
    return ok;
}
bool list_archive_raw(const std::string& archive, std::vector<ArchiveEntry>* out) {
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
    *out = parse_tar_listing(acp_to_utf8(text));
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
        if (c >= 0x80 || c == '?') { if (!star) r += '*'; star = true; } else { r += (char)c; star = false; }
    }
    return r;
}

/* The member must be unambiguous: exactly one entry in the archive matches the pattern we hand to tar. */
bool is_ps2_image_ext(const std::string& ext) { return ext == ".iso" || ext == ".chd" || ext == ".cso"; }
bool is_psp_image_ext(const std::string& ext) { return ext == ".iso" || ext == ".chd" || ext == ".cso" || ext == ".pbp"; }

/* mode 0 GameCube image, 1 PS2 image, 2 Wii U .wua (a "Vimm's Lair.txt" next to it is ignored) */
bool find_gc_entry(const std::vector<ArchiveEntry>& l, ArchiveEntry* out, int mode = 0) {
    for (const auto& a : l) {
        if (!archive_entry_safe(a.name) || is_junk_entry(a.name) || a.size < 0x1000) continue;
        { std::string xe = ascii_lower(path_utf8(utf8_path(a.name).extension())); if (!(mode == 4 ? is_psp_image_ext(xe) : mode == 3 ? is_3ds_zip_entry(xe) : mode == 2 ? xe == ".wua" : mode == 1 ? is_ps2_image_ext(xe) : is_gc_image_ext(xe))) continue; }
        const std::string pat = member_skeleton(a.name);
        int matches = 0;
        for (const auto& b : l) if (glob_star(pat, 0, b.name, 0)) ++matches;
        if (matches != 1) return false;
        *out = a;
        return true;
    }
    return false;
}

const char* const kTruncatedMsg = "this archive is incomplete (its end is missing, so a copy or download did not finish). Copy or download it again";

int entry_mode(System s) { return s == System::PSP ? 4 : s == System::N3DS ? 3 : s == System::WiiU ? 2 :s == System::PS2 ? 1 : 0; }
fs::path gc_unpack_dir(const GameEntry& e) { return lib_dir() / L"rom-cache" / (e.system == System::N3DS ? L"3ds" : e.system == System::WiiU ? L"wiiu" : e.system == System::PS2 ? L"ps2" : e.system == System::PSP ? L"psp" : L"gc") / utf8_path(sanitize_key(e.key)); }

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

/* A Wii U game in a .7z / .zip: "<name>.wua" next to a readme. Listed by its entry name (the real title id and the meta.xml name
 * are read from the unpacked .wua on Play); unpacked into rom-cache\wiiu\<key>\disc.wua. */
static bool id_wiiu_archive(const fs::path& arch, GameEntry* e) {
    if (!archive_sig_ok(path_utf8(arch))) return false;
    std::vector<ArchiveEntry> l;
    ArchiveEntry ent;
    if (!list_archive(path_utf8(arch), &l) || !find_gc_entry(l, &ent, 2)) return false;
    GameEntry g;
    g.system = System::WiiU;
    /* The KEY (and the catalog match that can turn it into a title-id key) is derived from the file inside, exactly as it always was: keys hold saves,
     * favourites and play time and must never move. Only the DISPLAY title prefers the archive's name. */
    const std::string file_title = title_from_filename(utf8_path(path_utf8(utf8_path(ent.name).filename())));
    g.title = container_title(arch, ent.name);
    g.key = "wiiu:file-" + sanitize_key(file_title);
    for (const auto& c : catalog())   // no title id before unpacking: a catalog game of the same name still gets its catalog id (remote-play rooms name games by it, so guests can find the stand-in)
        if (c.system == "wiiu" && ascii_lower(c.title) == ascii_lower(file_title)) {
            g.catalog_id = c.id;
            if (!c.serials.empty()) { g.serial = c.serials[0]; g.key = "wiiu:" + ascii_lower(c.serials[0]); }   // same key as the unpacked .wua (title id), so saves/rooms match
            break;
        }
    g.path = path_utf8(arch);
    g.issue = "extract";
    g.unpacked = ent.size;
    *e = std::move(g);
    return true;
}

static bool id_gc_archive(const fs::path& arch, GameEntry* e) {
    std::string ext = ascii_lower(path_utf8(arch.extension()));
    if (!archive_sig_ok(path_utf8(arch))) return false;
    std::vector<ArchiveEntry> l;
    ArchiveEntry ent;
    if (!list_archive(path_utf8(arch), &l) || !find_gc_entry(l, &ent)) return false;
    const std::string inner_ext = ascii_lower(path_utf8(utf8_path(ent.name).extension()));
    /* header: stream the first bytes of the entry into a temp file and reuse the normal identification */
    GameEntry g;
    bool ok = false;
    std::string head;
    /* a .gcz keeps its block table (12 bytes per 32 KB block, ~0.5 MB for a full disc) in front of block 0: read that far, the rest is cut off */
    if (tar_capture(L"-xOf " + tar_archive_arg(path_utf8(arch)) + L" " + tar_member_arg(ent.name), inner_ext == ".gcz" ? (2u << 20) : 0x9000, 60000, &head) && head.size() >= 0x440) {
        static std::atomic<unsigned> seq{0};
        fs::path probe_dir = lib_dir() / L"rom-cache" / L"_probe";
        std::error_code ec;
        fs::create_directories(probe_dir, ec);
        fs::path probe = probe_dir / (L"p" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(seq++) + utf8_path(inner_ext).wstring());
        {
            std::ofstream o(probe, std::ios::binary);
            o.write(head.data(), (std::streamsize)head.size());
        }
        ok = id_gc(probe, inner_ext, ent.size, &g, title_src(container_title(arch, ent.name))) && !g.serial.empty();
        fs::remove(probe, ec);
    }
    const std::string header_title = ok ? g.title : std::string();   // the disc header's own name (the archive and file names are tried first)
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
    else {   /* not in the catalog: the archive's name (a pack folder / file name the player knows), else the file inside, else the folder, else the disc header */
        bool generic = false;
        std::string t = container_title(arch, ent.name, &generic);
        if (t.size() > 5 && ascii_lower(t.substr(t.size() - 5)) == ".nkit") t.resize(t.size() - 5);
        g.title = (generic && !header_title.empty() && !generic_stem(header_title)) ? header_title : t;
    }
    g.path = path_utf8(arch);
    g.issue = "extract";
    g.unpacked = ent.size;
    *e = std::move(g);
    return true;
}

/* ---- Identify a disc archive by name (no archive content is read) ----
 * 1. a serial in the archive / entry name ("[SCUS-94570]") that one of the two bundled Redump title databases knows;
 * 2. the name (without extension / region tags) equal to a database title: PS1 titles carry their region, PS2 titles are matched with the
 *    region tag of the name against the serial prefix;
 * 3. when both consoles' databases match, the folder the archive sits in ("GamesPS1" / "GamesPS2") breaks the tie, and only then. */
struct NameId { bool found = false; bool tiebreak = false; System sys = System::PS1; std::string serial; };
static NameId id_disc_by_name(const std::string& archive_path, const std::string& entry_name) {
    NameId r;
    const fs::path ap = utf8_path(archive_path);
    std::string low_dir = ascii_lower(path_utf8(ap.parent_path()));
    int hint = 0;   // 1 = PS1 folder, 2 = PS2 folder (innermost wins)
    {
        size_t p1 = low_dir.rfind("ps1"), p2 = low_dir.rfind("ps2"), px = low_dir.rfind("psx");
        if (px != std::string::npos && (p1 == std::string::npos || px > p1)) p1 = px;
        if (p1 != std::string::npos || p2 != std::string::npos) hint = (p2 == std::string::npos || (p1 != std::string::npos && p1 > p2)) ? 1 : 2;
    }
    std::string stems[2] = {path_utf8(ap.stem()), entry_name.empty() ? std::string() : path_utf8(utf8_path(entry_name).stem())};
    for (const std::string& st : stems) {
        if (st.empty()) continue;
        std::string p1s, p2s;
        std::string sn = disc::serial_from_filename(st);
        if (!sn.empty()) {
            if (!ps1::title_for_serial(sn).empty()) p1s = sn;
            if (!hub::ps2::title_for(sn).empty()) p2s = sn;
        }
        if (p1s.empty() && p2s.empty()) {
            p1s = ps1::serial_for_name(st);
            p2s = hub::ps2::serial_for_name(st);
        }
        if (p1s.empty() && p2s.empty()) continue;
        r.tiebreak = !p1s.empty() && !p2s.empty();
        r.serial = (p1s.empty() ? p2s : p2s.empty() ? p1s : hint == 2 ? p2s : p1s);
        r.sys = (p1s.empty() ? System::PS2 : p2s.empty() ? System::PS1 : hint == 2 ? System::PS2 : System::PS1);
        r.found = true;
        return r;
    }
    return r;
}

/* key_src: where a serial-less key ("ps2:file-<name>") comes from (never changes); title_name: where the title falls back to. */
static void fill_ps2(const hub::ps2::Ident& id, const fs::path& key_src, const fs::path& title_name, GameEntry* e) {
    e->system = System::PS2;
    e->serial = id.serial;
    e->key = hub::ps2::key_for(id.serial, sanitize_key(title_from_filename(key_src)));
    e->title = !id.title.empty() ? id.title : title_from_filename(title_name);
}

static bool id_ps2_file(const fs::path& p, const fs::path& title_name, GameEntry* e) {
    hub::ps2::Ident id;
    if (!hub::ps2::identify_file(path_utf8(p), &id)) return false;
    fill_ps2(id, p, title_name, e);
    return true;
}

/* A PS2 image inside a .zip/.7z: the first 4 MB are streamed to find SYSTEM.CNF (plain .iso); .chd/.cso use the serial in the name.
 * find_ps1_entry(.., prefer_m3u = false) is the entry identification has always looked at (the .cue, not the .m3u playlist). */
namespace { bool find_ps1_entry(const std::vector<ArchiveEntry>& l, ArchiveEntry* out, uint64_t* total, bool prefer_m3u = true); }

/* A PlayStation 2 CD kept as cue + bin(s) in an archive (a Redump / Vimm style set). Named like a PS2 game, or SYSTEM.CNF in the first data
 * track says BOOT2 (2352-byte sectors). Listed under PlayStation 2; unpacked with the PlayStation 1 routines (whole set into rom-cache\ps1\<key>). */
static bool id_ps2_cue_archive(const fs::path& arch, const std::vector<ArchiveEntry>& l, GameEntry* e) {
    ArchiveEntry ent;
    uint64_t total = 0;
    if (!find_ps1_entry(l, &ent, &total, false)) return false;
    std::string serial;
    NameId n = id_disc_by_name(path_utf8(arch), ent.name);
    if (n.found) {
        if (n.sys != System::PS2) return false;
        serial = n.serial;
    } else {
        for (const auto& a : l) {
            std::string xe = ascii_lower(path_utf8(utf8_path(a.name).extension()));
            if (!archive_entry_safe(a.name) || is_junk_entry(a.name) || a.size < 0x8000 || (xe != ".bin" && xe != ".img" && xe != ".iso")) continue;
            std::string head;
            if (!tar_capture(L"-xOf " + tar_archive_arg(path_utf8(arch)) + L" " + tar_member_arg(a.name), 8u << 20, 90000, &head) || head.size() < 0x8000) break;
            disc::Info info;
            if (disc::probe_buffer(std::make_shared<std::string>(std::move(head)), &info) && info.kind == disc::Cnf::Ps2 && !info.serial.empty()) serial = info.serial;
            break;
        }
        if (serial.empty()) return false;
    }
    hub::ps2::Ident id;
    id.serial = serial;
    id.title = hub::ps2::title_for(serial);
    GameEntry g;
    fill_ps2(id, utf8_path(ent.name), title_src(container_title(arch, ent.name)), &g);
    g.path = path_utf8(arch);
    g.issue = "extract";
    g.unpacked = total;
    *e = std::move(g);
    return true;
}

static bool id_ps2_archive(const fs::path& arch, GameEntry* e) {
    std::string ext = ascii_lower(path_utf8(arch.extension()));
    if (!archive_sig_ok(path_utf8(arch))) return false;
    std::vector<ArchiveEntry> l;
    ArchiveEntry ent;
    if (!list_archive(path_utf8(arch), &l)) return false;
    if (!find_gc_entry(l, &ent, true)) return id_ps2_cue_archive(arch, l, e);   // no .iso / .chd: a CD kept as cue + bin
    const std::string inner_ext = ascii_lower(path_utf8(utf8_path(ent.name).extension()));
    hub::ps2::Ident id;
    bool ok = false;
    if (inner_ext == ".chd" && ascii_lower(path_utf8(arch.extension())) == ".zip") {   // a STORED zip entry is a plain byte range: libchdr reads the CHD in place
        mz_zip_archive za;
        std::memset(&za, 0, sizeof za);
        FILE* zf = _wfopen(arch.wstring().c_str(), L"rbN");
        if (zf) {
            if (mz_zip_reader_init_cfile(&za, zf, 0, 0)) {
                mz_uint n = mz_zip_reader_get_num_files(&za);
                for (mz_uint i = 0; i < n && !ok; ++i) {
                    mz_zip_archive_file_stat st;
                    if (!mz_zip_reader_file_stat(&za, i, &st) || st.m_is_directory || st.m_filename != ent.name || st.m_method != 0) continue;
                    unsigned char lh[30];
                    if (_fseeki64(zf, (long long)st.m_local_header_ofs, SEEK_SET) != 0 || std::fread(lh, 1, 30, zf) != 30) break;
                    uint64_t data = st.m_local_header_ofs + 30 + (lh[26] | (lh[27] << 8)) + (lh[28] | (lh[29] << 8));
                    ok = hub::ps2::identify_chd_slice(path_utf8(arch), data, st.m_uncomp_size, ent.name, &id);
                    if (!ok && id.ps1) { mz_zip_reader_end(&za); std::fclose(zf); return false; }
                }
                mz_zip_reader_end(&za);
            }
            std::fclose(zf);
        }
    }
    if (!ok && inner_ext == ".iso") {
        auto head = std::make_shared<std::string>();
        if (tar_capture(L"-xOf " + tar_archive_arg(path_utf8(arch)) + L" " + tar_member_arg(ent.name), 4u << 20, 60000, head.get()) && head->size() > 0x10000)
            ok = hub::ps2::identify_buffer(head, &id);
        if (!ok && id.ps1) return false;
    }
    if (!ok && inner_ext == ".cso") {   // compressed ISO: the first blocks hold the volume descriptor and SYSTEM.CNF
        auto head = std::make_shared<std::string>();
        if (tar_capture(L"-xOf " + tar_archive_arg(path_utf8(arch)) + L" " + tar_member_arg(ent.name), 16u << 20, 90000, head.get()) && head->size() > 0x1000) {
            std::string cnf, serial;
            if (disc::read_system_cnf(psp::cso_buffer_reader(head), &cnf)) {
                disc::Cnf kind = disc::parse_system_cnf(cnf, &serial);
                if (kind == disc::Cnf::Ps1) return false;
                if (kind == disc::Cnf::Ps2 && !serial.empty()) { id.serial = serial; id.title = hub::ps2::title_for(serial); ok = true; }
            }
        }
    }
    if (!ok) {
        NameId n = id_disc_by_name(path_utf8(arch), ent.name);
        if (!n.found || n.sys != System::PS2) return false;
        std::string s = n.serial;
        id = hub::ps2::Ident();
        id.serial = s;
        id.title = hub::ps2::title_for(s);
    }
    GameEntry g;
    fill_ps2(id, utf8_path(ent.name), title_src(container_title(arch, ent.name)), &g);
    g.path = path_utf8(arch);
    g.issue = "extract";
    g.unpacked = ent.size;
    *e = std::move(g);
    return true;
}

/* ---- PlayStation Portable (PPSSPP): PARAM.SFO of an ISO / CSO / CHD image or an EBOOT.PBP. Probed BEFORE the PS1 / PS2 / GC claims: a PSP image has
 * PSP_GAME/PARAM.SFO and nothing the other consoles look for; a PS1 eboot .pbp is refused here and stays PlayStation 1. ---- */
static void fill_psp(const hub::psp::Ident& id, const fs::path& key_src, const fs::path& title_name, GameEntry* e) {   // key_src: never changes; title_name: the title fallback
    e->system = System::PSP;
    e->serial = id.disc_id;
    e->key = hub::psp::key_for(id.disc_id, sanitize_key(title_from_filename(key_src)));
    std::string t = id.title;
    for (char& c : t) if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    while (!t.empty() && t.back() == ' ') t.pop_back();
    e->title = !t.empty() ? t : title_from_filename(title_name);
}

/* ---- Nintendo Switch / Switch 2 (Lichii): .nsp .nsz .xci .xcz .nro .nup (Lichii's own package) and loose exefs/main dumps. Headers only; update /
 * DLC packages are not games (a .nup holding only a standalone update is: Lichii boots it). Not read from inside .zip/.7z/.rar (Lichii cannot open
 * archives). ---- */
static bool is_nx_ext(const std::string& ext) { return ext == ".nsp" || ext == ".nsz" || ext == ".xci" || ext == ".xcz" || ext == ".nro" || ext == ".nup"; }
static bool is_exefs_main(const fs::path& p) { return p.filename() == L"main" && ascii_lower(path_utf8(p.parent_path().filename())) == "exefs"; }
static bool id_nx_file(const fs::path& p, GameEntry* e) {
    hub::nx::Ident id;
    if (!hub::nx::identify_file(path_utf8(p), &id) || id.update_or_dlc) return false;
    const fs::path name_src = id.loose ? title_src(path_utf8(p.parent_path().parent_path().filename())) : p;   // a dump is named by its top folder
    e->system = System::Switch;
    e->nx2 = id.nx2;
    e->key = hub::nx::key_for(id.title_id, sanitize_key(title_from_filename(name_src)));
    if (id.title_id) { char b[17]; std::snprintf(b, sizeof b, "%016llX", (unsigned long long)id.title_id); e->serial = b; }
    e->title = !id.title.empty() ? id.title : title_from_filename(name_src);
    return true;
}

static bool id_psp_file(const fs::path& p, const fs::path& title_name, GameEntry* e) {
    hub::psp::Ident id;
    if (!hub::psp::identify_file(path_utf8(p), &id)) return false;
    fill_psp(id, p, title_name, e);
    return true;
}

/* A PSP image inside a .zip/.7z/.rar: the start of the member is streamed (an ISO / CSO / PBP prefix) and PARAM.SFO read from it. */
static bool id_psp_archive(const fs::path& arch, GameEntry* e) {
    if (!archive_sig_ok(path_utf8(arch))) return false;
    std::vector<ArchiveEntry> l;
    ArchiveEntry ent;
    if (!list_archive(path_utf8(arch), &l)) return false;
    if (!find_gc_entry(l, &ent, 4)) return false;
    const std::string xe = ascii_lower(path_utf8(utf8_path(ent.name).extension()));
    if (xe == ".chd") return false;   // content not readable without unpacking: left to the PS1 / PS2 claims
    auto head = std::make_shared<std::string>();
    if (!tar_capture(L"-xOf " + tar_archive_arg(path_utf8(arch)) + L" " + tar_member_arg(ent.name), 8u << 20, 60000, head.get()) || head->size() < 0x100) return false;
    hub::psp::Ident id;
    if (!hub::psp::identify_buffer(head, &id)) return false;
    GameEntry g;
    fill_psp(id, utf8_path(ent.name), title_src(container_title(arch, ent.name)), &g);
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

/* "<archive size>|<archive mtime>|": what every stamp starts with */
static std::string archive_stamp_prefix(const GameEntry& e) {
    std::string s = gc_stamp(e, 0, "");
    return s.substr(0, s.find('|', s.find('|') + 1) + 1);
}
static bool is_zip_ext(const std::string& path) { return ascii_lower(path_utf8(utf8_path(path).extension())) == ".zip"; }
/* games whose archive is unpacked by gc_extract (tar.exe): discs, Wii U, and 3DS images in .7z/.rar */
static bool is_disc_like(const GameEntry& e) {
    return e.system == System::GC || e.system == System::PS2 || e.system == System::PSP || e.system == System::WiiU || (e.system == System::N3DS && !is_zip_ext(e.path));
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
    return list_archive(e.path, &l) && find_gc_entry(l, &ent, entry_mode(e.system)) ? ent.size : 0;
}

static bool gc_extract(const GameEntry& e, const std::function<bool(uint64_t, uint64_t)>& progress, std::string* out_path, std::string* err) {
    std::vector<ArchiveEntry> l;
    ArchiveEntry ent;
    if (!fs::exists(tar_exe())) { *err = "Windows' tar.exe was not found (needs Windows 10 version 1803 or newer)"; return false; }
    if (!list_archive(e.path, &l)) { *err = archive_truncated(e.path) ? kTruncatedMsg : "cannot read the archive"; return false; }
    if (!find_gc_entry(l, &ent, entry_mode(e.system))) { *err = e.system == System::WiiU ? "no .wua inside the archive" : e.system == System::PSP ? "no PSP game image inside the archive" : "no disc image inside the archive"; return false; }
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

static bool identify_rom(const fs::path& p, const fs::path& title_name, GameEntry* out);
static bool id_ps1_archive(const fs::path& arch, GameEntry* e);

/* An archive whose directory cannot be read (an unfinished download, an unsupported method) still shows up when its name is exactly a Redump /
 * PS2 title: Play then explains what is wrong with the archive. */
static bool id_disc_archive_by_name_only(const fs::path& arch, GameEntry* e) {
    NameId n = id_disc_by_name(path_utf8(arch), std::string());
    if (!n.found) return false;
    GameEntry g;
    g.system = n.sys;
    g.serial = n.serial;
    if (n.sys == System::PS1) {
        g.key = "ps1:" + sanitize_key(n.serial);
        g.title = ps1_display_title(ps1::title_for_serial(n.serial), true);
    } else {
        hub::ps2::Ident id;
        id.serial = n.serial;
        id.title = hub::ps2::title_for(n.serial);
        fill_ps2(id, arch, title_src(container_title(arch, std::string())), &g);
    }
    g.path = path_utf8(arch);
    g.issue = "extract";
    *e = std::move(g);
    return true;
}

/* ---- cartridge ROMs and 3DS images inside .7z / .rar (and zips miniz cannot read) ----
 * Small cartridge games are unpacked ONCE at scan time into rom-cache\a<hash of the archive path>\rom.<ext> (stamp: archive size|mtime|entry|size),
 * identified from the unpacked file (so the key equals the unzipped game's key) and launched from there. */
static bool is_cart_ext(const std::string& x) {
    return x == ".z64" || x == ".v64" || x == ".n64" || x == ".gba" || x == ".gb" || x == ".gbc" || x == ".nds" || x == ".nes" || x == ".sfc" || x == ".smc";
}

static bool find_rom_entry(const std::vector<ArchiveEntry>& l, ArchiveEntry* out) {
    for (const auto& a : l) {
        if (!archive_entry_safe(a.name) || is_junk_entry(a.name)) continue;
        std::string ext = ascii_lower(path_utf8(utf8_path(a.name).extension()));
        if (!is_cart_ext(ext) || a.size < 16 * 1024 || a.size > (ext == ".nds" ? 1024ull : 128ull) * 1024 * 1024) continue;
        const std::string pat = member_skeleton(a.name);
        int matches = 0;
        for (const auto& b : l) if (glob_star(pat, 0, b.name, 0)) ++matches;
        if (matches != 1) continue;
        *out = a;
        return true;
    }
    return false;
}

static uint64_t fnv64(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

static bool extract_archived_rom(const fs::path& arch, fs::path* out_rom, fs::path* inner_name) {
    std::string ap = path_utf8(arch);
    if (!archive_sig_ok(ap) || !fs::exists(tar_exe())) return false;
    std::vector<ArchiveEntry> l;
    ArchiveEntry ent;
    if (!list_archive(ap, &l) || !find_rom_entry(l, &ent)) return false;
    std::error_code ec;
    uintmax_t asz = fs::file_size(arch, ec);
    long long mt = (long long)fs::last_write_time(arch, ec).time_since_epoch().count();
    const std::string ext = ascii_lower(path_utf8(utf8_path(ent.name).extension()));
    char hx[24];
    std::snprintf(hx, sizeof hx, "a%016llx", (unsigned long long)fnv64(ascii_lower(ap)));
    fs::path dir = lib_dir() / L"rom-cache" / utf8_path(hx);
    fs::path wdir = long_path(dir);
    fs::path rom = wdir / (L"rom" + utf8_path(ext).wstring());
    const std::string want = std::to_string(asz) + "|" + std::to_string(mt) + "|" + ent.name + "|" + std::to_string(ent.size);
    std::string have;
    { std::ifstream in(wdir / L"stamp.txt", std::ios::binary); if (in) std::getline(in, have); }
    if (have != want || !fs::exists(rom, ec) || fs::file_size(rom, ec) != ent.size) {
        fs::remove_all(wdir, ec);
        fs::create_directories(wdir, ec);
        fs::path part = rom; part += L".part";
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        HANDLE proc = tar_spawn(L"-xOf " + tar_archive_arg(ap) + L" " + tar_member_arg(ent.name), f);
        bool good = false;
        if (proc) {
            if (WaitForSingleObject(proc, 300000) == WAIT_TIMEOUT) TerminateProcess(proc, 1);
            DWORD code = 1;
            WaitForSingleObject(proc, 5000);
            GetExitCodeProcess(proc, &code);
            CloseHandle(proc);
            LARGE_INTEGER sz{};
            GetFileSizeEx(f, &sz);
            good = code == 0 && (uint64_t)sz.QuadPart == ent.size;
        }
        FlushFileBuffers(f);
        CloseHandle(f);
        if (!good) { fs::remove_all(wdir, ec); return false; }
        fs::rename(part, rom, ec);
        if (ec) { fs::remove_all(wdir, ec); return false; }
        std::ofstream st(wdir / L"stamp.txt", std::ios::binary | std::ios::trunc);
        st << want << '\n';
    }
    *out_rom = rom;
    *inner_name = utf8_path(ent.name);
    return true;
}

/* A 3DS image in a .7z / .rar: only its first 17 KB are streamed; unpacked on request like a zipped one. */
static bool id_3ds_archive(const fs::path& arch, GameEntry* e) {
    std::string ap = path_utf8(arch);
    if (!archive_sig_ok(ap)) return false;
    std::vector<ArchiveEntry> l;
    ArchiveEntry ent;
    if (!list_archive(ap, &l) || !find_gc_entry(l, &ent, 3) || ent.size < kN3dsHeaderBytes) return false;
    std::string head;
    if (!tar_capture(L"-xOf " + tar_archive_arg(ap) + L" " + tar_member_arg(ent.name), kN3dsHeaderBytes, 60000, &head) || head.size() < kN3dsHeaderBytes) return false;
    N3dsInfo info;
    if (!parse_3ds_header((const uint8_t*)head.data(), head.size(), &info)) return false;
    if (info.container != "3dsx" && !info.is_application) return false;
    GameEntry g;
    fill_3ds(info, arch, title_src(container_title(arch, ent.name)), &g);
    g.path = ap;
    g.issue = "extract";
    g.unpacked = ent.size;
    *e = std::move(g);
    return true;
}

namespace { const char* unsupported_reason(const std::string& ext); }   // defined with the scan code below

/* An archive lists ONE game (the first image / ROM in it). When it holds several, the rest used to just not exist: say so. "" = nothing to say. */
static std::string archive_extra_games_note(const std::string& path) {
    std::vector<ArchiveEntry> l;
    if (!list_archive(path, &l)) return std::string();
    int n = 0;
    std::string first;
    std::set<std::string> groups;   // "Game (Disc 1)" and "Game (Disc 2)" are one game
    for (const auto& a : l) {
        if (!archive_entry_safe(a.name) || is_junk_entry(a.name)) continue;
        const std::string x = ascii_lower(path_utf8(utf8_path(a.name).extension()));
        if (x == ".m3u") return std::string();   // a playlist: the discs are one multi-disc game
        const bool game = (is_cart_ext(x) && a.size >= 16 * 1024) || (is_3ds_zip_entry(x) && a.size >= 0x1000) || x == ".wua" || x == ".iso" || x == ".chd" || x == ".cso" || x == ".pbp" ||
                          x == ".gcm" || x == ".rvz" || x == ".wia" || x == ".ciso" || x == ".gcz";
        if (!game || a.size < 0x1000) continue;
        std::string stem = ascii_lower(path_utf8(utf8_path(a.name).stem())), g;
        for (size_t i = 0; i < stem.size();) {   // drop "(disc 2)", "(disc 2 of 3)", "[cd1]" groups
            char open = stem[i];
            size_t close = (open == '(' || open == '[') ? stem.find(open == '(' ? ')' : ']', i) : std::string::npos;
            if (close != std::string::npos) {
                std::string in = stem.substr(i + 1, close - i - 1);
                if (in.rfind("disc", 0) == 0 || in.rfind("disk", 0) == 0 || in.rfind("cd", 0) == 0) { i = close + 1; continue; }
            }
            g.push_back(stem[i++]);
        }
        if (!groups.insert(collapse_spaces(g)).second) continue;
        if (n++ == 0) first = path_utf8(utf8_path(a.name).filename());
    }
    if (n < 2) return std::string();
    return "holds " + std::to_string(n) + " game files, but an archive lists only its first one (" + first + "). Unpack the archive and add the others as separate files";
}

std::string archive_skip_reason(const std::string& path) {
    std::vector<ArchiveEntry> l;
    if (!archive_sig_ok(path)) return "this is not a valid archive (it may be damaged or renamed)";
    if (!list_archive(path, &l)) return archive_truncated(path) ? kTruncatedMsg : "this archive could not be read (damaged, encrypted, or an unsupported compression)";
    if (ascii_lower(path_utf8(utf8_path(path).extension())) == ".zip") {   // a password protected zip lists fine and then yields nothing
        mz_zip_archive za;
        std::memset(&za, 0, sizeof za);
        if (FILE* f = _wfopen(utf8_path(path).wstring().c_str(), L"rbN")) {
            bool enc = false;
            if (mz_zip_reader_init_cfile(&za, f, 0, 0)) {
                mz_uint n = mz_zip_reader_get_num_files(&za);
                for (mz_uint i = 0; i < n && !enc; ++i) enc = mz_zip_reader_is_file_encrypted(&za, i) != 0;
                mz_zip_reader_end(&za);
            }
            std::fclose(f);
            if (enc) return "is password protected; RetroHub cannot open it. Unpack it with the password and add the game files";
        }
    }
    std::string nested;
    for (const auto& a : l) {
        std::string x = ascii_lower(path_utf8(utf8_path(a.name).extension()));
        if (x == ".zip" || x == ".7z" || x == ".rar") { nested = path_utf8(utf8_path(a.name).filename()); break; }
    }
    if (!nested.empty()) return "contains another archive (" + nested + "); nested archives are not opened. Unpack the outer archive once and try again";
    /* what is inside, so the message says why it was not taken */
    std::string disc_name, cart_name, rpx_name, other_name, other_reason;
    bool wii_u_image = false;
    for (const auto& a : l) {
        if (is_junk_entry(a.name)) continue;
        std::string n = path_utf8(utf8_path(a.name).filename());
        std::string x = ascii_lower(path_utf8(utf8_path(a.name).extension()));
        if (x == ".chd" || x == ".cso" || x == ".cue" || x == ".iso" || x == ".gcm" || x == ".ciso" || x == ".gcz" || x == ".rvz" || x == ".wia" || x == ".pbp" || x == ".m3u" || x == ".bin" || x == ".img") { if (disc_name.empty() || x == ".cue") disc_name = n; }
        else if (is_cart_ext(x) || is_3ds_zip_entry(x) || x == ".wua") { if (cart_name.empty()) cart_name = n; }
        else if (x == ".rpx") rpx_name = n;
        else if (x == ".wux" || x == ".wud") wii_u_image = true;
        else if (const char* r = unsupported_reason(x)) { if (other_name.empty()) { other_name = n; other_reason = r; } }
    }
    if (!rpx_name.empty()) return "holds a Wii U game folder (" + rpx_name + "), which RetroHub reads from a normal folder or a .wua file, not from inside an archive. Unpack the archive and add the folder, or convert the game to a .wua";
    if (wii_u_image) return "holds a Wii U disc image (.wux / .wud); those are not opened from inside an archive. Unpack the archive and add the file";
    if (!other_name.empty()) return "holds " + other_name + ": " + other_reason;
    if (!disc_name.empty()) return "holds a disc image (" + disc_name + ") that RetroHub cannot identify without unpacking it. Name the archive after the game (a Redump style name works), or unpack it and add the disc image";
    if (!cart_name.empty()) return "holds " + cart_name + ", which is not a game RetroHub recognizes (bad header, wrong size, or an update / DLC)";
    return "no game files RetroHub recognizes inside";
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
            if (id_psp_archive(p, &g) || id_gc_archive(p, &g) || id_ps2_archive(p, &g) || id_wiiu_archive(p, &g)) { g.size = size; g_identify_calls.fetch_add(1); *out = std::move(g); return true; }
        }
        {   /* a zip holding a PlayStation disc (cue + bins / chd), named like its Redump title: listed, unpacked on Play */
            GameEntry g;
            if (id_ps1_archive(p, &g)) { g.size = size; g_identify_calls.fetch_add(1); *out = std::move(g); return true; }
        }
        fs::path rom, inner;
        if (!extract_zipped_rom(p, &rom, &inner) && !extract_archived_rom(p, &rom, &inner)) return false;   // miniz first; bsdtar for zip64 / odd methods
        if (!identify_rom(rom, title_src(cart_title(p, path_utf8(inner))), out)) return false;   // a title the header cannot give: the file inside, else the zip's name
        out->save_stem = path_utf8(inner.stem());   // per-game files (DS .srm) keep the inner name's stem
        return true;
    }
    if (ext == ".7z" || ext == ".rar") {
        GameEntry g;
        if (!id_psp_archive(p, &g) && !id_gc_archive(p, &g) && !id_wiiu_archive(p, &g) && !id_ps1_archive(p, &g) && !id_ps2_archive(p, &g)) {
            GameEntry z;
            if (id_3ds_archive(p, &z)) { z.size = size; g_identify_calls.fetch_add(1); *out = std::move(z); return true; }
            fs::path rom, inner;
            if (extract_archived_rom(p, &rom, &inner)) {   // N64 / GB / GBC / GBA / NES / SNES / DS cartridge
                if (!identify_rom(rom, title_src(cart_title(p, path_utf8(inner))), out)) return false;
                out->save_stem = path_utf8(inner.stem());
                return true;
            }
            std::vector<ArchiveEntry> l;
            if (!archive_sig_ok(path_utf8(p)) || list_archive(path_utf8(p), &l) || !id_disc_archive_by_name_only(p, &g)) return false;
        }
        g.size = size;
        g_identify_calls.fetch_add(1);
        *out = std::move(g);
        return true;
    }
    return identify_rom(p, loose_name_src(p), out);   // a file called "game" / "disc" is titled from its folder
}

/* name_src: what a TITLE falls back to (the name chosen for an extracted ROM, the file or its folder otherwise). Keys are never derived from it: a
 * name-based key (ps1:file-, gc:file-, 3ds:file-, wiiu:file-) always comes from the file `p` itself, as it always did. */
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
    else if (ext == ".nes") ok = id_nes(p, name_src, size, &e);
    else if (ext == ".sfc" || ext == ".smc") ok = id_snes(p, name_src, size, &e);
    else if ((ext == ".iso" || ext == ".cso" || ext == ".chd" || ext == ".pbp") && id_psp_file(p, name_src, &e)) ok = true;   // before the PS1 / PS2 / GC claims
    else if (ext == ".cue" || ext == ".chd" || ext == ".m3u" || ext == ".pbp" || ext == ".bin" || ext == ".img" || ext == ".cso") {
        // PlayStation 1 or 2: the disc says (SYSTEM.CNF BOOT = PS1, BOOT2 = PS2). PS1 is probed first; a miss falls through to PS2.
        ok = id_ps1(p, ext, &e, name_src);
        if (!ok) { e = GameEntry(); ok = ext != ".m3u" && ext != ".pbp" && id_ps2_file(p, name_src, &e); }
    }
    else if (ext == ".iso" || ext == ".gcm" || ext == ".rvz" || ext == ".wia" || ext == ".ciso" || ext == ".gcz") {
        ok = id_gc(p, ext, size, &e, name_src);
        if (!ok && ext == ".iso") { e = GameEntry(); ok = id_ps1(p, ext, &e, name_src); }   // a PlayStation data disc
    }
    else if (is_nx_ext(ext) || is_exefs_main(p)) ok = id_nx_file(p, &e);
    else if (is_3ds_extension(ext)) ok = id_3ds(p, ext, &e, name_src);
    else if (ext == ".wua" || ext == ".wud" || ext == ".wux" || ext == ".rpx") ok = id_wiiu(p, ext, size, &e, name_src);
    if (!ok && ext == ".iso") { e = GameEntry(); ok = id_ps2_file(p, name_src, &e); }
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

/* ---- PlayStation games in .zip / .7z ----
 * Redump sets are cue + several bins, so the whole archive is unpacked (never silently: the hub asks first) to
 * rom-cache/ps1/<key>/ and the .cue (or .chd / .pbp / .iso) is launched. Identification is by file name (Redump title -> serial);
 * the disc inside is not read. */
namespace {

fs::path ps1_unpack_dir(const GameEntry& e) { return lib_dir() / L"rom-cache" / L"ps1" / utf8_path(sanitize_key(e.key)); }

int ps1_entry_rank(const std::string& ext) {
    if (ext == ".m3u") return 0;   // a multi-disc set: the playlist, not its first disc
    if (ext == ".cue") return 1;
    if (ext == ".chd") return 2;
    if (ext == ".pbp") return 3;
    if (ext == ".iso") return 4;
    if (ext == ".bin" || ext == ".img") return 5;
    return -1;
}

bool find_ps1_entry(const std::vector<ArchiveEntry>& l, ArchiveEntry* out, uint64_t* total, bool prefer_m3u) {
    int best = 99;
    uint64_t sum = 0;
    for (const auto& a : l) {
        if (!archive_entry_safe(a.name) || is_junk_entry(a.name)) continue;
        sum += a.size;
        const std::string x = ascii_lower(path_utf8(utf8_path(a.name).extension()));
        if (x == ".m3u" && !prefer_m3u) continue;   // identification reads the same entry it always did (the .cue): a Redump name lookup depends on it
        int r = ps1_entry_rank(x);
        if (r >= 0 && r < best) { best = r; *out = a; }
    }
    if (total) *total = sum;
    return best != 99;
}

std::string ps1_stamp(const GameEntry& e, const std::string& entry) {
    std::error_code ec;
    fs::path a = utf8_path(e.path);
    uintmax_t sz = fs::file_size(a, ec);
    auto mt = fs::last_write_time(a, ec).time_since_epoch().count();
    return std::to_string(sz) + "|" + std::to_string((long long)mt) + "|" + entry;
}

} // namespace

/* A PS2 CD archived as cue + bin (no .iso / .chd inside) is unpacked and located like a PlayStation 1 set. Decided once per archive from its listing. */
static bool uses_ps1_unpack(const GameEntry& e) {
    if (e.system == System::PS1) return true;
    if (e.system != System::PS2 || e.issue != "extract") return false;
    static std::mutex mu;
    static std::map<std::string, bool> cache;
    { std::lock_guard<std::mutex> lk(mu); auto it = cache.find(e.path); if (it != cache.end()) return it->second; }
    std::vector<ArchiveEntry> l;
    ArchiveEntry a;
    uint64_t t = 0;
    bool r = list_archive(e.path, &l) && !find_gc_entry(l, &a, 1) && find_ps1_entry(l, &a, &t);
    std::lock_guard<std::mutex> lk(mu);
    cache[e.path] = r;
    return r;
}

static bool id_ps1_archive(const fs::path& arch, GameEntry* e) {
    std::string ext = ascii_lower(path_utf8(arch.extension()));
    if (!archive_sig_ok(path_utf8(arch))) return false;
    std::vector<ArchiveEntry> l;
    ArchiveEntry ent;
    uint64_t total = 0;
    if (!list_archive(path_utf8(arch), &l) || !find_ps1_entry(l, &ent, &total, false)) return false;
    NameId n = id_disc_by_name(path_utf8(arch), ent.name);
    std::string serial;
    if (n.found) {
        if (n.sys != System::PS1) return false;
        serial = n.serial;
    } else {   /* not a Redump name: read SYSTEM.CNF from the start of the first data track (same key as the unpacked disc) */
        for (const auto& a : l) {
            std::string xe = ascii_lower(path_utf8(utf8_path(a.name).extension()));
            if (!archive_entry_safe(a.name) || is_junk_entry(a.name) || a.size < 0x8000 || (xe != ".bin" && xe != ".img" && xe != ".iso")) continue;
            std::string head;
            if (!tar_capture(L"-xOf " + tar_archive_arg(path_utf8(arch)) + L" " + tar_member_arg(a.name), 8u << 20, 90000, &head) || head.size() < 0x8000) break;
            disc::Info info;
            if (disc::probe_buffer(std::make_shared<std::string>(std::move(head)), &info) && info.kind == disc::Cnf::Ps1 && !info.serial.empty()) serial = info.serial;
            break;
        }
        if (serial.empty()) return false;
    }
    GameEntry g;
    g.system = System::PS1;
    g.serial = serial;
    g.key = "ps1:" + sanitize_key(serial);
    { std::string rd = ps1::title_for_serial(serial); g.title = rd.empty() ? container_title(arch, ent.name) : ps1_display_title(rd, true); }
    g.path = path_utf8(arch);
    g.issue = "extract";
    g.unpacked = total;
    *e = std::move(g);
    return true;
}

static std::string ps1_extracted_path(const GameEntry& e) {
    std::error_code ec;
    fs::path dir = ps1_unpack_dir(e);
    std::string stamp;
    {
        std::ifstream in(dir / L"stamp.txt", std::ios::binary);
        if (!in) return "";
        std::getline(in, stamp);
    }
    size_t b = stamp.find('|');
    if (b == std::string::npos) return "";
    b = stamp.find('|', b + 1);
    if (b == std::string::npos) return "";
    std::string entry = stamp.substr(b + 1);
    if (ps1_stamp(e, entry) != stamp) return "";
    fs::path f = dir / utf8_path(entry);
    return fs::exists(f, ec) ? path_utf8(f) : std::string();
}

static uint64_t ps1_unpacked_size(const GameEntry& e) {
    if (e.unpacked) return e.unpacked;
    std::vector<ArchiveEntry> l;
    ArchiveEntry ent;
    uint64_t total = 0;
    return list_archive(e.path, &l) && find_ps1_entry(l, &ent, &total) ? total : 0;
}

/* The unpacked disc is read: SYSTEM.CNF must agree with where the archive was filed (BOOT = PS1, BOOT2 = PS2). A mismatch removes the unpacked
 * copy and says so. A disc that cannot be probed (.pbp, .m3u, unreadable) is allowed through. */
static bool confirm_extracted_disc(const GameEntry& e, const std::string& path, std::string* err) {
    disc::Info info;
    if (!disc::probe_file(path, &info) || info.kind == disc::Cnf::None) return true;
    bool want_ps2 = e.system == System::PS2;
    if ((info.kind == disc::Cnf::Ps2) == want_ps2) return true;
    std::error_code ec;
    fs::remove_all((uses_ps1_unpack(e) ? ps1_unpack_dir(e) : gc_unpack_dir(e)), ec);
    *err = std::string("the disc inside is a PlayStation ") + (want_ps2 ? "1" : "2") + " game (" + info.serial + "), not PlayStation " + (want_ps2 ? "2" : "1") + ". Rescan after renaming the archive";
    return false;
}

static bool ps1_extract(const GameEntry& e, const std::function<bool(uint64_t, uint64_t)>& progress, std::string* out_path, std::string* err) {
    std::vector<ArchiveEntry> l;
    ArchiveEntry ent;
    uint64_t total = 0;
    if (!list_archive(e.path, &l)) { *err = archive_truncated(e.path) ? kTruncatedMsg : "cannot read the archive"; return false; }
    if (!find_ps1_entry(l, &ent, &total)) { *err = "no PlayStation disc image inside the archive"; return false; }
    fs::path dir = ps1_unpack_dir(e);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    ULARGE_INTEGER free_bytes{};
    if (GetDiskFreeSpaceExW(dir.wstring().c_str(), &free_bytes, nullptr, nullptr) && free_bytes.QuadPart < total + (64ull << 20)) {
        *err = "not enough free disk space (" + std::to_string((total >> 20) + 64) + " MB needed)";
        return false;
    }
    bool ok = true, cancelled = false;
    bool use_tar = ascii_lower(path_utf8(utf8_path(e.path).extension())) != ".zip";
    if (!use_tar) {
        mz_zip_archive za;
        std::memset(&za, 0, sizeof za);
        FILE* zf = _wfopen(utf8_path(e.path).wstring().c_str(), L"rbN");
        if (!zf) { *err = "cannot open the zip"; return false; }
        if (mz_zip_reader_init_cfile(&za, zf, 0, 0)) mz_zip_reader_end(&za); else use_tar = true;   // zip64 / an odd method miniz cannot read: bsdtar does
        std::fclose(zf);
    }
    if (!use_tar) {
        mz_zip_archive za;
        std::memset(&za, 0, sizeof za);
        FILE* zf = _wfopen(utf8_path(e.path).wstring().c_str(), L"rbN");
        if (!zf) { *err = "cannot open the zip"; return false; }
        if (!mz_zip_reader_init_cfile(&za, zf, 0, 0)) { std::fclose(zf); *err = "not a valid zip"; return false; }
        uint64_t done = 0;
        mz_uint n = mz_zip_reader_get_num_files(&za);
        for (mz_uint i = 0; i < n && ok && !cancelled; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&za, i, &st) || st.m_is_directory || !archive_entry_safe(st.m_filename) || is_junk_entry(st.m_filename)) continue;
            fs::path dst = dir / utf8_path(st.m_filename);
            fs::create_directories(dst.parent_path(), ec);
            ExtractCtx ctx;
            ctx.f = _wfopen(dst.wstring().c_str(), L"wb");
            if (!ctx.f) { *err = "cannot write " + path_utf8(dst); ok = false; break; }
            ctx.done = done;
            ctx.total = total;
            ctx.cb = &progress;
            bool r = mz_zip_reader_extract_to_callback(&za, i, extract_write_cb, &ctx, 0) != 0;
            std::fclose(ctx.f);
            done = ctx.done;
            if (ctx.cancelled) cancelled = true;
            else if (!r) { *err = "extraction failed (disk full or damaged archive)"; ok = false; }
        }
        mz_zip_reader_end(&za);
        std::fclose(zf);
    } else {
        if (!fs::exists(tar_exe())) { *err = "Windows' tar.exe was not found (needs Windows 10 version 1803 or newer)"; return false; }
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        /* extracts into the working directory: a "-C <dir>" argument would be squeezed through the ANSI code page */
        HANDLE proc = nul == INVALID_HANDLE_VALUE ? nullptr : tar_spawn(L"-xf " + tar_archive_arg(e.path), nul, &dir);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!proc) { *err = "cannot start tar.exe"; return false; }
        ULONGLONG last_move = GetTickCount64();
        uint64_t last = 0;
        for (;;) {
            DWORD w = WaitForSingleObject(proc, 300);
            uint64_t sz = 0;
            for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code e2;
                if (it->is_regular_file(e2)) sz += it->file_size(e2);
            }
            if (sz != last) { last = sz; last_move = GetTickCount64(); }
            else if (w == WAIT_TIMEOUT && GetTickCount64() - last_move > 180000) { TerminateProcess(proc, 1); *err = "extraction stalled (no data for 3 minutes); try again"; ok = false; WaitForSingleObject(proc, 5000); break; }
            if (progress && !progress(sz, total)) { cancelled = true; TerminateProcess(proc, 1); WaitForSingleObject(proc, 5000); break; }
            if (w != WAIT_TIMEOUT) break;
        }
        DWORD code = 1;
        GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
        if (!cancelled && ok && code != 0) { *err = "extraction failed (disk full or damaged archive)"; ok = false; }
    }
    if (cancelled || !ok) {
        fs::remove_all(dir, ec);
        if (cancelled) *err = "cancelled";
        return false;
    }
    /* The listing printed the name in the ANSI code page (characters it lacks became '?'); the file on disk has the real one. Find it. */
    std::string rel = ent.name;
    if (!fs::exists(dir / utf8_path(rel), ec)) {
        const std::string pat = member_skeleton(ent.name);
        const std::string want_ext = ascii_lower(path_utf8(utf8_path(ent.name).extension()));
        std::string found;
        for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end && found.empty(); it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            std::string r = path_utf8(fs::relative(it->path(), dir, e2));
            for (char& c : r) if (c == '\\') c = '/';
            if (ascii_lower(path_utf8(it->path().extension())) == want_ext && it->file_size(e2) == ent.size && glob_star(pat, 0, r, 0)) found = r;
        }
        if (found.empty()) { *err = "the unpacked disc image was not found (unusual file name)"; fs::remove_all(dir, ec); return false; }
        rel = found;
    }
    {
        std::ofstream st(dir / L"stamp.txt", std::ios::binary | std::ios::trunc);
        st << ps1_stamp(e, rel) << '\n';
        if (!st) { *err = "cannot write the unpack record"; return false; }
    }
    if (out_path) *out_path = path_utf8(dir / utf8_path(rel));
    return true;
}

std::string extract_3ds_dir(const GameEntry& e) {
    if (uses_ps1_unpack(e)) return path_utf8(ps1_unpack_dir(e));
    if (e.system == System::GC || e.system == System::PS2 || e.system == System::PSP || e.system == System::WiiU) return path_utf8(gc_unpack_dir(e));
    return path_utf8(lib_dir() / L"rom-cache" / L"3ds" / utf8_path(sanitize_key(e.key)));
}

uint64_t zip_3ds_unpacked_size(const GameEntry& e) {
    if (uses_ps1_unpack(e)) return ps1_unpacked_size(e);
    if (is_disc_like(e)) return gc_unpacked_size(e);
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
        if (is_3ds_zip_entry(ascii_lower(path_utf8(utf8_path(st.m_filename).extension()))) && !is_junk_entry(st.m_filename)) size = st.m_uncomp_size;
    }
    mz_zip_reader_end(&za);
    std::fclose(zf);
    return size;
}

std::string extracted_3ds_path(const GameEntry& e) {
    if (uses_ps1_unpack(e)) return ps1_extracted_path(e);
    if (is_disc_like(e)) return gc_extracted_path(e);
    std::error_code ec;
    fs::path dir = utf8_path(extract_3ds_dir(e));
    {   /* a stamp (archive size|mtime) says which archive this was unpacked from; a changed archive is unpacked again. Unpacks from older versions have none. */
        std::ifstream in(dir / L"stamp.txt", std::ios::binary);
        std::string stamp;
        if (in && std::getline(in, stamp) && stamp.rfind(archive_stamp_prefix(e), 0) != 0) return "";
    }
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string ext = ascii_lower(path_utf8(it->path().extension()));
        if (is_3ds_zip_entry(ext) && it->path().filename().wstring().find(L".part") == std::wstring::npos) return path_utf8(it->path());
    }
    return "";
}

static bool extract_3ds_zip_impl(const GameEntry& e, const std::function<bool(uint64_t, uint64_t)>& progress,
                                 std::string* out_path, std::string* err) {
    std::string dummy;
    if (!err) err = &dummy;
    if (e.system == System::PS1 || e.system == System::PS2) {
        std::string got;
        if (!(uses_ps1_unpack(e) ? ps1_extract(e, progress, &got, err) : gc_extract(e, progress, &got, err))) return false;
        if (!confirm_extracted_disc(e, got, err)) return false;
        if (out_path) *out_path = got;
        return true;
    }
    if (is_disc_like(e)) return gc_extract(e, progress, out_path, err);
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
        if (!is_3ds_zip_entry(ascii_lower(path_utf8(name.extension()))) || is_junk_entry(st.m_filename)) continue;
        /* Unpacked beside the old copy and swapped in only when complete: a full disk, a cancel or a bad zip leaves the previous unpack alone, and an
         * earlier unpack of another file (a changed archive, another copy of the game) never stays beside the new one. */
        fs::path dir = utf8_path(extract_3ds_dir(e));
        fs::path work = dir.parent_path() / (dir.filename().wstring() + L".unpacking");
        std::error_code ec;
        fs::remove_all(work, ec);
        fs::create_directories(work, ec);
        ULARGE_INTEGER free_bytes{};
        if (GetDiskFreeSpaceExW(work.wstring().c_str(), &free_bytes, nullptr, nullptr) &&
            free_bytes.QuadPart < st.m_uncomp_size + (64ull << 20)) {
            fs::remove_all(work, ec);
            *err = "not enough free disk space (" + std::to_string((st.m_uncomp_size >> 20) + 64) + " MB needed)";
            break;
        }
        fs::path final_path = dir / name, staged = work / name, part = work / (name.wstring() + L".part");
        ExtractCtx ctx;
        ctx.f = _wfopen(part.wstring().c_str(), L"wbN");
        if (!ctx.f) { fs::remove_all(work, ec); *err = "cannot write " + path_utf8(part); break; }
        ctx.total = st.m_uncomp_size;
        ctx.cb = &progress;
        mz_bool r = mz_zip_reader_extract_to_callback(&za, i, extract_write_cb, &ctx, 0);
        bool wrote = std::fflush(ctx.f) == 0;
        std::fclose(ctx.f);
        if (!r || !wrote || ctx.done != st.m_uncomp_size) {
            fs::remove_all(work, ec);
            *err = ctx.cancelled ? "cancelled" : "extraction failed (disk full or damaged zip)";
            break;
        }
        fs::rename(part, staged, ec);
        if (ec) { fs::remove_all(work, ec); *err = "cannot move the unpacked image into place"; break; }
        {
            std::ofstream sf(work / L"stamp.txt", std::ios::binary | std::ios::trunc);
            sf << gc_stamp(e, st.m_uncomp_size, path_utf8(name)) << '\n';
            if (!sf) { fs::remove_all(work, ec); *err = "cannot write the unpack record"; break; }
        }
        fs::remove_all(dir, ec);
        fs::rename(work, dir, ec);
        if (ec) { fs::remove_all(work, ec); *err = "cannot move the unpacked image into place"; break; }
        if (out_path) *out_path = path_utf8(final_path);
        ok = true;
    }
    mz_zip_reader_end(&za);
    std::fclose(zf);
    if (!ok && err->empty()) *err = "no 3DS image inside the zip";
    return ok;
}

bool extract_3ds_zip(const GameEntry& e, const std::function<bool(uint64_t, uint64_t)>& progress,
                     std::string* out_path, std::string* err) {
    bool ok = extract_3ds_zip_impl(e, progress, out_path, err);
    if (ok) rom_cache_trim(rom_cache_limit());   // LRU eviction (the copy just unpacked is protected: used within the hour)
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

std::mutex g_skip_mu;
std::vector<SkippedArchive> g_skipped;
std::vector<GameEntry> g_extra_copies;     // library-1: every copy of a game the last scan found besides the one it lists
std::vector<FolderScanStatus> g_folder_status;   // library-3: per scanned folder, reachable / complete
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
    std::string skip;   // !ok archive: why it was skipped
};

constexpr int kCacheVersion = 12;  // 12: Switch (nx2 flag; .nsp/.xci were refused before)
//  // 11: archive/folder-name titles, loose skip reasons, ps1 entry names from disk; 10: cartridges/3DS in 7z/rar, PS1 content ids, Wii U archive keys, skip reasons; 2: serial matching + GBA catalog_id; 5: GB/GBC serials; 6: 3DS/Wii U/DS; 7: save_stem (short rom-cache names); 8: GB/GBC catalog titles; forces a rescan

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
        r.skip = f.str("sk");
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
            r.e.nx2 = jbool(f, "nx2");   // absent in older caches = false
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
        if (!r.skip.empty()) { o += ",\"sk\":"; o += json::quote(r.skip); }   // why a file was skipped, or what an archive holds beyond the game listed
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
            if (r.e.nx2) o += ",\"nx2\":true";
        }
        o.push_back('}');
    }
    o += "]}\n";
    write_text_atomic(lib_dir() / L"library_cache.json", o);
}

bool is_archive_file(const std::string& path) {
    std::string ext = ascii_lower(path_utf8(fs::path(utf8_path(path)).extension()));
    return ext == ".zip" || ext == ".7z" || ext == ".rar";
}

/* Files a ROM folder holds that RetroHub cannot use. They are looked at (not identified) so the player is told why a game is missing
 * ("Settings > skipped files") instead of the file silently not showing up. */
const char* unsupported_reason(const std::string& ext) {
    if (ext == ".nsp" || ext == ".xci" || ext == ".nsz" || ext == ".xcz" || ext == ".nup") return "this Nintendo Switch file is not a game RetroHub can list (an update / DLC package, damaged, or inside an archive: Switch games are read from loose files, not from .zip / .7z / .rar)";
    if (ext == ".wbfs" || ext == ".wad") return "this is a Wii game file; RetroHub plays GameCube discs, not Wii games";
    if (ext == ".gz" || ext == ".xz" || ext == ".bz2" || ext == ".zst" || ext == ".tgz" || ext == ".tar")
        return "this archive type is not supported; re-pack it as a .zip or .7z, or unpack it and add the game files";
    if (ext == ".001") return "this is the first part of a split archive; RetroHub opens single .zip, .7z and .rar files. Join or unpack the parts first";
    if (ext == ".nrg" || ext == ".mdf" || ext == ".ecm" || ext == ".gdi" || ext == ".cdi") return "this disc image format is not supported; convert it to .cue/.bin, .iso or .chd";
    return nullptr;
}

bool scan_extension(const std::string& ext) {
    static const char* const kExt[] = {".z64", ".v64", ".n64", ".gba", ".gb", ".gbc", ".iso", ".7z", ".rar",
                                       ".gcm", ".rvz", ".wia", ".ciso", ".gcz", ".zip", ".nds", ".nes", ".sfc", ".smc",
                                       ".3ds", ".cci", ".cia", ".cxi", ".3dsx", ".zcci", ".zcia", ".zcxi", ".z3dsx",
                                       ".wua", ".wud", ".wux", ".rpx", ".chd", ".cso", ".cue",
                                       ".m3u", ".pbp", ".bin", ".img", ".nro", ".nup"};
    for (const char* e : kExt)
        if (ext == e) return true;
    return unsupported_reason(ext) != nullptr;
}

/* Why a loose file with a game extension was not listed ("" = say nothing: BIOS dumps and the extra tracks of a disc share the .bin extension). */
std::string loose_skip_reason(const std::string& path_u8) {
    const fs::path p = utf8_path(path_u8);
    const std::string ext = ascii_lower(path_utf8(p.extension()));
    if (const char* r = unsupported_reason(ext)) return r;
    if (ext == ".nro") return "this homebrew file is not a valid .nro";
    if (ext.empty() && is_exefs_main(p)) return "this exefs folder is not a Switch game RetroHub can list (needs main.npdm next to main, and not an update / DLC)";
    std::vector<uint8_t> h;
    if (ext == ".iso" || ext == ".gcm") {
        if (read_at(p, 0x18, 4, &h) && be32(h.data()) == 0x5D1C9EA3u) return "this is a Wii disc; RetroHub plays GameCube discs, not Wii games";
        return "this disc image is not a GameCube, PlayStation, PlayStation 2 or PSP game RetroHub recognizes (damaged, another console, or a data disc)";
    }
    if (ext == ".rvz" || ext == ".wia") {
        if (read_at(p, 0, 0x4C, &h) && be32(h.data() + 0x48) == 2) return "this is a Wii disc; RetroHub plays GameCube discs, not Wii games";
        return "this compressed disc is not a GameCube game RetroHub recognizes (damaged or not a GameCube disc)";
    }
    if (ext == ".ciso" || ext == ".gcz") return "this compressed disc is not a GameCube game RetroHub recognizes (damaged or not a GameCube disc)";
    if (ext == ".z64" || ext == ".v64" || ext == ".n64") return "this is not a usable Nintendo 64 ROM (too small or damaged)";
    if (ext == ".gba") return "this is not a Game Boy Advance ROM RetroHub recognizes (bad header or wrong size)";
    if (ext == ".gb" || ext == ".gbc") return "this is not a Game Boy ROM RetroHub recognizes (bad header or wrong size)";
    if (ext == ".nds") return "this is not a Nintendo DS ROM RetroHub recognizes (bad header or wrong size)";
    if (ext == ".nes") return "this is not an iNES / NES 2.0 ROM RetroHub recognizes (bad header or truncated)";
    if (ext == ".sfc" || ext == ".smc") return "this is not a Super Nintendo ROM RetroHub recognizes (no valid internal header)";
    if (is_3ds_extension(ext)) return "this is not a Nintendo 3DS game image RetroHub can play (an update, DLC, a system title, or damaged)";
    if (ext == ".wua" || ext == ".wud" || ext == ".wux") return "this is not a Wii U game RetroHub recognizes (damaged, or not a base game)";
    if (ext == ".rpx") return "this .rpx is not inside a Wii U game folder (needs code\\ and meta\\meta.xml next to it)";
    if (ext == ".cue") {
        std::string missing;
        for (const std::string& f : ps1::referenced_files(path_u8)) {
            std::error_code ec;
            if (!fs::exists(utf8_path(f), ec)) { missing = path_utf8(utf8_path(f).filename()); break; }
        }
        if (!missing.empty()) return "this .cue sheet points at " + missing + ", which is not next to it. Put the disc's .bin file(s) in the same folder";
        return "this .cue sheet is not a PlayStation or PlayStation 2 disc RetroHub recognizes";
    }
    if (ext == ".chd") return "this CHD is not a PlayStation, PlayStation 2 or PSP disc RetroHub recognizes (damaged, or another console)";
    if (ext == ".cso") return "this compressed disc is not a PlayStation 2 or PSP game RetroHub recognizes";
    if (ext == ".pbp") return "this .pbp is not a PlayStation or PSP game RetroHub recognizes";
    if (ext == ".m3u") return "this playlist does not point at a PlayStation disc RetroHub recognizes";
    return std::string();   // .bin / .img: stay quiet (BIOS files, loose tracks)
}

struct Candidate {
    fs::path path;
    std::string path_utf8;
    uint64_t size = 0;
    long long mtime = 0;
    bool done = false;
    CacheRec rec;
};

/* A directory entry that is a symlink or an NTFS junction (MSVC's std::filesystem reports a junction as file_type::junction, which is_symlink() misses). library-9 */
bool is_link_or_junction(const fs::directory_entry& de) {
    std::error_code ec;
    fs::file_status st = de.symlink_status(ec);
    if (ec) return false;
    return st.type() == fs::file_type::symlink || st.type() == fs::file_type::junction;
}

constexpr size_t kMaxWalkDepth = 24;   // library-9: a backstop for any loop the link check cannot see

/* Directory walk that skips unreadable dirs and never follows symlinks/junctions. */
void walk(const fs::path& root, std::vector<Candidate>& out, std::set<std::string>& seen, bool* incomplete = nullptr) {
    std::vector<std::pair<fs::path, size_t>> stack{{root, 0}};
    while (!stack.empty()) {
        fs::path dir = std::move(stack.back().first);
        const size_t depth = stack.back().second;
        stack.pop_back();
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) { if (incomplete) *incomplete = true; continue; }   // a folder that cannot be listed is "unknown", not "empty"
        std::vector<fs::path> subdirs;
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) { if (incomplete) *incomplete = true; break; }
            const fs::directory_entry& de = *it;
            std::error_code e1;
            if (de.is_symlink(e1) || is_link_or_junction(de)) continue;
            if (de.is_directory(e1)) { if (de.path().filename() != L"__MACOSX") { if (depth < kMaxWalkDepth) subdirs.push_back(de.path()); else if (incomplete) *incomplete = true; } continue; }   // __MACOSX: resource forks of a macOS unzip
            if (!de.is_regular_file(e1) || e1) continue;
            if (de.path().filename().native().compare(0, 2, L"._") == 0) continue;   // "._Game.z64": the same
            std::string ext = ascii_lower(path_utf8(de.path().extension()));
            if (!scan_extension(ext)) {
                /* Switch 2 / loose dumps: an extension-less "main" counts only inside a folder called exefs (keeps scans cheap) */
                if (!(ext.empty() && de.path().filename() == L"main" && ascii_lower(path_utf8(dir.filename())) == "exefs")) continue;
            }
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
        for (auto& s : subdirs) stack.emplace_back(std::move(s), depth + 1);
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
            rom_cache_trim(rom_cache_limit());   // honour the size limit once per run
            for (fs::recursive_directory_iterator it(rc, ec), end; !ec && it != end; it.increment(ec))
                if (it->path().extension() == L".part") { std::error_code e2; fs::remove(it->path(), e2); }
            std::vector<fs::path> half;   // "<key>.unpacking": a 3DS unpack that was cut off (nothing else is running yet)
            for (fs::recursive_directory_iterator it(rc, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_directory(ec) && it->path().extension() == L".unpacking") half.push_back(it->path());
            for (const fs::path& h : half) { std::error_code e2; fs::remove_all(h, e2); }
        });
    }
    std::vector<Candidate> cands;
    std::set<std::string> seen;
    std::vector<std::string> prefixes;
    std::vector<std::string> unknown_prefixes;   // folders that are offline or only partly readable: their cached games stay listed (library-3)
    std::vector<FolderScanStatus> fstat;
    for (const std::string& f : folders) {
        std::error_code ec;
        fs::path root = fs::absolute(utf8_path(f), ec);
        std::string pre = ascii_lower(path_utf8(root));
        if (pre.empty() || (pre.back() != '\\' && pre.back() != '/')) pre.push_back('\\');
        FolderScanStatus st;
        st.folder = f;
        if (ec || !fs::is_directory(root, ec)) { st.reachable = false; st.complete = false; unknown_prefixes.push_back(pre); fstat.push_back(st); continue; }
        bool incomplete = false;
        walk(root, cands, seen, &incomplete);
        if (incomplete) { st.complete = false; unknown_prefixes.push_back(pre); }
        else prefixes.push_back(pre);   // only a fully read folder may drop cache rows of vanished files
        fstat.push_back(st);
    }

    std::unordered_map<std::string, CacheRec> cache = cache_load();

    /* Reuse unchanged cache entries; collect the rest as work. */
    std::vector<size_t> work;
    for (size_t i = 0; i < cands.size(); ++i) {
        Candidate& c = cands[i];
        auto it = cache.find(ascii_lower(c.path_utf8));
        bool unpacked_gone = false;   /* a cartridge unpacked into rom-cache that was cleared / trimmed: unpack again */
        if (it != cache.end() && it->second.ok && it->second.e.issue.empty() && !it->second.e.path.empty() && ascii_lower(it->second.e.path) != ascii_lower(c.path_utf8) && is_archive_file(c.path_utf8)) {
            std::error_code xe;
            unpacked_gone = !fs::exists(long_path(utf8_path(it->second.e.path)), xe);
        }
        if (it != cache.end() && it->second.size == c.size && it->second.mtime == c.mtime && !unpacked_gone) {
            c.rec = it->second;
            if (c.rec.e.path.empty()) c.rec.e.path = c.path_utf8;   // zipped ROMs keep their extracted path
            c.done = true;
        } else {
            work.push_back(i);
        }
    }

    std::vector<GameEntry> offline_games;   // cached games of offline / half-readable folders that this scan did not see
    if (!unknown_prefixes.empty()) {
        std::set<std::string> have;
        for (const Candidate& c : cands) have.insert(ascii_lower(c.path_utf8));
        for (const auto& kv : cache) {
            if (!kv.second.ok || have.count(kv.first)) continue;
            for (size_t u = 0; u < unknown_prefixes.size(); ++u) {
                const std::string& pre = unknown_prefixes[u];
                if (kv.first.compare(0, pre.size(), pre) != 0) continue;
                GameEntry g = kv.second.e;
                if (g.path.empty()) g.path = kv.first;
                offline_games.push_back(std::move(g));
                for (auto& fs_ : fstat) {
                    std::string fp = ascii_lower(path_utf8(fs::absolute(utf8_path(fs_.folder))));
                    if (!fp.empty() && fp.back() != '\\' && fp.back() != '/') fp.push_back('\\');
                    if (fp == pre) ++fs_.kept;
                }
                break;
            }
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
            if (ok) {
                if (e.path.empty()) e.path = c.path_utf8;
                e.size = c.size;
                c.rec.e = std::move(e);
                if (is_archive_file(c.path_utf8)) { try { c.rec.skip = archive_extra_games_note(c.path_utf8); } catch (...) {} }   // listed, but not everything in it
            }
            else if (is_archive_file(c.path_utf8)) { try { c.rec.skip = archive_skip_reason(c.path_utf8); } catch (...) {} }
            else { try { c.rec.skip = loose_skip_reason(c.path_utf8); } catch (...) {} }   // a loose file that is no game: the reason is listed, the file does not just vanish
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

    {
        std::vector<SkippedArchive> sk;
        for (const Candidate& c : cands) if (!c.rec.skip.empty()) sk.push_back({c.path_utf8, c.rec.skip});
        std::lock_guard<std::mutex> lk(g_skip_mu);
        g_skipped = std::move(sk);
    }

    /* Files a .cue (its bins) or an .m3u (its discs) already stands for are not games of their own. */
    std::set<std::string> hidden;
    for (const Candidate& c : cands) {
        if (!c.rec.ok || (c.rec.e.system != System::PS1 && c.rec.e.system != System::PS2)) continue;
        std::string ext = ascii_lower(path_utf8(c.path.extension()));
        if (ext != ".cue" && ext != ".m3u") continue;
        for (const std::string& f : ps1::referenced_files(c.path_utf8)) hidden.insert(ascii_lower(path_utf8(fs::path(utf8_path(f)).lexically_normal())));
    }

    /* Dedupe by key (one card per game), then sort. Among copies of the same game the
     * unzipped one is listed (dedupe_prefers); otherwise the first found wins. */
    std::vector<GameEntry> out;
    std::map<std::string, size_t> by_key;   // key -> index in out
    std::vector<std::string> out_src;       // original file path of each out entry
    std::vector<GameEntry> extras;          // the other copies (library-1)
    for (const Candidate& c : cands) {
        if (!c.rec.ok) continue;
        if ((c.rec.e.system == System::PS1 || c.rec.e.system == System::PS2) && !hidden.empty() && hidden.count(ascii_lower(path_utf8(c.path.lexically_normal())))) continue;
        auto it = by_key.find(c.rec.e.key);
        if (it == by_key.end()) {
            by_key.emplace(c.rec.e.key, out.size());
            out.push_back(c.rec.e);
            out_src.push_back(c.path_utf8);
        } else if (dedupe_prefers(c.path_utf8, out_src[it->second])) {
            extras.push_back(out[it->second]);
            out[it->second] = c.rec.e;
            out_src[it->second] = c.path_utf8;
        } else extras.push_back(c.rec.e);
    }
    for (GameEntry& g : offline_games) {   // games of a drive that is not connected: listed from the last good scan (Play says the drive is missing)
        auto it = by_key.find(g.key);
        if (it == by_key.end()) { by_key.emplace(g.key, out.size()); out.push_back(std::move(g)); }
    }
    {
        std::lock_guard<std::mutex> lk(g_skip_mu);
        g_extra_copies = std::move(extras);
        g_folder_status = std::move(fstat);
    }
    std::stable_sort(out.begin(), out.end(), [](const GameEntry& a, const GameEntry& b) {
        if (a.system != b.system) return (int)a.system < (int)b.system;
        std::string ta = ascii_lower(a.title), tb = ascii_lower(b.title);
        if (ta != tb) return ta < tb;
        return a.path < b.path;
    });
    return out;
}

std::vector<GameEntry> scan_extra_copies() {
    std::lock_guard<std::mutex> lk(g_skip_mu);
    return g_extra_copies;
}
std::vector<FolderScanStatus> scan_folder_status() {
    std::lock_guard<std::mutex> lk(g_skip_mu);
    return g_folder_status;
}

/* library-13: keys of files the scan cache already identified (same path, size and mtime), without reading them. */
std::set<std::string> cached_keys_for_files(const std::vector<std::string>& paths, std::vector<std::string>* uncached) {
    std::set<std::string> keys;
    std::unordered_map<std::string, CacheRec> cache = cache_load();
    for (const std::string& p : paths) {
        bool hit = false;
        auto it = cache.find(ascii_lower(p));
        if (it != cache.end() && it->second.ok && !it->second.e.key.empty()) {
            std::error_code e1, e2;
            fs::path fp = utf8_path(p);
            uint64_t sz = fs::file_size(fp, e1);
            long long mt = (long long)fs::last_write_time(fp, e2).time_since_epoch().count();
            if (!e1 && !e2 && sz == it->second.size && mt == it->second.mtime) { keys.insert(it->second.e.key); hit = true; }
        }
        if (!hit && uncached) uncached->push_back(p);
    }
    return keys;
}

std::vector<SkippedArchive> scan_skipped_archives() {
    std::lock_guard<std::mutex> lk(g_skip_mu);
    return g_skipped;
}

/* ---- unpacked-games cache ---- */
namespace {
std::atomic<uint64_t> g_cache_limit{40ull << 30};

uint64_t dir_bytes(const fs::path& d) {
    uint64_t n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) n += it->file_size(e2);
    }
    return n;
}

/* cart: a cartridge ROM unpacked at scan time (rom-cache\<crc|a-hash>). Its library entry points straight at the file, so it is evicted only when the
 * discs and 3DS images are not enough: the library cannot unpack it again on Play, only on the next scan. */
struct CacheItem { fs::path dir; uint64_t bytes = 0; fs::file_time_type used; bool cart = false; };

/* One item per unpacked game: rom-cache/<sys>/<key> (discs, 3DS) or rom-cache/<crc|a-hash> (cartridges). */
std::vector<CacheItem> cache_items() {
    std::vector<CacheItem> v;
    std::error_code ec;
    fs::path root = lib_dir() / L"rom-cache";
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        std::wstring n = it->path().filename().wstring();
        if (n == L"_probe") continue;
        std::vector<fs::path> dirs;
        bool cart = false;
        if (n == L"3ds" || n == L"gc" || n == L"ps1" || n == L"ps2" || n == L"psp" || n == L"wiiu" || n == L"hacks") {   // hacks: one item per patched image (Plan 4 H3)
            std::error_code e2;
            for (fs::directory_iterator j(it->path(), e2), je; !e2 && j != je; j.increment(e2)) if (j->is_directory(e2)) dirs.push_back(j->path());
        } else { dirs.push_back(it->path()); cart = true; }
        for (const fs::path& d : dirs) {
            CacheItem ci;
            ci.dir = d;
            ci.cart = cart;
            ci.bytes = dir_bytes(d);
            std::error_code e3, e4;
            ci.used = fs::last_write_time(d, e3);
            auto st = fs::last_write_time(d / L"stamp.txt", e4);
            if (!e4 && st > ci.used) ci.used = st;
            v.push_back(std::move(ci));
        }
    }
    return v;
}

std::mutex g_pin_mu;
std::string g_pin_dir;   // lowercase UTF-8 path of the running game's unpacked folder ("" = none)

/* The cache folder of an entry (same rule as rom_cache_touch); "" when the game is not unpacked into the cache. */
std::string cache_dir_of(const GameEntry& e) {
    if (e.issue.empty() && e.path.empty()) return {};
    fs::path d = utf8_path(e.path).parent_path();
    fs::path root = lib_dir() / L"rom-cache";
    if (ascii_lower(path_utf8(d)).rfind(ascii_lower(path_utf8(root)), 0) != 0) d = utf8_path(extract_3ds_dir(e));
    return d.empty() ? std::string() : ascii_lower(path_utf8(d));
}

/* True when the item holds the pinned folder (or is inside it): it must not be removed while the game runs. */
bool cache_item_pinned(const fs::path& item) {
    std::lock_guard<std::mutex> lk(g_pin_mu);
    if (g_pin_dir.empty()) return false;
    std::string d = ascii_lower(path_utf8(item));
    while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    const std::string& p = g_pin_dir;
    auto under = [](const std::string& a, const std::string& b) {   // a is b or inside b
        return a.size() >= b.size() && a.compare(0, b.size(), b) == 0 && (a.size() == b.size() || a[b.size()] == '\\' || a[b.size()] == '/');
    };
    return under(p, d) || under(d, p);
}
} // namespace

uint64_t rom_cache_bytes() {
    uint64_t n = 0;
    for (const CacheItem& c : cache_items()) n += c.bytes;
    return n;
}

bool rom_cache_clear() {
    bool ok = true;
    for (const CacheItem& c : cache_items()) {
        if (cache_item_pinned(c.dir)) { ok = false; continue; }   // the running game's files stay (library-14)
        std::error_code ec;
        fs::remove_all(c.dir, ec);
        if (ec) ok = false;
    }
    std::error_code ec;
    fs::remove(lib_dir() / L"library_cache.json", ec);   // cartridge entries point into the cache: identify them again
    return ok;
}

void rom_cache_pin(const GameEntry* e) {
    std::string d = e ? cache_dir_of(*e) : std::string();
    while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    std::lock_guard<std::mutex> lk(g_pin_mu);
    g_pin_dir = d;
}

void rom_cache_set_limit(uint64_t bytes) { g_cache_limit = bytes; }
uint64_t rom_cache_limit() { return g_cache_limit; }

uint64_t rom_cache_trim(uint64_t max_bytes) {
    if (max_bytes == 0) return 0;
    std::vector<CacheItem> items = cache_items();
    uint64_t total = 0, freed = 0;
    for (const CacheItem& c : items) total += c.bytes;
    std::sort(items.begin(), items.end(), [](const CacheItem& a, const CacheItem& b) { return a.cart != b.cart ? !a.cart : a.used < b.used; });   // discs / 3DS first, cartridges last
    const auto recent = fs::file_time_type::clock::now() - std::chrono::hours(1);
    for (const CacheItem& c : items) {
        if (total <= max_bytes) break;
        if (c.used > recent || cache_item_pinned(c.dir)) continue;   // just unpacked, or the running game's (library-14)
        std::error_code ec;
        fs::remove_all(c.dir, ec);
        if (ec) continue;
        total -= std::min(total, c.bytes);
        freed += c.bytes;
    }
    return freed;
}

void rom_cache_touch(const GameEntry& e) {
    if (e.issue.empty() && e.path.empty()) return;
    std::error_code ec;
    fs::path d = utf8_path(e.path).parent_path();
    fs::path root = lib_dir() / L"rom-cache";
    if (ascii_lower(path_utf8(d)).rfind(ascii_lower(path_utf8(root)), 0) != 0) d = utf8_path(extract_3ds_dir(e));   // a still-packed disc/3DS entry: its unpack folder
    if (d.empty() || !fs::exists(d, ec)) return;
    auto now = fs::file_time_type::clock::now();
    fs::last_write_time(d, now, ec);
    fs::last_write_time(d / L"stamp.txt", now, ec);
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
    prefs::mirror_favorite(key, fav);
}

std::vector<std::string> favorite_keys() {
    std::lock_guard<std::mutex> lk(g_state_mu);
    state_load_locked();
    std::vector<std::string> r;
    for (const auto& kv : g_state) if (kv.second.favorite) r.push_back(kv.first);
    return r;
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

/* Screenshot aids only (hub headless mode, src/hub/shot.cpp): ASSIGN instead of add, so a fake history is the same on every run. */
void shot_set_play(const std::string& key, uint64_t last_played, uint64_t seconds) {
    std::lock_guard<std::mutex> lk(g_state_mu);
    state_load_locked();
    GameState& g = g_state[key];
    g.last_played = last_played;
    g.play_seconds = seconds;
    g_state_dirty = true;
    state_flush_locked();
}

void shot_clear_play(bool all) {
    std::lock_guard<std::mutex> lk(g_state_mu);
    state_load_locked();
    for (auto it = g_state.begin(); it != g_state.end();) {
        if (all || it->first.compare(0, 5, "fake:") == 0) { it->second.last_played = 0; it->second.play_seconds = 0; if (!it->second.favorite) { it = g_state.erase(it); continue; } }
        ++it;
    }
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

std::set<std::string> local_removed() {
    std::set<std::string> s;
    std::ifstream f(lib_dir() / L"removed_here.txt", std::ios::binary);
    std::string ln;
    while (std::getline(f, ln)) {
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
        if (!ln.empty()) s.insert(ln);
    }
    return s;
}

bool local_removed_set(const std::string& key, bool removed) {
    if (key.empty() || key.find_first_of("\r\n") != std::string::npos) return false;
    std::set<std::string> s = local_removed();
    if (removed ? !s.insert(key).second : !s.erase(key)) return true;   // nothing to change
    fs::path dst = lib_dir() / L"removed_here.txt", tmp = lib_dir() / L"removed_here.txt.tmp";
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        for (const auto& k : s) f << k << "\n";
        if (!f) return false;
    }
    fs::remove(dst, ec);
    fs::rename(tmp, dst, ec);
    return !ec;
}

} // namespace hub
