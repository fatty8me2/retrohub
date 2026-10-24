// Console test for PlayStation 2 (PCSX2): disc identification (iso / raw bin+cue / cso / chd name), library scan,
// BIOS discovery, PCSX2.ini writers, pad bindings, memory card session mapping, launch arguments.
// Usage: hub_ps2_test                    offline unit checks (what ctest runs)
//        hub_ps2_test --download <dir>   also download + verify + install PCSX2 into <dir> (network; never launched), then
//                                        run prepare_pcsx2 on it and read the config back.
#include "emulators.h"
#include "library.h"
#include "logic.h"
#include "miniz.h"
#include "pcsx2.h"
#include "ps2.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <thread>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace hub;

static int g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

typedef std::vector<uint8_t> Bytes;

static void put_le32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); }
static bool write_file(const fs::path& p, const Bytes& d) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(d.data()), (std::streamsize)d.size());
    return (bool)f;
}
static bool write_text(const fs::path& p, const std::string& t) { return write_file(p, Bytes(t.begin(), t.end())); }
static std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static std::string U8(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }

/* A tiny ISO9660 image (2048-byte sectors): PVD at 16, root directory at 20, SYSTEM.CNF at 21. */
static Bytes make_iso(const std::string& cnf) {
    Bytes b(64 * 2048, 0);
    uint8_t* pvd = &b[16 * 2048];
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    pvd[156] = 34;
    put_le32(b, 16 * 2048 + 156 + 2, 20);
    put_le32(b, 16 * 2048 + 156 + 10, 2048);
    uint8_t* d = &b[20 * 2048];
    const char* nm = "SYSTEM.CNF;1";
    size_t nl = std::strlen(nm), len = 33 + nl;
    if (len & 1) ++len;
    d[0] = (uint8_t)len;
    put_le32(b, 20 * 2048 + 2, 21);
    put_le32(b, 20 * 2048 + 10, (uint32_t)cnf.size());
    d[32] = (uint8_t)nl;
    std::memcpy(d + 33, nm, nl);
    std::memcpy(&b[21 * 2048], cnf.data(), cnf.size());
    return b;
}
static std::string ps2_cnf(const char* serial_file) {
    return std::string("BOOT2 = cdrom0:\\") + serial_file + ";1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";
}
/* 2352-byte raw sectors (mode 1: sync + header, user data at +16). */
static Bytes make_raw(const Bytes& iso) {
    Bytes r;
    for (size_t s = 0; s * 2048 < iso.size(); ++s) {
        Bytes sec(2352, 0);
        sec[0] = 0;
        for (int i = 1; i <= 10; ++i) sec[i] = 0xFF;
        sec[11] = 0;
        sec[15] = 1;
        std::memcpy(&sec[16], &iso[s * 2048], 2048);
        r.insert(r.end(), sec.begin(), sec.end());
    }
    return r;
}
/* CSO v1: 24-byte header, block index, raw-deflate blocks (bit 31 = stored). */
static Bytes make_cso(const Bytes& iso) {
    const uint32_t bs = 2048;
    size_t n = iso.size() / bs;
    Bytes out(24 + (n + 1) * 4, 0);
    std::memcpy(&out[0], "CISO", 4);
    put_le32(out, 4, 24);
    put_le32(out, 8, (uint32_t)iso.size());
    put_le32(out, 16, bs);
    out[20] = 1;
    out[21] = 0;
    for (size_t i = 0; i < n; ++i) {
        put_le32(out, 24 + i * 4, (uint32_t)out.size() | 0);
        std::vector<uint8_t> comp(bs * 2);
        size_t cl = tdefl_compress_mem_to_mem(comp.data(), comp.size(), &iso[i * bs], bs, 128);
        bool plain = cl == 0 || cl >= bs;
        if (plain) {
            uint32_t pos = (uint32_t)out.size() | 0x80000000u;
            put_le32(out, 24 + i * 4, pos);
            out.insert(out.end(), iso.begin() + i * bs, iso.begin() + (i + 1) * bs);
        } else {
            out.insert(out.end(), comp.begin(), comp.begin() + cl);
        }
    }
    put_le32(out, 24 + n * 4, (uint32_t)out.size());
    return out;
}

static void test_serials() {
    CHECK(ps2::normalize_serial("SLUS_209.46") == "SLUS-20946");
    CHECK(ps2::normalize_serial("slus-20946") == "SLUS-20946");
    CHECK(ps2::normalize_serial("SCES50294") == "SCES-50294");
    CHECK(ps2::normalize_serial("SLUS_99") == "");
    CHECK(ps2::normalize_serial("ab-12345") == "");
    CHECK(ps2::serial_from_filename("Grand Theft Auto - San Andreas [SLUS-20946].chd") == "SLUS-20946");
    CHECK(ps2::serial_from_filename("SLES_523.51 - game.iso") == "SLES-52351");
    CHECK(ps2::serial_from_filename("No serial here (USA).iso").empty());
    CHECK(ps2::serial_from_filename("XSLUS-20946.iso").empty());   // glued to a longer word: not a serial
    CHECK(ps2::title_for("SLUS-20946") == "Grand Theft Auto - San Andreas");
    CHECK(ps2::title_for("SLUS-00000").empty());
    CHECK(ps2::kDbCount > 10000);
    for (size_t i = 1; i < ps2::kDbCount; ++i) if (std::strcmp(ps2::kDb[i - 1], ps2::kDb[i]) >= 0) { CHECK(false && "db sorted"); break; }
    std::string s;
    CHECK(ps2::parse_system_cnf("BOOT2 = cdrom0:\\SLUS_209.46;1\r\nVER = 1.00\r\n", &s) == ps2::Cnf::Ps2 && s == "SLUS-20946");
    CHECK(ps2::parse_system_cnf("BOOT = cdrom:\\SCUS_941.63;1\r\nTCB = 4\r\n", &s) == ps2::Cnf::Ps1 && s == "SCUS-94163");
    CHECK(ps2::parse_system_cnf("hello", &s) == ps2::Cnf::None);
    CHECK(ps2::cue_first_file("FILE \"My Game (Track 1).bin\" BINARY\r\n TRACK 01 MODE2/2352\r\n") == "My Game (Track 1).bin");
    CHECK(ps2::cue_first_file("FILE game.bin BINARY\n") == "game.bin");
}

static void test_images(const fs::path& d) {
    Bytes iso = make_iso(ps2_cnf("SLUS_209.46"));
    write_file(d / "gta.iso", iso);
    ps2::Ident id;
    CHECK(ps2::identify_file(U8(d / "gta.iso"), &id) && id.serial == "SLUS-20946" && id.title == "Grand Theft Auto - San Andreas");

    write_file(d / "rawgame.bin", make_raw(iso));
    write_text(d / "rawgame.cue", "FILE \"rawgame.bin\" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n");
    id = ps2::Ident();
    CHECK(ps2::identify_file(U8(d / "rawgame.cue"), &id) && id.serial == "SLUS-20946");
    CHECK(ps2::identify_file(U8(d / "rawgame.bin"), &id) && id.serial == "SLUS-20946");

    write_file(d / "game.cso", make_cso(iso));
    id = ps2::Ident();
    CHECK(ps2::identify_file(U8(d / "game.cso"), &id) && id.serial == "SLUS-20946");

    write_file(d / "Okage [SCUS-97129].chd", Bytes(64, 0));   // not decoded: serial from the name, must be a known PS2 game
    id = ps2::Ident();
    CHECK(ps2::identify_file(U8(d / "Okage [SCUS-97129].chd"), &id) && id.serial == "SCUS-97129" && id.title == "Okage - Shadow King");
    write_file(d / "unknown.chd", Bytes(64, 0));
    CHECK(!ps2::identify_file(U8(d / "unknown.chd"), &id));

    write_file(d / "psx.iso", make_iso("BOOT = cdrom:\\SCUS_941.63;1\r\n"));   // PlayStation 1: not ours
    id = ps2::Ident();
    CHECK(!ps2::identify_file(U8(d / "psx.iso"), &id) && id.ps1);
    write_file(d / "junk.iso", Bytes(100000, 7));
    CHECK(!ps2::identify_file(U8(d / "junk.iso"), &id));

    auto buf = std::make_shared<std::string>((const char*)iso.data(), iso.size());   // archive probe path
    id = ps2::Ident();
    CHECK(ps2::identify_buffer(buf, &id) && id.serial == "SLUS-20946");
}

static void test_scan(const fs::path& d) {
    fs::path games = d / "GamesPS2";
    write_file(games / "Grand Theft Auto.iso", make_iso(ps2_cnf("SLUS_209.46")));
    write_file(games / "sub" / "Okage [SCUS-97129].chd", Bytes(64, 0));
    write_file(games / "cuegame.bin", make_raw(make_iso(ps2_cnf("SLES_523.51"))));
    write_text(games / "cuegame.cue", "FILE \"cuegame.bin\" BINARY\r\n  TRACK 01 MODE1/2352\r\n");
    write_file(games / "psx.iso", make_iso("BOOT = cdrom:\\SCUS_941.63;1\r\n"));
    library_set_data_dir(U8(d / "userdata"));
    auto lib = scan_library({U8(games)}, nullptr);
    int ps2n = 0;
    bool gta = false, okage = false, cue = false, psx = false;
    for (const auto& g : lib) {
        if (g.system != System::PS2) { if (g.path.find("psx.iso") != std::string::npos) psx = true; continue; }
        ++ps2n;
        if (g.key == "ps2:slus-20946" && g.title == "Grand Theft Auto - San Andreas") gta = true;
        if (g.key == "ps2:scus-97129") okage = true;
        if (g.key == "ps2:sles-52351" && g.path.find("cuegame.cue") != std::string::npos) cue = true;
    }
    CHECK(gta && okage && cue && !psx);
    CHECK(ps2n == 3);
    CHECK(std::string(system_short(System::PS2)) == "ps2" && system_from_short("ps2") == System::PS2 && std::string(system_name(System::PS2)) == "PlayStation 2");
    library_set_data_dir("");
}

static void test_bios(const fs::path& d) {
    CHECK(hubemu::pcsx2_bios_name_ok("scph39001.bin", 4u << 20));
    CHECK(hubemu::pcsx2_bios_name_ok("SCPH-70012.BIN", 4u << 20));
    CHECK(hubemu::pcsx2_bios_name_ok("ps2-0230a-20080220.bin", 4u << 20));
    CHECK(!hubemu::pcsx2_bios_name_ok("scph1001.bin", 512 * 1024));   // PS1 BIOS
    CHECK(!hubemu::pcsx2_bios_name_ok("scph39001.rom", 4u << 20));
    CHECK(!hubemu::pcsx2_bios_name_ok("game.bin", 4u << 20));
    fs::path root = d / "bios_root";
    CHECK(hubemu::pcsx2_find_bios({U8(root)}).file.empty());
    write_file(root / "Games" / "x.txt", Bytes(1, 0));
    write_file(root / "BIOS" / "scph1001.bin", Bytes(512 * 1024, 1));
    CHECK(hubemu::pcsx2_find_bios({U8(root / "Games")}).file.empty());
    { std::ofstream f(root / "BIOS" / "scph39001.bin", std::ios::binary); Bytes z(4u << 20, 2); f.write((const char*)z.data(), (std::streamsize)z.size()); }
    hubemu::Bios b = hubemu::pcsx2_find_bios({U8(root / "Games")});   // sibling BIOS folder of a Games folder
    CHECK(b.file == "scph39001.bin" && fs::path(b.dir).filename() == "BIOS");
    b = hubemu::pcsx2_find_bios({U8(root)});                           // BIOS subfolder
    CHECK(b.file == "scph39001.bin");
    auto dirs = hubemu::pcsx2_bios_search_dirs("D:\\chosen", {"E:\\Games"});
    CHECK(dirs.size() >= 3 && dirs[0] == "D:\\chosen");
    CHECK(std::string(hubemu::pcsx2_bios_missing_text()).find("BIOS") != std::string::npos);
}

static hubemu::PadDesc pad(const char* name, const char* key) { hubemu::PadDesc p; p.name = name; p.key = key; p.guid = key; return p; }

static void test_config(const fs::path& d) {
    using namespace hubemu;
    std::string def = pcsx2_ini_defaults("");
    CHECK(hubui::ini_get(def, "UI", "SetupWizardIncomplete", "") == "false");
    CHECK(hubui::ini_get(def, "UI", "ConfirmShutdown", "") == "false");
    CHECK(hubui::ini_get(def, "AutoUpdater", "CheckAtStartup", "") == "false");
    CHECK(hubui::ini_get(def, "InputSources", "SDL", "") == "true");
    // user edits survive a second pass
    std::string edited = hubui::ini_set(def, "EmuCore", "CdvdShareWrite", "true");
    CHECK(hubui::ini_get(pcsx2_ini_defaults(edited), "EmuCore", "CdvdShareWrite", "") == "true");

    // bindings
    hubctl::Term a; a.axis = false; a.idx = 0;
    CHECK(pcsx2_binding(a, 0) == "SDL-0/A");
    hubctl::Term up; up.axis = true; up.idx = 1; up.sign = -1;
    CHECK(pcsx2_binding(up, 1) == "SDL-1/-LeftY");
    hubctl::Term rt; rt.axis = true; rt.idx = 5; rt.sign = 0;
    CHECK(pcsx2_binding(rt, 2) == "SDL-2/+RightTrigger");
    hubctl::Term dp; dp.axis = false; dp.idx = 11;
    CHECK(pcsx2_binding(dp, 0) == "SDL-0/DPadUp");

    // pads: Xbox -> DualShock 2 by position
    hubctl::Profile prof = hubctl::default_profile("ps2");
    CHECK(prof.system == "ps2" && !hubctl::controls_for("ps2").empty() && hubctl::is_system("ps2") && hubctl::system_key(System::PS2) == "ps2");
    std::vector<PadDesc> sdl = {pad("Xbox One", "k0"), pad("DualSense", "k1"), pad("Pro", "k2"), pad("8BitDo", "k3")};
    Ps2Pads p1 = pcsx2_pads_with({sdl[1]}, sdl);
    CHECK(p1.count == 1 && p1.sdl[0] == 1 && !p1.multitap);
    Ps2Pads p2 = pcsx2_pads_with({sdl[2], sdl[0]}, sdl);   // port order differs from SDL order
    CHECK(p2.count == 2 && p2.sdl[0] == 2 && p2.sdl[1] == 0 && !p2.multitap);
    Ps2Pads p4 = pcsx2_pads_with(sdl, sdl);
    CHECK(p4.count == 4 && p4.multitap);

    std::string ini = pcsx2_ini_pads("", p2, prof);
    CHECK(hubui::ini_get(ini, "Pad1", "Type", "") == "DualShock2");
    CHECK(hubui::ini_get(ini, "Pad1", "Cross", "") == "SDL-2/A" && hubui::ini_get(ini, "Pad1", "Circle", "") == "SDL-2/B");
    CHECK(hubui::ini_get(ini, "Pad1", "Square", "") == "SDL-2/X" && hubui::ini_get(ini, "Pad1", "Triangle", "") == "SDL-2/Y");
    CHECK(hubui::ini_get(ini, "Pad1", "L2", "") == "SDL-2/+LeftTrigger" && hubui::ini_get(ini, "Pad1", "R2", "") == "SDL-2/+RightTrigger");
    CHECK(hubui::ini_get(ini, "Pad1", "Select", "") == "SDL-2/Back" && hubui::ini_get(ini, "Pad1", "Start", "") == "SDL-2/Start");
    CHECK(hubui::ini_get(ini, "Pad1", "LUp", "") == "SDL-2/-LeftY" && hubui::ini_get(ini, "Pad1", "LRight", "") == "SDL-2/+LeftX");
    CHECK(hubui::ini_get(ini, "Pad1", "RDown", "") == "SDL-2/+RightY" && hubui::ini_get(ini, "Pad1", "RLeft", "") == "SDL-2/-RightX");
    CHECK(hubui::ini_get(ini, "Pad1", "LargeMotor", "") == "SDL-2/LargeMotor");
    CHECK(hubui::ini_get(ini, "Pad2", "Cross", "") == "SDL-0/A" && hubui::ini_get(ini, "Pad3", "Type", "") == "None");
    CHECK(hubui::ini_get(ini, "Pad", "MultitapPort1", "") == "false");
    std::string tap = pcsx2_ini_pads("", p4, prof);
    CHECK(hubui::ini_get(tap, "Pad", "MultitapPort1", "") == "true");
    CHECK(hubui::ini_get(tap, "Pad1", "Cross", "") == "SDL-0/A" && hubui::ini_get(tap, "Pad3", "Cross", "") == "SDL-1/A");
    CHECK(hubui::ini_get(tap, "Pad4", "Cross", "") == "SDL-2/A" && hubui::ini_get(tap, "Pad5", "Cross", "") == "SDL-3/A");
    CHECK(hubui::ini_get(tap, "Pad2", "Type", "") == "None");
    std::string kb = pcsx2_ini_pads(ini, Ps2Pads(), prof);   // no pads: keyboard on Pad1
    CHECK(hubui::ini_get(kb, "Pad1", "Cross", "") == "Keyboard/K" && hubui::ini_get(kb, "Pad2", "Type", "") == "None");
    // custom mapping + deadzone
    prof.buttons["cross"] = "b";
    prof.analogs["left_stick"].deadzone = 0.2;
    std::string cu = pcsx2_ini_pads("", p1, prof);
    CHECK(hubui::ini_get(cu, "Pad1", "Cross", "") == "SDL-1/B" && hubui::ini_get(cu, "Pad1", "Deadzone", "") == "0.2");

    // per launch
    hub::gs::Values v = hub::gs::defaults_for(System::PS2);
    CHECK(hub::gs::is_fullscreen(v) && v["ps2.res"] == "2" && v["ps2.renderer"] == "auto");
    v["ps2.res"] = "3"; v["ps2.renderer"] = "vulkan"; v["ps2.widescreen"] = "true";
    Bios bios{"C:\\bios", "scph39001.bin"};
    std::string l = pcsx2_ini_launch("", bios, pcsx2_card_rel("ps2:slus-20946"), v);
    CHECK(hubui::ini_get(l, "Folders", "Bios", "") == "C:\\bios" && hubui::ini_get(l, "Filenames", "BIOS", "") == "scph39001.bin");
    CHECK(hubui::ini_get(l, "EmuCore/GS", "Renderer", "") == "14" && hubui::ini_get(l, "EmuCore/GS", "upscale_multiplier", "") == "3");
    CHECK(hubui::ini_get(l, "EmuCore", "EnableWideScreenPatches", "") == "true");
    CHECK(hubui::ini_get(l, "MemoryCards", "Slot1_Filename", "") == "RetroHub/ps2_slus-20946/Mcd001.ps2" && hubui::ini_get(l, "MemoryCards", "Slot2_Enable", "") == "false");
    v["ps2.renderer"] = "dx12";
    CHECK(hubui::ini_get(pcsx2_ini_launch("", bios, "x", v), "EmuCore/GS", "Renderer", "") == "15");

    CHECK(pcsx2_args("D:\\g\\a b.iso", true) == "-batch -nogui -fullscreen -- \"D:\\g\\a b.iso\"");
    CHECK(pcsx2_args("D:\\g\\a.iso", false).find("-nofullscreen") != std::string::npos);

    // prepare on a scratch install dir
    fs::path inst = d / "pcsx2inst";
    fs::create_directories(inst);
    std::string err = prepare_pcsx2(U8(inst), p2, hubctl::default_profile("ps2"), bios, "ps2:slus-20946", hub::gs::defaults_for(System::PS2));
    CHECK(err.empty() && fs::exists(inst / "portable.ini") && fs::exists(inst / "inis" / "PCSX2.ini"));
    std::string back = read_text(inst / "inis" / "PCSX2.ini");
    CHECK(hubui::ini_get(back, "UI", "SetupWizardIncomplete", "") == "false" && hubui::ini_get(back, "Filenames", "BIOS", "") == "scph39001.bin");
    CHECK(hubui::ini_get(back, "Pad1", "Cross", "") == "SDL-2/A");
    CHECK(fs::is_directory(inst / "memcards" / "RetroHub" / "ps2_slus-20946"));
    // a user edit of an unrelated key survives the next launch; changed pads are rewritten
    write_text(inst / "inis" / "PCSX2.ini", hubui::ini_set(back, "EmuCore", "CdvdShareWrite", "true"));
    err = prepare_pcsx2(U8(inst), p1, hubctl::default_profile("ps2"), bios, "ps2:slus-20946", hub::gs::defaults_for(System::PS2));
    back = read_text(inst / "inis" / "PCSX2.ini");
    CHECK(hubui::ini_get(back, "EmuCore", "CdvdShareWrite", "") == "true" && hubui::ini_get(back, "Pad1", "Cross", "") == "SDL-1/A");
}

static void test_emulator_table(const fs::path& d) {
    using namespace hubemu;
    const Release& r = release(Kind::Pcsx2);
    CHECK(std::string(r.sha256).size() == 64 && r.size > 20000000 && std::string(r.version) == "2.8.2");
    CHECK(std::wstring(r.exe) == L"pcsx2-qt.exe" && std::wstring(r.host) == L"github.com");
    CHECK(kind_for(System::PS2) == Kind::Pcsx2 && std::string(name(Kind::Pcsx2)) == "PCSX2");
    std::string md = managed_dir(Kind::Pcsx2, U8(d));
    CHECK(fs::path(md).filename() == "pcsx2");
    CHECK(find(Kind::Pcsx2, "", "", U8(d / "none")).exe.empty());   // never reuses a user's own PCSX2
    // memory card session: <install>/memcards/RetroHub/<game> <-> <vault>/ps2/<game>/battery/memcards/RetroHub/<game>
    fs::path inst = d / "sessinst", vault = d / "vault";
    fs::create_directories(inst);
    write_text(inst / "pcsx2-qt.exe", "x");
    Install in; in.exe = U8(inst / "pcsx2-qt.exe"); in.dir = U8(inst); in.managed = true;
    GameEntry e; e.system = System::PS2; e.key = "ps2:slus-20946";
    std::string battery = U8(vault / "ps2" / "ps2_slus-20946" / "battery");
    write_text(vault / "ps2" / "ps2_slus-20946" / "battery" / "memcards" / "RetroHub" / "ps2_slus-20946" / "Mcd001.ps2", "VAULTCARD");
    SaveSession s = begin_session(e, in, battery, "");
    CHECK(s.active && s.rels.size() == 1 && s.rels[0] == "memcards/RetroHub/ps2_slus-20946");
    CHECK(read_text(inst / "memcards" / "RetroHub" / "ps2_slus-20946" / "Mcd001.ps2") == "VAULTCARD");
    write_text(inst / "memcards" / "RetroHub" / "ps2_slus-20946" / "Mcd001.ps2", "NEWSAVE");
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));   // newest wins: the card must be newer than the vault copy
    end_session(s);
    CHECK(read_text(vault / "ps2" / "ps2_slus-20946" / "battery" / "memcards" / "RetroHub" / "ps2_slus-20946" / "Mcd001.ps2") == "NEWSAVE");
}

static int test_download(const std::string& root) {
    using namespace hubemu;
    std::string err;
    std::printf("downloading PCSX2 into %s ...\n", root.c_str());
    bool ok = install_blocking(Kind::Pcsx2, root, &err, [](int ph, uint64_t got, uint64_t total) {
        std::printf("  phase %d  %llu / %llu\r", ph, (unsigned long long)got, (unsigned long long)total);
    });
    std::printf("\n");
    if (!ok) { std::printf("DOWNLOAD FAILED: %s\n", err.c_str()); return 1; }
    Install in = find(Kind::Pcsx2, "", "", root);
    CHECK(!in.exe.empty() && in.managed);
    std::printf("installed: %s\n", in.exe.c_str());
    CHECK(fs::exists(fs::path(in.dir) / "portable.ini"));
    CHECK(fs::exists(fs::path(in.dir) / "retrohub-install.txt"));
    Bios bios{"C:\\nowhere", "scph39001.bin"};
    Ps2Pads pads = pcsx2_pads_with({}, {});
    std::string e = prepare_pcsx2(in.dir, pads, hubctl::default_profile("ps2"), bios, "ps2:test", hub::gs::defaults_for(System::PS2));
    CHECK(e.empty());
    std::string ini = read_text(fs::path(in.dir) / "inis" / "PCSX2.ini");
    CHECK(hubui::ini_get(ini, "UI", "SetupWizardIncomplete", "") == "false" && hubui::ini_get(ini, "Pad1", "Cross", "") == "Keyboard/K");
    std::printf("config readback OK (%zu bytes); PCSX2 was not started\n", ini.size());
    return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
    fs::path d = fs::temp_directory_path() / ("hub_ps2_test_" + std::to_string((unsigned long long)std::time(nullptr)));
    fs::create_directories(d);
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
    std::printf("serials\n"); test_serials();
    std::printf("images\n"); test_images(d / "img");
    std::printf("scan\n"); test_scan(d / "scan");
    std::printf("bios\n"); test_bios(d / "bios");
    std::printf("cfg\n"); test_config(d / "cfg");
    std::printf("tbl\n"); test_emulator_table(d / "tbl");
    } catch (const std::exception& ex) { std::printf("EXCEPTION %s\n", ex.what()); return 2; }
    int dl = 0;
    if (argc >= 3 && std::string(argv[1]) == "--download") dl = test_download(argv[2]);
    std::error_code ec;
    fs::remove_all(d, ec);
    std::printf("hub_ps2_test: %d check(s) failed%s\n", g_fail, dl ? ", download failed" : "");
    return (g_fail || dl) ? 1 : 0;
}
