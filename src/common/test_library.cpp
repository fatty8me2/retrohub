// Console test for the multi-system library (library.cpp). Exit code 0 = all passed.
// Usage: hublibrary_test [folder-to-scan-and-print]
#include "library.h"
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
    CHECK(e.catalog_id == "pokeemerald");  // serial match via find_game_for
    CHECK(e.title == "Pokemon Emer");
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
    CHECK(e.serial == "AAUE" && e.catalog_id == "pokegold");
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
        std::string u8root;
        std::wstring wroot = (dir / fs::path(L"Jeux Vid\u00E9o")).wstring();
        int n = WideCharToMultiByte(CP_UTF8, 0, wroot.c_str(), (int)wroot.size(), nullptr, 0, nullptr, nullptr);
        u8root.assign((size_t)n, 'x');
        WideCharToMultiByte(CP_UTF8, 0, wroot.c_str(), (int)wroot.size(), &u8root[0], n, nullptr, nullptr);
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

int main(int argc, char** argv) {
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / "hublibrary_test_dir";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    CHECK(system_from_short("gbc") == System::GBC && std::string(system_short(System::GC)) == "gc");
    CHECK(std::string(system_name(System::GBA)) == "Game Boy Advance");

    test_identify(dir);
    test_long_zip(dir);
    test_scan_cache_state(dir);
    library_set_data_dir("");
    fs::remove_all(dir, ec);

    if (argc > 1) {
        std::printf("--- scanning %s ---\n", argv[1]);
        // Optional argv[2]: scratch data dir (cache, rom-cache), e.g. a deep path to test long-path handling.
        library_set_data_dir(argc > 2 ? std::string(argv[2]) : (fs::temp_directory_path() / "hublibrary_realscan").string());
        for (int pass = 0; pass < 2; ++pass) {
            uint64_t c = library_identify_count();
            auto t0 = std::chrono::steady_clock::now();
            std::vector<GameEntry> r = scan_library({argv[1]}, nullptr);
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
