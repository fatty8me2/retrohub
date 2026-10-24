/* PlayStation 1 library tests: serial parsing, catalog lookup, disc probing on synthetic discs (scratch dir), scan grouping
 * (cue + bins, m3u multi-disc), PS1 vs PS2 by SYSTEM.CNF, BIOS discovery / staging, controller defaults. No emulator, no network. */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "controls.h"
#include "library.h"
#include "ps1.h"

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

/* ISO 9660 with a SYSTEM.CNF (2048-byte sectors). cnf empty = no SYSTEM.CNF (a plain data disc). */
static std::vector<uint8_t> make_iso(const std::string& cnf) {
    std::vector<uint8_t> img(24 * 2048, 0);
    uint8_t* pvd = &img[16 * 2048];
    pvd[0] = 1; std::memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    std::vector<uint8_t> root = dir_rec(std::string(1, '\0'), 20, 2048, 2);
    std::memcpy(pvd + 156, root.data(), root.size());
    std::vector<uint8_t> dir;
    auto add = [&](const std::vector<uint8_t>& r) { dir.insert(dir.end(), r.begin(), r.end()); };
    add(dir_rec(std::string(1, '\0'), 20, 2048, 2));
    add(dir_rec(std::string(1, '\1'), 20, 2048, 2));
    if (!cnf.empty()) add(dir_rec("SYSTEM.CNF;1", 21, (uint32_t)cnf.size(), 0));
    std::memcpy(&img[20 * 2048], dir.data(), dir.size());
    std::memcpy(&img[21 * 2048], cnf.data(), cnf.size());
    return img;
}
static std::string cnf_for(const std::string& serial, const char* key = "BOOT") {
    std::string s = serial;
    for (char& c : s) if (c == '-') c = '_';
    std::string exe = s.substr(0, 4) + "_" + s.substr(5, 3) + "." + s.substr(8, 2);
    return std::string(key) + " = cdrom:\\" + exe + ";1\r\nTCB = 4\r\n";
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
/* 2048 -> raw MODE2/2352 (sync, header, subheader; EDC/ECC left zero) */
static std::vector<uint8_t> to_raw2352(const std::vector<uint8_t>& iso) {
    size_t n = iso.size() / 2048;
    std::vector<uint8_t> raw(n * 2352, 0);
    for (size_t i = 0; i < n; i++) {
        uint8_t* s = &raw[i * 2352];
        s[0] = 0; std::memset(s + 1, 255, 10); s[11] = 0;
        s[15] = 2;
        s[18] = s[22] = 8;   // form 1 subheader
        std::memcpy(s + 24, &iso[i * 2048], 2048);
    }
    return raw;
}

int main() {
    /* ---- serial parsing ---- */
    CHECK(ps1::normalize_serial("SLUS_006.94") == "SLUS-00694");
    CHECK(ps1::normalize_serial("slus-00694") == "SLUS-00694");
    CHECK(ps1::normalize_serial("cdrom:\\SCUS_945.70;1") == "SCUS-94570");
    CHECK(ps1::normalize_serial("PSX.EXE").empty());
    CHECK(ps1::serial_from_system_cnf("BOOT = cdrom:\\SCUS_945.70;1\r\nTCB = 4\r\n") == "SCUS-94570");
    CHECK(ps1::serial_from_system_cnf("BOOT=cdrom:SLES_012.34;1\nEVENT=10") == "SLES-01234");
    CHECK(ps1::serial_from_system_cnf("BOOT2 = cdrom0:\\SLUS_200.00;1\r\nVER = 1.00\r\n").empty());   // a PS2 disc
    CHECK(ps1::serial_region("SCUS-94570") == 'U' && ps1::serial_region("SLES-01234") == 'E' && ps1::serial_region("SLPS-00060") == 'J');
    CHECK(ps1::disc_number("Final Fantasy VII (USA) (Disc 2)") == 2 && ps1::disc_number("Crash Bash (USA)") == 0);

    /* ---- catalog ---- */
    CHECK(kPs1DbCount > 5000);
    CHECK(ps1::title_for_serial("SCUS-94570") == "Crash Bash (USA)");
    CHECK(ps1::title_for_serial("SCUS_945.70") == "Crash Bash (USA)");
    CHECK(ps1::serial_for_name("Crash Bash (USA)") == "SCUS-94570");
    CHECK(ps1::serial_for_name("crash bash (usa)") == "SCUS-94570");
    CHECK(ps1::title_for_serial("ZZZZ-00000").empty());

    /* ---- scratch library ---- */
    fs::path root = fs::temp_directory_path() / L"ps1_test_scratch";
    std::error_code ec;
    fs::remove_all(root, ec);
    library_set_data_dir((root / "data").u8string());

    std::vector<uint8_t> crash = make_iso(cnf_for("SCUS-94570"));
    write(root / "games" / "Crash Bash (USA)" / "Crash Bash (USA).bin", to_raw2352(crash));
    write_text(root / "games" / "Crash Bash (USA)" / "Crash Bash (USA).cue", "FILE \"Crash Bash (USA).bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n");
    // multi-disc with m3u (2048-byte image, MODE1/2048)
    write(root / "games" / "FF" / "FF (Disc 1).bin", make_iso(cnf_for("SCUS-94163")));
    write(root / "games" / "FF" / "FF (Disc 2).bin", make_iso(cnf_for("SCUS-94164")));
    write_text(root / "games" / "FF" / "FF (Disc 1).cue", "FILE \"FF (Disc 1).bin\" BINARY\r\n  TRACK 01 MODE1/2048\r\n    INDEX 01 00:00:00\r\n");
    write_text(root / "games" / "FF" / "FF (Disc 2).cue", "FILE \"FF (Disc 2).bin\" BINARY\r\n  TRACK 01 MODE1/2048\r\n    INDEX 01 00:00:00\r\n");
    write_text(root / "games" / "FF" / "FF.m3u", "FF (Disc 1).cue\r\nFF (Disc 2).cue\r\n");
    // homebrew: SYSTEM.CNF BOOT = PSX.EXE (no serial)
    write(root / "games" / "homebrew.iso", make_iso("BOOT = cdrom:\\PSX.EXE;1\r\n"));
    // not PlayStation: a PS2 disc and a plain data disc
    write(root / "other" / "ps2.iso", make_iso(cnf_for("SLUS-20001", "BOOT2")));
    write(root / "other" / "data.iso", make_iso(""));
    write(root / "other" / "random.bin", std::vector<uint8_t>(100000, 7));

    GameEntry g;
    CHECK(identify_game((root / "games" / "Crash Bash (USA)" / "Crash Bash (USA).cue").u8string(), &g));
    CHECK(g.system == System::PS1 && g.serial == "SCUS-94570" && g.key == "ps1:scus-94570" && g.title == "Crash Bash");
    GameEntry gb;
    CHECK(identify_game((root / "games" / "Crash Bash (USA)" / "Crash Bash (USA).bin").u8string(), &gb) && gb.key == g.key);   // the bin alone is the same game
    GameEntry gm;
    CHECK(identify_game((root / "games" / "FF" / "FF.m3u").u8string(), &gm));
    CHECK(gm.key == "ps1:scus-94163" && gm.title == "Final Fantasy VII");
    GameEntry gh;
    CHECK(identify_game((root / "games" / "homebrew.iso").u8string(), &gh) && gh.system == System::PS1 && gh.serial.empty() && gh.key == "ps1:file-homebrew");
    GameEntry gx;
    CHECK(!identify_game((root / "other" / "ps2.iso").u8string(), &gx) || gx.system == System::PS2);   // never PS1
    CHECK(!identify_game((root / "other" / "data.iso").u8string(), &gx));
    CHECK(!identify_game((root / "other" / "random.bin").u8string(), &gx));

    std::vector<GameEntry> lib = scan_library({root.u8string()}, nullptr);
    int ps1 = 0;
    bool crash_ok = false, m3u_ok = false, bin_listed = false, disc2_listed = false;
    for (const GameEntry& e : lib) {
        if (e.system != System::PS1) continue;
        ++ps1;
        if (e.key == "ps1:scus-94570") { crash_ok = true; if (e.path.find(".cue") == std::string::npos) bin_listed = true; }
        if (e.key == "ps1:scus-94163") m3u_ok = e.path.find(".m3u") != std::string::npos;
        if (e.key == "ps1:scus-94164") disc2_listed = true;
    }
    CHECK(crash_ok && !bin_listed);        // the .cue is the game, its .bin is not a second card
    CHECK(m3u_ok && !disc2_listed);        // the .m3u stands for both discs
    CHECK(ps1 == 3);                        // Crash Bash, Final Fantasy VII, homebrew
    CHECK(system_from_short("ps1") == System::PS1 && std::string(system_short(System::PS1)) == "ps1" && std::string(system_name(System::PS1)) == "PlayStation");

    /* ---- BIOS ---- */
    CHECK(ps1::bios_known("490F666E1AFB15B7362B406ED1CEA246") && ps1::bios_known("490f666e1afb15b7362b406ed1cea246")->region == 'U');
    CHECK(std::string(ps1::bios_core_name('U')) == "scph5501.bin" && std::string(ps1::bios_core_name('E')) == "scph5502.bin");
    CHECK(ps1::bios_name_plausible("SCPH-1001.BIN") && ps1::bios_name_plausible("scph5501.bin") && !ps1::bios_name_plausible("readme.txt"));
    write(root / "Everything" / "BIOS" / "scph7502.bin", std::vector<uint8_t>(524288, 0x42));   // unknown checksum, European by name
    write(root / "Everything" / "BIOS" / "scph9999.bin", std::vector<uint8_t>(1000, 1));         // wrong size: ignored
    std::vector<std::string> dirs = ps1::bios_search_dirs({(root / "Everything" / "GamesPS1").u8string()}, "", (root / "stage").u8string(),
                                                          (root / "home").u8string(), (root / "exe").u8string(), "");
    bool has_bios_dir = false;
    for (const std::string& d : dirs) if (d == (root / "Everything" / "BIOS").u8string()) has_bios_dir = true;
    CHECK(has_bios_dir);                    // BIOS beside the games folder is found
    std::vector<ps1::BiosFound> found = ps1::find_bios(dirs);
    CHECK(found.size() == 1 && found[0].region == 'E' && found[0].known == nullptr);
    CHECK(ps1::stage_bios(found, (root / "stage").u8string()) == 1);
    CHECK(fs::exists(root / "stage" / "scph5502.bin") && fs::exists(root / "stage" / "scph5501.bin") && fs::exists(root / "stage" / "scph5500.bin"));   // others borrow it
    CHECK(ps1::find_bios({(root / "nothing").u8string()}).empty());

    /* ---- controller profile: Xbox -> DualShock ---- */
    hubctl::Profile p = hubctl::default_profile("ps1");
    CHECK(p.system == "ps1" && p.buttons["b"] == "a" && p.buttons["a"] == "b" && p.buttons["y"] == "x" && p.buttons["x"] == "y");   // Cross=A, Circle=B, Square=X, Triangle=Y
    CHECK(p.buttons["l"] == "leftshoulder" && p.buttons["r2"] == "righttrigger@16000" && p.buttons["l3"] == "leftstick" && p.buttons["up"] == "dpup");
    CHECK(p.analogs.count("left_stick") && p.analogs.count("right_stick") && p.analogs["left_stick"].x == "leftx");
    CHECK(hubctl::system_key(System::PS1) == "ps1" && hubctl::is_system("ps1"));
    bool cross = false;
    for (auto& c : hubctl::controls_for("ps1")) if (c.name == "b" && c.label == "Cross") cross = true;
    CHECK(cross);

    library_set_data_dir("");
    fs::remove_all(root, ec);
    if (g_fail) { std::printf("ps1_test: %d FAILED\n", g_fail); return 1; }
    std::printf("ps1_test: all passed\n");
    return 0;
}
