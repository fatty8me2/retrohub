// Console test for NES / SNES support: header detection, identification + scan (plain and zipped), system names,
// cheat code formats, controller profiles (positional + label layout), per-game settings, Find my games / Add games.
#include "addgames.h"
#include "cheat_store.h"
#include "cheats.h"
#include "controls.h"
#include "exit_combo.h"
#include "findgames.h"
#include "game_settings.h"
#include "library.h"
#include "miniz.h"
#include "winutil.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace hub;
namespace fs = std::filesystem;
typedef std::vector<uint8_t> Bytes;

static int g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } \
    } while (0)

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

static uint32_t crc32_of(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
static std::string hex8(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%08x", v); return b; }

static Bytes make_nes(uint8_t prg16, uint8_t chr8, uint8_t flags6, uint8_t flags7, uint32_t seed) {
    Bytes v(16 + (size_t)prg16 * 16384 + (size_t)chr8 * 8192 + ((flags6 & 4) ? 512 : 0));
    Bytes body = noise(v.size() - 16, seed);
    std::memcpy(&v[16], body.data(), body.size());
    v[0] = 'N'; v[1] = 'E'; v[2] = 'S'; v[3] = 0x1A;
    v[4] = prg16; v[5] = chr8; v[6] = flags6; v[7] = flags7;
    for (int i = 8; i < 16; ++i) v[i] = 0;
    return v;
}

/* LoROM (hi = false) or HiROM image with a real internal header at the right place. */
static Bytes make_snes(bool hi, const char* title, uint32_t seed, size_t size = 128 * 1024, bool copier = false, bool good_sum = true) {
    Bytes v = noise(size, seed);
    size_t off = hi ? 0xFFC0 : 0x7FC0;
    std::memset(&v[off], ' ', 21);
    std::memcpy(&v[off], title, std::strlen(title));
    v[off + 0x15] = hi ? 0x21 : 0x20;
    v[off + 0x16] = 0x02; v[off + 0x17] = 0x07; v[off + 0x18] = 0x01;
    unsigned sum = good_sum ? 0x1234 : 0x1111;
    unsigned comp = good_sum ? (sum ^ 0xFFFF) : 0x2222;
    v[off + 0x1C] = uint8_t(comp); v[off + 0x1D] = uint8_t(comp >> 8);
    v[off + 0x1E] = uint8_t(sum); v[off + 0x1F] = uint8_t(sum >> 8);
    if (copier) {
        Bytes c(512, 0);
        c.insert(c.end(), v.begin(), v.end());
        return c;
    }
    return v;
}

static void test_headers() {
    // iNES 1 / NES 2.0
    Bytes a = make_nes(2, 1, 0x02 | 0x10, 0x00, 1);   // battery, mapper 1
    NesHeader h;
    CHECK(nes_parse_header(a.data(), a.size(), a.size(), &h));
    CHECK(h.battery && !h.trainer && !h.nes2 && h.mapper == 1 && h.prg == 32768 && h.chr == 8192 && h.data_off == 16);
    Bytes b = make_nes(1, 1, 0x04, 0x08, 2);          // trainer, NES 2.0
    CHECK(nes_parse_header(b.data(), b.size(), b.size(), &h));
    CHECK(h.nes2 && h.trainer && h.data_off == 16 + 512);
    Bytes m = make_nes(1, 0, 0x40, 0x20, 3);          // mapper = 0x20 | 4 (high nibble from byte 7)
    CHECK(nes_parse_header(m.data(), m.size(), m.size(), &h) && h.mapper == 0x24);
    Bytes bad = a; bad[3] = 0;
    CHECK(!nes_parse_header(bad.data(), bad.size(), bad.size(), &h));   // no magic
    Bytes trunc(a.begin(), a.begin() + 1000);
    CHECK(!nes_parse_header(trunc.data(), trunc.size(), trunc.size(), &h));   // declared PRG does not fit
    Bytes zero = a; zero[4] = 0;
    CHECK(!nes_parse_header(zero.data(), zero.size(), zero.size(), &h));
    // SNES
    CHECK(snes_copier_header_size(1024 * 1024) == 0 && snes_copier_header_size(1024 * 1024 + 512) == 512);
    Bytes lo = make_snes(false, "LOROM TEST", 4);
    SnesInfo si = snes_probe(lo.data(), lo.size());
    CHECK(si.valid && !si.hirom && si.checksum_ok && si.header == 0 && si.title == "LOROM TEST");
    Bytes hi = make_snes(true, "HIROM TEST", 5, 256 * 1024);
    si = snes_probe(hi.data(), hi.size());
    CHECK(si.valid && si.hirom && si.checksum_ok && si.title == "HIROM TEST");
    Bytes cp = make_snes(false, "COPIER", 6, 128 * 1024, true);
    si = snes_probe(cp.data(), cp.size());
    CHECK(si.valid && !si.hirom && si.header == 512 && si.title == "COPIER");
    Bytes badsum = make_snes(false, "HACKED ROM TITLE", 7, 128 * 1024, false, false);
    si = snes_probe(badsum.data(), badsum.size());
    CHECK(si.valid && !si.checksum_ok);   // printable title still accepted (ROM hacks)
    Bytes junk(128 * 1024, 0);
    CHECK(!snes_probe(junk.data(), junk.size()).valid);
}

static void test_names() {
    CHECK(std::string(system_short(System::NES)) == "nes" && std::string(system_short(System::SNES)) == "snes");
    CHECK(system_from_short("NES") == System::NES && system_from_short("snes") == System::SNES);
    CHECK(std::string(system_name(System::SNES)) == "Super Nintendo");
    CHECK(std::string(system_name(System::NES)).find("Nintendo Entertainment") == 0);
}

static void test_scan(const fs::path& dir) {
    const fs::path lib = dir / "lib";
    Bytes nes = make_nes(2, 1, 0x02, 0, 11);
    uint32_t want_nes = crc32_of(nes.data() + 16, nes.size() - 16);
    Bytes snes = make_snes(false, "SCAN LOROM", 12, 256 * 1024);
    uint32_t want_snes = crc32_of(snes.data(), snes.size());
    Bytes snes_cp = make_snes(false, "SCAN LOROM", 12, 256 * 1024, true);   // the same game with a copier header: same key
    CHECK(write_file(lib / "Super Test (USA).nes", nes));
    CHECK(write_file(lib / "Fancy Quest (USA).sfc", snes));
    CHECK(write_file(lib / "copier" / "Fancy Quest (USA).smc", snes_cp));
    CHECK(write_file(lib / "notarom.nes", noise(40000, 3)));          // no iNES magic
    CHECK(write_file(lib / "notarom.sfc", Bytes(1024 * 40, 0)));      // .sfc/.smc are accepted by extension...
    GameEntry e;
    CHECK(identify_game((lib / "Super Test (USA).nes").string(), &e));
    CHECK(e.system == System::NES && e.key == "nes:" + hex8(want_nes) && e.title.find("Super Test") == 0);
    CHECK(identify_game((lib / "Fancy Quest (USA).sfc").string(), &e));
    CHECK(e.system == System::SNES && e.key == "snes:" + hex8(want_snes) && e.serial == "SCAN LOROM");
    GameEntry e2;
    CHECK(identify_game((lib / "copier" / "Fancy Quest (USA).smc").string(), &e2) && e2.key == e.key);   // headerless CRC
    CHECK(!identify_game((lib / "notarom.nes").string(), &e));

    // zipped
    {
        const fs::path zp = lib / "zipped" / "Zipped Game.zip";
        fs::create_directories(zp.parent_path());
        mz_zip_archive z;
        std::memset(&z, 0, sizeof z);
        CHECK(mz_zip_writer_init_file(&z, zp.string().c_str(), 0));
        Bytes zn = make_nes(1, 1, 0, 0, 21);
        CHECK(mz_zip_writer_add_mem(&z, "Inner Game (USA).nes", zn.data(), zn.size(), MZ_DEFAULT_COMPRESSION));
        CHECK(mz_zip_writer_finalize_archive(&z));
        mz_zip_writer_end(&z);
        library_set_data_dir((dir / "userdata").string());
        GameEntry ze;
        CHECK(identify_game(zp.string(), &ze) && ze.system == System::NES && ze.key == "nes:" + hex8(crc32_of(zn.data() + 16, zn.size() - 16)));
        CHECK(ze.save_stem == "Inner Game (USA)");
        library_set_data_dir("");
    }

    library_set_data_dir((dir / "userdata2").string());
    std::vector<GameEntry> r = scan_library({lib.string()}, nullptr);
    int nes_n = 0, snes_n = 0;
    for (auto& g : r) { nes_n += g.system == System::NES; snes_n += g.system == System::SNES; }
    CHECK(nes_n == 2);    // Super Test + the zipped one
    CHECK(snes_n >= 2);   // Fancy Quest (deduped by key) + notarom.sfc (accepted by extension)
    library_set_data_dir("");
}

static void test_cheats() {
    using namespace cheats;
    Target t;
    CHECK(target_for_system("nes", &t) && t == Target::NES && target_for_system("SNES", &t) && t == Target::SNES);
    std::string o, err;
    // NES Game Genie 6 / 8 letters
    CHECK(normalize_code(Target::NES, Format::GameGenie, "sxiopo", &o, &err) && o == "SXIOPO");
    CHECK(normalize_code(Target::NES, Format::GameGenie, "APZLGITY", &o, &err) && o == "APZLGITY");
    CHECK(!normalize_code(Target::NES, Format::GameGenie, "SXIOP", &o, &err));      // 5 letters
    CHECK(!normalize_code(Target::NES, Format::GameGenie, "SXIOPB", &o, &err));     // B is not in the alphabet
    CHECK(nes_game_genie_ok("GOSSIP") && !nes_game_genie_ok("GOSSIPS1"));
    // NES raw
    CHECK(normalize_code(Target::NES, Format::Raw, "0325:ff", &o, &err) && o == "0325:FF");
    CHECK(normalize_code(Target::NES, Format::Raw, "C000 01", &o, &err) && o == "C000:01");
    CHECK(normalize_code(Target::NES, Format::Raw, "0325:12:FF", &o, &err) && o == "0325:12:FF");
    CHECK(!normalize_code(Target::NES, Format::Raw, "0325:F", &o, &err));
    CHECK(!normalize_code(Target::NES, Format::GameShark, "0325:FF", &o, &err));
    // SNES
    CHECK(normalize_code(Target::SNES, Format::GameGenie, "c2c8-a6dd", &o, &err) && o == "C2C8-A6DD");
    CHECK(normalize_code(Target::SNES, Format::GameGenie, "C2C8A6DD", &o, &err) && o == "C2C8-A6DD");
    CHECK(!normalize_code(Target::SNES, Format::GameGenie, "C2C8-A6DG", &o, &err));   // G is not a SNES GG digit
    CHECK(normalize_code(Target::SNES, Format::ActionReplay, "7e0dbe05", &o, &err) && o == "7E0DBE05");
    CHECK(normalize_code(Target::SNES, Format::ActionReplay, "7E0DBE 05", &o, &err) && o == "7E0DBE05");
    CHECK(!normalize_code(Target::SNES, Format::ActionReplay, "7E0DBE0", &o, &err));
    CHECK(!normalize_code(Target::SNES, Format::CodeBreaker, "7E0DBE05", &o, &err));
    CHECK(snes_game_genie_ok("DF47-0915") && !snes_game_genie_ok("DF47-091"));
    // libretro string + hash
    Cheat c;
    c.format = Format::GameGenie; c.codes = {"SXIOPO", "APZLGITY"}; c.enabled = true;
    CHECK(retro_code_string(Target::NES, c, &o, &err) && o == "SXIOPO+APZLGITY");
    CheatSet set; set.cheats.push_back(c);
    CHECK(set_hash(set, Target::NES) != kEmptyHash);
    // store: format detection from pasted text
    Format f;
    CHECK(cheatstore::detect_format(Target::NES, "SXIOPO", &f) && f == Format::GameGenie);
    CHECK(cheatstore::detect_format(Target::NES, "0325 FF", &f) && f == Format::Raw);
    CHECK(cheatstore::detect_format(Target::SNES, "C2C8-A6DD", &f) && f == Format::GameGenie);
    CHECK(cheatstore::detect_format(Target::SNES, "7E0DBE05", &f) && f == Format::ActionReplay);
    CHECK(cheatstore::detect_format(Target::SNES, "7E0DBE 05", &f) && f == Format::ActionReplay);
    CHECK(!cheatstore::detect_format(Target::SNES, "hello", &f));
}

static const int kBtnA = 0, kBtnB = 1, kBtnX = 2, kBtnY = 3, kBtnLeftShoulder = 9, kBtnRightShoulder = 10;
static bool pressed(const hubctl::Profile& p, const char* ctl, int sdl_button) {
    hubctl::Mapper m(p);
    hubctl::PadState s;
    s.b[sdl_button] = true;
    return m.down(m.id(ctl), s);
}

static void test_controls() {
    using namespace hubctl;
    CHECK(is_system("nes") && is_system("snes"));
    CHECK(system_key(System::NES) == "nes" && system_key(System::SNES) == "snes");
    Profile n = default_profile("nes"), s = default_profile("snes");
    CHECK(controls_for("nes").size() == 8 && controls_for("snes").size() == 12);
    // positional: Xbox A (south) -> console B; Xbox B (east) -> console A; SNES X/Y likewise
    CHECK(pressed(n, "b", kBtnA) && pressed(n, "a", kBtnB) && !pressed(n, "a", kBtnA));
    CHECK(pressed(s, "b", kBtnA) && pressed(s, "a", kBtnB) && pressed(s, "y", kBtnX) && pressed(s, "x", kBtnY));
    CHECK(pressed(s, "l", kBtnLeftShoulder) && pressed(s, "r", kBtnRightShoulder));
    CHECK(pressed(s, "select", kBtnBack) && pressed(s, "start", kBtnStart));
    CHECK(has_keyboard("nes") && has_keyboard("snes") && !key_controls_for("snes").empty());
    // label layout toggle through the store
    Store st;
    CHECK(!label_layout_active(st, "snes", ""));
    CHECK(set_label_layout(st, "snes", "", true));
    CHECK(label_layout_active(st, "snes", ""));
    Profile r = resolve(st, "snes");
    CHECK(pressed(r, "a", kBtnA) && pressed(r, "b", kBtnB) && pressed(r, "x", kBtnX) && pressed(r, "y", kBtnY));
    CHECK(set_label_layout(st, "snes", "", false));
    CHECK(!label_layout_active(st, "snes", ""));
    CHECK(!set_label_layout(st, "n64", "", true));
    // retro profile is unchanged, nes/snes are separate system keys
    CHECK(default_profile("retro").system == "retro" && n.system == "nes");
}

static bool has_pair(const std::vector<std::string>& a, const std::string& k, const std::string& v) {
    for (size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == k && a[i + 1] == v) return true;
    return false;
}
static bool has_arg(const std::vector<std::string>& a, const std::string& k) {
    for (auto& x : a) if (x == k) return true;
    return false;
}

static void test_settings() {
    using namespace hub::gs;
    CHECK(defaults_for(System::NES).count("nes.region") && !defaults_for(System::SNES).count("nes.region"));
    CHECK(defaults_for(System::SNES).count("snes.multitap") && defaults_for(System::SNES).count("scale.integer"));
    CHECK(defaults_for(System::NES).count("scale.filter"));
    CHECK(retrorun_args(System::NES, defaults_for(System::NES)).empty());
    CHECK(retrorun_args(System::SNES, defaults_for(System::SNES)).empty());
    Store s;
    s.set_game("nes:k", System::NES, "nes.region", "pal");
    s.set_game("nes:k", System::NES, "scale.filter", "linear");
    s.set_game("nes:k", System::NES, "scale.integer", "false");
    auto ra = retrorun_args(System::NES, s.resolve(System::NES, "nes:k"));
    CHECK(has_pair(ra, "--opt", "nestopia_force_system=pal") && has_pair(ra, "--filter", "linear") && has_pair(ra, "--integer-scale", "0"));
    s.set_game("snes:k", System::SNES, "snes.region", "ntsc");
    s.set_game("snes:k", System::SNES, "snes.multitap", "on");
    ra = retrorun_args(System::SNES, s.resolve(System::SNES, "snes:k"));
    CHECK(has_pair(ra, "--opt", "snes9x_region=ntsc") && has_arg(ra, "--multitap"));
    s.set_game("snes:k", System::SNES, "snes.multitap", "off");
    CHECK(has_arg(retrorun_args(System::SNES, s.resolve(System::SNES, "snes:k")), "--no-multitap"));
}

static void test_find_add() {
    using namespace hub::fg;
    CHECK(classify_ext(".nes") == NES && classify_ext(".sfc") == SNES && classify_ext(".smc") == SNES);
    CHECK(std::string(sys_label(NES)) == "NES" && std::string(sys_label(SNES)) == "SNES");
    CHECK(hub::addgames::is_game_file("C:\\x\\a.nes") && hub::addgames::is_game_file("C:\\x\\a.SFC") && hub::addgames::is_game_file("a.smc"));
    CHECK(hub::addgames::console_folder(System::NES) == "NES" && hub::addgames::console_folder(System::SNES) == "SNES");
}

int main() {
    fs::path dir = fs::temp_directory_path() / "nes_snes_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    test_headers();
    test_names();
    test_scan(dir);
    test_cheats();
    test_controls();
    test_settings();
    test_find_add();
    fs::remove_all(dir, ec);
    if (g_fail) { std::printf("nes_snes_test: %d check(s) failed\n", g_fail); return 1; }
    std::printf("nes_snes_test: all checks passed\n");
    return 0;
}
