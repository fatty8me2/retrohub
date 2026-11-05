/* verify (L3): known-dump list parse, header / byte-order normalization, hash cache, background runner. No network, scratch files only.
 * Optional argv[1]: the real data/known_dumps.bin (sanity counts). */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "miniz.h"
#include "verify.h"

namespace fs = std::filesystem;
using namespace hub;
using namespace hub::verify;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static uint32_t crc_of(const std::vector<uint8_t>& v, size_t from = 0) { return crc32_update(0, v.data() + from, v.size() - from); }

static bool hash_mem(System s, const std::vector<uint8_t>& d, FileHash* h) {
    size_t pos = 0;
    return hash_stream(s, d.size(), [&](uint8_t* b, size_t n) {
        size_t k = std::min(n, d.size() - pos);
        std::memcpy(b, d.data() + pos, k);
        pos += k;
        return k;
    }, h);
}

static std::vector<uint8_t> pattern(size_t n, uint32_t seed = 1) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed;
    for (auto& b : v) { x = x * 1664525u + 1013904223u; b = (uint8_t)(x >> 24); }
    return v;
}

static void write_file(const fs::path& p, const std::vector<uint8_t>& d) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write((const char*)d.data(), (std::streamsize)d.size());
}

int main(int argc, char** argv) {
    /* ---- crc32 ---- */
    CHECK(crc32_update(0, (const uint8_t*)"123456789", 9) == 0xCBF43926u);

    /* ---- list parse ---- */
    const char* text =
        "# comment\n"
        "@nes\t2\n"
        "0A0B0C0D\t40960\tGame A (USA)\n"
        "000000FF\t100\tShort (Japan)\n"
        "bad row without tabs\n"
        "ZZZZZZZZ\t5\tNot hex\n"
        "@gbc\t1\r\n"
        "DEADBEEF\t32768\tColor Game (USA)\r\n"
        "@nope\t1\n"
        "11111111\t1\tUnknown system\n"
        "@n64\t1\n"
        "12345678\t8388608\tTab\tin title\n";
    KnownDb db;
    CHECK(db.load_text(text));
    CHECK(db.count(System::NES) == 2 && db.count(System::GBC) == 1 && db.count(System::N64) == 1 && db.total() == 4);
    const std::string* t = db.find(System::NES, 0x0A0B0C0D, 40960);
    CHECK(t && *t == "Game A (USA)");
    CHECK(!db.find(System::NES, 0x0A0B0C0D, 40961));                 // size must match too
    CHECK(!db.find(System::SNES, 0x0A0B0C0D, 40960));                // other system's table
    CHECK(db.find(System::GB, 0xDEADBEEF, 32768) != nullptr);        // GB asks GBC's list as well
    CHECK(db.find(System::N64, 0x12345678, 8388608) && *db.find(System::N64, 0x12345678, 8388608) == "Tab\tin title");
    CHECK(!KnownDb().load_text("") && !KnownDb().load_text("# only comments\n"));

    /* blob: header line + zlib stream, as tools/gen_known_dumps.py writes it */
    {
        std::string raw = text;
        mz_ulong cl = mz_compressBound((mz_ulong)raw.size());
        std::string z(cl, 0);
        CHECK(mz_compress2((unsigned char*)&z[0], &cl, (const unsigned char*)raw.data(), (mz_ulong)raw.size(), 9) == MZ_OK);
        z.resize(cl);
        KnownDb b;
        CHECK(b.load_blob("RHKD1 CC-BY-SA-4.0 test\n" + z));
        CHECK(b.total() == 4 && b.header().find("CC-BY-SA") != std::string::npos);
        CHECK(!KnownDb().load_blob("WRONG header\n" + z));
        CHECK(!KnownDb().load_blob("RHKD1 x\nnot zlib data"));
        CHECK(!KnownDb().load_blob("no newline at all"));
    }

    /* ---- header / byte order normalization ---- */
    {
        std::vector<uint8_t> prg = pattern(32768, 7);
        std::vector<uint8_t> nes = {'N', 'E', 'S', 0x1A, 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        nes.insert(nes.end(), prg.begin(), prg.end());
        FileHash h, hp;
        CHECK(hash_mem(System::NES, nes, &h) && h.crc == crc_of(prg) && h.size == prg.size());   // iNES header skipped
        CHECK(hash_mem(System::NES, prg, &hp) && hp.crc == h.crc);                              // a headerless file hashes the same
        std::vector<uint8_t> notnes = nes; notnes[0] = 'X';
        CHECK(hash_mem(System::NES, notnes, &h) && h.crc == crc_of(notnes) && h.size == notnes.size());   // no magic: nothing skipped

        std::vector<uint8_t> snes = pattern(1u << 17, 9);
        std::vector<uint8_t> copier(512, 0xAA);
        copier.insert(copier.end(), snes.begin(), snes.end());
        CHECK(hash_mem(System::SNES, copier, &h) && h.crc == crc_of(snes) && h.size == snes.size());   // 512-byte copier header stripped
        CHECK(hash_mem(System::SNES, snes, &h) && h.crc == crc_of(snes));
        CHECK(hash_mem(System::GBA, copier, &h) && h.crc == crc_of(copier));                          // other systems never strip

        std::vector<uint8_t> z64 = pattern(3u << 20, 11);   // spans several 1 MB chunks
        z64[0] = 0x80; z64[1] = 0x37; z64[2] = 0x12; z64[3] = 0x40;
        std::vector<uint8_t> v64 = z64, n64 = z64;
        for (size_t i = 0; i + 3 < z64.size(); i += 4) {
            std::swap(v64[i], v64[i + 1]); std::swap(v64[i + 2], v64[i + 3]);
            std::swap(n64[i], n64[i + 3]); std::swap(n64[i + 1], n64[i + 2]);
        }
        CHECK(v64[0] == 0x37 && n64[0] == 0x40);
        uint32_t want = crc_of(z64);
        CHECK(hash_mem(System::N64, z64, &h) && h.crc == want && h.size == z64.size());
        CHECK(hash_mem(System::N64, v64, &h) && h.crc == want);
        CHECK(hash_mem(System::N64, n64, &h) && h.crc == want);
        std::vector<uint8_t> odd(z64.begin(), z64.begin() + 4099);   // a trailing partial word is left alone
        CHECK(hash_mem(System::N64, odd, &h) && h.crc == crc_of(odd));

        FileHash e;
        CHECK(!hash_mem(System::GB, {}, &e));                                                          // empty file
        std::atomic<bool> stop{true};
        CHECK(!hash_stream(System::GB, 10, [](uint8_t*, size_t) { return (size_t)0; }, &e, &stop));    // cancelled
        CHECK(!hash_stream(System::GB, 100, [](uint8_t*, size_t) { return (size_t)0; }, &e));          // short read
    }

    /* ---- supported systems ---- */
    CHECK(supported(System::N64) && supported(System::NES) && supported(System::NDS) && supported(System::GBC));
    CHECK(!supported(System::PS1) && !supported(System::PS2) && !supported(System::GC) && !supported(System::WiiU) && !supported(System::N3DS) && !supported(System::PSP));

    /* ---- cache ---- */
    fs::path tmp = fs::temp_directory_path() / ("hub_verify_test_" + std::to_string((unsigned long long)std::time(nullptr)));
    fs::create_directories(tmp);
    {
        HashCache c;
        FileHash h; h.crc = 0xCAFEBABE; h.size = 1234;
        c.put("C:\\Games\\A.gba", System::GBA, 2000, 77, h);
        FileHash g;
        CHECK(c.get("c:\\games\\a.GBA", System::GBA, 2000, 77, &g) && g.crc == 0xCAFEBABE && g.size == 1234);   // path is case-insensitive
        CHECK(!c.get("C:\\Games\\A.gba", System::GBA, 2001, 77, &g));    // size changed
        CHECK(!c.get("C:\\Games\\A.gba", System::GBA, 2000, 78, &g));    // mtime changed
        CHECK(!c.get("C:\\Games\\A.gba", System::GB, 2000, 77, &g));     // other system
        std::string f = (tmp / "cache.tsv").string();
        CHECK(c.save(f));
        HashCache d;
        CHECK(d.load(f) && d.size() == 1 && d.get("C:\\Games\\A.gba", System::GBA, 2000, 77, &g) && g.crc == 0xCAFEBABE && g.size == 1234);
        { std::ofstream bad(tmp / "bad.tsv"); bad << "not a cache\n"; }
        CHECK(!d.load((tmp / "bad.tsv").string()) && d.size() == 0);
        CHECK(!d.load((tmp / "missing.tsv").string()));
    }

    /* ---- runner: real files, cache hit on the second run, changed file re-hashed ---- */
    {
        std::vector<uint8_t> good = pattern(65536, 21), bad = pattern(65536, 22);
        write_file(tmp / "good.gba", good);
        write_file(tmp / "bad.gba", bad);
        std::vector<uint8_t> nes = {'N', 'E', 'S', 0x1A, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        std::vector<uint8_t> prg = pattern(16384, 23);
        nes.insert(nes.end(), prg.begin(), prg.end());
        write_file(tmp / "g.nes", nes);
        char buf[160];
        KnownDb k;
        std::snprintf(buf, sizeof buf, "@gba\t1\n%08X\t%llu\tGood Game (World)\n@nes\t1\n%08X\t%llu\tNES Game (USA)\n",
                      crc_of(good), (unsigned long long)good.size(), crc_of(prg), (unsigned long long)prg.size());
        CHECK(k.load_text(buf));
        std::vector<Item> items = {
            {"gba:good", (tmp / "good.gba").string(), System::GBA},
            {"gba:bad", (tmp / "bad.gba").string(), System::GBA},
            {"gba:gone", (tmp / "gone.gba").string(), System::GBA},
            {"nes:g", (tmp / "g.nes").string(), System::NES},
            {"ps1:disc", (tmp / "disc.cue").string(), System::PS1},   // disc: skipped, no result
        };
        std::string cf = (tmp / "vc.tsv").string();
        Runner r;
        r.start(items, &k, cf);
        for (int i = 0; i < 2000 && r.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(!r.running() && r.total() == 4 && r.done() == 4);
        Result a;
        CHECK(r.get("gba:good", &a) && a.v == Verdict::Good && a.title == "Good Game (World)");
        CHECK(r.get("gba:bad", &a) && a.v == Verdict::Unknown);
        CHECK(r.get("gba:gone", &a) && a.v == Verdict::Unreadable);
        CHECK(r.get("nes:g", &a) && a.v == Verdict::Good && a.title == "NES Game (USA)");
        CHECK(!r.get("ps1:disc", &a));
        HashCache pc;
        CHECK(pc.load(cf) && pc.size() == 3);

        /* second run: the cache answers (prove it by poisoning a cached CRC for an unchanged file) */
        FileHash fake; fake.crc = 0x1234; fake.size = 1;
        std::error_code ec;
        pc.put((tmp / "bad.gba").string(), System::GBA, fs::file_size(tmp / "bad.gba"), (long long)fs::last_write_time(tmp / "bad.gba").time_since_epoch().count(), fake);
        CHECK(pc.save(cf));
        r.start(items, &k, cf);
        for (int i = 0; i < 2000 && r.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(r.get("gba:bad", &a) && a.crc == 0x1234);   // came from the cache, not the file
        /* cache-only: answers what the cache knows, reads nothing, and says nothing about a changed file */
        write_file(tmp / "bad.gba", pattern(71000, 25));
        r.start(items, &k, cf, true);
        for (int i = 0; i < 2000 && r.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(r.done() == 4 && r.get("gba:good", &a) && a.v == Verdict::Good && r.get("nes:g", &a) && a.v == Verdict::Good);
        CHECK(!r.get("gba:bad", &a));
        /* a changed file (new mtime/size) is hashed again */
        r.start(items, &k, cf);
        for (int i = 0; i < 2000 && r.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(r.get("gba:bad", &a) && a.crc != 0x1234 && a.crc == crc_of(pattern(71000, 25)));
    }
    fs::remove_all(tmp);

    /* ---- the real list (optional) ---- */
    if (argc > 1) {
        KnownDb real;
        CHECK(real.load_file(argv[1]));
        CHECK(real.count(System::NES) > 5000 && real.count(System::SNES) > 3000 && real.count(System::N64) > 1000 && real.count(System::GB) > 1500 &&
              real.count(System::GBC) > 2000 && real.count(System::GBA) > 3000 && real.count(System::NDS) > 7000);
        CHECK(real.header().find("CC-BY-SA-4.0") != std::string::npos);
        /* Super Mario 64 (USA) z64 */
        const std::string* sm = real.find(System::N64, 0x3CE60709u, 8388608);
        CHECK(sm && sm->find("Super Mario 64 (USA)") == 0);
        std::printf("known list: %zu entries (%s)\n", real.total(), real.header().c_str());
    }

    if (g_fail) { std::printf("verify_test: %d failure(s)\n", g_fail); return 1; }
    std::printf("verify_test: all passed\n");
    return 0;
}
