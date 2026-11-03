/* Formats matrix: every console x every container/layout/name the library has to cope with, on synthetic fixtures (valid headers, no real
 * ROMs), checked end to end the way a user hits them: scan -> identified? right console? sensible title? stable key (same as the loose file,
 * same on rescan)? unpack on Play -> launch path (a real image, .cue not .bin, never an archive)? cache reused? plus the "wrong console
 * folder", unknown/damaged files (must be reported, never vanish) and a too-small rom-cache limit.
 * Usage: formats_matrix_test [fixture-id-substring] [--keep] [--quiet]     (exit 1 on any failed cell)
 * Scratch data in %TEMP%\formats_matrix_<pid>; 7z written by Windows' bsdtar; .rar = the 7z bytes under a .rar name (bsdtar sniffs content). */
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "library.h"
#include "miniz.h"
#include "ps2.h"

namespace fs = std::filesystem;
using namespace hub;
typedef std::vector<uint8_t> Bytes;

/* ------------------------------------------------------------------ bytes */
static void put_be32(Bytes& v, size_t o, uint32_t x) { v[o] = uint8_t(x >> 24); v[o + 1] = uint8_t(x >> 16); v[o + 2] = uint8_t(x >> 8); v[o + 3] = uint8_t(x); }
static void put_be64(Bytes& v, size_t o, uint64_t x) { put_be32(v, o, uint32_t(x >> 32)); put_be32(v, o + 4, uint32_t(x)); }
static void put_le32(Bytes& v, size_t o, uint32_t x) { for (int i = 0; i < 4; ++i) v[o + i] = uint8_t(x >> (8 * i)); }
static void put_le64(Bytes& v, size_t o, uint64_t x) { put_le32(v, o, uint32_t(x)); put_le32(v, o + 4, uint32_t(x >> 32)); }
static void put_le16(Bytes& v, size_t o, uint32_t x) { v[o] = uint8_t(x); v[o + 1] = uint8_t(x >> 8); }
static void put(Bytes& v, size_t o, const std::string& s) { if (o + s.size() > v.size()) v.resize(o + s.size()); std::memcpy(&v[o], s.data(), s.size()); }
/* compressible but not constant: archives stay small, content differs per seed */
static Bytes filler(size_t n, uint32_t seed) { Bytes v(n); for (size_t i = 0; i < n; ++i) v[i] = uint8_t(((i >> 5) * 7 + seed * 31 + (i & 3)) & 0xFF); return v; }
static Bytes noise(size_t n, uint32_t seed) { Bytes v(n); uint32_t x = seed * 2654435761u + 1; for (auto& b : v) { x = x * 1103515245u + 12345u; b = uint8_t(x >> 16); } return v; }
static Bytes text(const std::string& s) { return Bytes(s.begin(), s.end()); }

/* ------------------------------------------------------------------ cartridge fixtures */
static Bytes make_n64_z64(const std::string& title, uint32_t seed) {
    Bytes v = filler(1024 * 1024, seed);
    put_be32(v, 0, 0x80371240u);
    for (size_t i = 0x20; i < 0x34; ++i) v[i] = ' ';
    put(v, 0x20, title); put(v, 0x3B, "NFXE");
    return v;
}
static Bytes swap_pairs(Bytes v) { for (size_t i = 0; i + 1 < v.size(); i += 2) std::swap(v[i], v[i + 1]); return v; }                 // .v64
static Bytes swap_words(Bytes v) { for (size_t i = 0; i + 3 < v.size(); i += 4) { std::swap(v[i], v[i + 3]); std::swap(v[i + 1], v[i + 2]); } return v; }   // .n64 (little endian)
static Bytes make_gba(const std::string& title, const std::string& code, uint32_t seed) {
    Bytes v = filler(128 * 1024, seed);
    std::memset(&v[0xA0], 0, 12);
    put(v, 0xA0, title); put(v, 0xAC, code); put(v, 0xB0, "01");
    v[0xB2] = 0x96;
    unsigned s = 0;
    for (int i = 0xA0; i <= 0xBC; ++i) s += v[i];
    v[0xBD] = uint8_t(-(int)s - 0x19);
    return v;
}
static uint16_t crc16m(const Bytes& v, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) { crc ^= v[i]; for (int b = 0; b < 8; ++b) crc = (crc & 1) ? uint16_t((crc >> 1) ^ 0xA001) : uint16_t(crc >> 1); }
    return crc;
}
static Bytes make_nds(const std::string& title, const std::string& code, uint32_t seed) {
    Bytes v = filler(64 * 1024, seed);
    std::memset(&v[0], 0, 0x20);
    put(v, 0, title); put(v, 0x0C, code);
    v[0x1E] = 1; v[0x15C] = 0x56; v[0x15D] = 0xCF;
    uint16_t c = crc16m(v, 0x15E);
    v[0x15E] = uint8_t(c); v[0x15F] = uint8_t(c >> 8);
    return v;
}
static Bytes make_gb(const std::string& title, uint8_t cgb, uint16_t global, uint32_t seed) {
    Bytes v = filler(32 * 1024, seed);
    std::memset(&v[0x134], 0, 16);
    put(v, 0x134, title);
    v[0x143] = cgb;
    std::memset(&v[0x13F], 0, 4);
    v[0x14E] = uint8_t(global >> 8); v[0x14F] = uint8_t(global);
    uint8_t x = 0;
    for (int i = 0x134; i <= 0x14C; ++i) x = uint8_t(x - v[i] - 1);
    v[0x14D] = x;
    return v;
}
static Bytes make_nes(uint32_t seed) {
    Bytes v(16 + 16384 + 8192);
    Bytes body = filler(v.size() - 16, seed);
    std::memcpy(&v[16], body.data(), body.size());
    v[0] = 'N'; v[1] = 'E'; v[2] = 'S'; v[3] = 0x1A; v[4] = 1; v[5] = 1; v[6] = 2;
    for (int i = 7; i < 16; ++i) v[i] = 0;
    return v;
}
static Bytes make_snes(uint32_t seed) {
    Bytes v = filler(128 * 1024, seed);
    size_t off = 0x7FC0;
    std::memset(&v[off], ' ', 21);
    std::memcpy(&v[off], "FAKELAND SNES", 13);
    v[off + 0x15] = 0x20; v[off + 0x16] = 0x02; v[off + 0x17] = 0x07; v[off + 0x18] = 0x01;
    unsigned sum = 0x1234, comp = sum ^ 0xFFFF;
    v[off + 0x1C] = uint8_t(comp); v[off + 0x1D] = uint8_t(comp >> 8); v[off + 0x1E] = uint8_t(sum); v[off + 0x1F] = uint8_t(sum >> 8);
    return v;
}

/* ------------------------------------------------------------------ Nintendo discs / 3DS / Wii U */
static Bytes gc_header(const std::string& id6, const std::string& title) {
    Bytes h(0x440, 0);
    put(h, 0, id6); put_be32(h, 0x1C, 0xC2339F3Du); put(h, 0x20, title);
    return h;
}
static Bytes make_gc_iso(const std::string& id6, const std::string& title) { Bytes h = gc_header(id6, title); h.resize(0x4000, 0x11); return h; }
static Bytes make_rvz(const char* magic4, uint32_t disc_type, const std::string& id6, const std::string& title) {
    Bytes v(0x2000, 0x33);
    std::memcpy(&v[0], magic4, 4);
    put_be32(v, 0x48, disc_type); put_be32(v, 0x54, 0x200000);
    Bytes h = gc_header(id6, title);
    std::memcpy(&v[0x58], h.data(), 0x80);
    return v;
}
static Bytes make_ciso(const std::string& id6, const std::string& title) {
    Bytes v(0x8000, 0);
    put(v, 0, "CISO"); put_le32(v, 4, 0x200000); v[8] = 1;
    Bytes h = gc_header(id6, title);
    v.insert(v.end(), h.begin(), h.end());
    v.resize(0x8000 + 0x2000, 0x22);
    return v;
}
static Bytes zlib_fixed_literals(const Bytes& data) {
    Bytes out = {0x78, 0x01};
    uint32_t acc = 0; int nb = 0;
    auto bit = [&](int b) { acc |= (uint32_t)b << nb; if (++nb == 8) { out.push_back((uint8_t)acc); acc = 0; nb = 0; } };
    auto code = [&](uint32_t c, int len) { for (int i = len - 1; i >= 0; --i) bit((c >> i) & 1); };
    bit(1); bit(1); bit(0);
    for (uint8_t b : data) { if (b < 144) code(0x30u + b, 8); else code(0x190u + (b - 144), 9); }
    code(0, 7);
    if (nb) out.push_back((uint8_t)acc);
    out.insert(out.end(), {0, 0, 0, 0});
    return out;
}
static Bytes make_gcz(const Bytes& block0, uint32_t blocks) {   /* `blocks` > 1 pushes block 0 far from the file start, like a real disc */
    Bytes v(32 + 12ull * blocks, 0);
    put_le32(v, 0, 0xB10BC001u); put_le64(v, 8, block0.size()); put_le64(v, 16, 0x100000); put_le32(v, 24, 0x8000); put_le32(v, 28, blocks);
    for (uint32_t i = 0; i < blocks; ++i) put_le64(v, 32 + 8ull * i, i == 0 ? 0 : block0.size() + i);
    v.insert(v.end(), block0.begin(), block0.end());
    v.resize(v.size() + 0x2000, 0x44);
    return v;
}
static Bytes make_ncsd(uint64_t tid, const std::string& product) {
    Bytes b(0x4400, 0);
    std::memcpy(&b[0x100], "NCSD", 4);
    put_le32(b, 0x120, 0x20); put_le32(b, 0x124, 0x1000);
    size_t n = 0x4000;
    std::memcpy(&b[n + 0x100], "NCCH", 4);
    put_le64(b, n + 0x108, tid); put_le64(b, n + 0x118, tid);
    std::memcpy(&b[n + 0x150], product.data(), product.size());
    b[n + 0x188 + 7] = 0x04;   // decrypted
    b.resize(0x6000, 0x55);    // above the 17 KB header read
    return b;
}
static Bytes make_3dsx() { Bytes b(0x6000, 0x66); std::memcpy(b.data(), "3DSX", 4); return b; }
static Bytes make_cia(uint64_t tid) {
    Bytes cia(0x2040 + 0x40 + 0x40 + 0x400 + 0x4000, 0x77);
    std::memset(&cia[0], 0, 0x2040 + 0x40 + 0x40 + 0x400);
    put_le32(cia, 0, 0x2020); put_le32(cia, 8, 0x40); put_le32(cia, 0x0C, 0x40); put_le32(cia, 0x10, 0x300);
    size_t tmd = 0x2040 + 0x40 + 0x40;
    put_be32(cia, tmd, 0x010004);
    put_be64(cia, tmd + 4 + 0x100 + 0x3C + 0x4C, tid);
    return cia;
}
static Bytes make_wua(const std::string& base_dir) {
    std::string a = base_dir, c = "0005000e1fffe500_v0";
    Bytes names; names.push_back((uint8_t)a.size()); names.insert(names.end(), a.begin(), a.end());
    size_t second = names.size();
    names.push_back((uint8_t)c.size()); names.insert(names.end(), c.begin(), c.end());
    Bytes tree(48, 0);
    put_be32(tree, 0, 0x7FFFFFFFu); put_be32(tree, 4, 1); put_be32(tree, 8, 2);
    put_be32(tree, 16, 0); put_be32(tree, 20, 3); put_be32(tree, 24, 0);
    put_be32(tree, 32, (uint32_t)second); put_be32(tree, 36, 3); put_be32(tree, 40, 0);
    Bytes f(8000, 0xAB);
    size_t names_off = f.size();
    f.insert(f.end(), names.begin(), names.end());
    size_t tree_off = f.size();
    f.insert(f.end(), tree.begin(), tree.end());
    size_t foot = f.size();
    f.resize(foot + 144, 0);
    put_be64(f, foot + 0, 0); put_be64(f, foot + 8, 8000);
    put_be64(f, foot + 32, names_off); put_be64(f, foot + 40, names.size());
    put_be64(f, foot + 48, tree_off); put_be64(f, foot + 56, tree.size());
    put_be64(f, foot + 128, f.size());
    put_be32(f, foot + 136, 0x61bf3a01u); put_be32(f, foot + 140, 0x169f52d6u);
    return f;
}
static Bytes make_wux(const std::string& product) {
    Bytes wux(0x20 + 0x8000 * 2, 0);
    std::memcpy(wux.data(), "WUX0", 4);
    put_le32(wux, 4, 0x1099d02e); put_le32(wux, 8, 0x8000); put_le64(wux, 12, 0x8000); put_le32(wux, 0x20, 0);
    std::memcpy(&wux[0x8000], product.data(), product.size());
    return wux;
}

/* ------------------------------------------------------------------ PlayStation / PSP */
static void put32b(Bytes& b, size_t off, uint32_t v) { for (int i = 0; i < 4; i++) b[off + i] = (v >> (8 * i)) & 255; for (int i = 0; i < 4; i++) b[off + 4 + i] = (v >> (8 * (3 - i))) & 255; }
static Bytes dir_rec(const std::string& name, uint32_t extent, uint32_t size, uint8_t flags) {
    size_t nl = name.size(), rl = 33 + nl + (nl % 2 == 0 ? 1 : 0);
    Bytes r(rl, 0);
    r[0] = (uint8_t)rl; put32b(r, 2, extent); put32b(r, 10, size); r[25] = flags; r[28] = 1; r[31] = 1; r[32] = (uint8_t)nl;
    std::memcpy(&r[33], name.data(), nl);
    return r;
}
static Bytes make_cnf_iso(const std::string& cnf, size_t sectors, uint32_t filler_seed) {
    Bytes img(sectors * 2048, 0);
    uint8_t* pvd = &img[16 * 2048];
    pvd[0] = 1; std::memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    Bytes root = dir_rec(std::string(1, '\0'), 20, 2048, 2);
    std::memcpy(pvd + 156, root.data(), root.size());
    Bytes dir;
    auto add = [&](const Bytes& r) { dir.insert(dir.end(), r.begin(), r.end()); };
    add(dir_rec(std::string(1, '\0'), 20, 2048, 2));
    add(dir_rec(std::string(1, '\1'), 20, 2048, 2));
    add(dir_rec("SYSTEM.CNF;1", 21, (uint32_t)cnf.size(), 0));
    std::memcpy(&img[20 * 2048], dir.data(), dir.size());
    std::memcpy(&img[21 * 2048], cnf.data(), cnf.size());
    for (size_t i = 24 * 2048; i < img.size(); ++i) img[i] = (uint8_t)(((i >> 6) * 13 + filler_seed) & 0xFF);
    return img;
}
static Bytes to_raw2352(const Bytes& iso) {
    size_t n = iso.size() / 2048;
    Bytes raw(n * 2352, 0);
    for (size_t i = 0; i < n; i++) {
        uint8_t* s = &raw[i * 2352];
        s[0] = 0; std::memset(s + 1, 0xFF, 10); s[11] = 0; s[15] = 2;
        s[16] = s[20] = 0; s[17] = s[21] = 0; s[18] = s[22] = 8; s[19] = s[23] = 0;
        std::memcpy(s + 24, &iso[i * 2048], 2048);
    }
    return raw;
}
static void be32w(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (24 - 8 * i)); }
static Bytes make_chd(const Bytes& iso, bool cd, bool raw_mode2) {   /* CHD v5, uncompressed hunks. cd: 2448-byte frames */
    const uint32_t unit = cd ? 2448 : 2048, per = 8, hunk = unit * per;
    size_t sectors = iso.size() / 2048, nh = (sectors + per - 1) / per;
    size_t data_start = ((124 + 4 * nh + hunk - 1) / hunk) * hunk;
    Bytes b(data_start + nh * hunk, 0);
    std::memcpy(&b[0], "MComprHD", 8);
    be32w(b, 8, 124); be32w(b, 12, 5);
    for (int i = 0; i < 8; ++i) b[32 + i] = uint8_t(uint64_t(nh) * hunk >> (56 - 8 * i));
    for (int i = 0; i < 8; ++i) b[40 + i] = uint8_t(uint64_t(124) >> (56 - 8 * i));
    be32w(b, 56, hunk); be32w(b, 60, unit);
    for (size_t h = 0; h < nh; ++h) be32w(b, 124 + 4 * h, uint32_t(data_start / hunk + h));
    for (size_t sct = 0; sct < sectors; ++sct) {
        size_t at = data_start + (sct / per) * hunk + (sct % per) * unit;
        if (cd) {
            for (int i = 1; i <= 10; ++i) b[at + i] = 0xFF;
            b[at + 15] = raw_mode2 ? 2 : 1;
            std::memcpy(&b[at + (raw_mode2 ? 24 : 16)], &iso[sct * 2048], 2048);
        } else std::memcpy(&b[at], &iso[sct * 2048], 2048);
    }
    return b;
}
static Bytes with_noise(Bytes iso, uint32_t seed) {   // incompressible tail so a .cso ends up bigger than the 4 KB the library treats as a possible image
    Bytes n = noise(64 * 2048, seed);
    iso.insert(iso.end(), n.begin(), n.end());
    return iso;
}
static Bytes make_cso(const Bytes& iso) {
    const uint32_t bs = 2048;
    size_t n = iso.size() / bs;
    Bytes out(24 + (n + 1) * 4, 0);
    std::memcpy(&out[0], "CISO", 4);
    put_le32(out, 4, 24); put_le32(out, 8, (uint32_t)iso.size()); put_le32(out, 16, bs); out[20] = 1;
    for (size_t i = 0; i < n; ++i) {
        put_le32(out, 24 + i * 4, (uint32_t)out.size());
        std::vector<uint8_t> comp(bs * 2);
        size_t cl = tdefl_compress_mem_to_mem(comp.data(), comp.size(), &iso[i * bs], bs, 128);
        if (cl == 0 || cl >= bs) { put_le32(out, 24 + i * 4, (uint32_t)out.size() | 0x80000000u); out.insert(out.end(), iso.begin() + i * bs, iso.begin() + (i + 1) * bs); }
        else out.insert(out.end(), comp.begin(), comp.begin() + cl);
    }
    put_le32(out, 24 + n * 4, (uint32_t)out.size());
    return out;
}
struct SfoField { std::string key, text; bool number = false; uint32_t num = 0; };
static Bytes make_sfo(const std::vector<SfoField>& fields) {
    std::string keys; std::vector<uint32_t> koff;
    for (auto& f : fields) { koff.push_back((uint32_t)keys.size()); keys += f.key; keys.push_back(0); }
    while (keys.size() % 4) keys.push_back(0);
    Bytes data; std::vector<uint32_t> doff, dlen;
    for (auto& f : fields) {
        doff.push_back((uint32_t)data.size());
        if (f.number) { dlen.push_back(4); for (int i = 0; i < 4; ++i) data.push_back(uint8_t(f.num >> (8 * i))); }
        else { dlen.push_back((uint32_t)f.text.size() + 1); data.insert(data.end(), f.text.begin(), f.text.end()); data.push_back(0); while (data.size() % 4) data.push_back(0); }
    }
    const size_t n = fields.size(), kt = 20 + n * 16, dt = kt + keys.size();
    Bytes b(dt + data.size(), 0);
    std::memcpy(&b[0], "\0PSF", 4);
    put_le32(b, 4, 0x0101); put_le32(b, 8, (uint32_t)kt); put_le32(b, 12, (uint32_t)dt); put_le32(b, 16, (uint32_t)n);
    for (size_t i = 0; i < n; ++i) {
        put_le16(b, 20 + i * 16, koff[i]); put_le16(b, 20 + i * 16 + 2, fields[i].number ? 0x0404 : 0x0204);
        put_le32(b, 20 + i * 16 + 4, dlen[i]); put_le32(b, 20 + i * 16 + 8, fields[i].number ? 4 : (uint32_t)((dlen[i] + 3) & ~3u)); put_le32(b, 20 + i * 16 + 12, doff[i]);
    }
    std::memcpy(&b[kt], keys.data(), keys.size());
    std::memcpy(&b[dt], data.data(), data.size());
    return b;
}
static void psp_dir_record(Bytes& b, size_t at, const std::string& name, uint32_t lba, uint32_t size, uint8_t flags) {
    size_t len = 33 + name.size();
    if (len & 1) ++len;
    b[at] = (uint8_t)len; put_le32(b, at + 2, lba); put_le32(b, at + 10, size); b[at + 25] = flags; b[at + 32] = (uint8_t)name.size();
    std::memcpy(&b[at + 33], name.data(), name.size());
}
static Bytes make_psp_iso(const std::string& id, const std::string& title) {
    Bytes sfo = make_sfo({{"CATEGORY", "UG"}, {"DISC_ID", id}, {"PARENTAL_LEVEL", "", true, 5}, {"TITLE", title}});
    Bytes b(64 * 2048, 0);
    uint8_t* pvd = &b[16 * 2048];
    pvd[0] = 1; std::memcpy(pvd + 1, "CD001", 5); std::memcpy(pvd + 8, "PSP GAME", 8); pvd[156] = 34;
    put_le32(b, 16 * 2048 + 156 + 2, 20); put_le32(b, 16 * 2048 + 156 + 10, 2048);
    psp_dir_record(b, 20 * 2048, "PSP_GAME", 21, 2048, 2);
    psp_dir_record(b, 21 * 2048, "PARAM.SFO;1", 22, (uint32_t)sfo.size(), 0);
    std::memcpy(&b[22 * 2048], sfo.data(), sfo.size());
    for (size_t i = 24 * 2048; i < b.size(); ++i) b[i] = (uint8_t)((i >> 7) & 0xFF);
    return b;
}

/* ------------------------------------------------------------------ archives */
typedef std::vector<std::pair<std::string, Bytes>> Members;   // UTF-8 member names
static fs::path P8(const std::string& s) { return fs::u8path(s); }
static std::string S8(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
static void write_file(const fs::path& p, const Bytes& d) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write((const char*)d.data(), (std::streamsize)d.size());
}
static Bytes read_file(const fs::path& p) { std::ifstream f(p, std::ios::binary); return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
static bool make_zip(const fs::path& out, const Members& m) {
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    fs::remove(out, ec);
    mz_zip_archive z;
    std::memset(&z, 0, sizeof z);
    FILE* f = _wfopen(out.wstring().c_str(), L"wb");
    if (!f) return false;
    if (!mz_zip_writer_init_cfile(&z, f, 0)) { std::fclose(f); return false; }
    bool ok = true;
    for (const auto& e : m) ok = mz_zip_writer_add_mem(&z, e.first.c_str(), e.second.data(), e.second.size(), MZ_DEFAULT_COMPRESSION) && ok;
    ok = mz_zip_writer_finalize_archive(&z) && ok;
    mz_zip_writer_end(&z);
    std::fclose(f);
    return ok;
}
static bool make_7z(const fs::path& out, const Members& m, const fs::path& stage) {
    std::error_code ec;
    fs::remove_all(stage, ec);
    fs::create_directories(stage, ec);
    for (const auto& e : m) write_file(stage / P8(e.first), e.second);
    fs::create_directories(out.parent_path(), ec);
    fs::remove(out, ec);
    std::wstring cmd = L"\"" + std::wstring(_wgetenv(L"SystemRoot")) + L"\\System32\\tar.exe\" --format 7zip -cf \"" + out.wstring() + L"\" -C \"" + stage.wstring() + L"\"";
    for (const auto& e : m) cmd += L" \"" + P8(e.first).wstring() + L"\"";
    {   /* CreateProcessW directly: cmd.exe would squeeze the Unicode names through the OEM code page */
        STARTUPINFOW si{}; si.cb = sizeof si;
        si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 120000);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
    }
    fs::remove_all(stage, ec);
    return fs::exists(out) && is_7z_file(S8(out));
}

/* ------------------------------------------------------------------ names */
static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static std::string upper_ext(const std::string& name) {
    size_t d = name.find_last_of('.');
    if (d == std::string::npos) return name;
    std::string e = name.substr(d);
    for (auto& c : e) c = (char)std::toupper((unsigned char)c);
    return name.substr(0, d) + e;
}
/* what the library should show for a file/archive name with nothing else to go on */
static std::string clean_title(const std::string& stem) {
    std::string o; int depth = 0;
    for (char c : stem) {
        if (c == '(' || c == '[') { ++depth; continue; }
        if (c == ')' || c == ']') { if (depth > 0) --depth; continue; }
        if (depth == 0) o.push_back(c == '_' ? ' ' : c);
    }
    std::string r; bool sp = false;
    for (char c : o) { if (c == ' ') { sp = true; continue; } if (sp && !r.empty()) r.push_back(' '); sp = false; r.push_back(c); }
    return r;
}
static bool generic_title(const std::string& t) {
    std::string l = lower(t);
    static const char* const g[] = {"game", "disc", "disk", "rom", "data", "main", "image", "iso", "boot", "cd", "dvd", "dump", "cartridge", "untitled", "unknown", "track", "backup", "default"};
    for (const char* x : g) if (l == x || l.rfind(x, 0) == 0 && l.size() <= std::strlen(x) + 2 && l.find_first_not_of("0123456789 ", std::strlen(x)) == std::string::npos) return true;
    return l.empty();
}

/* ------------------------------------------------------------------ fixtures */
struct Fx {
    std::string id;                       // "gc-iso"
    System sys = System::Unknown;
    std::string stem;                     // display stem
    Members files;                        // member names (relative, may hold sub folders for loadiine)
    std::string main;                     // member the library lists / launches (loose and unpacked)
    std::set<std::string> launch_exts;    // acceptable extensions of the launch path after the unpack
    bool arch = true;                     // archives of this are expected to be listed
    bool loose_name_title = false;        // loose title comes from the file name (no header / database title)
    bool arch_name_title = false;         // archived title must be the archive name (no header / database title)
    bool key_same_in_arch = true;         // key equals the loose key
    bool extract_play = false;            // archived entry unpacks on Play (issue == "extract") rather than at scan time
};
typedef std::function<Fx(const std::string& stem, bool generic)> Maker;
struct Recipe { std::string id; Maker make; };

static Fx cart(const std::string& id, System s, const std::string& stem, bool generic, const std::string& ext, const Bytes& data, bool name_title) {
    Fx f; f.id = id; f.sys = s; f.stem = stem;
    std::string nm = (generic ? std::string("game") : stem) + ext;
    f.files.push_back({nm, data}); f.main = nm;
    f.loose_name_title = f.arch_name_title = name_title;
    f.launch_exts = {lower(ext)};
    return f;
}
static std::string cue_for(const std::vector<std::string>& bins, const char* mode) {
    std::string c;
    int t = 1;
    for (const auto& b : bins) { char buf[64]; std::snprintf(buf, sizeof buf, "  TRACK %02d %s\r\n    INDEX 01 00:00:00\r\n", t, t == 1 ? mode : "AUDIO"); c += "FILE \"" + b + "\" BINARY\r\n" + buf; ++t; }
    return c;
}

static std::vector<Recipe> recipes() {
    std::vector<Recipe> r;
    auto add = [&](const std::string& id, Maker m) { r.push_back({id, std::move(m)}); };
    /* ---- N64 ---- */
    add("n64-z64", [](const std::string& s, bool g) { return cart("n64-z64", System::N64, s, g, ".z64", make_n64_z64("FAKELAND N64", 1), false); });
    add("n64-v64", [](const std::string& s, bool g) { return cart("n64-v64", System::N64, s, g, ".v64", swap_pairs(make_n64_z64("FAKELAND N64", 1)), false); });
    add("n64-n64", [](const std::string& s, bool g) { return cart("n64-n64", System::N64, s, g, ".n64", swap_words(make_n64_z64("FAKELAND N64", 1)), false); });
    /* ---- handheld ---- */
    add("gb", [](const std::string& s, bool g) { return cart("gb", System::GB, s, g, ".gb", make_gb("FAKEGB", 0, 0x1234, 1), false); });
    add("gbc", [](const std::string& s, bool g) { return cart("gbc", System::GBC, s, g, ".gbc", make_gb("FAKEGBC", 0x80, 0xABCD, 1), false); });
    add("gba", [](const std::string& s, bool g) { return cart("gba", System::GBA, s, g, ".gba", make_gba("FAKEGBA", "AFKG", 1), false); });
    add("nds", [](const std::string& s, bool g) { return cart("nds", System::NDS, s, g, ".nds", make_nds("FAKEDS", "AFKD", 1), false); });
    add("nes", [](const std::string& s, bool g) { return cart("nes", System::NES, s, g, ".nes", make_nes(1), true); });
    add("snes", [](const std::string& s, bool g) { return cart("snes", System::SNES, s, g, ".sfc", make_snes(1), true); });
    /* ---- 3DS ---- */
    auto d3 = [](const std::string& id, const std::string& ext, const Bytes& data) {
        return [=](const std::string& s, bool g) { Fx f = cart(id, System::N3DS, s, g, ext, data, true); f.extract_play = true; return f; };
    };
    add("3ds-3ds", d3("3ds-3ds", ".3ds", make_ncsd(0x0004000000FA1100ull, "CTR-P-FKAA")));
    add("3ds-cci", d3("3ds-cci", ".cci", make_ncsd(0x0004000000FA1200ull, "CTR-P-FKBA")));
    add("3ds-cia", d3("3ds-cia", ".cia", make_cia(0x0004000000FA1300ull)));
    add("3ds-3dsx", d3("3ds-3dsx", ".3dsx", make_3dsx()));
    /* ---- GameCube ---- */
    auto gcx = [](const std::string& id, const std::string& ext, const std::string& gid, std::function<Bytes(const std::string&, const std::string&)> mk) {
        return [=](const std::string& s, bool g) {
            Fx f = cart(id, System::GC, s, g, ext, mk(gid, "Fakeland " + id), false);
            f.arch_name_title = true; f.extract_play = true; return f;
        };
    };
    add("gc-iso", gcx("gc-iso", ".iso", "GFAE01", [](auto i, auto t) { return make_gc_iso(i, t); }));
    add("gc-gcm", gcx("gc-gcm", ".gcm", "GFBE01", [](auto i, auto t) { return make_gc_iso(i, t); }));
    add("gc-ciso", gcx("gc-ciso", ".ciso", "GFCE01", [](auto i, auto t) { return make_ciso(i, t); }));
    add("gc-rvz", gcx("gc-rvz", ".rvz", "GFDE01", [](auto i, auto t) { return make_rvz("RVZ\x01", 1, i, t); }));
    add("gc-wia", gcx("gc-wia", ".wia", "GFEE01", [](auto i, auto t) { return make_rvz("WIA\x01", 1, i, t); }));
    add("gc-gcz", gcx("gc-gcz", ".gcz", "GFFE01", [](auto i, auto t) { return make_gcz(zlib_fixed_literals(gc_header(i, t)), 40000); }));   // 40000 blocks: block 0 far from the file start
    add("gc-nkit", [](const std::string& s, bool g) {
        Fx f = cart("gc-nkit", System::GC, s, g, ".iso", make_gc_iso("GFGE01", "Fakeland gc-nkit"), false);
        std::string nm = (g ? std::string("game") : s) + ".nkit.iso";
        f.files[0].first = nm; f.main = nm; f.arch_name_title = true; f.extract_play = true; f.launch_exts = {".iso"}; return f; });
    /* ---- Wii U ---- */
    add("wiiu-wua", [](const std::string& s, bool g) {
        Fx f = cart("wiiu-wua", System::WiiU, s, g, ".wua", make_wua("000500001fffe500_v0"), false);
        f.arch_name_title = true; f.key_same_in_arch = false; f.extract_play = true; return f; });
    add("wiiu-wux", [](const std::string& s, bool g) { Fx f = cart("wiiu-wux", System::WiiU, s, g, ".wux", make_wux("WUP-P-FKXE"), true); f.arch = false; return f; });
    add("wiiu-loadiine", [](const std::string& s, bool g) {
        Fx f; f.id = "wiiu-loadiine"; f.sys = System::WiiU; f.stem = s; f.arch = false;
        std::string base = g ? "game" : s;
        f.files.push_back({base + "/code/fake.rpx", filler(8192, 3)});
        f.files.push_back({base + "/meta/meta.xml", text("<?xml version=\"1.0\"?><menu><title_id type=\"hexBinary\" length=\"8\">000500001fffe600</title_id><longname_en>Fakeland Loadiine</longname_en></menu>")});
        f.files.push_back({base + "/content/a.bin", filler(4096, 4)});
        f.main = base + "/code/fake.rpx"; f.launch_exts = {".rpx"}; return f; });
    /* ---- PlayStation ---- */
    auto ps_sets = [](const std::string& id, System sys, const char* cnf_key, const std::string& serial_cnf, uint32_t seed) {
        return [=](const std::string& s, bool g) {
            std::string cnf = std::string(cnf_key) + " = cdrom0:\\" + serial_cnf + ";1\r\nVER = 1.00\r\n";
            std::string b = g ? "disc" : s;
            Fx f; f.id = id; f.sys = sys; f.stem = s; f.extract_play = true;
            f.files.push_back({b + ".bin", to_raw2352(make_cnf_iso(cnf, 40, seed))});
            f.files.push_back({b + ".cue", text(cue_for({b + ".bin"}, "MODE2/2352"))});
            f.main = b + ".cue"; f.launch_exts = {".cue"}; f.loose_name_title = f.arch_name_title = true; return f;
        };
    };
    add("ps1-cuebin", ps_sets("ps1-cuebin", System::PS1, "BOOT", "SLUS_999.01", 1));
    add("ps2-cuebin", ps_sets("ps2-cuebin", System::PS2, "BOOT2", "SLUS_999.02", 2));
    add("ps1-multitrack", [](const std::string& s, bool g) {
        std::string b = g ? "disc" : s, t1 = b + " (Track 1).bin", t2 = b + " (Track 2).bin";
        Fx f; f.id = "ps1-multitrack"; f.sys = System::PS1; f.stem = s; f.extract_play = true;
        f.files.push_back({t1, to_raw2352(make_cnf_iso("BOOT = cdrom:\\SLUS_999.03;1\r\nVER = 1.00\r\n", 40, 3))});
        f.files.push_back({t2, noise(2352 * 30, 9)});
        f.files.push_back({b + ".cue", text(cue_for({t1, t2}, "MODE2/2352"))});
        f.main = b + ".cue"; f.launch_exts = {".cue"}; f.loose_name_title = f.arch_name_title = true; return f; });
    add("ps1-iso", [](const std::string& s, bool g) { Fx f = cart("ps1-iso", System::PS1, s, g, ".iso", make_cnf_iso("BOOT = cdrom:\\SLUS_999.04;1\r\nVER = 1.00\r\n", 40, 4), true); f.extract_play = true; return f; });
    add("ps1-chd", [](const std::string& s, bool g) {
        Fx f = cart("ps1-chd", System::PS1, s, g, ".chd", make_chd(make_cnf_iso("BOOT = cdrom:\\SLUS_999.05;1\r\nVER = 1.00\r\n", 40, 5), true, true), true);
        f.extract_play = true; f.arch = false; return f; });   // a compressed CHD cannot be read without unpacking: archives of it are reported, not listed
    add("ps1-m3u", [](const std::string& s, bool g) {
        std::string b = g ? "disc" : s;
        Fx f; f.id = "ps1-m3u"; f.sys = System::PS1; f.stem = s; f.extract_play = true;
        f.files.push_back({b + " (Disc 1).bin", to_raw2352(make_cnf_iso("BOOT = cdrom:\\SLUS_999.06;1\r\nVER = 1.00\r\n", 40, 6))});
        f.files.push_back({b + " (Disc 1).cue", text(cue_for({b + " (Disc 1).bin"}, "MODE2/2352"))});
        f.files.push_back({b + " (Disc 2).bin", to_raw2352(make_cnf_iso("BOOT = cdrom:\\SLUS_999.07;1\r\nVER = 1.00\r\n", 40, 7))});
        f.files.push_back({b + " (Disc 2).cue", text(cue_for({b + " (Disc 2).bin"}, "MODE2/2352"))});
        f.files.push_back({b + ".m3u", text(b + " (Disc 1).cue\r\n" + b + " (Disc 2).cue\r\n")});
        f.main = b + ".m3u"; f.launch_exts = {".m3u"}; f.loose_name_title = f.arch_name_title = true; return f; });
    add("ps2-iso", [](const std::string& s, bool g) {
        Fx f = cart("ps2-iso", System::PS2, s, g, ".iso", make_cnf_iso("BOOT2 = cdrom0:\\SLUS_999.08;1\r\nVER = 1.00\r\n", 40, 8), true); f.extract_play = true; return f; });
    add("ps2-cso", [](const std::string& s, bool g) {
        Fx f = cart("ps2-cso", System::PS2, s, g, ".cso", make_cso(with_noise(make_cnf_iso("BOOT2 = cdrom0:\\SLUS_999.09;1\r\nVER = 1.00\r\n", 40, 9), 9)), true); f.extract_play = true; return f; });
    add("ps2-chd", [](const std::string& s, bool g) {
        Fx f = cart("ps2-chd", System::PS2, s, g, ".chd", make_chd(make_cnf_iso("BOOT2 = cdrom0:\\SLUS_999.10;1\r\nVER = 1.00\r\n", 40, 10), false, false), true); f.extract_play = true; f.arch = false; return f; });
    /* ---- PSP ---- */
    add("psp-iso", [](const std::string& s, bool g) { Fx f = cart("psp-iso", System::PSP, s, g, ".iso", make_psp_iso("ULUS99001", "Fakeland PSP"), false); f.extract_play = true; return f; });
    add("psp-cso", [](const std::string& s, bool g) { Fx f = cart("psp-cso", System::PSP, s, g, ".cso", make_cso(with_noise(make_psp_iso("ULUS99002", "Fakeland PSP CSO"), 11)), false); f.extract_play = true; return f; });
    return r;
}

/* ------------------------------------------------------------------ variants */
enum Var { LOOSE, LOOSE_SUB, LOOSE_GENERIC, ZIP, ZIP_JUNK, Z7, Z7_JUNK, RAR, ZIP_UP, Z7_UP, ZIP_SPECIAL, Z7_SPECIAL, Z7_CJK, ZIP_GENERIC, Z7_GENERIC, MIS_LOOSE, MIS_ZIP, MIS_7Z, NVARS };
static const char* const kVarName[NVARS] = {"loose", "sub", "loose_gen", "zip", "zip_junk", "7z", "7z_junk", "rar", "ZIP_UP", "7Z_UP", "zip_spec", "7z_spec", "7z_cjk", "zip_gen", "7z_gen", "mis_loose", "mis_zip", "mis_7z"};
static const char* const kSpecial = u8"Fäke Pokémon's, The (USA, Europe) [!] Édition";
static const char* const kCjk = u8"ポケモン Fake Edition (Japan)";
static bool is_archive_var(Var v) { return (v >= ZIP && v <= Z7_GENERIC) || v == MIS_ZIP || v == MIS_7Z; }

static Members junk_for(const std::string& main) {
    Members j;
    j.push_back({"__MACOSX/._" + P8(main).filename().u8string(), noise(4096, 77)});   // AppleDouble resource fork, same extension, big enough to be taken for a disc
    j.push_back({".DS_Store", noise(6148, 5)});
    j.push_back({"Thumbs.db", noise(5000, 6)});
    j.push_back({"readme.txt", text("downloaded from somewhere\r\n")});
    j.push_back({"release.nfo", text("nfo\r\n")});
    j.push_back({"cover.jpg", noise(9000, 8)});
    return j;
}

struct Built {
    fs::path dir;           // folder to scan
    fs::path container;     // archive path, or the loose main file
    Fx fx;
    std::string stem;
    bool expect_found = true;
};

static std::string g_wrong_folder(const Fx& f) {
    switch (f.sys) {
    case System::N64: return "GBA"; case System::GB: return "GBC"; case System::GBC: return "GB"; case System::GBA: return "N64"; case System::NDS: return "3DS";
    case System::N3DS: return "NDS"; case System::NES: return "SNES"; case System::SNES: return "NES"; case System::GC: return "PS2"; case System::WiiU: return "GameCube";
    case System::PS1: return "PS2"; case System::PS2: return "PS1"; case System::PSP: return "PS2"; default: return "Other";
    }
}

static bool build_cell(const Recipe& rc, Var v, const fs::path& cells, const fs::path& stage, Built* out) {
    std::string stem = "Fakeland " + rc.id + " (USA)";
    if (v == ZIP_SPECIAL || v == Z7_SPECIAL) stem = kSpecial;
    const std::string arc_stem = v == Z7_CJK ? std::string(kCjk) : stem;   // bsdtar crashes writing 7z members with CJK names: only the ARCHIVE name is CJK, the member stays ASCII
    bool generic = v == LOOSE_GENERIC || v == ZIP_GENERIC || v == Z7_GENERIC;
    Fx fx = rc.make(stem, generic);
    fs::path dir = cells / (rc.id + "__" + kVarName[v]);
    if (v == MIS_LOOSE || v == MIS_ZIP || v == MIS_7Z) dir = cells / g_wrong_folder(fx) / (rc.id + "__" + kVarName[v]);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    Members m = fx.files;
    if (v == ZIP_UP || v == Z7_UP) { for (auto& e : m) e.first = upper_ext(e.first); fx.main = upper_ext(fx.main); }
    Built b; b.fx = fx; b.dir = dir; b.stem = arc_stem;
    switch (v) {
    case LOOSE: case MIS_LOOSE:
        for (auto& e : m) write_file(dir / P8(e.first), e.second);
        b.container = dir / P8(fx.main);
        break;
    case LOOSE_SUB:
        for (auto& e : m) write_file(dir / "a" / "b c" / P8(e.first), e.second);
        b.container = dir / "a" / "b c" / P8(fx.main);
        break;
    case LOOSE_GENERIC:   // game folder named after the game, files inside named "game"/"disc"
        dir = dir / P8(arc_stem);
        for (auto& e : m) write_file(dir / P8(e.first), e.second);
        b.container = dir / P8(fx.main); b.dir = dir.parent_path();
        break;
    default: {   // archives
        bool sub = v == ZIP_JUNK || v == Z7_JUNK;
        if (sub) {
            Members n;
            Members j = junk_for(fx.main);
            for (auto& e : j) n.push_back(e);   // junk at the archive root, the AppleDouble file (same extension as the game) listed FIRST
            for (auto& e : m) n.push_back({"Some Folder/Inner (v1.1)/" + e.first, e.second});
            m = std::move(n);
        }
        bool seven = v == Z7 || v == Z7_JUNK || v == RAR || v == Z7_UP || v == Z7_SPECIAL || v == Z7_CJK || v == Z7_GENERIC || v == MIS_7Z;
        std::string ext = v == RAR ? ".rar" : seven ? ".7z" : ".zip";
        if (v == ZIP_UP) ext = ".ZIP";
        if (v == Z7_UP) ext = ".7Z";
        fs::path arc = dir / (P8(arc_stem) += ext);
        bool ok;
        if (v == Z7_CJK) {   // bsdtar cannot even write an archive whose path has CJK characters: write it under an ASCII name, then rename
            fs::path tmp = dir / "plain.7z";
            ok = make_7z(tmp, m, stage);
            if (ok) { std::error_code rec; fs::rename(tmp, arc, rec); ok = !rec; }
        } else ok = seven ? make_7z(arc, m, stage) : make_zip(arc, m);
        if (!ok) return false;
        b.container = arc;
        break;
    }
    }
    *out = b;
    return true;
}

/* ------------------------------------------------------------------ checks */
struct CellResult { bool ran = false; std::string codes; std::vector<std::string> notes; };
static std::map<std::string, std::map<int, CellResult>> g_matrix;   // fixture -> variant -> result
static int g_cells = 0, g_failed = 0;
static bool g_quiet = false;

static const GameEntry* find_sys(const std::vector<GameEntry>& v, System s) { for (auto& e : v) if (e.system == s) return &e; return nullptr; }
static bool is_archive_ext(const std::string& p) { std::string e = lower(S8(P8(p).extension())); return e == ".zip" || e == ".7z" || e == ".rar"; }

static void run_cell(const Recipe& rc, Var v, const fs::path& cells, const fs::path& stage, const GameEntry* loose_base) {
    CellResult& R = g_matrix[rc.id][v];
    R.ran = true;
    rom_cache_clear();   // every cell starts with an empty cache: the same game appears in many containers and they must not share an unpack folder
    auto fail = [&](char code, const std::string& why) { if (R.codes.find(code) == std::string::npos) R.codes.push_back(code); R.notes.push_back(std::string(1, code) + ": " + why); };
    Built b;
    if (!build_cell(rc, v, cells, stage, &b)) { fail('B', "fixture build failed (bsdtar?)"); ++g_cells; ++g_failed; return; }
    const Fx& fx = b.fx;
    const bool archive = is_archive_var(v);
    const bool expect_found = !archive || fx.arch;
    auto v1 = scan_library({S8(b.dir)}, nullptr);
    if (!expect_found) {
        /* unsupported inside an archive: nothing listed, but the archive is reported with a reason (never silently dropped) */
        if (!v1.empty()) fail('F', "unexpectedly listed: " + v1[0].title);
        bool reported = false;
        for (auto& s : scan_skipped_archives()) if (P8(s.path) == b.container && s.reason.size() > 12) reported = true;
        if (!reported) fail('M', "unsupported archive not reported with a reason");
    } else {
        if (v1.size() != 1) {
            fail('F', "expected 1 game, found " + std::to_string(v1.size()) + (v1.empty() ? "" : " (" + v1[0].title + " / " + v1[1 % v1.size()].title + ")"));
            std::string why;
            for (auto& s : scan_skipped_archives()) if (P8(s.path) == b.container) why = s.reason;
            if (!why.empty()) R.notes.push_back("   skipped: " + why);
        } else {
            GameEntry e = v1[0];
            if (e.system != fx.sys) fail('S', std::string("system ") + system_short(e.system) + " want " + system_short(fx.sys));
            /* title */
            const std::string want_name = clean_title(b.stem);
            if (e.title.empty() || generic_title(e.title) || e.title.find("SLUS") != std::string::npos || e.title.find("GFAE") != std::string::npos) fail('T', "title '" + e.title + "' is empty/generic/a serial");
            else if (archive && fx.arch_name_title && e.title != want_name) fail('T', "title '" + e.title + "' want '" + want_name + "'");
            else if (!archive && v != LOOSE_GENERIC && fx.loose_name_title && e.title != want_name) fail('T', "title '" + e.title + "' want '" + want_name + "'");
            else if (v == LOOSE_GENERIC && fx.loose_name_title && e.title != want_name) fail('T', "generic file name in game folder: title '" + e.title + "' want '" + want_name + "'");
            else if (archive && !fx.arch_name_title && loose_base && e.title != loose_base->title) fail('T', "archived title '" + e.title + "' differs from loose '" + loose_base->title + "'");
            /* key */
            if (loose_base && archive && fx.key_same_in_arch && e.key != loose_base->key && (loose_base->key.find(":file-") == std::string::npos || b.stem == "Fakeland " + rc.id + " (USA)")) fail('K', "key " + e.key + " differs from loose " + loose_base->key);
            /* launch path */
            std::string lp = e.path;
            std::string pre_key = e.key, pre_title = e.title;
            if (e.issue == "extract") {
                if (!archive) fail('X', "loose file flagged extract");
                std::string out, err;
                if (!extracted_3ds_path(e).empty()) { /* already unpacked at install/scan: fine */ }
                bool ok = extract_3ds_zip(e, [](uint64_t, uint64_t) { return true; }, &out, &err);
                if (!ok) fail('X', "unpack failed: " + err);
                else {
                    lp = out;
                    if (!fs::exists(P8(out))) fail('P', "unpacked launch path missing: " + out);
                    std::string xe = lower(S8(P8(out).extension()));
                    if (!fx.launch_exts.count(xe)) fail('P', "launch ext " + xe + " (path " + out + ")");
                    if (extracted_3ds_path(e) != out) fail('C', "extracted_3ds_path != unpack result");
                    if (fs::exists(P8(out)) && fs::is_regular_file(P8(out)) && fs::file_size(P8(out)) == 0) fail('P', "launch file is empty");
                    /* single-file formats: bytes survive the round trip */
                    if (fx.files.size() == 1 && fs::exists(P8(out)) && read_file(P8(out)) != fx.files[0].second) fail('X', "unpacked bytes differ");
                    if (fx.sys == System::PS1 || fx.sys == System::PS2) {   // PCSX2 route: the image handed over must be a real image, never the .cue
                        if (fx.sys == System::PS2) {
                            std::string img, ierr;
                            if (!ps2::launch_image(out, extract_3ds_dir(e), &img, &ierr)) fail('P', "ps2::launch_image: " + ierr);
                            else if (lower(S8(P8(img).extension())) == ".cue" || !fs::exists(P8(img))) fail('P', "ps2 image " + img);
                        }
                    }
                    /* cache reuse: still current after the rescan, same file untouched */
                    auto t0 = fs::last_write_time(P8(out));
                    uint64_t c0 = library_identify_count();
                    auto v2 = scan_library({S8(b.dir)}, nullptr);
                    if (library_identify_count() != c0) fail('R', "rescan re-identified");
                    if (v2.size() != 1 || v2[0].key != pre_key || v2[0].title != pre_title || v2[0].path != e.path) fail('R', "rescan changed the entry after unpack");
                    else if (extracted_3ds_path(v2[0]) != out) fail('C', "unpack not reused after rescan");
                    if (fs::exists(P8(out)) && fs::last_write_time(P8(out)) != t0) fail('C', "unpacked file rewritten by rescan");
                }
            } else {
                if (!fs::exists(P8(lp))) fail('P', "path missing " + lp);
                std::string xe = lower(S8(P8(lp).extension()));
                if (archive && is_archive_ext(lp)) fail('P', "launch path is the archive " + lp);
                if (!fx.launch_exts.count(xe) && !(fx.sys == System::N64 && xe == ".z64")) fail('P', "launch ext " + xe);
                if (archive && fx.files.size() == 1 && fs::exists(P8(lp)) && read_file(P8(lp)) != fx.files[0].second) fail('P', "archived cart bytes differ");
                if (!archive && v != LOOSE_GENERIC && fx.files.size() > 1 && S8(P8(lp).filename()) != S8(P8(fx.main).filename())) fail('P', "loose launch file " + lp + " is not " + fx.main);
                uint64_t c0 = library_identify_count();
                auto v2 = scan_library({S8(b.dir)}, nullptr);
                if (library_identify_count() != c0) fail('R', "rescan re-identified");
                if (v2.size() != 1 || v2[0].key != pre_key || v2[0].path != e.path || v2[0].title != pre_title) fail('R', "rescan changed the entry");
            }
        }
    }
    ++g_cells;
    if (!R.codes.empty()) ++g_failed;
}

/* the loose-baseline entry of a recipe (for key/title comparison) */
static bool baseline(const Recipe& rc, const fs::path& cells, const fs::path& stage, GameEntry* out) {
    Built b;
    if (!build_cell(rc, LOOSE, cells, stage, &b)) return false;
    auto v = scan_library({S8(b.dir)}, nullptr);
    if (v.size() != 1) return false;
    *out = v[0];
    return true;
}

/* ------------------------------------------------------------------ non-matrix checks */
struct Simple { std::string name; bool ok; std::string note; };
static std::vector<Simple> g_simple;
static void simple(const std::string& n, bool ok, const std::string& note = std::string()) { g_simple.push_back({n, ok, note}); if (!ok) ++g_failed; ++g_cells; }

static const SkippedArchive* skipped_for(const std::vector<SkippedArchive>& s, const fs::path& p) { for (auto& x : s) if (P8(x.path) == p) return &x; return nullptr; }

static void test_unknown_and_damaged(const fs::path& root, const fs::path& stage) {
    const fs::path d = root / "unknown";
    std::error_code ec;
    fs::remove_all(d, ec);
    struct U { const char* name; Bytes data; const char* want; };   // want: substring the reason must contain ("" = any)
    Bytes zero;
    std::vector<U> us;
    us.push_back({"Broken Disc.iso", noise(100 * 1024, 1), "recogni"});
    us.push_back({"Tiny.z64", noise(10, 2), ""});
    us.push_back({"Odd Cart.gba", noise(200 * 1024, 3), ""});
    us.push_back({"Odd Cart.gb", noise(40 * 1024, 3), ""});
    us.push_back({"Odd Cart.nds", noise(100 * 1024, 3), ""});
    us.push_back({"Dangling.cue", text("FILE \"missing.bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n"), ""});
    us.push_back({"Wii Game.rvz", make_rvz("RVZ\x01", 2, "RFAE01", "Wii"), "Wii"});
    us.push_back({"Switch Game.nsp", noise(5000, 4), "Switch"});
    us.push_back({"Wii Game.wbfs", noise(5000, 4), "Wii"});
    us.push_back({"Pack.tar.gz", noise(5000, 4), "archive"});
    us.push_back({"Pack.7z.001", noise(5000, 4), "split"});
    us.push_back({"Pack.part1.rar", noise(5000, 4), ""});
    us.push_back({"Empty.zip", zero, ""});
    us.push_back({"Corrupt.7z", noise(5000, 4), ""});
    us.push_back({"Corrupt.zip", noise(5000, 4), ""});
    us.push_back({"Corrupt.rar", noise(5000, 4), ""});
    for (auto& u : us) write_file(d / u.name, u.data);
    /* truncated 7z: a good one cut in half */
    {
        fs::path good = root / "unknown_src" / "Good Thing.7z";
        make_7z(good, {{"Good Thing.iso", noise(300 * 1024, 5)}}, stage);
        Bytes gb = read_file(good);
        gb.resize(gb.size() / 2);
        write_file(d / "Cut Short.7z", gb);
    }
    /* nested archive and Wii U loadiine inside an archive */
    {
        fs::path inner = root / "unknown_src" / "inner.zip";
        make_zip(inner, {{"Game.iso", make_gc_iso("GNXE01", "Nested")}});
        make_zip(d / "Nested.zip", {{"inner.zip", read_file(inner)}});
        make_zip(d / "Loadiine Game.zip", {{"Game/code/x.rpx", filler(8192, 1)}, {"Game/meta/meta.xml", text("<menu/>")}});
        make_zip(d / "Only Docs.zip", {{"readme.txt", text("hello")}, {"cover.jpg", noise(3000, 1)}});
        make_zip(d / "PS1 Compressed.zip", {{"My Disc.chd", make_chd(make_cnf_iso("BOOT = cdrom:\\SLUS_999.55;1\r\n", 40, 5), true, true)}});
    }
    auto games = scan_library({S8(d)}, nullptr);
    auto sk = scan_skipped_archives();
    for (auto& g : games) std::printf("   unknown dir: unexpectedly listed %s (%s) %s\n", g.title.c_str(), g.key.c_str(), g.path.c_str());
    const char* const all[] = {"Broken Disc.iso", "Tiny.z64", "Odd Cart.gba", "Odd Cart.gb", "Odd Cart.nds", "Dangling.cue", "Wii Game.rvz", "Switch Game.nsp", "Wii Game.wbfs", "Pack.tar.gz", "Pack.7z.001", "Pack.part1.rar",
                               "Empty.zip", "Corrupt.7z", "Corrupt.zip", "Corrupt.rar", "Cut Short.7z", "Nested.zip", "Loadiine Game.zip", "Only Docs.zip", "PS1 Compressed.zip"};
    for (const char* n : all) {
        const SkippedArchive* s = skipped_for(sk, d / n);
        std::string want;
        for (auto& u : us) if (std::strcmp(u.name, n) == 0) want = u.want;
        bool listed = false;
        for (auto& g : games) if (P8(g.path).filename() == P8(n)) listed = true;
        bool ok = (s && s->reason.size() > 12 && (want.empty() || s->reason.find(want) != std::string::npos)) || (listed && std::strcmp(n, "Tiny.z64") == 0);
        simple(std::string("unknown/damaged is reported: ") + n, ok, s ? ("reason: " + s->reason + (want.empty() ? "" : "   (wanted \"" + want + "\")")) : (listed ? "listed as a game" : "SILENTLY DROPPED"));
    }
}

static void test_cache_limit(const fs::path& root, const fs::path& stage) {
    /* rom-cache limit far below one game: unpack still works, the fresh copy survives, an old one goes, the listing keeps every game, Play re-unpacks */
    const fs::path d = root / "limit";
    std::error_code ec;
    fs::remove_all(d, ec);
    make_7z(d / "A Disc Game.7z", {{"A Disc Game.iso", make_gc_iso("GLAE01", "A Disc Game")}}, stage);
    make_7z(d / "B Disc Game.7z", {{"B Disc Game.iso", make_gc_iso("GLBE01", "B Disc Game")}}, stage);
    make_7z(d / "C Cart.7z", {{"C Cart.gba", make_gba("CCART", "ACCR", 3)}}, stage);
    rom_cache_clear();
    rom_cache_set_limit(1);
    auto v = scan_library({S8(d)}, nullptr);
    simple("limit: three games listed", v.size() == 3, std::to_string(v.size()));
    GameEntry a, b, c;
    for (auto& g : v) { if (g.title == "A Disc Game") a = g; else if (g.title == "B Disc Game") b = g; else c = g; }
    std::string oa, ob, err;
    simple("limit: unpack A ok", !a.key.empty() && extract_3ds_zip(a, nullptr, &oa, &err), err);
    simple("limit: fresh copy survives a 1-byte limit", !extracted_3ds_path(a).empty());
    /* age A beyond the one-hour grace so the next unpack evicts it */
    {
        auto old = fs::file_time_type::clock::now() - std::chrono::hours(3);
        fs::path da = P8(extract_3ds_dir(a));
        fs::last_write_time(da / "stamp.txt", old, ec); fs::last_write_time(da, old, ec);
    }
    simple("limit: unpack B ok", !b.key.empty() && extract_3ds_zip(b, nullptr, &ob, &err), err);
    simple("limit: old copy A evicted, B kept", extracted_3ds_path(a).empty() && !extracted_3ds_path(b).empty());
    auto v2 = scan_library({S8(d)}, nullptr);
    simple("limit: evicted game still listed", v2.size() == 3);
    GameEntry a2;
    for (auto& g : v2) if (g.title == "A Disc Game") a2 = g;
    simple("limit: Play re-unpacks the evicted game", !a2.key.empty() && extract_3ds_zip(a2, nullptr, &oa, &err) && !extracted_3ds_path(a2).empty(), err);
    /* the archived cartridge sits in the cache too: even after eviction pressure the entry must stay playable (path exists) or be re-created by a rescan */
    {
        auto old = fs::file_time_type::clock::now() - std::chrono::hours(3);
        std::error_code e2;
        for (const auto& it : fs::directory_iterator(root / "data" / "rom-cache", e2)) if (it.is_directory(e2) && it.path().filename() != "gc") { fs::last_write_time(it.path(), old, e2); }
        for (auto& g : v2) if (g.title == "C Cart") { /* cart unpacked at scan time */ }
        rom_cache_trim(1);
        std::string cp;
        for (auto& g : scan_library({S8(d)}, nullptr)) if (g.system == System::GBA) cp = g.path;
        simple("limit: archived cartridge playable after trim + rescan", !cp.empty() && fs::exists(P8(cp)), cp);
    }
    rom_cache_set_limit(40ull << 30);
}

static void test_misc(const fs::path& root, const fs::path& stage) {
    /* a game known only by its (Redump) archive name, filed under every kind of console folder: the console is the game's, not the folder's */
    {
        Bytes cdish = filler(700 * 1024, 3);
        put(cdish, 0x8001, "CD001");
        struct N { const char* arc; const char* inner; System want; };
        const N ns[] = {{"Crash Bandicoot (USA)", ".bin", System::PS1}, {"Gauntlet - Dark Legacy (USA)", ".iso", System::PS2}};
        for (const N& n : ns)
            for (const char* folder : {"PS1", "PS2", "GameCube", "Games"})
                for (int z = 0; z < 2; ++z) {
                    const fs::path d = root / "names" / folder / (z ? "zip" : "7z") / n.arc;
                    std::error_code ec;
                    fs::remove_all(d, ec);
                    const std::string member = std::string(n.arc) + n.inner;
                    if (z) make_zip(d / (std::string(n.arc) + ".zip"), {{member, cdish}}); else make_7z(d / (std::string(n.arc) + ".7z"), {{member, cdish}}, stage);
                    auto v = scan_library({S8(d)}, nullptr);
                    simple(std::string("name-only disc ") + n.arc + " in a '" + folder + "' folder (" + (z ? "zip" : "7z") + ") is " + system_short(n.want),
                           v.size() == 1 && v[0].system == n.want, v.empty() ? "not listed" : std::string(system_short(v[0].system)) + " / " + v[0].title);
                }
    }
    /* a changed archive with another file name inside: exactly one image stays in the cache folder and it is the new one */
    {
        const fs::path d = root / "reunpack";
        std::error_code ec;
        fs::remove_all(d, ec);
        const fs::path arc = d / "Three D Game.zip";
        make_zip(arc, {{"First Name.3ds", make_ncsd(0x0004000000FA3100ull, "CTR-P-FKDA")}});
        auto v = scan_library({S8(d)}, nullptr);
        std::string out, err;
        bool ok1 = v.size() == 1 && extract_3ds_zip(v[0], nullptr, &out, &err);
        make_zip(arc, {{"Second Name.3ds", make_ncsd(0x0004000000FA3100ull, "CTR-P-FKDA")}});
        fs::last_write_time(arc, fs::last_write_time(arc, ec) + std::chrono::seconds(30), ec);
        auto v2 = scan_library({S8(d)}, nullptr);
        std::string out2;
        bool ok2 = v2.size() == 1 && extract_3ds_zip(v2[0], nullptr, &out2, &err);
        int images = 0;
        if (ok2) for (const auto& it : fs::directory_iterator(P8(out2).parent_path(), ec)) if (lower(S8(it.path().extension())) == ".3ds") ++images;
        simple("changed zip with another inner name: one image, the new one", ok1 && ok2 && images == 1 && P8(out2).filename() == "Second Name.3ds" && extracted_3ds_path(v2[0]) == out2, ok2 ? out2 : err);
    }
    /* a disc whose only file is a raw .bin (no .cue) is listed under its console: the hub writes a one-track cue sheet when you press Play */
    {
        const fs::path d = root / "lonebin";
        std::error_code ec;
        fs::remove_all(d, ec);
        write_file(d / "Lonely Track.bin", to_raw2352(make_cnf_iso("BOOT = cdrom:\\SLUS_999.77;1\r\nVER = 1.00\r\n", 40, 3)));
        auto v = scan_library({S8(d)}, nullptr);
        simple("lone .bin disc is listed (PlayStation)", v.size() == 1 && v[0].system == System::PS1 && lower(S8(P8(v[0].path).extension())) == ".bin", v.empty() ? "not listed" : v[0].path);
    }
}

/* ------------------------------------------------------------------ main */
static void bad_param(const wchar_t* expr, const wchar_t* func, const wchar_t* file, unsigned line, uintptr_t) {
    std::fwprintf(stderr, L"CRT invalid parameter: %ls in %ls (%ls:%u)\n", expr ? expr : L"?", func ? func : L"?", file ? file : L"?", line);
    std::fflush(stderr);
    std::_Exit(66);
}
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    std::fprintf(stderr, "CRASH: exception %08lx at %p\n", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
int main(int argc, char** argv) {
    _set_invalid_parameter_handler(bad_param);
    SetUnhandledExceptionFilter(crash_filter);
    std::string filter;
    bool keep = false;
    for (int i = 1; i < argc; ++i) { std::string a = argv[i]; if (a == "--keep") keep = true; else if (a == "--quiet") g_quiet = true; else filter = a; }
    fs::path root = fs::temp_directory_path() / (L"formats_matrix_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    library_set_data_dir(S8(root / "data"));
    const fs::path stage = root / "stage", cells = root / "cells";
    bool have7z = make_7z(root / "probe.7z", {{"a.txt", Bytes(100, 'a')}}, stage);
    std::printf("7z writer: %s\n", have7z ? "yes" : "NO (7z cells will fail)");

    std::vector<Recipe> rs = recipes();
    for (const Recipe& rc : rs) {
        if (!filter.empty() && rc.id.find(filter) == std::string::npos) continue;
        GameEntry base;
        bool have_base = baseline(rc, cells, stage, &base);
        for (int vi = 0; vi < NVARS; ++vi) {
            Var v = (Var)vi;
            Fx probe = rc.make("x", false);
            if (v == LOOSE_GENERIC && !probe.loose_name_title) continue;   // generic file names only matter where the title comes from the name
            if ((v == ZIP_GENERIC || v == Z7_GENERIC) && (!probe.arch_name_title || !probe.arch)) continue;
            if (!g_quiet) { std::fprintf(stderr, "[%s / %s]\n", rc.id.c_str(), kVarName[v]); std::fflush(stderr); }
            try { run_cell(rc, v, cells, stage, have_base ? &base : nullptr); }
            catch (const std::exception& ex) { CellResult& R = g_matrix[rc.id][v]; R.ran = true; R.codes += "E"; R.notes.push_back(std::string("E: exception escaped the library: ") + ex.what()); ++g_cells; ++g_failed; }
            catch (...) { CellResult& R = g_matrix[rc.id][v]; R.ran = true; R.codes += "E"; R.notes.push_back("E: unknown exception escaped the library"); ++g_cells; ++g_failed; }
        }
    }
    if (filter.empty()) { test_unknown_and_damaged(root, stage); test_cache_limit(root, stage); test_misc(root, stage); }

    /* the matrix */
    std::printf("\nFORMATS MATRIX  ('.' ok, letters = failed checks: B build F found S system T title K key R rescan X unpack P launch-path C cache M message; ' ' n/a)\n");
    std::printf("%-15s", "fixture");
    for (int vi = 0; vi < NVARS; ++vi) std::printf(" %-9s", kVarName[vi]);
    std::printf("\n");
    for (const Recipe& rc : rs) {
        auto it = g_matrix.find(rc.id);
        if (it == g_matrix.end()) continue;
        std::printf("%-15s", rc.id.c_str());
        for (int vi = 0; vi < NVARS; ++vi) {
            auto c = it->second.find(vi);
            std::string s = c == it->second.end() || !c->second.ran ? " " : c->second.codes.empty() ? "." : c->second.codes;
            std::printf(" %-9s", s.c_str());
        }
        std::printf("\n");
    }
    std::printf("\nFAILED CELLS\n");
    for (const Recipe& rc : rs) {
        auto it = g_matrix.find(rc.id);
        if (it == g_matrix.end()) continue;
        for (auto& c : it->second) {
            if (c.second.codes.empty()) continue;
            std::printf(" %s / %s\n", rc.id.c_str(), kVarName[c.first]);
            for (auto& n : c.second.notes) std::printf("     %s\n", n.c_str());
        }
    }
    if (!g_simple.empty()) {
        std::printf("\nOTHER CHECKS\n");
        for (auto& s : g_simple) { std::printf(" %s %s%s%s\n", s.ok ? "ok  " : "FAIL", s.name.c_str(), s.note.empty() ? "" : "   -- ", s.note.c_str()); }
    }
    std::printf("\nformats_matrix: %d checks/cells, %d failed\n", g_cells, g_failed);
    library_set_data_dir("");
    if (!keep) fs::remove_all(root, ec);
    else std::printf("kept %s\n", S8(root).c_str());
    return g_failed ? 1 : 0;
}
