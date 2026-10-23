// Console test for the multi-system library (library.cpp). Exit code 0 = all passed.
// Usage: hublibrary_test [folder-to-scan-and-print]
#include "winutil.h"
#include "library.h"
#include "games.h"
#include "miniz.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace hub;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

typedef std::vector<uint8_t> Bytes;

static bool write_file(const fs::path& p, const Bytes& d) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(d.data()), (std::streamsize)d.size());
    return (bool)f;
}

static Bytes noise(size_t n, uint32_t seed) {
    Bytes v(n);
    uint32_t x = seed;
    for (auto& b : v) { x = x * 1103515245u + 12345u; b = uint8_t(x >> 16); }
    return v;
}

static void put(Bytes& v, size_t off, const char* s) { std::memcpy(&v[off], s, std::strlen(s)); }
static void put_be32(Bytes& v, size_t off, uint32_t x) {
    v[off] = uint8_t(x >> 24); v[off + 1] = uint8_t(x >> 16); v[off + 2] = uint8_t(x >> 8); v[off + 3] = uint8_t(x);
}
static void put_le32(Bytes& v, size_t off, uint32_t x) {
    v[off + 3] = uint8_t(x >> 24); v[off + 2] = uint8_t(x >> 16); v[off + 1] = uint8_t(x >> 8); v[off] = uint8_t(x);
}
static void put_le64(Bytes& v, size_t off, uint64_t x) { put_le32(v, off, (uint32_t)x); put_le32(v, off + 4, (uint32_t)(x >> 32)); }

static Bytes make_n64(const char* title, uint32_t seed) {
    Bytes v = noise(1024 * 1024, seed);
    put_be32(v, 0, 0x80371240u);
    for (size_t i = 0x20; i < 0x34; ++i) v[i] = ' ';
    put(v, 0x20, title);
    put(v, 0x3B, "NTSE");
    return v;
}

static std::string ascii_lower_t(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static Bytes make_gba(const char* title, const char* code, uint32_t seed, size_t size = 128 * 1024, bool good = true) {
    Bytes v = noise(size, seed);
    std::memset(&v[0xA0], 0, 12);
    put(v, 0xA0, title);
    put(v, 0xAC, code);
    put(v, 0xB0, "01");
    v[0xB2] = 0x96;
    unsigned s = 0;
    for (int i = 0xA0; i <= 0xBC; ++i) s += v[i];
    v[0xBD] = uint8_t(-(int)s - 0x19);
    if (!good) { v[0xBD] ^= 0x55; v[0xB2] = 0; }
    return v;
}

/* Nintendo DS header: title 0x00 (12), game code 0x0C, unit code 0x12, ROM version 0x1E, logo CRC 0x15C, header CRC16 0x15E. */
static uint16_t nds_crc16(const Bytes& v, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= v[i];
        for (int b = 0; b < 8; ++b) crc = (crc & 1) ? uint16_t((crc >> 1) ^ 0xA001) : uint16_t(crc >> 1);
    }
    return crc;
}
static Bytes make_nds(const char* title, const char* code, uint8_t version, uint32_t seed, size_t size = 64 * 1024, bool good = true) {
    Bytes v = noise(size, seed);
    std::memset(&v[0], 0, 0x20);
    put(v, 0x00, title);
    put(v, 0x0C, code);
    v[0x12] = 0;
    v[0x1E] = version;
    v[0x15C] = 0x56; v[0x15D] = good ? 0xCF : 0x00;
    uint16_t c = nds_crc16(v, 0x15E);
    v[0x15E] = uint8_t(c); v[0x15F] = uint8_t(c >> 8);
    return v;
}

static Bytes make_gb(const char* title, uint8_t cgb_flag, uint16_t global, uint32_t seed, bool good = true,
                     const char* mfr = nullptr) {
    Bytes v = noise(32 * 1024, seed);
    std::memset(&v[0x134], 0, 16);
    put(v, 0x134, title);
    v[0x143] = cgb_flag;
    std::memset(&v[0x13F], 0, 4);
    if (mfr) std::memcpy(&v[0x13F], mfr, 4);
    v[0x14E] = uint8_t(global >> 8);
    v[0x14F] = uint8_t(global);
    uint8_t x = 0;
    for (int i = 0x134; i <= 0x14C; ++i) x = uint8_t(x - v[i] - 1);
    v[0x14D] = good ? x : uint8_t(x + 1);
    return v;
}

static Bytes gc_header(const char* id6, uint8_t disc, const char* title) {
    Bytes h(0x440, 0);
    put(h, 0, id6);
    h[6] = disc;
    put_be32(h, 0x1C, 0xC2339F3Du);
    put(h, 0x20, title);
    return h;
}

static Bytes make_gc_iso(const char* id6, uint8_t disc, const char* title) {
    Bytes h = gc_header(id6, disc, title);
    h.resize(0x2000, 0x11);
    return h;
}

static Bytes make_rvz(const char* magic4, uint32_t disc_type, const char* id6, uint8_t disc, const char* title) {
    Bytes v(0x400, 0);
    std::memcpy(&v[0], magic4, 4);
    put_be32(v, 0x48, disc_type);
    put_be32(v, 0x58 - 4, 0x200000);
    Bytes h = gc_header(id6, disc, title);
    std::memcpy(&v[0x58], h.data(), 0x80);
    return v;
}

static Bytes make_ciso(const char* id6, uint8_t disc, const char* title) {
    Bytes v(0x8000, 0);
    put(v, 0, "CISO");
    put_le32(v, 4, 0x200000);
    v[8] = 1;
    Bytes h = gc_header(id6, disc, title);
    v.insert(v.end(), h.begin(), h.end());
    v.resize(0x8000 + 0x1000, 0x22);
    return v;
}

/* zlib stream of `data` using one fixed-Huffman block of literals. */
static Bytes zlib_fixed_literals(const Bytes& data) {
    Bytes out = {0x78, 0x01};
    uint32_t acc = 0; int nb = 0;
    auto bit = [&](int b) { acc |= (uint32_t)b << nb; if (++nb == 8) { out.push_back((uint8_t)acc); acc = 0; nb = 0; } };
    auto code = [&](uint32_t c, int len) { for (int i = len - 1; i >= 0; --i) bit((c >> i) & 1); };
    bit(1); bit(1); bit(0);  // BFINAL=1, BTYPE=01 (LSB first)
    for (uint8_t b : data) {
        if (b < 144) code(0x30u + b, 8);
        else code(0x190u + (b - 144), 9);
    }
    code(0, 7);  // end of block
    if (nb) out.push_back((uint8_t)acc);
    out.insert(out.end(), {0, 0, 0, 0});  // adler (ignored)
    return out;
}

static Bytes zlib_stored(const Bytes& data) {
    Bytes out = {0x78, 0x01, 0x01};
    uint16_t n = (uint16_t)data.size();
    out.push_back(uint8_t(n)); out.push_back(uint8_t(n >> 8));
    out.push_back(uint8_t(~n)); out.push_back(uint8_t((uint16_t)~n >> 8));
    out.insert(out.end(), data.begin(), data.end());
    out.insert(out.end(), {0, 0, 0, 0});
    return out;
}

static Bytes make_gcz(const Bytes& block0) {
    Bytes v(32 + 12, 0);
    put_le32(v, 0, 0xB10BC001u);
    put_le64(v, 8, block0.size());
    put_le64(v, 16, 0x100000);
    put_le32(v, 24, 0x8000);
    put_le32(v, 28, 1);
    put_le64(v, 32, 0);  // block 0 pointer
    v.insert(v.end(), block0.begin(), block0.end());
    return v;
}

static const GameEntry* find_key(const std::vector<GameEntry>& v, const std::string& key) {
    for (const auto& e : v) if (e.key == key) return &e;
    return nullptr;
}

static void test_identify(const fs::path& dir) {
    GameEntry e;

    // GBA
    Bytes gba = make_gba("POKEMON EMER", "BPEE", 7);
    CHECK(write_file(dir / "id" / "emerald.gba", gba));
    CHECK(identify_game((dir / "id" / "emerald.gba").string(), &e));
    CHECK(e.system == System::GBA);
    CHECK(e.serial == "BPEE");
    CHECK(e.catalog_id == "pokeemerald" && e.title == "Pokemon Emerald" && e.key.rfind("gba:bpee-", 0) == 0);  // serial match via find_game_for
    {   // user's other GBA headers: catalog titles replace the 12-char header names
        struct { const char* hdr; const char* code; const char* id; const char* title; } t[] = {
            {"POKEMON FIRE", "BPRE", "pokefirered", "Pokemon FireRed"}, {"POKEMON LEAF", "BPGE", "pokeleafgreen", "Pokemon LeafGreen"},
            {"POKEMON RUBY", "AXVE", "pokeruby", "Pokemon Ruby"}, {"POKEMON SAPP", "AXPE", "pokesapphire", "Pokemon Sapphire"}};
        GameEntry x;
        for (auto& r : t) {
            CHECK(write_file(dir / "id" / (std::string(r.code) + ".gba"), make_gba(r.hdr, r.code, 31)));
            CHECK(identify_game((dir / "id" / (std::string(r.code) + ".gba")).string(), &x));
            CHECK(x.catalog_id == r.id && x.title == r.title && x.key.rfind(std::string("gba:") + ascii_lower_t(r.code) + "-", 0) == 0);
        }
    }
    CHECK(e.key.rfind("gba:bpee-", 0) == 0 && e.key.size() == 9 + 8);
    CHECK(e.size == gba.size());
    CHECK(fs::path(e.path).is_absolute());
    // CRC32 sanity: known vector "123456789" -> cbf43926 is checked via a file whose crc we can't easily
    // compute here; instead make sure the key changes with content.
    Bytes gba2 = gba; gba2[5000] ^= 1;
    CHECK(write_file(dir / "id" / "emerald2.gba", gba2));
    GameEntry e2;
    CHECK(identify_game((dir / "id" / "emerald2.gba").string(), &e2) && e2.key != e.key);
    CHECK(write_file(dir / "id" / "bad.gba", make_gba("BAD", "ABCD", 8, 128 * 1024, false)));
    CHECK(!identify_game((dir / "id" / "bad.gba").string(), &e));
    CHECK(write_file(dir / "id" / "tiny.gba", make_gba("TINY", "ABCD", 9, 4096)));
    CHECK(!identify_game((dir / "id" / "tiny.gba").string(), &e));

    // GB / GBC
    CHECK(write_file(dir / "id" / "tetris.gb", make_gb("TETRIS", 0, 0x1234, 1)));
    CHECK(identify_game((dir / "id" / "tetris.gb").string(), &e));
    CHECK(e.system == System::GB && e.key == "gb:tetris-1234" && e.title == "Tetris");
    CHECK(write_file(dir / "id" / "zelda.gbc", make_gb("ZELDA DX", 0x80, 0xABCD, 2)));
    CHECK(identify_game((dir / "id" / "zelda.gbc").string(), &e));
    CHECK(e.system == System::GBC && e.key == "gbc:zelda-dx-abcd" && e.title == "Zelda DX");
    CHECK(e.serial == "ZELDA DX");  // CGB cart without a manufacturer code: header title
    // Catalog serials for GB/GBC (games.cpp): CGB manufacturer code, else the header title
    CHECK(write_file(dir / "id" / "gold.gbc", make_gb("POKEMON_GLD", 0x80, 0x682D, 9, true, "AAUE")));
    CHECK(identify_game((dir / "id" / "gold.gbc").string(), &e));
    CHECK(e.serial == "AAUE" && e.catalog_id == "pokegold" && e.title == "Pokemon Gold" && e.key == "gbc:pokemon_gld-682d");
    // The user's real headers: raw names must still resolve to proper catalog titles
    CHECK(write_file(dir / "id" / "crystal.gbc", make_gb("PM_CRYSTAL", 0x80, 0x129F, 20, true, "BYTE")));
    CHECK(identify_game((dir / "id" / "crystal.gbc").string(), &e));
    CHECK(e.catalog_id == "pokecrystal" && e.title == "Pokemon Crystal" && e.key == "gbc:pm_crystal-129f");
    CHECK(write_file(dir / "id" / "silver.gbc", make_gb("POKEMON_SLV", 0x80, 0x0DAE, 21, true, "AAXE")));
    CHECK(identify_game((dir / "id" / "silver.gbc").string(), &e));
    CHECK(e.catalog_id == "pokesilver" && e.title == "Pokemon Silver" && e.key == "gbc:pokemon_slv-0dae");
    CHECK(write_file(dir / "id" / "yel2.gbc", make_gb("POKEMON YEL", 0x80, 0x047C, 22)));
    CHECK(identify_game((dir / "id" / "yel2.gbc").string(), &e));
    CHECK(e.catalog_id == "pokeyellow" && e.title == "Pokemon Yellow" && e.key == "gbc:pokemon-yel-047c");
    CHECK(write_file(dir / "id" / "gold-sp.gbc", make_gb("POKEMON GLD", 0x80, 0x682D, 23)));   // space instead of underscore
    CHECK(identify_game((dir / "id" / "gold-sp.gbc").string(), &e) && e.catalog_id == "pokegold");
    CHECK(write_file(dir / "id" / "red.gb", make_gb("POKEMON RED", 0, 0x91E6, 10)));
    CHECK(identify_game((dir / "id" / "red.gb").string(), &e));
    CHECK(e.serial == "POKEMON RED" && e.catalog_id == "pokered");
    CHECK(write_file(dir / "id" / "yellow.gb", make_gb("POKEMON YELLOW", 0x80, 0x047C, 11)));
    CHECK(identify_game((dir / "id" / "yellow.gb").string(), &e));
    CHECK(e.serial == "POKEMON YEL" && e.catalog_id == "pokeyellow");
    // Valid-alphanumeric junk in the code field: not a catalog code, so the header title matches
    CHECK(write_file(dir / "id" / "gold-junk.gbc", make_gb("POKEMON_GLD", 0x80, 0x682D, 12, true, "ZZ9Q")));
    CHECK(identify_game((dir / "id" / "gold-junk.gbc").string(), &e));
    CHECK(e.serial == "ZZ9Q" && e.catalog_id == "pokegold");
    CHECK(write_file(dir / "id" / "yellow-junk.gbc", make_gb("POKEMON YELLOW", 0x80, 0x047C, 13, true, "Q7Q7")));
    CHECK(identify_game((dir / "id" / "yellow-junk.gbc").string(), &e));
    CHECK(e.serial == "Q7Q7" && e.catalog_id == "pokeyellow");
    // Junk code with an unknown title: no catalog match
    CHECK(write_file(dir / "id" / "unknown.gbc", make_gb("NOPE GAME", 0x80, 0x0003, 14, true, "ZZ9Q")));
    CHECK(identify_game((dir / "id" / "unknown.gbc").string(), &e) && e.catalog_id.empty());
    // .gbc extension but DMG header -> header decides
    CHECK(write_file(dir / "id" / "plain.gbc", make_gb("PLAIN", 0, 0x0001, 3)));
    CHECK(identify_game((dir / "id" / "plain.gbc").string(), &e) && e.system == System::GB);
    // .gb with CGB-only flag
    CHECK(write_file(dir / "id" / "only.gb", make_gb("ONLY CGB", 0xC0, 0x0002, 4)));
    CHECK(identify_game((dir / "id" / "only.gb").string(), &e) && e.system == System::GBC);
    CHECK(write_file(dir / "id" / "badsum.gb", make_gb("BAD", 0, 1, 5, false)));
    CHECK(!identify_game((dir / "id" / "badsum.gb").string(), &e));
    // Empty header title falls back to the file name, tags stripped.
    CHECK(write_file(dir / "id" / "Some_Game (USA) [!].gb", make_gb("", 0, 0x0042, 6)));
    CHECK(identify_game((dir / "id" / "Some_Game (USA) [!].gb").string(), &e));
    CHECK(e.title == "Some Game" && e.key == "gb:untitled-0042");

    // N64
    Bytes n = make_n64("SUPER TEST 64", 99);
    CHECK(write_file(dir / "id" / "t.z64", n));
    CHECK(identify_game((dir / "id" / "t.z64").string(), &e));
    CHECK(e.system == System::N64 && e.serial == "NTSE" && e.title == "Super Test 64");
    CHECK(e.key.rfind("n64:", 0) == 0 && e.key.size() == 4 + 32 && e.catalog_id.empty());
    // Not a catalog dump, but header ID NKTE (Mario Kart 64 USA) identifies it by serial.
    Bytes mk = make_n64("MARIO KART", 5);
    put(mk, 0x3B, "NKTE");
    CHECK(write_file(dir / "id" / "mk.z64", mk));
    GameEntry emk;
    CHECK(identify_game((dir / "id" / "mk.z64").string(), &emk));
    CHECK(emk.serial == "NKTE" && emk.catalog_id == "mk64" && emk.title == "Mario Kart 64");
    Bytes v = n;
    for (size_t i = 0; i + 1 < v.size(); i += 2) std::swap(v[i], v[i + 1]);
    CHECK(write_file(dir / "id" / "t.v64", v));
    GameEntry ev;
    CHECK(identify_game((dir / "id" / "t.v64").string(), &ev) && ev.key == e.key);

    // GameCube
    CHECK(write_file(dir / "id" / "melee.iso", make_gc_iso("GALE01", 0, "Super Smash Bros. Melee")));
    CHECK(identify_game((dir / "id" / "melee.iso").string(), &e));
    CHECK(e.system == System::GC && e.serial == "GALE01" && e.key == "gc:gale01-0" && e.title == "Super Smash Bros. Melee");
    CHECK(write_file(dir / "id" / "d2.gcm", make_gc_iso("GXXE01", 1, "TWO DISC GAME")));
    CHECK(identify_game((dir / "id" / "d2.gcm").string(), &e) && e.key == "gc:gxxe01-1" && e.title == "Two Disc Game");
    Bytes notgc(0x2000, 0); put(notgc, 0, "GALE01");
    CHECK(write_file(dir / "id" / "wii_or_ps2.iso", notgc));
    CHECK(!identify_game((dir / "id" / "wii_or_ps2.iso").string(), &e));

    CHECK(write_file(dir / "id" / "a.rvz", make_rvz("RVZ\x01", 1, "GM4E01", 0, "MARIO KART")));
    CHECK(identify_game((dir / "id" / "a.rvz").string(), &e));
    CHECK(e.system == System::GC && e.key == "gc:gm4e01-0" && e.title == "Mario Kart");
    CHECK(write_file(dir / "id" / "b.wia", make_rvz("WIA\x01", 1, "GM4P01", 0, "Mario Kart PAL")));
    CHECK(identify_game((dir / "id" / "b.wia").string(), &e) && e.key == "gc:gm4p01-0");
    CHECK(write_file(dir / "id" / "wii.rvz", make_rvz("RVZ\x01", 2, "RSPE01", 0, "Wii Sports")));
    CHECK(!identify_game((dir / "id" / "wii.rvz").string(), &e));

    CHECK(write_file(dir / "id" / "c.ciso", make_ciso("GZLE01", 0, "Zelda WW")));
    CHECK(identify_game((dir / "id" / "c.ciso").string(), &e));
    CHECK(e.key == "gc:gzle01-0" && e.title == "Zelda WW");

    Bytes h = gc_header("GFZE01", 0, "F-Zero GX");
    CHECK(write_file(dir / "id" / "fixed.gcz", make_gcz(zlib_fixed_literals(h))));
    CHECK(identify_game((dir / "id" / "fixed.gcz").string(), &e));
    CHECK(e.key == "gc:gfze01-0" && e.title == "F-Zero GX");
    CHECK(write_file(dir / "id" / "stored.gcz", make_gcz(zlib_stored(gc_header("GPIE01", 0, "Pikmin")))));
    CHECK(identify_game((dir / "id" / "stored.gcz").string(), &e));
    CHECK(e.key == "gc:gpie01-0" && e.title == "Pikmin");
    // Undecodable block -> filename fallback.
    Bytes junk(100, 0xFF);
    CHECK(write_file(dir / "id" / "Weird Game (USA).gcz", make_gcz(junk)));
    CHECK(identify_game((dir / "id" / "Weird Game (USA).gcz").string(), &e));
    CHECK(e.key == "gc:file-weird-game" && e.title == "Weird Game" && e.serial.empty());

    /* Nintendo DS (.nds): header code is the serial, key = code + version + header CRC16 (no whole-file hash). */
    {
        Bytes dia = make_nds("POKEMON D", "ADAE", 5, 31);
        CHECK(write_file(dir / "id" / "diamond.nds", dia));
        GameEntry n;
        CHECK(identify_game((dir / "id" / "diamond.nds").string(), &n));
        CHECK(n.system == System::NDS && n.serial == "ADAE" && n.catalog_id == "pokediamond" && n.title == "Pokemon Diamond");
        char want[64];
        std::snprintf(want, sizeof want, "nds:adae-05%04x", (unsigned)nds_crc16(dia, 0x15E));
        CHECK(n.key == want);
        CHECK(std::string(system_short(n.system)) == "nds" && system_from_short("NDS") == System::NDS);
        CHECK(std::string(system_name(System::NDS)) == "Nintendo DS");
        // Same release padded to another size (trimmed vs untrimmed dump) keeps the key; another revision does not.
        Bytes padded = dia;
        padded.resize(256 * 1024, 0xFF);
        CHECK(write_file(dir / "id" / "diamond-padded.nds", padded));
        GameEntry np;
        CHECK(identify_game((dir / "id" / "diamond-padded.nds").string(), &np) && np.key == n.key);
        CHECK(write_file(dir / "id" / "diamond-r0.nds", make_nds("POKEMON D", "ADAE", 0, 31)));
        GameEntry nr;
        CHECK(identify_game((dir / "id" / "diamond-r0.nds").string(), &nr) && nr.key != n.key);
        // Unknown game: no catalog entry, title from the header; bad logo CRC / tiny file are rejected.
        CHECK(write_file(dir / "id" / "homebrew.nds", make_nds("MY HOMEBREW", "ABCD", 0, 32)));
        GameEntry nh;
        CHECK(identify_game((dir / "id" / "homebrew.nds").string(), &nh) && nh.catalog_id.empty() && nh.serial == "ABCD");
        CHECK(write_file(dir / "id" / "badlogo.nds", make_nds("BAD", "ABCD", 0, 33, 64 * 1024, false)));
        CHECK(!identify_game((dir / "id" / "badlogo.nds").string(), &nh));
        CHECK(write_file(dir / "id" / "tiny.nds", make_nds("TINY", "ABCD", 0, 34, 0x200)));
        CHECK(!identify_game((dir / "id" / "tiny.nds").string(), &nh));
        // A .nds is only a DS game by extension + header; a GBA-looking file with that extension is not.
        CHECK(write_file(dir / "id" / "notds.nds", make_gba("POKEMON EMER", "BPEE", 7)));
        CHECK(!identify_game((dir / "id" / "notds.nds").string(), &nh));
    }
    CHECK(!identify_game((dir / "id" / "missing.gba").string(), &e));
    CHECK(write_file(dir / "id" / "readme.txt", Bytes(100, 'a')));
    CHECK(!identify_game((dir / "id" / "readme.txt").string(), &e));
}

/* Zipped ROM with a ~120-char inner name under a ~150-char data dir: the old "<zip>-<crc>\<inner name>" layout
 * exceeded MAX_PATH. Also: a corrupt entry must leave no empty rom-cache folder behind. */
static void test_long_zip(const fs::path& dir) {
    fs::path base = dir / "longzip";
    while (base.string().size() < 150) base /= "deep_folder_0123456789";
    const std::string inner = std::string(105, 'P') + " (USA) [test].z64";
    CHECK(inner.size() >= 115);
    const fs::path zipdir = dir / "zips_long";
    const fs::path zip = zipdir / "Pokemon.zip";
    const fs::path bad_zip = zipdir / "Corrupt.zip";
    Bytes rom = make_n64("LONGZIP TEST", 7);
    library_set_data_dir((base / "userdata").string());
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(zipdir);

    mz_zip_archive z;
    std::memset(&z, 0, sizeof z);
    CHECK(mz_zip_writer_init_file(&z, zip.string().c_str(), 0));
    CHECK(mz_zip_writer_add_mem(&z, inner.c_str(), rom.data(), rom.size(), MZ_DEFAULT_COMPRESSION));
    CHECK(mz_zip_writer_finalize_archive(&z));
    mz_zip_writer_end(&z);

    GameEntry e;
    CHECK(identify_game(zip.string(), &e));
    CHECK(e.system == System::N64 && !e.title.empty());
    CHECK(e.save_stem == inner.substr(0, inner.size() - 4));   // DS-style saves keep the inner name's stem
    CHECK(e.path.size() < 240 && fs::exists(fs::path(e.path)));
    const std::string rom_name = fs::path(e.path).filename().string();
    CHECK(rom_name.size() < 16);
    // Identified again from the cache: same save stem.
    GameEntry e2;
    CHECK(identify_game(zip.string(), &e2) && e2.save_stem == e.save_stem && e2.path == e.path);

    // Corrupt the stored (uncompressed) data of a second zip: extraction fails on CRC.
    mz_zip_archive z2;
    std::memset(&z2, 0, sizeof z2);
    CHECK(mz_zip_writer_init_file(&z2, bad_zip.string().c_str(), 0));
    Bytes bad_rom = make_n64("BROKEN", 8);
    CHECK(mz_zip_writer_add_mem(&z2, "Bad.z64", bad_rom.data(), bad_rom.size(), MZ_NO_COMPRESSION));
    CHECK(mz_zip_writer_finalize_archive(&z2));
    mz_zip_writer_end(&z2);
    {
        std::string bytes;
        {
            std::ifstream in(bad_zip, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        bytes[30 + 7 + 500] ^= 0x5A;   // local header (30) + name "Bad.z64" (7), then inside the data
        std::ofstream out(bad_zip, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), (std::streamsize)bytes.size());
    }
    char crc_dir[16];
    std::snprintf(crc_dir, sizeof crc_dir, "%08x", (unsigned)mz_crc32(0, bad_rom.data(), bad_rom.size()));
    GameEntry bad;
    CHECK(!identify_game(bad_zip.string(), &bad));
    fs::path bad_dir = base / "userdata" / "rom-cache" / crc_dir;
    CHECK(!fs::exists(bad_dir, ec) || fs::is_empty(bad_dir, ec));
    library_set_data_dir("");
}

static void test_scan_cache_state(const fs::path& dir) {
    const fs::path lib = dir / "lib";
    CHECK(write_file(lib / "a" / "emerald.gba", make_gba("POKEMON EMER", "BPEE", 7)));
    CHECK(write_file(lib / "b" / "emerald-copy.GBA", make_gba("POKEMON EMER", "BPEE", 7)));  // dupe key
    CHECK(write_file(lib / "tetris.gb", make_gb("TETRIS", 0, 0x1234, 1)));
    CHECK(write_file(lib / "c" / "zelda.gbc", make_gb("ZELDA DX", 0x80, 0xABCD, 2)));
    CHECK(write_file(lib / "n64" / "x.z64", make_n64("ALPHA GAME", 1)));
    CHECK(write_file(lib / "n64" / "y.n64", make_n64("BETA GAME", 2)));
    CHECK(write_file(lib / "melee.iso", make_gc_iso("GALE01", 0, "Super Smash Bros. Melee")));
    CHECK(write_file(lib / "junk.gba", Bytes(200 * 1024, 0)));
    CHECK(write_file(lib / "notes.txt", Bytes(10, 1)));

    {   /* non-ASCII games folder: UTF-8 in settings -> wide path (e.g. DocumentsJeux Videos with an accent) */
        const fs::path jx = dir / fs::path(L"Jeux Vid\u00E9o") / "Game Boy";
        CHECK(write_file(jx / "uni.gb", make_gb("UNICODE", 0, 0x4321, 9)));
        std::wstring wroot = (dir / fs::path(L"Jeux Vid\u00E9o")).wstring();
        std::string u8root = winutil::to_utf8(wroot);
        library_set_data_dir((dir / "userdata_u").string());
        std::vector<GameEntry> ru = scan_library({u8root}, nullptr);
        CHECK(ru.size() == 1);
    }
    library_set_data_dir((dir / "userdata").string());
    uint64_t c0 = library_identify_count();
    int last_done = -1, last_total = -1, calls = 0;
    auto prog = [&](int d, int t) { last_done = d; last_total = t; ++calls; };
    std::vector<GameEntry> r = scan_library({lib.string()}, prog);
    uint64_t c1 = library_identify_count();

    CHECK(r.size() == 6);  // gba, gb, gbc, 2x n64, gc ; dupe + junk removed
    CHECK(c1 - c0 == 8);   // 8 candidate files with known extensions were identified
    CHECK(last_total == 8 && last_done == 8 && calls > 0);
    // Sorted by system (N64, GBA, GB, GBC, GC) then title.
    CHECK(r.size() == 6 && r[0].system == System::N64 && r[0].title == "Alpha Game");
    CHECK(r.size() == 6 && r[1].title == "Beta Game");
    CHECK(r.size() == 6 && r[2].system == System::GBA);
    CHECK(r.size() == 6 && r[3].system == System::GB);
    CHECK(r.size() == 6 && r[4].system == System::GBC);
    CHECK(r.size() == 6 && r[5].system == System::GC);
    const GameEntry* g = find_key(r, "gba:bpee-" + r[2].key.substr(9));
    CHECK(g && g->path.find("emerald.gba") != std::string::npos);  // first path kept
    CHECK(fs::exists(dir / "userdata" / "library_cache.json"));

    // Second scan: everything from cache.
    std::vector<GameEntry> r2 = scan_library({lib.string()}, nullptr);
    CHECK(library_identify_count() == c1);
    CHECK(r2.size() == r.size());
    for (size_t i = 0; i < r.size() && i < r2.size(); ++i)
        CHECK(r[i].key == r2[i].key && r[i].title == r2[i].title && r[i].path == r2[i].path &&
              r[i].size == r2[i].size && r[i].system == r2[i].system && r[i].serial == r2[i].serial);

    // Change one file (size changes) -> exactly one re-identify.
    CHECK(write_file(lib / "tetris.gb", [] { Bytes b = make_gb("TETRIS", 0, 0x1234, 1); b.resize(64 * 1024); return b; }()));
    std::vector<GameEntry> r3 = scan_library({lib.string()}, nullptr);
    CHECK(library_identify_count() == c1 + 1);
    CHECK(r3.size() == 6);
    // Delete a file -> it disappears without any re-identify.
    fs::remove(lib / "melee.iso");
    std::vector<GameEntry> r4 = scan_library({lib.string()}, nullptr);
    CHECK(library_identify_count() == c1 + 1 && r4.size() == 5);

    // State round-trip.
    const std::string k = r3[0].key;
    GameState s0 = game_state(k);
    CHECK(!s0.favorite && s0.last_played == 0 && s0.play_seconds == 0);
    set_favorite(k, true);
    record_play(k, 1700000000, 120);
    record_play(k, 1700001000, 30);
    GameState s1 = game_state(k);
    CHECK(s1.favorite && s1.last_played == 1700001000 && s1.play_seconds == 150);
    flush_library_state();
    CHECK(fs::exists(dir / "userdata" / "library.json"));
    library_set_data_dir((dir / "userdata").string());  // drops memory, reloads from disk
    GameState s2 = game_state(k);
    CHECK(s2.favorite && s2.last_played == 1700001000 && s2.play_seconds == 150);
    set_favorite(k, false);
    CHECK(!game_state(k).favorite && game_state(k).play_seconds == 150);
    CHECK(!game_state("unknown:key").favorite);
    flush_library_state();
}

/* Regression: a parallel scan of many real-format zips (GBA/GB, long names, non-ASCII folder). Every zip also makes the
 * scan spawn tar.exe to probe for a disc image; that child used to inherit other threads' open .part files, so the
 * rename failed and whole systems came up empty (GBA 0, GB 0). All zips must be identified with the right system. */
static void test_parallel_zip_scan(const fs::path& dir) {
    fs::path root = dir / fs::path(L"zipscan éè ポケモン");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ud", ec);
    library_set_data_dir(winutil::to_utf8((root / "ud").wstring()));
    const int N = 40;
    for (int i = 0; i < N; ++i) {
        char t[32], nm[160], code[8];
        std::snprintf(t, sizeof t, "ZIPTEST %02d", i);
        std::snprintf(nm, sizeof nm, "Pokemon - Zip Regression Version %02d (USA, Europe) (SGB Enhanced) (GB Compatible)", i);
        std::snprintf(code, sizeof code, "BZ%c%c", 'A' + i / 26, 'A' + i % 26);
        bool gba = (i % 2) == 0;
        Bytes rom = gba ? make_gba(t, code, 100 + i, 8 * 1024 * 1024) : make_gb(t, 0, (uint16_t)(0x2000 + i), 200 + i);
        mz_zip_archive z;
        std::memset(&z, 0, sizeof z);
        FILE* zf = _wfopen((root / (std::string(nm) + ".zip")).wstring().c_str(), L"wb");   // wide: the folder name is non-ASCII
        CHECK(zf != nullptr);
        if (!zf) continue;
        std::string inner = std::string(nm) + (gba ? ".gba" : ".gb");
        CHECK(mz_zip_writer_init_cfile(&z, zf, 0));
        CHECK(mz_zip_writer_add_mem(&z, inner.c_str(), rom.data(), rom.size(), MZ_BEST_COMPRESSION));
        CHECK(mz_zip_writer_finalize_archive(&z));
        mz_zip_writer_end(&z);
        std::fclose(zf);
    }
    std::vector<GameEntry> r = scan_library({winutil::to_utf8(root.wstring())}, nullptr);
    int gba = 0, gb = 0;
    for (const auto& e : r) {
        if (e.system == System::GBA) ++gba;
        if (e.system == System::GB) ++gb;
    }
    CHECK(gba == N / 2);
    CHECK(gb == N / 2);
    library_set_data_dir("");
    fs::remove_all(root, ec);
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 4 && std::string(argv[1]) == "--unpack") {   // manual check: hublibrary_test --unpack <archive> <scratch data dir>
        library_set_data_dir(argv[3]);
        GameEntry g, back;
        if (!identify_game(argv[2], &g) || g.issue != "extract") { std::printf("not an archived game\n"); return 2; }
        std::string out, err;
        auto t0 = std::chrono::steady_clock::now();
        uint64_t seen = 0;
        bool ok = extract_3ds_zip(g, [&](uint64_t d, uint64_t t) { if (d / (64u << 20) != seen / (64u << 20)) std::printf("  %llu / %llu MB\n", (unsigned long long)(d >> 20), (unsigned long long)(t >> 20)); seen = d; return true; }, &out, &err);
        auto s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("unpack %s in %llds: %s %s\n", ok ? "ok" : "FAILED", (long long)s, out.c_str(), err.c_str());
        if (ok && identify_game(out, &back)) std::printf("extracted image: serial=%s key=%s title=%s size=%llu\n", back.serial.c_str(), back.key.c_str(), back.title.c_str(), (unsigned long long)back.size);
        return ok ? 0 : 1;
    }
    if (argc == 4 && std::string(argv[1]) == "--ident") {   // manual check: hublibrary_test --ident <file> <scratch data dir>
        library_set_data_dir(argv[3]);
        GameEntry g;
        bool ok = identify_game(argv[2], &g);
        std::printf("identify %s sys=%s title=%s path=%s\n", ok ? "ok" : "FAILED", ok ? system_short(g.system) : "-", g.title.c_str(), g.path.c_str());
        return ok ? 0 : 1;
    }
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / "hublibrary_test_dir";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    CHECK(system_from_short("gbc") == System::GBC && std::string(system_short(System::GC)) == "gc");
    CHECK(std::string(system_name(System::GBA)) == "Game Boy Advance");

    // dedupe: an unzipped copy beats a zipped/7z copy of the same game; otherwise first wins
    CHECK(dedupe_prefers("D:/roms/a.z64", "D:/roms/a.zip"));
    CHECK(dedupe_prefers("D:/roms/a.gba", "D:/roms/b.7z"));
    CHECK(!dedupe_prefers("D:/roms/a.zip", "D:/roms/a.z64"));
    CHECK(!dedupe_prefers("D:/roms/a.z64", "D:/roms/b.n64"));
    CHECK(!dedupe_prefers("D:/roms/a.zip", "D:/roms/b.7z"));

    test_identify(dir);
    test_long_zip(dir);
    test_parallel_zip_scan(dir);
    test_scan_cache_state(dir);
    library_set_data_dir("");
    fs::remove_all(dir, ec);

    // ---- GameCube discs inside .7z / .zip archives ----
    {
        // pure helpers
        CHECK(archive_entry_safe("Mario Kart - Double Dash!! (USA).ciso"));
        CHECK(archive_entry_safe("sub/dir/disc.iso"));
        CHECK(!archive_entry_safe("../evil.iso"));
        CHECK(!archive_entry_safe("a/../../evil.iso"));
        CHECK(!archive_entry_safe("..\\evil.iso"));
        CHECK(!archive_entry_safe("/abs/disc.iso"));
        CHECK(!archive_entry_safe("C:\\disc.iso"));
        CHECK(!archive_entry_safe(""));
        CHECK(archive_entry_safe("v1..2/disc.iso"));   // ".." inside a name is fine
        auto lst = parse_tar_listing(
            "-rw-r--r--  0 0      0  1088455232 Oct 20  2022 Legend of Zelda, The - The Wind Waker (USA, Canada).ciso\r\n"
            "drwxr-xr-x  0 0      0           0 Oct 20  2022 folder/\r\n"
            "-rw-r--r--  0 0      0         326 Oct 02  2025 Vimm's Lair.txt\r\n");
        CHECK(lst.size() == 2 && lst[0].size == 1088455232ull && lst[0].name == "Legend of Zelda, The - The Wind Waker (USA, Canada).ciso");
        CHECK(lst[1].name == "Vimm's Lair.txt" && lst[1].size == 326);
        CHECK(gc_serial_for_name("Mario Kart - Double Dash!! (USA).7z") == "GM4E01");
        CHECK(gc_serial_for_name("Super Smash Bros. Melee (USA) (En,Ja) (Rev 2).nkit.iso") == "GALE01");
        CHECK(gc_serial_for_name("Totally Unknown Game (USA).7z").empty());
        CHECK(find_game_for("gc", "GXXE01", "") && find_game_for("gc", "GMSE01", "") && find_game_for("gc", "GP6E01", ""));

        fs::path gdir = fs::temp_directory_path() / "hublibrary_gc_archive";
        std::error_code gec;
        fs::remove_all(gdir, gec);
        fs::create_directories(gdir / "data");
        library_set_data_dir((gdir / "data").string());

        Bytes disc(0x2000, 0x11);
        std::memcpy(disc.data(), "GM4E01", 6);
        disc[6] = 0;
        disc[0x1C] = 0xC2; disc[0x1D] = 0x33; disc[0x1E] = 0x9F; disc[0x1F] = 0x3D;
        std::memcpy(disc.data() + 0x20, "MARIOKART DOUBLE DASH!!", 23);
        for (size_t i = 0x440; i < disc.size(); ++i) disc[i] = (uint8_t)(i * 7);
        fs::create_directories(gdir / "src");
        write_file(gdir / "src" / "Mario Kart - Double Dash!! (USA).iso", disc);

        auto make_7z = [&](const std::string& out, const std::string& member) {
            std::wstring cmd = L"\"\"" + std::wstring(_wgetenv(L"SystemRoot")) + L"\\System32\\tar.exe\" --format 7zip -cf \"" +
                               fs::path(out).wstring() + L"\" -C \"" + (gdir / "src").wstring() + L"\" \"" + fs::path(member).wstring() + L"\"\"";
            _wsystem(cmd.c_str());
        };
        // zip (miniz) and 7z (Windows bsdtar writes 7z too)
        mz_zip_archive gz;
        std::memset(&gz, 0, sizeof gz);
        const std::string zpath = (gdir / "Mario Kart - Double Dash!! (USA).zip").string();
        CHECK(mz_zip_writer_init_file(&gz, zpath.c_str(), 0));
        CHECK(mz_zip_writer_add_mem(&gz, "Mario Kart - Double Dash!! (USA).iso", disc.data(), disc.size(), MZ_DEFAULT_COMPRESSION));
        CHECK(mz_zip_writer_finalize_archive(&gz));
        mz_zip_writer_end(&gz);
        const std::string spath = (gdir / "Mario Kart - Double Dash!! (USA).7z").string();
        make_7z(spath, "Mario Kart - Double Dash!! (USA).iso");
        CHECK(fs::exists(spath) && is_7z_file(spath) && !is_7z_file(zpath));

        for (const std::string& arch : {spath, zpath}) {
            GameEntry g;
            CHECK(identify_game(arch, &g));
            CHECK(g.system == System::GC && g.serial == "GM4E01" && g.issue == "extract" && g.catalog_id == "mkdd");
            CHECK(g.key == "gc:gm4e01-0" && g.path == arch);
            CHECK(extracted_3ds_path(g).empty());
            CHECK(zip_3ds_unpacked_size(g) == disc.size());
            std::string out, err;
            CHECK(!extract_3ds_zip(g, [](uint64_t, uint64_t) { return false; }, &out, &err) && err == "cancelled");
            CHECK(extracted_3ds_path(g).empty());
            {
                std::error_code e1;
                bool part = false;
                for (auto& f : fs::recursive_directory_iterator(gdir / "data" / "rom-cache" / "gc", e1))
                    if (f.path().extension() == ".part") part = true;
                CHECK(!part);   // a cancelled unpack leaves no partial file
            }
            uint64_t last = 0;
            CHECK(extract_3ds_zip(g, [&](uint64_t d, uint64_t) { last = d; return true; }, &out, &err));
            CHECK(!out.empty() && out == extracted_3ds_path(g) && fs::u8path(out).filename() == "disc.iso" && out.size() < 200);
            CHECK(fs::file_size(fs::u8path(out)) == disc.size());
            {
                std::ifstream in(fs::u8path(out), std::ios::binary);
                Bytes got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                CHECK(got == disc);
            }
            GameEntry back;   // the unpacked image identifies as the same disc
            CHECK(identify_game(out, &back) && back.serial == "GM4E01" && back.key == g.key);
            fs::remove_all(fs::u8path(extract_3ds_dir(g)), gec);
        }

        // path traversal: an archive whose only disc entry escapes is rejected, nothing is written outside rom-cache
        mz_zip_archive ez;
        std::memset(&ez, 0, sizeof ez);
        const std::string epath = (gdir / "evil.zip").string();
        CHECK(mz_zip_writer_init_file(&ez, epath.c_str(), 0));
        CHECK(mz_zip_writer_add_mem(&ez, "../escape.iso", disc.data(), disc.size(), MZ_DEFAULT_COMPRESSION));
        CHECK(mz_zip_writer_finalize_archive(&ez));
        mz_zip_writer_end(&ez);
        GameEntry ev;
        CHECK(!identify_game(epath, &ev));
        GameEntry fake;
        fake.system = System::GC; fake.key = "gc:gm4e01-0"; fake.path = epath; fake.issue = "extract";
        std::string o2, e2s;
        CHECK(!extract_3ds_zip(fake, nullptr, &o2, &e2s));
        CHECK(!fs::exists(gdir / "escape.iso") && !fs::exists(gdir / "data" / "escape.iso"));

        // a 7z with no disc image, and a file named .7z with the wrong signature: not listed
        write_file(gdir / "src" / "notes.txt", Bytes(100, 'x'));
        const std::string tpath = (gdir / "notes.7z").string();
        make_7z(tpath, "notes.txt");
        GameEntry nt;
        CHECK(!identify_game(tpath, &nt));
        write_file(gdir / "fake.7z", Bytes(64, 'x'));
        CHECK(!identify_game((gdir / "fake.7z").string(), &nt));

        // non-ASCII archive and entry names (tar -tv output encoding), then stamp invalidation
        {
            CHECK(!archive_entry_safe("evil\".iso"));
            fs::path ud = gdir / L"u";
            fs::create_directories(ud / L"in");
            Bytes zd = disc;
            std::memcpy(zd.data(), "GZLE01", 6);
            write_file(ud / L"in" / L"Zelda ü.iso", zd);
            fs::path u7 = ud / L"Zelda ü.7z";
            std::wstring cmd = L"\"\"" + std::wstring(_wgetenv(L"SystemRoot")) + L"\\System32\\tar.exe\" --format 7zip -cf \"" + u7.wstring() +
                               L"\" -C \"" + (ud / L"in").wstring() + L"\" \"Zelda ü.iso\"\"";
            _wsystem(cmd.c_str());
            GameEntry u;
            CHECK(identify_game(u7.u8string(), &u));
            CHECK(u.serial == "GZLE01" && u.issue == "extract");
            std::string uo, ue;
            CHECK(extract_3ds_zip(u, nullptr, &uo, &ue));
            {
                std::ifstream in(fs::u8path(uo), std::ios::binary);
                Bytes got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                CHECK(got == zd);
            }
            CHECK(extracted_3ds_path(u) == uo);
            // replacing the archive invalidates the unpacked copy
            fs::last_write_time(u7, fs::file_time_type::clock::now() + std::chrono::hours(1));
            CHECK(extracted_3ds_path(u).empty());
            // a truncated image is not trusted either
            CHECK(extract_3ds_zip(u, nullptr, &uo, &ue) && extracted_3ds_path(u) == uo);
            { std::ofstream t(fs::u8path(uo), std::ios::binary | std::ios::trunc); t << "x"; }
            CHECK(extracted_3ds_path(u).empty());

            // two entries that the pattern handed to tar could both match: refused
            mz_zip_archive az;
            std::memset(&az, 0, sizeof az);
            const std::string amb = (gdir / "amb.zip").string();
            CHECK(mz_zip_writer_init_file(&az, amb.c_str(), 0));
            CHECK(mz_zip_writer_add_mem(&az, "Zelda \xC3\xBC.iso", zd.data(), zd.size(), MZ_DEFAULT_COMPRESSION));
            CHECK(mz_zip_writer_add_mem(&az, "Zelda \xC3\xA4.iso", zd.data(), zd.size(), MZ_DEFAULT_COMPRESSION));
            CHECK(mz_zip_writer_finalize_archive(&az));
            mz_zip_writer_end(&az);
            GameEntry am;
            CHECK(!identify_game(amb, &am));
        }

        // a library scan lists one card per game: Double Dash (.iso, .zip, .7z) and the non-ASCII Zelda;
        // the unzipped .iso copy is the one listed (dedupe_prefers)
        std::vector<GameEntry> sc = scan_library({gdir.string()}, nullptr);
        int gc_cards = 0, unzipped = 0;
        for (const auto& g : sc) {
            if (g.system != System::GC) continue;
            ++gc_cards;
            if (fs::path(g.path).extension() == ".iso") ++unzipped;
        }
        if (gc_cards != 2 || unzipped != 2) for (const auto& g : sc) std::printf("  %s [%s]\n", g.path.c_str(), g.issue.c_str());
        CHECK(gc_cards == 2);   // Double Dash (listed once) + the non-ASCII Zelda
        CHECK(unzipped == 2);   // both cards point at the unzipped image, not the archive
        library_set_data_dir("");
        fs::remove_all(gdir, gec);
    }

    if (argc > 1) {
        std::printf("--- scanning %s ---\n", argv[1]);
        // Optional argv[2]: scratch data dir (cache, rom-cache), e.g. a deep path to test long-path handling.
        library_set_data_dir(argc > 2 ? std::string(argv[2]) : (fs::temp_directory_path() / "hublibrary_realscan").string());
        std::vector<std::string> roots{argv[1]};
        if (const char* extra = std::getenv("HUBLIB_EXTRA_FOLDERS")) {   // ';'-separated additional scan roots
            std::string x = extra;
            for (size_t a = 0; a < x.size();) { size_t b = x.find(';', a); if (b == std::string::npos) b = x.size(); if (b > a) roots.push_back(x.substr(a, b - a)); a = b + 1; }
        }
        for (int pass = 0; pass < 2; ++pass) {
            uint64_t c = library_identify_count();
            auto t0 = std::chrono::steady_clock::now();
            std::vector<GameEntry> r = scan_library(roots, nullptr);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            std::printf("pass %d: %zu game(s), %llu identified, %lld ms\n", pass + 1, r.size(),
                        (unsigned long long)(library_identify_count() - c), (long long)ms);
            if (pass == 0) {
                for (const auto& e : r)
                    std::printf("  [%s] %s | key=%s serial=%s catalog=%s size=%llu\n    %s\n", system_short(e.system),
                                e.title.c_str(), e.key.c_str(), e.serial.c_str(), e.catalog_id.c_str(),
                                (unsigned long long)e.size, e.path.c_str());
            }
        }
        library_set_data_dir("");
    }

    if (g_fail == 0) {
        std::printf("hublibrary_test: all checks passed\n");
        return 0;
    }
    std::printf("hublibrary_test: %d check(s) failed\n", g_fail);
    return 1;
}
