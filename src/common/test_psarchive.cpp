/* PlayStation disc archives (.7z / .rar): identification without extraction (serial in name, Redump title databases for PS1 and PS2, folder
 * hint as a tie break), a misplaced PS2 game, truncated archives, extraction with SYSTEM.CNF confirmation. Synthetic archives made with
 * Windows' bsdtar in a scratch dir. No emulator, no network. */
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "library.h"
#include "ps1.h"
#include "ps2.h"

namespace fs = std::filesystem;
using namespace hub;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    for (int i = 0; i < 4; i++) b[off + i] = (v >> (8 * i)) & 255;
    for (int i = 0; i < 4; i++) b[off + 4 + i] = (v >> (8 * (3 - i))) & 255;
}
static std::vector<uint8_t> dir_rec(const std::string& name, uint32_t extent, uint32_t size, uint8_t flags) {
    size_t nl = name.size(), rl = 33 + nl + (nl % 2 == 0 ? 1 : 0);
    std::vector<uint8_t> r(rl, 0);
    r[0] = (uint8_t)rl;
    put32(r, 2, extent);
    put32(r, 10, size);
    r[25] = flags;
    r[28] = 1; r[31] = 1;
    r[32] = (uint8_t)nl;
    std::memcpy(&r[33], name.data(), nl);
    return r;
}
static std::vector<uint8_t> make_iso(const std::string& cnf, size_t sectors = 24) {
    std::vector<uint8_t> img(sectors * 2048, 0);
    uint8_t* pvd = &img[16 * 2048];
    pvd[0] = 1; std::memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    std::vector<uint8_t> root = dir_rec(std::string(1, '\0'), 20, 2048, 2);
    std::memcpy(pvd + 156, root.data(), root.size());
    std::vector<uint8_t> dir;
    auto add = [&](const std::vector<uint8_t>& r) { dir.insert(dir.end(), r.begin(), r.end()); };
    add(dir_rec(std::string(1, '\0'), 20, 2048, 2));
    add(dir_rec(std::string(1, '\1'), 20, 2048, 2));
    add(dir_rec("SYSTEM.CNF;1", 21, (uint32_t)cnf.size(), 0));
    std::memcpy(&img[20 * 2048], dir.data(), dir.size());
    std::memcpy(&img[21 * 2048], cnf.data(), cnf.size());
    for (size_t i = 24 * 2048; i < img.size(); ++i) img[i] = (uint8_t)(i * 31 + (i >> 9));   // incompressible-ish filler
    return img;
}
static std::string cnf_for(const std::string& serial, const char* key) {
    std::string s = serial;
    for (char& c : s) if (c == '-') c = '_';
    return std::string(key) + " = cdrom0:\\" + s.substr(0, 4) + "_" + s.substr(5, 3) + "." + s.substr(8, 2) + ";1\r\nVER = 1.00\r\n";
}
static void write(const fs::path& p, const std::vector<uint8_t>& d) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write((const char*)d.data(), (std::streamsize)d.size());
}
static void write_text(const fs::path& p, const std::string& t) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << t;
}
static std::vector<uint8_t> to_raw2352(const std::vector<uint8_t>& iso) {
    size_t n = iso.size() / 2048;
    std::vector<uint8_t> raw(n * 2352, 0);
    for (size_t i = 0; i < n; i++) {
        uint8_t* s = &raw[i * 2352];
        s[0] = 0; std::memset(s + 1, 0xFF, 10); s[11] = 0;
        s[15] = 2;
        s[16] = s[20] = 0; s[17] = s[21] = 0; s[18] = s[22] = 8; s[19] = s[23] = 0;
        std::memcpy(s + 24, &iso[i * 2048], 2048);
    }
    return raw;
}
/* A 7z with the given members of `dir` (bsdtar writes 7z). */
static void make_7z(const fs::path& out, const fs::path& dir, const std::vector<std::string>& members) {
    fs::create_directories(out.parent_path());
    std::wstring cmd = L"\"\"" + std::wstring(_wgetenv(L"SystemRoot")) + L"\\System32\\tar.exe\" --format 7zip -cf \"" + out.wstring() + L"\" -C \"" + dir.wstring() + L"\"";
    for (const auto& m : members) cmd += L" \"" + fs::path(m).wstring() + L"\"";
    cmd += L"\"";
    _wsystem(cmd.c_str());
}

static const GameEntry* by_path(const std::vector<GameEntry>& v, const fs::path& p) {
    for (const auto& g : v) if (fs::path(g.path) == p) return &g;
    return nullptr;
}

int main() {
    /* ---- names ---- */
    CHECK(ps2::region_from_name("Dragon Ball Z - Budokai 3 (USA)") == 'U');
    CHECK(ps2::region_from_name("Foo (Europe) (En,Fr)") == 'E');
    CHECK(ps2::region_from_name("Foo [NTSC-J]") == 'J');
    CHECK(ps2::region_from_name("Foo") == 0);
    CHECK(ps2::serial_for_name("Dragon Ball Z - Budokai Tenkaichi 3 (USA) (En,Ja).7z") == "SLUS-21678");
    CHECK(ps2::serial_for_name("Dragon Ball Z - Budokai 3 (USA).7z") == "SLUS-20998");
    CHECK(ps2::serial_for_name("Dragon Ball Z - Budokai 3 (Europe)") == "SLES-52730");
    CHECK(ps2::serial_for_name("dragon ball z - budokai 3") == "SLES-52730" || ps2::serial_for_name("dragon ball z - budokai 3") == "SLUS-20998");
    CHECK(ps2::serial_for_name("Digimon Rumble Arena 2 (USA)") == "SLUS-21067");
    CHECK(ps2::serial_for_name("Crash Bash (USA)").empty());
    CHECK(ps2::serial_for_name("Totally Unknown Game (USA)").empty());
    CHECK(ps1::serial_for_name("Digimon Rumble Arena 2 (USA)").empty());

    /* ---- BIOS folder next to the games is searched (Desktop\Everything\BIOS beside GamesPS1) ---- */
    {
        auto dirs = ps1::bios_search_dirs({"C:\\Users\\x\\Desktop\\Everything\\GamesPS1"}, "", "", "C:\\Users\\x", "C:\\App\\", "");
        bool seen = false;
        for (auto& d : dirs) if (d.find("Everything\\BIOS") != std::string::npos || d.find("Everything/BIOS") != std::string::npos) seen = true;
        CHECK(seen);
    }

    /* ---- scratch library ---- */
    fs::path root = fs::temp_directory_path() / L"psarchive_test_scratch";
    std::error_code ec;
    fs::remove_all(root, ec);
    library_set_data_dir((root / "data").u8string());
    const fs::path src = root / "src";

    // 1. a PS2 game put in the PS1 folder (like Digimon Rumble Arena 2): PS2 by content
    write(src / "Digimon Rumble Arena 2 (USA).iso", make_iso(cnf_for("SLUS-21067", "BOOT2"), 400));
    const fs::path a_digimon = root / "GamesPS1" / "Digimon Rumble Arena 2 (USA).7z";
    make_7z(a_digimon, src, {"Digimon Rumble Arena 2 (USA).iso"});
    CHECK(is_7z_file(a_digimon.string()) && archive_sig_ok(a_digimon.string()) && !archive_truncated(a_digimon.string()));

    // 2. cue + bin PS1 set whose archive name carries the serial
    write(src / "Crash Bash (USA).bin", to_raw2352(make_iso(cnf_for("SCUS-94570", "BOOT"))));
    write_text(src / "Crash Bash (USA).cue", "FILE \"Crash Bash (USA).bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n");
    const fs::path a_crash = root / "GamesPS1" / "Crash Bash [NTSC-U] [SCUS-94570].7z";
    make_7z(a_crash, src, {"Crash Bash (USA).cue", "Crash Bash (USA).bin"});

    // 3. an unfinished copy of a PS2 7z: the end header is missing
    write(src / "Dragon Ball Z - Budokai 3 (USA).iso", make_iso(cnf_for("SLUS-20998", "BOOT2"), 600));
    const fs::path full = root / "full" / "Dragon Ball Z - Budokai 3 (USA).7z";
    make_7z(full, src, {"Dragon Ball Z - Budokai 3 (USA).iso"});
    const fs::path a_trunc = root / "GamesPS2" / "Dragon Ball Z - Budokai 3 (USA).7z";
    {
        std::error_code e2;
        uintmax_t sz = fs::file_size(full, e2);
        std::vector<char> buf((size_t)sz);
        std::ifstream in(full, std::ios::binary);
        in.read(buf.data(), (std::streamsize)buf.size());
        fs::create_directories(a_trunc.parent_path());
        std::ofstream o(a_trunc, std::ios::binary);
        o.write(buf.data(), (std::streamsize)(buf.size() - 40));   // header + end header chopped
    }
    CHECK(is_7z_file(a_trunc.string()) && archive_truncated(a_trunc.string()) && !archive_truncated(full.string()));

    // 4. a RAR we cannot read (signature only): listed by its title, with the RAR tag path
    std::vector<uint8_t> rar = {'R', 'a', 'r', '!', 0x1A, 0x07, 0x01, 0x00};
    rar.resize(4096, 0x55);
    const fs::path a_rar = root / "GamesPS1" / "Spyro the Dragon (USA).rar";
    write(a_rar, rar);
    CHECK(is_rar_file(a_rar.string()) && archive_sig_ok(a_rar.string()) && !is_rar_file(a_digimon.string()));
    // a .rar that is not a rar and a 7z with no disc: never listed
    write(root / "GamesPS1" / "Crash Bash (Europe).rar", std::vector<uint8_t>(3000, 'x'));
    write(src / "notes.txt", std::vector<uint8_t>(3000, 'n'));
    make_7z(root / "GamesPS1" / "Notes (USA).7z", src, {"notes.txt"});

    // 5. a PS2 disc inside an archive named like a PS1 title: filed as PS1 by name, caught by SYSTEM.CNF on extraction
    write(src / "Crash Bash (USA)x.iso", make_iso(cnf_for("SLUS-20998", "BOOT2"), 100));
    fs::copy_file(src / "Crash Bash (USA)x.iso", src / "Crash Bash (USA).iso", fs::copy_options::overwrite_existing);
    const fs::path a_mismatch = root / "mismatch" / "Tekken 3 (USA).7z";
    make_7z(a_mismatch, src, {"Crash Bash (USA).iso"});

    std::vector<GameEntry> all = scan_library({(root / "GamesPS1").u8string(), (root / "GamesPS2").u8string(), (root / "mismatch").u8string()}, nullptr);

    const GameEntry* dig = by_path(all, a_digimon);
    CHECK(dig && dig->system == System::PS2 && dig->serial == "SLUS-21067" && dig->title == "Digimon Rumble Arena 2" && dig->issue == "extract");
    const GameEntry* cr = by_path(all, a_crash);
    CHECK(cr && cr->system == System::PS1 && cr->serial == "SCUS-94570" && cr->issue == "extract" && cr->unpacked > 0);
    const GameEntry* tr = by_path(all, a_trunc);
    CHECK(tr && tr->system == System::PS2 && tr->serial == "SLUS-20998" && tr->issue == "extract");
    const GameEntry* rr = by_path(all, a_rar);
    CHECK(rr && rr->system == System::PS1 && rr->serial == "SCUS-94228");
    CHECK(!by_path(all, root / "GamesPS1" / "Crash Bash (Europe).rar"));
    {   // a real 7z that carries a .rar extension (as in the wild) is read by content
        fs::copy_file(a_crash, root / "GamesPS1" / "Crash Bash [NTSC-U] [SCUS-94570].rar", fs::copy_options::overwrite_existing);
        GameEntry ge;
        CHECK(identify_game((root / "GamesPS1" / "Crash Bash [NTSC-U] [SCUS-94570].rar").string(), &ge) && ge.system == System::PS1 && ge.serial == "SCUS-94570" && ge.unpacked > 0);
    }
    CHECK(!by_path(all, root / "GamesPS1" / "Notes (USA).7z"));
    for (const auto& g : all) std::printf("  entry %s sys=%d serial=%s issue=%s unp=%llu\n", g.path.c_str(), (int)g.system, g.serial.c_str(), g.issue.c_str(), (unsigned long long)g.unpacked);
    std::printf("  trunc: 7z=%d trunc=%d fulltrunc=%d\n", (int)is_7z_file(a_trunc.string()), (int)archive_truncated(a_trunc.string()), (int)archive_truncated(full.string()));
    const GameEntry* mm = by_path(all, a_mismatch);
    CHECK(mm && mm->system == System::PS1);

    // extraction: progress, cache, SYSTEM.CNF confirmation
    if (dig) {
        std::string out, err;
        uint64_t last = 0;
        CHECK(extracted_3ds_path(*dig).empty());
        CHECK(extract_3ds_zip(*dig, [&](uint64_t d, uint64_t) { last = d; return true; }, &out, &err));
        CHECK(!out.empty() && fs::exists(fs::u8path(out)) && extracted_3ds_path(*dig) == out);
        CHECK(fs::file_size(fs::u8path(out)) == 400u * 2048u);
    }
    if (cr) {
        std::string out, err;
        CHECK(extract_3ds_zip(*cr, [](uint64_t, uint64_t) { return true; }, &out, &err));
        CHECK(out.size() > 4 && out.substr(out.size() - 4) == ".cue" && fs::exists(fs::u8path(out)) && extracted_3ds_path(*cr) == out);
    }
    if (tr) {
        std::string out, err;
        CHECK(!extract_3ds_zip(*tr, [](uint64_t, uint64_t) { return true; }, &out, &err));
        CHECK(err.find("incomplete") != std::string::npos);
    }
    if (mm) {
        std::string out, err;
        CHECK(!extract_3ds_zip(*mm, [](uint64_t, uint64_t) { return true; }, &out, &err));
        CHECK(err.find("PlayStation 2") != std::string::npos && extracted_3ds_path(*mm).empty());
    }

    {   // PS1 BIOS next to the games folders in GamesBios, PS2 dumps and macOS junk around it
        fs::path ev = root / "Everything";
        write(ev / "GamesBios" / "SCPH1001.BIN", std::vector<uint8_t>(524288, 9));
        write(ev / "GamesBios" / "._SCPH1001.BIN", std::vector<uint8_t>(524288, 9));
        write(ev / "GamesBios" / "__MACOSX" / "SCPH5501.BIN", std::vector<uint8_t>(524288, 8));
        write(ev / "GamesBios" / "ps2 bios usa" / "SCPH-39001_USA" / "SCPH-39001_USA.BIN", std::vector<uint8_t>(4194304, 7));
        fs::create_directories(ev / "GamesPS1");
        auto found = ps1::find_bios(ps1::bios_search_dirs({(ev / "GamesPS1").u8string()}, "", "", "", "", ""));
        CHECK(found.size() == 1 && fs::path(found[0].path).filename() == "SCPH1001.BIN");
    }

    library_set_data_dir("");
    fs::remove_all(root, ec);
    if (g_fail) { std::printf("psarchive_test: %d FAILED\n", g_fail); return 1; }
    std::printf("psarchive_test: all passed\n");
    return 0;
}
