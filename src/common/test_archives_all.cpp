/* Archives of every kind: cartridge ROMs (N64 GB GBC GBA NES SNES DS), PS1 cue+bin sets and a Wii U title inside .zip / .7z.
 * Same game plain vs archived must get the SAME key; rescans must not re-identify; changed archives re-unpack; the rom-cache
 * API (bytes / clear / trim / touch). Synthetic data in %TEMP%\archives_all_test, 7z written by Windows' bsdtar. */
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "library.h"
#include "miniz.h"

namespace fs = std::filesystem;
using namespace hub;
typedef std::vector<uint8_t> Bytes;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void write_file(const fs::path& p, const Bytes& d) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write((const char*)d.data(), (std::streamsize)d.size());
}
static Bytes read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static Bytes noise(size_t n, uint32_t seed) {
    Bytes v(n);
    uint32_t x = seed;
    for (auto& b : v) { x = x * 1103515245u + 12345u; b = uint8_t(x >> 16); }
    return v;
}
static void put(Bytes& v, size_t off, const char* s) { std::memcpy(&v[off], s, std::strlen(s)); }
static void put_be32(Bytes& v, size_t off, uint32_t x) { v[off] = uint8_t(x >> 24); v[off + 1] = uint8_t(x >> 16); v[off + 2] = uint8_t(x >> 8); v[off + 3] = uint8_t(x); }

/* ---- synthetic ROMs (copied from test_library.cpp / test_nes_snes.cpp) ---- */
static Bytes make_n64(const char* title, uint32_t seed) {
    Bytes v = noise(1024 * 1024, seed);
    put_be32(v, 0, 0x80371240u);
    for (size_t i = 0x20; i < 0x34; ++i) v[i] = ' ';
    put(v, 0x20, title);
    put(v, 0x3B, "NTSE");
    return v;
}
static Bytes make_gba(const char* title, const char* code, uint32_t seed) {
    Bytes v = noise(128 * 1024, seed);
    std::memset(&v[0xA0], 0, 12);
    put(v, 0xA0, title);
    put(v, 0xAC, code);
    put(v, 0xB0, "01");
    v[0xB2] = 0x96;
    unsigned s = 0;
    for (int i = 0xA0; i <= 0xBC; ++i) s += v[i];
    v[0xBD] = uint8_t(-(int)s - 0x19);
    return v;
}
static uint16_t nds_crc16(const Bytes& v, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= v[i];
        for (int b = 0; b < 8; ++b) crc = (crc & 1) ? uint16_t((crc >> 1) ^ 0xA001) : uint16_t(crc >> 1);
    }
    return crc;
}
static Bytes make_nds(const char* title, const char* code, uint8_t version, uint32_t seed) {
    Bytes v = noise(64 * 1024, seed);
    std::memset(&v[0], 0, 0x20);
    put(v, 0x00, title);
    put(v, 0x0C, code);
    v[0x12] = 0;
    v[0x1E] = version;
    v[0x15C] = 0x56; v[0x15D] = 0xCF;
    uint16_t c = nds_crc16(v, 0x15E);
    v[0x15E] = uint8_t(c); v[0x15F] = uint8_t(c >> 8);
    return v;
}
static Bytes make_gb(const char* title, uint8_t cgb_flag, uint16_t global, uint32_t seed) {
    Bytes v = noise(32 * 1024, seed);
    std::memset(&v[0x134], 0, 16);
    put(v, 0x134, title);
    v[0x143] = cgb_flag;
    std::memset(&v[0x13F], 0, 4);
    v[0x14E] = uint8_t(global >> 8);
    v[0x14F] = uint8_t(global);
    uint8_t x = 0;
    for (int i = 0x134; i <= 0x14C; ++i) x = uint8_t(x - v[i] - 1);
    v[0x14D] = x;
    return v;
}
static Bytes make_nes(uint8_t prg16, uint8_t chr8, uint8_t flags6, uint8_t flags7, uint32_t seed) {
    Bytes v(16 + (size_t)prg16 * 16384 + (size_t)chr8 * 8192 + ((flags6 & 4) ? 512 : 0));
    Bytes body = noise(v.size() - 16, seed);
    std::memcpy(&v[16], body.data(), body.size());
    v[0] = 'N'; v[1] = 'E'; v[2] = 'S'; v[3] = 0x1A;
    v[4] = prg16; v[5] = chr8; v[6] = flags6; v[7] = flags7;
    for (int i = 8; i < 16; ++i) v[i] = 0;
    return v;
}
static Bytes make_snes(bool hi, const char* title, uint32_t seed) {
    Bytes v = noise(128 * 1024, seed);
    size_t off = hi ? 0xFFC0 : 0x7FC0;
    std::memset(&v[off], ' ', 21);
    std::memcpy(&v[off], title, std::strlen(title));
    v[off + 0x15] = hi ? 0x21 : 0x20;
    v[off + 0x16] = 0x02; v[off + 0x17] = 0x07; v[off + 0x18] = 0x01;
    unsigned sum = 0x1234, comp = sum ^ 0xFFFF;
    v[off + 0x1C] = uint8_t(comp); v[off + 0x1D] = uint8_t(comp >> 8);
    v[off + 0x1E] = uint8_t(sum); v[off + 0x1F] = uint8_t(sum >> 8);
    return v;
}

/* ---- PS1 synthetic disc (from test_psarchive.cpp) ---- */
static void put32(Bytes& b, size_t off, uint32_t v) {
    for (int i = 0; i < 4; i++) b[off + i] = (v >> (8 * i)) & 255;
    for (int i = 0; i < 4; i++) b[off + 4 + i] = (v >> (8 * (3 - i))) & 255;
}
static Bytes dir_rec(const std::string& name, uint32_t extent, uint32_t size, uint8_t flags) {
    size_t nl = name.size(), rl = 33 + nl + (nl % 2 == 0 ? 1 : 0);
    Bytes r(rl, 0);
    r[0] = (uint8_t)rl;
    put32(r, 2, extent);
    put32(r, 10, size);
    r[25] = flags;
    r[28] = 1; r[31] = 1;
    r[32] = (uint8_t)nl;
    std::memcpy(&r[33], name.data(), nl);
    return r;
}
static Bytes make_iso(const std::string& cnf, size_t sectors, uint32_t filler) {
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
    for (size_t i = 24 * 2048; i < img.size(); ++i) img[i] = (uint8_t)(i * 31 + (i >> 9) + filler);
    return img;
}
static Bytes to_raw2352(const Bytes& iso) {
    size_t n = iso.size() / 2048;
    Bytes raw(n * 2352, 0);
    for (size_t i = 0; i < n; i++) {
        uint8_t* s = &raw[i * 2352];
        s[0] = 0; std::memset(s + 1, 0xFF, 10); s[11] = 0;
        s[15] = 2;
        s[16] = s[20] = 0; s[17] = s[21] = 0; s[18] = s[22] = 8; s[19] = s[23] = 0;
        std::memcpy(s + 24, &iso[i * 2048], 2048);
    }
    return raw;
}

/* ---- archive writers ---- */
typedef std::vector<std::pair<std::string, Bytes>> Members;

static bool make_zip(const fs::path& out, const Members& m) {
    fs::create_directories(out.parent_path());
    std::error_code ec;
    fs::remove(out, ec);
    mz_zip_archive z;
    std::memset(&z, 0, sizeof z);
    if (!mz_zip_writer_init_file(&z, out.string().c_str(), 0)) return false;
    bool ok = true;
    for (const auto& e : m) ok = mz_zip_writer_add_mem(&z, e.first.c_str(), e.second.data(), e.second.size(), MZ_DEFAULT_COMPRESSION) && ok;
    ok = mz_zip_writer_finalize_archive(&z) && ok;
    mz_zip_writer_end(&z);
    return ok;
}
static bool make_7z(const fs::path& out, const Members& m, const fs::path& stage) {
    std::error_code ec;
    fs::remove_all(stage, ec);
    fs::create_directories(stage);
    for (const auto& e : m) write_file(stage / e.first, e.second);
    fs::create_directories(out.parent_path());
    fs::remove(out, ec);
    std::wstring cmd = L"\"\"" + std::wstring(_wgetenv(L"SystemRoot")) + L"\\System32\\tar.exe\" --format 7zip -cf \"" + out.wstring() + L"\" -C \"" + stage.wstring() + L"\"";
    for (const auto& e : m) cmd += L" \"" + fs::path(e.first).wstring() + L"\"";
    cmd += L"\"";
    _wsystem(cmd.c_str());
    fs::remove_all(stage, ec);
    return fs::exists(out) && is_7z_file(out.string());
}
static void bump_mtime(const fs::path& p, int seconds) {
    std::error_code ec;
    fs::last_write_time(p, fs::last_write_time(p, ec) + std::chrono::seconds(seconds), ec);
}

static fs::path P8(const std::string& s) { return fs::u8path(s); }
static double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Cart {
    const char* name;      // library subfolder / file stem
    const char* ext;
    System sys;
    std::function<Bytes(uint32_t)> make;
};

int main() {
    fs::path root = fs::temp_directory_path() / L"archives_all_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    library_set_data_dir((root / "data").u8string());
    const fs::path stage = root / "stage";

    /* is bsdtar able to write 7z here? */
    bool have7z = make_7z(root / "probe.7z", {{"a.txt", Bytes(100, 'a')}}, stage);
    std::printf("7z writer: %s\n", have7z ? "yes" : "NO (7z cells skipped)");

    std::vector<Cart> carts = {
        {"N64", ".z64", System::N64, [](uint32_t s) { return make_n64("ARCHIVE N64", s); }},
        {"GB", ".gb", System::GB, [](uint32_t s) { return make_gb("ARCHGB", 0, 0x1234, s); }},
        {"GBC", ".gbc", System::GBC, [](uint32_t s) { return make_gb("ARCHGBC", 0x80, 0xABCD, s); }},
        {"GBA", ".gba", System::GBA, [](uint32_t s) { return make_gba("ARCHGBA", "AARC", s); }},
        {"NES", ".nes", System::NES, [](uint32_t s) { return make_nes(1, 1, 0x02, 0x00, s); }},
        {"SNES", ".sfc", System::SNES, [](uint32_t s) { return make_snes(false, "ARCHIVE SNES", s); }},
        {"DS", ".nds", System::NDS, [](uint32_t s) { return make_nds("ARCHDS", "AARD", 1, s); }},
    };

    for (const Cart& c : carts) {
        std::printf("-- %s\n", c.name);
        const std::string stem = std::string("Archive Test ") + c.name;
        const std::string inner = stem + c.ext;
        Bytes rom = c.make(1);
        const fs::path plain = root / "plain" / c.name;
        write_file(plain / inner, rom);
        auto pv = scan_library({plain.u8string()}, nullptr);
        CHECK(pv.size() == 1);
        if (pv.size() != 1) continue;
        const GameEntry pe = pv[0];
        CHECK(pe.system == c.sys && !pe.key.empty());

        for (int kind = 0; kind < 2; ++kind) {
            if (kind == 1 && !have7z) continue;
            const char* kn = kind == 0 ? "zip" : "7z";
            std::printf("   %s\n", kn);
            const fs::path dir = root / kn / c.name;
            const fs::path arc = dir / (stem + (kind == 0 ? ".zip" : ".7z"));
            auto pack = [&](const Bytes& r) {
                Members m = {{inner, r}, {"readme.txt", Bytes(300, 'r')}, {"release.nfo", Bytes(200, 'n')}, {"site.url", Bytes(60, 'u')}};
                return kind == 0 ? make_zip(arc, m) : make_7z(arc, m, stage);
            };
            CHECK(pack(rom));

            uint64_t c0 = library_identify_count();
            auto v = scan_library({dir.u8string()}, nullptr);
            CHECK(v.size() == 1);
            if (v.size() != 1) continue;
            const GameEntry& e = v[0];
            CHECK(library_identify_count() > c0);
            CHECK(e.key == pe.key);
            CHECK(!e.title.empty());
            CHECK(e.system == c.sys);
            CHECK(e.save_stem == stem);
            CHECK(!e.path.empty() && fs::exists(P8(e.path)));
            CHECK(fs::exists(P8(e.path)) && fs::file_size(P8(e.path)) == rom.size());

            uint64_t c1 = library_identify_count();
            double t0 = now_s();
            auto v2 = scan_library({dir.u8string()}, nullptr);
            double dt = now_s() - t0;
            CHECK(library_identify_count() == c1);
            CHECK(dt < 2.0);
            CHECK(v2.size() == 1 && v2[0].key == pe.key && v2[0].path == e.path);

            /* modified archive: new content, new mtime -> unpacked again */
            Bytes rom2 = c.make(2);
            CHECK(pack(rom2));
            bump_mtime(arc, 30);
            auto v3 = scan_library({dir.u8string()}, nullptr);
            CHECK(v3.size() == 1);
            if (v3.size() == 1) {
                CHECK(library_identify_count() > c1);
                CHECK(fs::exists(P8(v3[0].path)) && read_file(P8(v3[0].path)) == rom2);
            }

            /* rom-cache wiped: the next scan unpacks again */
            CHECK(rom_cache_clear());
            auto v4 = scan_library({dir.u8string()}, nullptr);
            CHECK(v4.size() == 1);
            if (v4.size() == 1) CHECK(fs::exists(P8(v4[0].path)) && read_file(P8(v4[0].path)) == rom2);
        }
    }

    /* ---- PS1: cue + bin inside archives not named like a Redump title ---- */
    /* mode 0: cooked 2048-byte .bin (must pass). mode 1: raw 2352 .bin, the usual Redump format: KNOWN library bug
     * (archive content fallback cannot read SYSTEM.CNF through disc::probe_buffer), reported but not failing. */
    for (int rawmode = 0; rawmode < 2; ++rawmode) {
        std::printf("-- PS1 %s\n", rawmode ? "raw2352 (known bug, informational)" : "cooked2048");
        const int fail_before = g_fail;
        auto mkbin = [&](size_t sectors, uint32_t fill) {
            Bytes iso = make_iso("BOOT = cdrom:\\SCUS_945.70;1\r\nVER = 1.00\r\n", sectors, fill);
            return rawmode ? to_raw2352(iso) : iso;
        };
        const std::string cnf = "BOOT = cdrom:\\SCUS_945.70;1\r\nVER = 1.00\r\n";
        (void)cnf;
        Bytes bin = mkbin(64, 0);
        const std::string cue = std::string("FILE \"Disc.bin\" BINARY\r\n  TRACK 01 ") + (rawmode ? "MODE2/2352" : "MODE1/2048") + "\r\n    INDEX 01 00:00:00\r\n";
        Bytes cueb(cue.begin(), cue.end());
        const std::string tag = rawmode ? "PS1raw" : "PS1";
        const fs::path plain = root / "plain" / tag;
        write_file(plain / "Disc.bin", bin);
        write_file(plain / "Disc.cue", cueb);
        auto pv = scan_library({plain.u8string()}, nullptr);
        CHECK(pv.size() == 1);
        std::string pkey = pv.size() == 1 ? pv[0].key : "";
        CHECK(pkey == "ps1:scus-94570");

        for (int kind = 0; kind < 2; ++kind) {
            if (kind == 1 && !have7z) continue;
            const char* kn = kind == 0 ? "zip" : "7z";
            std::printf("   %s\n", kn);
            const fs::path dir = root / kn / tag;
            const fs::path arc = dir / (kind == 0 ? "my backup.zip" : "my backup.7z");
            auto pack = [&](const Bytes& b) {
                Members m = {{"Disc.cue", cueb}, {"Disc.bin", b}, {"readme.txt", Bytes(100, 'r')}};
                return kind == 0 ? make_zip(arc, m) : make_7z(arc, m, stage);
            };
            CHECK(pack(bin));
            auto v = scan_library({dir.u8string()}, nullptr);
            CHECK(v.size() == 1);
            if (v.size() != 1) continue;
            GameEntry e = v[0];
            CHECK(e.system == System::PS1 && e.key == pkey && e.issue == "extract" && !e.title.empty());
            CHECK(extracted_3ds_path(e).empty());

            std::string out, err;
            CHECK(extract_3ds_zip(e, [](uint64_t, uint64_t) { return true; }, &out, &err));
            CHECK(fs::exists(P8(out)) && fs::path(P8(out)).extension() == ".cue");
            CHECK(extracted_3ds_path(e) == out);
            const fs::path marker = P8(extract_3ds_dir(e)) / "marker.tmp";
            write_file(marker, Bytes(4, 1));
            /* callers check extracted_3ds_path first (the stamp says it is current); extract_3ds_zip itself always unpacks */
            CHECK(extracted_3ds_path(e) == out && fs::exists(marker));

            uint64_t c1 = library_identify_count();
            scan_library({dir.u8string()}, nullptr);
            CHECK(library_identify_count() == c1);

            /* changed archive invalidates the unpack */
            CHECK(pack(mkbin(72, 5)));
            bump_mtime(arc, 30);
            auto v3 = scan_library({dir.u8string()}, nullptr);
            CHECK(v3.size() == 1);
            if (v3.size() == 1) {
                CHECK(v3[0].key == pkey);
                CHECK(extracted_3ds_path(v3[0]).empty());
                std::string out3;
                CHECK(extract_3ds_zip(v3[0], [](uint64_t, uint64_t) { return true; }, &out3, &err));
                CHECK(fs::exists(P8(out3)) && !fs::exists(marker));
            }
        }
        if (rawmode) {
            (void)fail_before;   // raw 2352-byte .bin sectors now identify from content too: failures count
        }
    }

    /* ---- nested archive ---- */
    std::printf("-- nested\n");
    if (have7z) {
        const fs::path dir = root / "nested";
        make_7z(root / "inner.7z", {{"x.txt", Bytes(500, 'x')}}, stage);
        const fs::path z = dir / "Outer.zip";
        CHECK(make_zip(z, {{"inner.7z", read_file(root / "inner.7z")}}));
        auto v = scan_library({dir.u8string()}, nullptr);
        CHECK(v.empty());
        bool seen = false;
        for (const auto& s : scan_skipped_archives())
            if (P8(s.path) == z && s.reason.find("another archive") != std::string::npos) seen = true;
        CHECK(seen);
        CHECK(archive_skip_reason(z.u8string()).find("another archive") != std::string::npos);
    } else std::printf("   skipped (no 7z writer)\n");

    /* ---- Wii U by catalog name ---- */
    std::printf("-- Wii U\n");
    {
        const fs::path dir = root / "wiiu";
        CHECK(make_zip(dir / "Super Mario 3D World.zip", {{"Super Mario 3D World.wua", noise(8192, 7)}, {"readme.txt", Bytes(50, 'r')}}));
        auto v = scan_library({dir.u8string()}, nullptr);
        CHECK(v.size() == 1 && v[0].system == System::WiiU && v[0].key == "wiiu:0005000010145c00");
    }

    /* ---- rom-cache API ---- */
    std::printf("-- rom-cache\n");
    {
        rom_cache_clear();
        const fs::path dn = root / "cache" / "n64", dg = root / "cache" / "gba";
        CHECK(make_zip(dn / "A.zip", {{"A.z64", make_n64("CACHE A", 3)}}));
        CHECK(make_zip(dg / "B.zip", {{"B.gba", make_gba("CACHEB", "CCHB", 4)}}));
        auto va = scan_library({dn.u8string()}, nullptr);
        auto vb = scan_library({dg.u8string()}, nullptr);
        CHECK(va.size() == 1 && vb.size() == 1);
        if (va.size() == 1 && vb.size() == 1) {
            CHECK(rom_cache_bytes() > 0);
            fs::path da = P8(va[0].path).parent_path(), db = P8(vb[0].path).parent_path();
            CHECK(fs::exists(da) && fs::exists(db) && da != db);
            auto old = fs::file_time_type::clock::now() - std::chrono::hours(2);
            auto setold = [&](const fs::path& d) { std::error_code e; fs::last_write_time(d / "stamp.txt", old, e); fs::last_write_time(d, old, e); };
            auto mtime = [&](const fs::path& d) { std::error_code e; return fs::last_write_time(d, e); };   // miniz-unpacked cartridges have no stamp.txt
            setold(da);
            uint64_t freed = rom_cache_trim(1);   // tiny max: only entries unused for over an hour go
            CHECK(freed > 0);
            CHECK(!fs::exists(da));
            CHECK(fs::exists(db));
            setold(db);
            CHECK(mtime(db) < fs::file_time_type::clock::now() - std::chrono::hours(1));
            rom_cache_touch(vb[0]);
            CHECK(mtime(db) > fs::file_time_type::clock::now() - std::chrono::minutes(5));
            rom_cache_trim(1);
            CHECK(fs::exists(db));   // touched: protected
        }
    }

    std::printf("3DS: skipped (no synthetic 3DS header helper in the repo tests)\n");
    library_set_data_dir("");
    std::error_code e2;
    fs::remove_all(root, e2);
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("archives_all_test: ALL PASSED\n");
    return 0;
}
