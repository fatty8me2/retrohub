// Console test for PlayStation Portable (PPSSPP): PARAM.SFO parsing, identification of ISO / CSO / CHD / PBP images built in memory
// (synthetic: no ROM or ISO is read from disk), PS1 eboot vs PSP routing, keys, library scan (plain files and a zip), SAVEDATA <-> vault
// mapping and the PPSSPP config writers. No network, no emulator.
// Usage: hub_psp_test                    what ctest runs
//        hub_psp_test --download <dir>   also downloads + verifies + unpacks the pinned PPSSPP into <dir> (network; never started)
#include <windows.h>
#include "emulators.h"
#include "library.h"
#include "logic.h"
#include "miniz.h"
#include "ppsspp.h"
#include "psp.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
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
static void put_le16(Bytes& b, size_t at, uint32_t v) { b[at] = uint8_t(v); b[at + 1] = uint8_t(v >> 8); }
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

/* ---- fixtures ---- */

/* PARAM.SFO: header, index entries, key table, data table. Strings are utf8 (0x0204), numbers int32 (0x0404). */
struct SfoField { std::string key; std::string text; bool number = false; uint32_t num = 0; };
static Bytes make_sfo(const std::vector<SfoField>& fields) {
    std::string keys;
    std::vector<uint32_t> koff;
    for (auto& f : fields) { koff.push_back((uint32_t)keys.size()); keys += f.key; keys.push_back(0); }
    while (keys.size() % 4) keys.push_back(0);
    Bytes data;
    std::vector<uint32_t> doff, dlen;
    for (auto& f : fields) {
        doff.push_back((uint32_t)data.size());
        if (f.number) { dlen.push_back(4); for (int i = 0; i < 4; ++i) data.push_back(uint8_t(f.num >> (8 * i))); }
        else {
            dlen.push_back((uint32_t)f.text.size() + 1);
            data.insert(data.end(), f.text.begin(), f.text.end());
            data.push_back(0);
            while (data.size() % 4) data.push_back(0);
        }
    }
    const size_t n = fields.size(), kt = 20 + n * 16, dt = kt + keys.size();
    Bytes b(dt + data.size(), 0);
    std::memcpy(&b[0], "\0PSF", 4);
    put_le32(b, 4, 0x0101);
    put_le32(b, 8, (uint32_t)kt);
    put_le32(b, 12, (uint32_t)dt);
    put_le32(b, 16, (uint32_t)n);
    for (size_t i = 0; i < n; ++i) {
        put_le16(b, 20 + i * 16, koff[i]);
        put_le16(b, 20 + i * 16 + 2, fields[i].number ? 0x0404 : 0x0204);
        put_le32(b, 20 + i * 16 + 4, dlen[i]);
        put_le32(b, 20 + i * 16 + 8, fields[i].number ? 4 : (uint32_t)((dlen[i] + 3) & ~3u));
        put_le32(b, 20 + i * 16 + 12, doff[i]);
    }
    std::memcpy(&b[kt], keys.data(), keys.size());
    std::memcpy(&b[dt], data.data(), data.size());
    return b;
}
static Bytes umd_sfo(const std::string& id, const std::string& title, const char* category = "UG") {
    return make_sfo({{"CATEGORY", category}, {"DISC_ID", id}, {"PARENTAL_LEVEL", "", true, 5}, {"TITLE", title}});
}

static void dir_record(Bytes& b, size_t at, const std::string& name, uint32_t lba, uint32_t size, uint8_t flags) {
    size_t len = 33 + name.size();
    if (len & 1) ++len;
    b[at] = (uint8_t)len;
    put_le32(b, at + 2, lba);
    put_le32(b, at + 10, size);
    b[at + 25] = flags;
    b[at + 32] = (uint8_t)name.size();
    std::memcpy(&b[at + 33], name.data(), name.size());
}
/* ISO9660 (2048-byte sectors): PVD 16, root 20 holding PSP_GAME, PSP_GAME 21 holding PARAM.SFO, the SFO 22. with_psp_game = false: a plain data disc. */
static Bytes make_psp_iso(const Bytes& sfo, bool with_psp_game = true) {
    Bytes b(64 * 2048, 0);
    uint8_t* pvd = &b[16 * 2048];
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    std::memcpy(pvd + 8, "PSP GAME", 8);
    pvd[156] = 34;
    put_le32(b, 16 * 2048 + 156 + 2, 20);
    put_le32(b, 16 * 2048 + 156 + 10, 2048);
    if (with_psp_game) {
        dir_record(b, 20 * 2048, "PSP_GAME", 21, 2048, 2);
        dir_record(b, 21 * 2048, "PARAM.SFO;1", 22, (uint32_t)sfo.size(), 0);
        std::memcpy(&b[22 * 2048], sfo.data(), sfo.size());
    } else {
        dir_record(b, 20 * 2048, "README.TXT;1", 22, 4, 0);
    }
    return b;
}
/* A PS1 / PS2 disc: SYSTEM.CNF in the root. */
static Bytes make_cnf_iso(const std::string& cnf) {
    Bytes b(64 * 2048, 0);
    uint8_t* pvd = &b[16 * 2048];
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[156] = 34;
    put_le32(b, 16 * 2048 + 156 + 2, 20);
    put_le32(b, 16 * 2048 + 156 + 10, 2048);
    dir_record(b, 20 * 2048, "SYSTEM.CNF;1", 21, (uint32_t)cnf.size(), 0);
    std::memcpy(&b[21 * 2048], cnf.data(), cnf.size());
    return b;
}
/* CISO v1: header, block index, raw-deflate blocks (bit 31 of an index entry = stored). */
static Bytes make_cso(const Bytes& iso) {
    const uint32_t bs = 2048;
    size_t n = iso.size() / bs;
    Bytes out(24 + (n + 1) * 4, 0);
    std::memcpy(&out[0], "CISO", 4);
    put_le32(out, 4, 24);
    put_le32(out, 8, (uint32_t)iso.size());
    put_le32(out, 16, bs);
    out[20] = 1;
    for (size_t i = 0; i < n; ++i) {
        put_le32(out, 24 + i * 4, (uint32_t)out.size());
        std::vector<uint8_t> comp(bs * 2);
        size_t cl = tdefl_compress_mem_to_mem(comp.data(), comp.size(), &iso[i * bs], bs, 128);
        if (cl == 0 || cl >= bs) {
            put_le32(out, 24 + i * 4, (uint32_t)out.size() | 0x80000000u);
            out.insert(out.end(), iso.begin() + i * bs, iso.begin() + (i + 1) * bs);
        } else {
            out.insert(out.end(), comp.begin(), comp.begin() + cl);
        }
    }
    put_le32(out, 24 + n * 4, (uint32_t)out.size());
    return out;
}
/* CHD v5, uncompressed (compressors[0] = 0): header, big-endian hunk map, hunks. unit 2448 = CD frames, 2048 = DVD. */
static void be32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (24 - 8 * i)); }
static Bytes make_chd(const Bytes& iso, bool cd) {
    const uint32_t unit = cd ? 2448 : 2048, per = 8, hunk = unit * per;
    size_t sectors = iso.size() / 2048, nh = (sectors + per - 1) / per;
    size_t data_start = ((124 + 4 * nh + hunk - 1) / hunk) * hunk;
    Bytes b(data_start + nh * hunk, 0);
    std::memcpy(&b[0], "MComprHD", 8);
    be32(b, 8, 124);
    be32(b, 12, 5);
    for (int i = 0; i < 8; ++i) b[32 + i] = uint8_t(uint64_t(nh) * hunk >> (56 - 8 * i));
    for (int i = 0; i < 8; ++i) b[40 + i] = uint8_t(uint64_t(124) >> (56 - 8 * i));
    be32(b, 56, hunk);
    be32(b, 60, unit);
    for (size_t h = 0; h < nh; ++h) be32(b, 124 + 4 * h, uint32_t(data_start / hunk + h));
    for (size_t sct = 0; sct < sectors; ++sct) {
        size_t at = data_start + (sct / per) * hunk + (sct % per) * unit;
        if (cd) {
            for (int i = 1; i <= 10; ++i) b[at + i] = 0xFF;
            b[at + 15] = 1;
            std::memcpy(&b[at + 16], &iso[sct * 2048], 2048);
        } else {
            std::memcpy(&b[at], &iso[sct * 2048], 2048);
        }
    }
    return b;
}
/* EBOOT.PBP: header (version, 8 offsets), PARAM.SFO, a little icon, DATA.PSP, DATA.PSAR (psar text). */
static Bytes make_pbp(const Bytes& sfo, const std::string& psar) {
    Bytes b(0x28, 0);
    std::memcpy(&b[0], "\0PBP", 4);
    put_le32(b, 4, 0x10000);
    const uint32_t o_sfo = 0x28, o_icon = o_sfo + (uint32_t)sfo.size(), o_psp = o_icon + 64, o_psar = o_psp + 256;
    put_le32(b, 8, o_sfo);
    put_le32(b, 12, o_icon);   // icon0
    put_le32(b, 16, o_icon + 64);
    put_le32(b, 20, o_icon + 64);
    put_le32(b, 24, o_icon + 64);
    put_le32(b, 28, o_icon + 64);
    put_le32(b, 32, o_psp);
    put_le32(b, 36, o_psar);
    b.insert(b.end(), sfo.begin(), sfo.end());
    b.resize(o_psar, 0x11);
    b.insert(b.end(), psar.begin(), psar.end());
    b.resize(b.size() + 64, 0);
    return b;
}

/* ---- tests ---- */

static void test_sfo_and_keys() {
    psp::Sfo s;
    Bytes raw = umd_sfo("ULUS10041", "Crisis Core -Final Fantasy VII-");
    CHECK(psp::parse_sfo(raw.data(), raw.size(), &s) && s.ok);
    CHECK(s.get("DISC_ID") == "ULUS10041" && s.get("TITLE") == "Crisis Core -Final Fantasy VII-" && s.get("CATEGORY") == "UG");
    CHECK(s.num.count("PARENTAL_LEVEL") && s.num["PARENTAL_LEVEL"] == 5);
    CHECK(!psp::parse_sfo(raw.data(), 10, &s));
    Bytes bad = raw; bad[1] = 'X';
    CHECK(!psp::parse_sfo(bad.data(), bad.size(), &s));
    Bytes cut(raw.begin(), raw.begin() + 30);   // truncated: never reads past the buffer
    psp::parse_sfo(cut.data(), cut.size(), &s);

    CHECK(psp::normalize_disc_id("ULUS10041") == "ULUS10041" && psp::normalize_disc_id("ulus-10041") == "ULUS10041" && psp::normalize_disc_id("NPUZ_00001") == "NPUZ00001");
    CHECK(psp::normalize_disc_id("ULUS1004").empty() && psp::normalize_disc_id("ULUS100412").empty() && psp::normalize_disc_id("").empty() && psp::normalize_disc_id("12345ABCD").empty());
    CHECK(psp::key_for("ULUS10041", "x") == "psp:ulus10041" && psp::key_for("", "my_game") == "psp:file-my_game");
    CHECK(psp::disc_id_from_key("psp:ulus10041") == "ULUS10041" && psp::disc_id_from_key("psp:file-my_game").empty() && psp::disc_id_from_key("ps2:slus-20946").empty());
    CHECK(psp::savedata_folder_matches("ULUS10041DATA00", "ULUS10041") && psp::savedata_folder_matches("ulus10041_SYSTEM", "ULUS10041"));
    CHECK(!psp::savedata_folder_matches("ULUS10042DATA00", "ULUS10041") && !psp::savedata_folder_matches("ULUS1004", "ULUS10041") && !psp::savedata_folder_matches("ULUS10041", ""));
    CHECK(std::string(system_short(System::PSP)) == "psp" && system_from_short("psp") == System::PSP && std::string(system_name(System::PSP)) == "PlayStation Portable");
}

static void test_images(const fs::path& d) {
    Bytes iso = make_psp_iso(umd_sfo("ULUS10041", "Crisis Core"));
    write_file(d / "crisis.iso", iso);
    psp::Ident id;
    CHECK(psp::identify_file(U8(d / "crisis.iso"), &id) && id.disc_id == "ULUS10041" && id.title == "Crisis Core" && id.category == "UG" && !id.ps1);

    write_file(d / "crisis.cso", make_cso(iso));
    id = psp::Ident();
    CHECK(psp::identify_file(U8(d / "crisis.cso"), &id) && id.disc_id == "ULUS10041");
    {   // the CSO reader over an in-memory prefix (archive members)
        auto head = std::make_shared<std::string>((const char*)make_cso(iso).data(), make_cso(iso).size());
        id = psp::Ident();
        CHECK(psp::identify_buffer(head, &id) && id.disc_id == "ULUS10041");
        auto cut = std::make_shared<std::string>(head->substr(0, 24 + 4 * 17));   // block data missing: no crash, no match
        CHECK(!psp::identify_buffer(cut, &id));
    }
    write_file(d / "dvd.chd", make_chd(iso, false));
    write_file(d / "cd.chd", make_chd(iso, true));
    id = psp::Ident();
    CHECK(psp::identify_file(U8(d / "dvd.chd"), &id) && id.disc_id == "ULUS10041");
    id = psp::Ident();
    CHECK(psp::identify_file(U8(d / "cd.chd"), &id) && id.title == "Crisis Core");
    {   // an ISO prefix held in memory
        auto buf = std::make_shared<std::string>((const char*)iso.data(), iso.size());
        id = psp::Ident();
        CHECK(psp::identify_buffer(buf, &id) && id.disc_id == "ULUS10041");
    }
    // homebrew / no DISC_ID: still a PSP game, keyed by file name
    write_file(d / "homebrew.iso", make_psp_iso(make_sfo({{"CATEGORY", "MG"}, {"TITLE", "My Homebrew"}})));
    id = psp::Ident();
    CHECK(psp::identify_file(U8(d / "homebrew.iso"), &id) && id.disc_id.empty() && id.title == "My Homebrew");

    // not PSP: a PS1 disc, a PS2 disc, a data disc without PSP_GAME, junk
    write_file(d / "psx.iso", make_cnf_iso("BOOT = cdrom:\\SCUS_941.63;1\r\n"));
    write_file(d / "ps2.iso", make_cnf_iso("BOOT2 = cdrom0:\\SLUS_209.46;1\r\n"));
    write_file(d / "data.iso", make_psp_iso(Bytes(), false));
    write_file(d / "junk.iso", Bytes(100000, 7));
    CHECK(!psp::identify_file(U8(d / "psx.iso"), &id) && !psp::identify_file(U8(d / "ps2.iso"), &id));
    CHECK(!psp::identify_file(U8(d / "data.iso"), &id) && !psp::identify_file(U8(d / "junk.iso"), &id));
    write_file(d / "short.cso", Bytes(10, 1));
    CHECK(!psp::identify_file(U8(d / "short.cso"), &id));
    CHECK(!psp::identify_file(U8(d / "nothing.iso"), &id));

    // PBP: PSP (UMD category, digital category, homebrew) vs a PS1 eboot
    write_file(d / "psn.pbp", make_pbp(umd_sfo("NPUZ00001", "Echochrome", "EG"), "NPUMDIMG"));
    id = psp::Ident();
    CHECK(psp::identify_file(U8(d / "psn.pbp"), &id) && id.disc_id == "NPUZ00001" && id.category == "EG" && !id.ps1);
    write_file(d / "hb.pbp", make_pbp(make_sfo({{"CATEGORY", "MG"}, {"TITLE", "Cave Story"}}), ""));
    id = psp::Ident();
    CHECK(psp::identify_file(U8(d / "hb.pbp"), &id) && id.disc_id.empty() && id.title == "Cave Story");
    write_file(d / "ps1_me.pbp", make_pbp(umd_sfo("SLUS00594", "Metal Gear", "ME"), "PSISOIMG0000"));
    id = psp::Ident();
    CHECK(!psp::identify_file(U8(d / "ps1_me.pbp"), &id) && id.ps1);
    write_file(d / "ps1_psar.pbp", make_pbp(make_sfo({{"CATEGORY", "EG"}, {"DISC_ID", "SCUS94163"}, {"TITLE", "Crash"}}), "PSISOIMG0000"));   // category lies, DATA.PSAR does not
    id = psp::Ident();
    CHECK(!psp::identify_file(U8(d / "ps1_psar.pbp"), &id) && id.ps1);
    write_file(d / "multi.pbp", make_pbp(make_sfo({{"CATEGORY", "EG"}, {"TITLE", "Final Fantasy VII"}}), "PSTITLEIMG00000"));
    CHECK(!psp::identify_file(U8(d / "multi.pbp"), &id) && id.ps1);
    write_file(d / "trunc.pbp", Bytes(20, 0));
    CHECK(!psp::identify_file(U8(d / "trunc.pbp"), &id));
}

static void test_scan(const fs::path& d) {
    fs::path games = d / "Games";
    write_file(games / "Crisis Core.iso", make_psp_iso(umd_sfo("ULUS10041", "Crisis Core -Final Fantasy VII-")));
    write_file(games / "sub" / "God of War.cso", make_cso(make_psp_iso(umd_sfo("UCUS98632", "God of War: Chains of Olympus"))));
    write_file(games / "Persona.chd", make_chd(make_psp_iso(umd_sfo("ULUS10480", "Persona 3 Portable")), false));
    write_file(games / "Echochrome" / "EBOOT.PBP", make_pbp(umd_sfo("NPUZ00001", "Echochrome", "EG"), "NPUMDIMG"));   // not a listed extension... .pbp is
    write_file(games / "PS1 Game.pbp", make_pbp(umd_sfo("SLUS00594", "Metal Gear Solid", "ME"), "PSISOIMG0000"));
    write_file(games / "psx.iso", make_cnf_iso("BOOT = cdrom:\\SCUS_941.63;1\r\n"));
    write_file(games / "ps2.iso", make_cnf_iso("BOOT2 = cdrom0:\\SLUS_209.46;1\r\n"));
    {   // a zip holding a PSP iso: listed, unpacked on Play
        mz_zip_archive za;
        std::memset(&za, 0, sizeof za);
        fs::create_directories(games);
        std::string zp = U8(games / "Zipped Game.zip");
        CHECK(mz_zip_writer_init_file(&za, zp.c_str(), 0));
        Bytes zi = make_psp_iso(umd_sfo("ULES00262", "Zipped PSP Game"));
        CHECK(mz_zip_writer_add_mem(&za, "Zipped Game.iso", zi.data(), zi.size(), MZ_BEST_SPEED));
        mz_zip_writer_finalize_archive(&za);
        mz_zip_writer_end(&za);
    }
    library_set_data_dir(U8(d / "userdata"));
    auto lib = scan_library({U8(games)}, nullptr);
    bool cc = false, gow = false, p3 = false, echo = false, zipped = false, ps1pbp = false, psx = false, ps2 = false;
    int psps = 0;
    for (const auto& g : lib) {
        if (g.system == System::PSP) {
            ++psps;
            if (g.key == "psp:ulus10041" && g.title == "Crisis Core -Final Fantasy VII-" && g.serial == "ULUS10041") cc = true;
            if (g.key == "psp:ucus98632" && g.title == "God of War: Chains of Olympus") gow = true;
            if (g.key == "psp:ulus10480") p3 = true;
            if (g.key == "psp:npuz00001") echo = true;
            if (g.key == "psp:ules00262" && g.issue == "extract") zipped = true;
        } else {
            if (g.path.find("PS1 Game.pbp") != std::string::npos && g.system == System::PS1) ps1pbp = true;
            if (g.path.find("psx.iso") != std::string::npos && g.system == System::PS1) psx = true;
            if (g.path.find("ps2.iso") != std::string::npos && g.system == System::PS2) ps2 = true;
        }
    }
    std::printf("flags cc=%d gow=%d p3=%d echo=%d zipped=%d ps1pbp=%d psx=%d ps2=%d psps=%d\n", cc, gow, p3, echo, zipped, ps1pbp, psx, ps2, psps);
    CHECK(cc && gow && p3 && echo && zipped);
    CHECK(ps1pbp && psx && ps2);   // a PS1 eboot .pbp, a PS1 disc and a PS2 disc keep their consoles
    CHECK(psps == 5);
    library_set_data_dir("");
}

static void test_emulator_and_saves(const fs::path& d) {
    using namespace hubemu;
    const Release& r = release(Kind::Ppsspp);
    CHECK(std::string(r.version) == "1.20.4" && std::wstring(r.host) == L"www.ppsspp.org" && std::wstring(r.path) == L"/files/1_20_4/ppsspp_win.zip");
    CHECK(std::string(r.sha256) == "a60f04ebdb0b5f1655422bd7f88349a46999b17ad5115d6ddb290c3934bd5163" && r.size == 27658759ull && std::wstring(r.exe) == L"PPSSPPWindows64.exe");
    CHECK(kind_for(System::PSP) == Kind::Ppsspp && std::string(name(Kind::Ppsspp)) == "PPSSPP");
    CHECK(fs::path(managed_dir(Kind::Ppsspp, U8(d / "root"))).filename() == "ppsspp");
    CHECK(find(Kind::Ppsspp, "", "", U8(d / "none")).exe.empty());   // never reuses a user's own PPSSPP

    // SAVEDATA <-> vault: <install>/memstick/PSP/SAVEDATA/<id>* <-> <vault>/psp/<key>/battery/SAVEDATA/<id>*
    fs::path inst = d / "inst", vault = d / "vault";
    write_text(inst / "PPSSPPWindows64.exe", "x");
    Install in; in.exe = U8(inst / "PPSSPPWindows64.exe"); in.dir = U8(inst); in.managed = true;
    GameEntry e; e.system = System::PSP; e.key = "psp:ulus10041";
    fs::path bat = vault / "psp" / "psp_ulus10041" / "battery";
    write_text(bat / "SAVEDATA" / "ULUS10041DATA00" / "DATA.BIN", "VAULTSAVE");
    write_text(bat / "SAVEDATA" / "ULUS10041_SYSTEM" / "SYS.BIN", "VAULTSYS");
    write_text(inst / "memstick" / "PSP" / "SAVEDATA" / "ULUS10042DATA00" / "OTHER.BIN", "OTHERGAME");   // another game's save: never touched
    SaveSession s = begin_session(e, in, U8(bat), "");
    CHECK(s.active && s.psp_disc_id == "ULUS10041" && s.rels.size() == 2);
    CHECK(read_text(inst / "memstick" / "PSP" / "SAVEDATA" / "ULUS10041DATA00" / "DATA.BIN") == "VAULTSAVE");
    CHECK(!fs::exists(inst / "memstick" / "PSP" / "SAVEDATA" / "ULUS10041DATA01"));
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));
    write_text(inst / "memstick" / "PSP" / "SAVEDATA" / "ULUS10041DATA00" / "DATA.BIN", "NEWSAVE");
    write_text(inst / "memstick" / "PSP" / "SAVEDATA" / "ULUS10041DATA01" / "NEW.BIN", "CREATED IN SESSION");   // a folder the game made
    end_session(s);
    CHECK(read_text(bat / "SAVEDATA" / "ULUS10041DATA00" / "DATA.BIN") == "NEWSAVE");
    CHECK(read_text(bat / "SAVEDATA" / "ULUS10041DATA01" / "NEW.BIN") == "CREATED IN SESSION");
    CHECK(!fs::exists(bat / "SAVEDATA" / "ULUS10042DATA00"));
    GameEntry hb; hb.system = System::PSP; hb.key = "psp:file-homebrew";   // no disc id: nothing to map
    CHECK(!begin_session(hb, in, U8(vault / "psp" / "hb" / "battery"), "").active);
}

static void test_config(const fs::path& d) {
    using namespace hubemu;
    hub::gs::Values v = hub::gs::defaults_for(System::PSP);
    CHECK(v["fullscreen"] == "true" && v["psp.res"] == "2");
    std::string ini = ppsspp_ini_defaults("");
    CHECK(ini.find("FirstRun = False") != std::string::npos && ini.find("CheckForNewVersion = False") != std::string::npos);
    CHECK(ini.find("AskForExitConfirmationAfterSeconds = 0") != std::string::npos);
    std::string l = ppsspp_ini_launch(ini, v);
    CHECK(l.find("FullScreen = True") != std::string::npos && l.find("InternalResolution = 2") != std::string::npos);
    v["fullscreen"] = "false"; v["psp.res"] = "4";
    l = ppsspp_ini_launch(l, v);
    CHECK(l.find("FullScreen = False") != std::string::npos && l.find("InternalResolution = 4") != std::string::npos && l.find("FullScreen = True") == std::string::npos);
    CHECK(l.find("FirstRun = False") != std::string::npos);   // a later write keeps the rest

    CHECK(ppsspp_args("C:\\g\\a b.iso", true) == "--fullscreen \"C:\\g\\a b.iso\"" && ppsspp_args("x.iso", false) == "--windowed \"x.iso\"");
    CHECK(ppsspp_state_keys(true) == std::vector<int>{VK_F2} && ppsspp_state_keys(false) == std::vector<int>{VK_F4});

    hubctl::Profile p = hubctl::default_profile("psp");
    CHECK(p.system == "psp" && hubctl::is_system("psp") && p.name == "default-psp");
    CHECK(hubctl::system_key(System::PSP) == "psp");
    std::string c = ppsspp_controls_ini(p, 0);
    CHECK(c.find("[ControlMapping]") != std::string::npos);
    CHECK(c.find("Cross = 20-96,1-54") != std::string::npos);        // Xbox A on XInput pad 0, plus PPSSPP's own Z key
    CHECK(c.find("Circle = 20-97,") != std::string::npos && c.find("Square = 20-99,") != std::string::npos && c.find("Triangle = 20-100,") != std::string::npos);
    CHECK(c.find("L = 20-102,") != std::string::npos && c.find("R = 20-103,") != std::string::npos);
    CHECK(c.find("Start = 20-108,") != std::string::npos && c.find("Select = 20-109,") != std::string::npos);
    CHECK(c.find("Up = 20-19,") != std::string::npos && c.find("Right = 20-22,") != std::string::npos);
    CHECK(c.find("An.Left = 20-" + std::to_string(4000 + 0 * 2 + 1) + ",") != std::string::npos);        // left stick X, negative
    CHECK(c.find("An.Up = 20-" + std::to_string(4000 + 1 * 2) + ",") != std::string::npos);              // left stick Y, positive = up on XInput
    CHECK(c.find("Save State") == std::string::npos && c.find("Load State") == std::string::npos && c.find("Pause") == std::string::npos && c.find("Fast-forward") == std::string::npos);
    std::string c2 = ppsspp_controls_ini(p, 2);
    CHECK(c2.find("Cross = 22-96,") != std::string::npos);
    hubctl::Term t; t.axis = true; t.idx = 5; t.sign = 1;
    CHECK(ppsspp_binding(t, 1) == "21-" + std::to_string(4000 + 18 * 2));   // right trigger
    t.idx = 3; t.sign = -1;
    CHECK(ppsspp_binding(t, 0) == "20-" + std::to_string(4000 + 14 * 2 + 1));   // right stick Y, negative
    hubctl::Term b; b.axis = false; b.idx = 5;   // Guide: no equivalent
    CHECK(ppsspp_binding(b, 0).empty() && ppsspp_binding(b, 9).empty());

    fs::path inst = d / "inst";
    write_text(inst / "installed.txt", "stale");
    CHECK(prepare_ppsspp(U8(inst), p, 0, hub::gs::defaults_for(System::PSP)).empty());
    std::string sys = U8(inst / "memstick" / "PSP" / "SYSTEM");
    CHECK(!fs::exists(inst / "installed.txt"));   // would put the data in Documents
    CHECK(read_text(fs::path(sys) / "ppsspp.ini").find("FirstRun = False") != std::string::npos);
    CHECK(read_text(fs::path(sys) / "controls.ini").find("Cross = 20-96") != std::string::npos);
    write_text(fs::path(sys) / "ppsspp.ini", read_text(fs::path(sys) / "ppsspp.ini") + "\n[Custom]\nKeep = Yes\n");
    CHECK(prepare_ppsspp(U8(inst), p, 1, hub::gs::defaults_for(System::PSP)).empty());   // a second launch keeps the user's own lines
    CHECK(read_text(fs::path(sys) / "ppsspp.ini").find("Keep = Yes") != std::string::npos);
    CHECK(read_text(fs::path(sys) / "controls.ini").find("Cross = 21-96") != std::string::npos);
}

static int test_download(const std::string& root) {
    using namespace hubemu;
    std::string err;
    std::printf("downloading PPSSPP into %s ...\n", root.c_str());
    bool ok = install_blocking(Kind::Ppsspp, root, &err, [](int ph, uint64_t got, uint64_t total) {
        static int last = -1;
        int pct = total ? (int)(got * 100 / total) : 0;
        if (ph != last || pct / 25 != 0) { last = ph; }
    });
    if (!ok) { std::printf("download failed: %s\n", err.c_str()); return 1; }
    std::string dir = managed_dir(Kind::Ppsspp, root);
    CHECK(fs::exists(fs::path(dir) / "PPSSPPWindows64.exe") && fs::exists(fs::path(dir) / "retrohub-install.txt") && fs::exists(fs::path(dir) / "memstick"));
    CHECK(!fs::exists(fs::path(dir) / "installed.txt"));
    CHECK(find(Kind::Ppsspp, "", "", root).managed);
    std::printf("installed: %s\n", dir.c_str());
    return 0;
}

int main(int argc, char** argv) {
    fs::path d = fs::temp_directory_path() / ("hub_psp_test_" + std::to_string((unsigned long long)GetCurrentProcessId()));
    fs::create_directories(d);
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        std::printf("sfo+keys\n"); test_sfo_and_keys();
        std::printf("images\n"); test_images(d / "img");
        std::printf("scan\n"); test_scan(d / "scan");
        std::printf("emu+saves\n"); test_emulator_and_saves(d / "emu");
        std::printf("cfg\n"); test_config(d / "cfg");
    } catch (const std::exception& ex) { std::printf("EXCEPTION %s\n", ex.what()); return 2; }
    int dl = 0;
    if (argc >= 3 && std::string(argv[1]) == "--download") dl = test_download(argv[2]);
    std::error_code ec;
    fs::remove_all(d, ec);
    std::printf("hub_psp_test: %d check(s) failed%s\n", g_fail, dl ? ", download failed" : "");
    return (g_fail || dl) ? 1 : 0;
}
