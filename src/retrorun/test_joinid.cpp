/* Console test: the game identity behind the join fingerprint (joinid.h, netcore-H4). The same game must give the same id on every PC whatever the container or header
 * (zip vs unpacked, iNES / SNES copier header, N64 byte order, a .cue sheet that points at the same data), and a different dump, a hack or another serial must not.
 * Scratch files only, no windows, no network. Exit 0 = pass. */
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "joinid.h"

namespace fs = std::filesystem;
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static fs::path g_dir;
static std::string put(const std::string& name, const std::vector<uint8_t>& d) {
    fs::path p = g_dir / name;
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write((const char*)d.data(), (std::streamsize)d.size());
    return p.u8string();
}
static std::string puts_(const std::string& name, const std::string& t) { return put(name, std::vector<uint8_t>(t.begin(), t.end())); }

static std::vector<uint8_t> noise(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed * 2654435761u + 1;
    for (auto& b : v) { x = x * 1664525u + 1013904223u; b = (uint8_t)(x >> 24); }
    return v;
}

static std::string zip_of(const std::string& name, const std::string& entry, const std::vector<uint8_t>& d) {
    fs::path p = g_dir / name;
    fs::create_directories(p.parent_path());
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_writer_init_file(&z, p.string().c_str(), 0)) return "";
    mz_zip_writer_add_mem(&z, entry.c_str(), d.data(), d.size(), MZ_DEFAULT_LEVEL);
    mz_zip_writer_finalize_archive(&z);
    mz_zip_writer_end(&z);
    return p.u8string();
}

int main() {
    g_dir = fs::temp_directory_path() / ("joinid_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(g_dir);

    // NES: an iNES header or none, zipped or not: one game
    {
        std::vector<uint8_t> prg = noise(32768, 1), hdr = {'N', 'E', 'S', 0x1A, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        std::vector<uint8_t> with = hdr; with.insert(with.end(), prg.begin(), prg.end());
        std::vector<uint8_t> hack = prg; hack[1000] ^= 1;
        std::vector<uint8_t> hackh = hdr; hackh.insert(hackh.end(), hack.begin(), hack.end());
        uint32_t a = joinid::of_game("nes", put("n/a.nes", with), ""), b = joinid::of_game("nes", put("n/b.nes", prg), ""), c = joinid::of_game("nes", zip_of("n/c.zip", "game.nes", with), "");
        uint32_t d = joinid::of_game("NES", put("n/d.nes", hackh), "");
        CHECK(a && a == b, "iNES header must not change the id (%08x vs %08x)", a, b);
        CHECK(a == c, "zipped and unpacked must match (%08x vs %08x)", a, c);
        CHECK(d && d != a, "a hacked ROM must differ");
    }
    // SNES: a 512-byte copier header is skipped
    {
        std::vector<uint8_t> rom = noise(0x20000, 2), hdr(512, 0);
        std::vector<uint8_t> with = hdr; with.insert(with.end(), rom.begin(), rom.end());
        uint32_t a = joinid::of_game("snes", put("s/a.sfc", rom), ""), b = joinid::of_game("snes", put("s/b.smc", with), "");
        CHECK(a && a == b, "SNES copier header must not change the id (%08x vs %08x)", a, b);
    }
    // N64: .z64 and byte-swapped .v64 / .n64 are one game
    {
        std::vector<uint8_t> z = noise(0x10000, 3);
        z[0] = 0x80; z[1] = 0x37; z[2] = 0x12; z[3] = 0x40;
        std::vector<uint8_t> v = z, n = z;
        for (size_t i = 0; i + 3 < v.size(); i += 4) { std::swap(v[i], v[i + 1]); std::swap(v[i + 2], v[i + 3]); std::swap(n[i], n[i + 3]); std::swap(n[i + 1], n[i + 2]); }
        uint32_t a = joinid::of_game("n64", put("m/a.z64", z), ""), b = joinid::of_game("n64", put("m/b.v64", v), ""), c = joinid::of_game("n64", put("m/c.n64", n), "");
        CHECK(a && a == b && a == c, "N64 byte order must not change the id (%08x %08x %08x)", a, b, c);
    }
    // GBA / GB: a plain CRC of the file (and of a zip's entry)
    {
        std::vector<uint8_t> g = noise(0x8000, 4), h = g; h[77] ^= 0x10;
        uint32_t a = joinid::of_game("gba", put("g/a.gba", g), ""), b = joinid::of_game("gba", zip_of("g/b.zip", "x.gba", g), ""), c = joinid::of_game("gba", put("g/c.gba", h), "");
        CHECK(a && a == b && c && c != a, "GBA ids: %08x %08x %08x", a, b, c);
    }
    // discs: a .cue is followed to its data, the sheet text and file names don't matter, the serial and the data do
    {
        std::vector<uint8_t> bin = noise(300000, 5), other = bin; other[10] ^= 0xFF;
        put("d1/game.bin", bin);
        puts_("d1/game.cue", "FILE \"game.bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n");
        put("d2/Game (USA).bin", bin);
        puts_("d2/Game (USA).cue", "REM made elsewhere\nFILE \"Game (USA).bin\" BINARY\n TRACK 01 MODE2/2352\n  INDEX 01 00:00:00\n");
        put("d3/game.bin", other);
        puts_("d3/game.cue", "FILE \"game.bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n");
        uint32_t a = joinid::of_game("ps1", g_dir.u8string() + "/d1/game.cue", "SLUS-00001");
        uint32_t b = joinid::of_game("ps1", g_dir.u8string() + "/d2/Game (USA).cue", "SLUS-00001");
        uint32_t c = joinid::of_game("ps1", g_dir.u8string() + "/d3/game.cue", "SLUS-00001");
        uint32_t d = joinid::of_game("ps1", g_dir.u8string() + "/d1/game.cue", "SLES-00002");
        uint32_t e = joinid::of_game("ps1", g_dir.u8string() + "/d1/game.bin", "SLUS-00001");
        CHECK(a && a == b, "same data behind another cue sheet must match (%08x vs %08x)", a, b);
        CHECK(c != a, "other data must differ");
        CHECK(d != a, "another serial must differ");
        CHECK(e == a, "the bin itself and its cue are the same game (%08x vs %08x)", e, a);
        puts_("d1/list.m3u", "# discs\ngame.cue\n");
        CHECK(joinid::of_game("ps1", g_dir.u8string() + "/d1/list.m3u", "SLUS-00001") == a, "an m3u follows to the first disc");
    }
    // size alone must matter: same samples cannot hide a longer image
    {
        std::vector<uint8_t> bin = noise(300000, 6), longer = bin; longer.push_back(0);
        uint32_t a = joinid::of_game("ps1", put("z/a.iso", bin), "S"), b = joinid::of_game("ps1", put("z/b.iso", longer), "S");
        CHECK(a != b, "another size must differ");
    }
    // CHD v5 header: the SHA-1s identify the disc, however it was compressed
    {
        std::vector<uint8_t> h(124, 0), h2;
        memcpy(h.data(), "MComprHD", 8);
        h[15] = 5;
        for (int i = 64; i < 104; i++) h[i] = (uint8_t)(i * 3);
        h2 = h; h2[20] = 99;   // a field outside the digests (compression type): same disc
        uint64_t x = 0, y = 0, z = 0;
        CHECK(joinid::chd_digest(h.data(), h.size(), &x) && joinid::chd_digest(h2.data(), h2.size(), &y) && x == y, "chd digest ignores non-digest fields");
        h2[70] ^= 1;
        CHECK(joinid::chd_digest(h2.data(), h2.size(), &z) && z != x, "chd digest follows the sha1");
        h2[15] = 4;
        CHECK(!joinid::chd_digest(h2.data(), h2.size(), &z) && !joinid::chd_digest(h.data(), 10, &z), "chd v4 / short header: no digest");
    }
    // unknown: unreadable / empty give 0, which never equals a real id (joinfp::check)
    {
        CHECK(joinid::of_game("gba", (g_dir / "nope.gba").u8string(), "") == 0 && joinid::of_game("gba", "", "") == 0, "missing file -> 0");
        CHECK(joinid::of_game("ps1", puts_("t/x.gdi", "3\n1 0 4 2352 track01.bin 0\n"), "") == 0, "a .gdi without a serial is unknown");
        CHECK(joinid::of_game("ps1", (g_dir / "t/x.gdi").u8string(), "SLUS-00009") != 0, "a .gdi with a serial still has one");
    }
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    printf(g_fail ? "FAILED\n" : "all joinid checks passed\n");
    return g_fail ? 1 : 0;
}
