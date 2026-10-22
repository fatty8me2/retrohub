// Console test for the 3DS (Azahar) / Wii U (Cemu) support: header parsers, library identification, save-folder
// mapping and the vault round trip, config generation, launch arguments.
// Usage: hub_emu_test                      offline unit checks (what ctest runs)
//        hub_emu_test --download <dir>     also download + verify + extract both emulators into <dir> (network, never launched)
#include "emu_formats.h"
#include "emu_saves.h"
#include "emulators.h"
#include "games.h"
#include "library.h"
#include "logic.h"
#include "miniz.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

static void put_le32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); }
static void put_le64(Bytes& b, size_t at, uint64_t v) { for (int i = 0; i < 8; ++i) b[at + i] = uint8_t(v >> (8 * i)); }
static void put_be32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (24 - 8 * i)); }
static void put_be64(Bytes& b, size_t at, uint64_t v) { for (int i = 0; i < 8; ++i) b[at + i] = uint8_t(v >> (56 - 8 * i)); }

static bool write_file(const fs::path& p, const Bytes& d) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(d.data()), (std::streamsize)d.size());
    return (bool)f;
}
static bool write_text(const fs::path& p, const std::string& t) { return write_file(p, Bytes(t.begin(), t.end())); }
static std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/* NCSD image with partition 0 at 0x4000: 0x4400 bytes. */
static Bytes make_ncsd(uint64_t title_id, const char* product, bool nocrypto) {
    Bytes b(0x4400, 0);
    std::memcpy(&b[0x100], "NCSD", 4);
    put_le32(b, 0x120, 0x20);     // partition 0 offset (media units) = 0x4000
    put_le32(b, 0x124, 0x1000);
    size_t n = 0x4000;
    std::memcpy(&b[n + 0x100], "NCCH", 4);
    put_le64(b, n + 0x108, title_id);
    put_le64(b, n + 0x118, title_id);
    std::memcpy(&b[n + 0x150], product, std::strlen(product));
    b[n + 0x188 + 7] = nocrypto ? 0x04 : 0x00;
    return b;
}

static void test_3ds_header() {
    Bytes b = make_ncsd(0x0004000000055D00ull, "CTR-P-EKJA", true);
    N3dsInfo i;
    CHECK(parse_3ds_header(b.data(), b.size(), &i));
    CHECK(i.container == "ncsd" && i.serial == "EKJA" && i.product_code == "CTR-P-EKJA");
    CHECK(i.title_id == 0x0004000000055D00ull && i.decrypted && i.is_application);
    CHECK(n3ds_key(i) == "3ds:0004000000055d00");

    Bytes enc = make_ncsd(0x0004000000055D00ull, "CTR-P-EKJA", false);
    CHECK(parse_3ds_header(enc.data(), enc.size(), &i) && !i.decrypted);

    /* truncated: partition 0 lies beyond the buffer */
    CHECK(!parse_3ds_header(b.data(), 0x300, &i));
    /* update title (0004000E) is not an application */
    Bytes upd = make_ncsd(0x0004000E00055D00ull, "CTR-U-EKJA", true);
    CHECK(parse_3ds_header(upd.data(), upd.size(), &i) && !i.is_application);
    /* garbage */
    Bytes junk(0x4400, 0x5A);
    CHECK(!parse_3ds_header(junk.data(), junk.size(), &i));

    /* bare NCCH (.cxi) */
    Bytes cxi(0x400, 0);
    std::memcpy(&cxi[0x100], "NCCH", 4);
    put_le64(cxi, 0x118, 0x0004000000164800ull);
    std::memcpy(&cxi[0x150], "CTR-P-BNDA", 10);
    cxi[0x188 + 7] = 0x04;
    CHECK(parse_3ds_header(cxi.data(), cxi.size(), &i) && i.container == "ncch" && i.serial == "BNDA" && i.decrypted);

    /* 3DSX homebrew */
    Bytes hb(0x40, 0);
    std::memcpy(hb.data(), "3DSX", 4);
    CHECK(parse_3ds_header(hb.data(), hb.size(), &i) && i.container == "3dsx" && n3ds_key(i).empty());

    /* CIA: header 0x2020, cert 0x40, ticket 0x40, TMD (RSA-2048 sig) with the title id at hdr+0x4C */
    Bytes cia(0x2040 + 0x40 + 0x40 + 0x400, 0);
    put_le32(cia, 0, 0x2020);
    put_le32(cia, 8, 0x40);
    put_le32(cia, 0x0C, 0x40);
    put_le32(cia, 0x10, 0x300);
    size_t tmd = 0x2040 + 0x40 + 0x40;   // the header block (0x2020) is padded to 0x2040
    put_be32(cia, tmd, 0x010004);
    size_t hdr = tmd + 4 + 0x100 + 0x3C;
    put_be64(cia, hdr + 0x4C, 0x00040000001B5100ull);
    CHECK(parse_3ds_header(cia.data(), cia.size(), &i));
    CHECK(i.container == "cia" && i.title_id == 0x00040000001B5100ull && i.serial == "001B5100" && i.is_application);

    CHECK(is_3ds_extension(".3ds") && is_3ds_extension(".zcia") && !is_3ds_extension(".zip") && !is_3ds_extension(".gba"));
    CHECK(is_3ds_zip_entry(".cci") && !is_3ds_zip_entry(".zcci"));
}

/* ZArchive (.wua) with two title folders at the root. */
static Bytes make_wua(bool base_first) {
    std::string a = "000500001019e500_v81", c = "0005000e1019e500_v81";
    if (!base_first) std::swap(a, c);
    Bytes names;
    names.push_back((uint8_t)a.size());
    names.insert(names.end(), a.begin(), a.end());
    size_t second = names.size();
    names.push_back((uint8_t)c.size());
    names.insert(names.end(), c.begin(), c.end());
    Bytes tree(48, 0);
    put_be32(tree, 0, 0x7FFFFFFFu); put_be32(tree, 4, 1); put_be32(tree, 8, 2);          // root dir -> nodes 1..2
    put_be32(tree, 16, 0); put_be32(tree, 20, 3); put_be32(tree, 24, 0);                  // dir A
    put_be32(tree, 32, (uint32_t)second); put_be32(tree, 36, 3); put_be32(tree, 40, 0);   // dir B
    Bytes f(1000, 0xAB);   // fake compressed data
    size_t names_off = f.size();
    f.insert(f.end(), names.begin(), names.end());
    size_t tree_off = f.size();
    f.insert(f.end(), tree.begin(), tree.end());
    size_t foot = f.size();
    f.resize(foot + 144, 0);
    uint64_t total = f.size();
    put_be64(f, foot + 0, 0);   put_be64(f, foot + 8, 1000);              // data
    put_be64(f, foot + 32, names_off); put_be64(f, foot + 40, names.size());
    put_be64(f, foot + 48, tree_off);  put_be64(f, foot + 56, tree.size());
    put_be64(f, foot + 128, total);
    put_be32(f, foot + 136, 0x61bf3a01u);
    put_be32(f, foot + 140, 0x169f52d6u);
    return f;
}

static ReadAtFn reader_for(const Bytes& b) {
    return [&b](uint64_t off, void* buf, size_t n) {
        if (off + n > b.size()) return false;
        std::memcpy(buf, b.data() + off, n);
        return true;
    };
}

static void test_wiiu_formats() {
    for (bool base_first : {true, false}) {
        Bytes w = make_wua(base_first);
        WiiuInfo i;
        CHECK(parse_wua(reader_for(w), w.size(), &i));
        CHECK(i.title_id == 0x000500001019e500ull && i.serial == "000500001019E500" && i.container == "wua");   // base game wins over the update
        CHECK(wiiu_key(i) == "wiiu:000500001019e500");
    }
    Bytes w = make_wua(true);
    WiiuInfo i;
    w[w.size() - 1] ^= 0xFF;   // bad magic
    CHECK(!parse_wua(reader_for(w), w.size(), &i));
    Bytes tiny(10, 0);
    CHECK(!parse_wua(reader_for(tiny), tiny.size(), &i));

    uint64_t t = 0;
    CHECK(parse_meta_xml_title_id("<?xml?><menu><title_id type=\"hexBinary\" length=\"8\">0005000010145000</title_id></menu>", &t) &&
          t == 0x0005000010145000ull);
    CHECK(!parse_meta_xml_title_id("<title_id>123</title_id>", &t));
    CHECK(!parse_meta_xml_title_id("nothing", &t));

    Bytes wud(32, 0);
    std::memcpy(wud.data(), "WUP-P-ALZE", 10);
    CHECK(parse_wud_header(wud.data(), wud.size(), &i) && i.serial == "ALZE" && i.container == "wud" && wiiu_key(i) == "wiiu:wup-p-alze");
    Bytes nowud(32, 'x');
    CHECK(!parse_wud_header(nowud.data(), nowud.size(), &i));

    /* wux: header, one-sector index table, sector 0 */
    Bytes wux(0x20 + 0x8000 * 2, 0);
    std::memcpy(wux.data(), "WUX0", 4);
    put_le32(wux, 4, 0x1099d02e);
    put_le32(wux, 8, 0x8000);
    put_le64(wux, 12, 0x8000);   // one sector uncompressed
    put_le32(wux, 0x20, 0);      // sector 0 -> stored sector 0
    std::memcpy(&wux[0x8000], "WUP-P-AXZE", 10);   // data starts at the first sector-aligned offset after the table
    CHECK(parse_wux(reader_for(wux), wux.size(), &i) && i.serial == "AXZE" && i.container == "wux");
}

static void test_catalog() {
    const GameInfo* g = find_game_for("3ds", "EKJA", "");
    CHECK(g && g->id == "pokex" && g->engine == Engine::Azahar && g->max_players == 1 && !g->online && g->genre == "RPG");
    g = find_game_for("3ds", "a2ba", "");
    CHECK(g && g->id == "pokeumoon");
    CHECK(find_game_for("3ds", "ECRA", "") && find_game_for("3ds", "ECLA", "") && find_game_for("3ds", "BNDA", "") &&
          find_game_for("3ds", "BNEA", "") && find_game_for("3ds", "EK2A", "") && find_game_for("3ds", "A2AA", ""));
    CHECK(find_game_for("gba", "EKJA", "") == nullptr);   // system scoped
    g = find_game_for("wiiu", "000500001019E500", "");
    CHECK(g && g->id == "tphd" && g->engine == Engine::Cemu && g->year == 2016 && g->genre == "Adventure");
    CHECK(system_from_short("3ds") == System::N3DS && system_from_short("wiiu") == System::WiiU);
    CHECK(std::string(system_short(System::N3DS)) == "3ds" && std::string(system_name(System::WiiU)) == "Wii U");
}

static void test_library_ids(const fs::path& dir) {
    fs::path games = dir / "games";
    write_file(games / "Pokemon X (USA).3ds", make_ncsd(0x0004000000055D00ull, "CTR-P-EKJA", true));
    write_file(games / "Pokemon Y (USA).cci", make_ncsd(0x0004000000055E00ull, "CTR-P-EK2A", false));   // encrypted
    write_file(games / "Update.3ds", make_ncsd(0x0004000E00055D00ull, "CTR-U-EKJA", true));
    write_file(games / "Zelda.wua", make_wua(true));
    /* loadiine folder */
    write_text(games / "Some Game" / "meta" / "meta.xml",
               "<menu><title_id type=\"hexBinary\" length=\"8\">0005000010145000</title_id></menu>");
    write_file(games / "Some Game" / "code" / "game.rpx", Bytes(64, 1));
    write_file(games / "Homebrew" / "code" / "app.rpx", Bytes(64, 1));   // no meta.xml: not listed
    /* zipped 3DS image */
    {
        Bytes img = make_ncsd(0x0004000000175E00ull, "CTR-P-BNEA", true);
        img.resize(0x20000, 0);
        fs::path z = games / "Pokemon Moon (USA).zip";
        fs::create_directories(z.parent_path());
        mz_zip_archive za;
        std::memset(&za, 0, sizeof za);
        CHECK(mz_zip_writer_init_file(&za, z.string().c_str(), 0));
        CHECK(mz_zip_writer_add_mem(&za, "Pokemon Moon (USA).3ds", img.data(), img.size(), MZ_DEFAULT_COMPRESSION));
        CHECK(mz_zip_writer_finalize_archive(&za));
        mz_zip_writer_end(&za);
    }
    library_set_data_dir((dir / "data").string());
    auto lib = scan_library({games.string()}, nullptr);
    auto find = [&](const std::string& key) -> const GameEntry* {
        for (auto& e : lib) if (e.key == key) return &e;
        return nullptr;
    };
    const GameEntry* x = find("3ds:0004000000055d00");
    CHECK(x && x->system == System::N3DS && x->serial == "EKJA" && x->catalog_id == "pokex" && x->issue.empty() && x->title == "Pokemon X");
    const GameEntry* y = find("3ds:0004000000055e00");
    CHECK(y && y->issue == "encrypted" && y->catalog_id == "pokey");
    CHECK(find("3ds:0004000e00055d00") == nullptr);   // update title: not a game
    const GameEntry* moon = find("3ds:0004000000175e00");
    CHECK(moon && moon->issue == "extract" && moon->path.size() > 4 && moon->path.substr(moon->path.size() - 4) == ".zip");
    CHECK(!fs::exists(dir / "data" / "rom-cache"));   // the zip was only read, nothing extracted
    if (moon) {   // explicit, player-confirmed unzip
        CHECK(extracted_3ds_path(*moon).empty());
        std::string out, err;
        uint64_t last = 0;
        CHECK(!extract_3ds_zip(*moon, [](uint64_t, uint64_t) { return false; }, &out, &err) && err == "cancelled" && extracted_3ds_path(*moon).empty());
        CHECK(extract_3ds_zip(*moon, [&](uint64_t done, uint64_t) { last = done; return true; }, &out, &err));
        CHECK(last == 0x20000 && fs::file_size(fs::u8path(out)) == 0x20000 && extracted_3ds_path(*moon) == out);
        GameEntry back;
        CHECK(identify_game(out, &back) && back.key == moon->key && back.issue.empty());   // the unpacked image is a normal .3ds
        std::error_code ec;
        fs::remove_all(fs::u8path(extract_3ds_dir(*moon)), ec);
    }
    const GameEntry* z = find("wiiu:000500001019e500");
    CHECK(z && z->system == System::WiiU && z->serial == "000500001019E500" && z->catalog_id == "tphd" && z->issue.empty());
    const GameEntry* rpx = find("wiiu:0005000010145000");
    CHECK(rpx && rpx->title == "Some Game");
    int wiiu = 0;
    for (auto& e : lib) if (e.system == System::WiiU) ++wiiu;
    CHECK(wiiu == 2);   // wua + the loadiine folder; the rpx without meta.xml is skipped
    library_set_data_dir("");
}

static void test_save_paths() {
    CHECK(n3ds_save_rel(0x0004000000055D00ull) == "title/00040000/00055d00/data");
    CHECK(n3ds_extdata_rel(0x0004000000055D00ull) == "extdata/00000000/00055d00");
    CHECK(wiiu_save_rel(0x000500001019E500ull) == "usr/save/00050000/1019e500");
    CHECK(hubemu::title_id_from_key("3ds:0004000000055d00") == 0x0004000000055D00ull);
    CHECK(hubemu::title_id_from_key("wiiu:000500001019e500") == 0x000500001019E500ull);
    CHECK(hubemu::title_id_from_key("3ds:file-homebrew") == 0);
    CHECK(hubemu::title_id_from_key("wiiu:wup-p-alze") == 0);

    /* Cemu data-dir rules, with a fake file system */
    std::set<std::string> dirs, files;
    std::map<std::string, std::string> text;
    FsProbe p;
    p.dir_exists = [&](const std::string& s) { return dirs.count(s) > 0; };
    p.file_exists = [&](const std::string& s) { return files.count(s) > 0; };
    p.read_text = [&](const std::string& s) { auto it = text.find(s); return it == text.end() ? std::string() : it->second; };
    CemuDirs d = cemu_dirs_for("C:\\Cemu_2.6", "C:\\Users\\me\\AppData\\Roaming", p);
    CHECK(!d.portable && d.user_data == "C:\\Users\\me\\AppData\\Roaming\\Cemu" && d.mlc == "C:\\Users\\me\\AppData\\Roaming\\Cemu\\mlc01");
    files.insert("C:\\Cemu_2.6\\settings.xml");
    d = cemu_dirs_for("C:\\Cemu_2.6\\", "C:\\AppData", p);
    CHECK(d.portable && d.user_data == "C:\\Cemu_2.6" && d.mlc == "C:\\Cemu_2.6\\mlc01");
    dirs.insert("C:\\Cemu_2.6\\portable");
    d = cemu_dirs_for("C:\\Cemu_2.6", "C:\\AppData", p);
    CHECK(d.portable && d.user_data == "C:\\Cemu_2.6\\portable");
    text["C:\\Cemu_2.6\\portable\\settings.xml"] = "<content><mlc_path>D:\\Wiiu\\mlc</mlc_path></content>";
    d = cemu_dirs_for("C:\\Cemu_2.6", "C:\\AppData", p);
    CHECK(d.mlc == "D:\\Wiiu\\mlc");
    CHECK(cemu_settings_mlc_path("<mlc_path></mlc_path>").empty() && cemu_settings_mlc_path("<mlc_path/>").empty());
}

static void set_mtime(const fs::path& p, int seconds_ago) {
    fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::seconds(seconds_ago));
}

static void test_tree_sync(const fs::path& dir) {
    fs::path a = dir / "a", b = dir / "b", bk = dir / "bk";
    write_text(a / "x" / "one.sav", "AAAA");
    write_text(a / "two.sav", "BBBB");
    TreeSyncReport r = sync_tree_newest(a.string(), b.string(), bk.string(), "t1");
    CHECK(r.ok && r.copied == 2 && read_text(b / "x" / "one.sav") == "AAAA");
    r = sync_tree_newest(a.string(), b.string(), bk.string(), "t1");
    CHECK(r.copied == 0 && r.identical == 2);   // idempotent

    /* source newer: wins, old destination file kept as a backup */
    write_text(a / "two.sav", "NEW!");
    set_mtime(a / "two.sav", 0);
    set_mtime(b / "two.sav", 100);
    r = sync_tree_newest(a.string(), b.string(), bk.string(), "t2");
    CHECK(r.copied == 1 && r.backed_up == 1 && read_text(b / "two.sav") == "NEW!" && read_text(bk / "two.sav.t2") == "BBBB");

    /* destination newer: left alone */
    write_text(a / "x" / "one.sav", "OLD?");
    set_mtime(a / "x" / "one.sav", 500);
    r = sync_tree_newest(a.string(), b.string(), bk.string(), "t3");
    CHECK(r.kept == 1 && r.copied == 0 && read_text(b / "x" / "one.sav") == "AAAA");

    /* missing source is fine; nothing is ever deleted */
    r = sync_tree_newest((dir / "nope").string(), b.string(), "", "t");
    CHECK(r.ok && r.copied == 0);
    CHECK(tree_has_files(b.string()) && !tree_has_files((dir / "empty").string()));
    fs::create_directories(dir / "empty" / "sub");
    CHECK(!tree_has_files((dir / "empty").string()));
}

static void test_sessions(const fs::path& dir) {
    /* ---- 3DS: vault <-> <install>\user\sdmc\Nintendo 3DS\<id0>\<id1> ---- */
    fs::path inst3 = dir / "azahar";
    fs::path vault = dir / "vault";
    fs::path battery = vault / "3ds" / "3ds-0004000000055d00" / "battery";
    fs::create_directories(inst3);
    hubemu::Install ai;
    ai.dir = inst3.string();
    ai.exe = (inst3 / "azahar.exe").string();
    ai.managed = true;
    GameEntry e;
    e.system = System::N3DS;
    e.key = "3ds:0004000000055d00";
    std::string zeros(32, '0');
    fs::path sd = inst3 / "user" / "sdmc" / "Nintendo 3DS" / zeros / zeros;
    CHECK(n3ds_sd_root((inst3 / "user").string()) == sd.string());   // not created yet: the all-zero ids

    /* PC 1 played: save exists only in the emulator */
    write_text(sd / "title" / "00040000" / "00055d00" / "data" / "00000001.sav", "SAVE-PC1");
    hubemu::SaveSession s = hubemu::begin_session(e, ai, battery.string(), vault.string());
    CHECK(s.active && s.rels.size() == 2);
    hubemu::end_session(s);
    CHECK(read_text(battery / "title" / "00040000" / "00055d00" / "data" / "00000001.sav") == "SAVE-PC1");

    /* PC 2: fresh emulator dir, the vault copy is restored before launch */
    fs::remove_all(inst3 / "user");
    s = hubemu::begin_session(e, ai, battery.string(), vault.string());
    CHECK(read_text(sd / "title" / "00040000" / "00055d00" / "data" / "00000001.sav") == "SAVE-PC1");
    /* the game updates its save; after the session the vault follows */
    write_text(sd / "title" / "00040000" / "00055d00" / "data" / "00000001.sav", "SAVE-PC2");
    set_mtime(sd / "title" / "00040000" / "00055d00" / "data" / "00000001.sav", 0);
    set_mtime(battery / "title" / "00040000" / "00055d00" / "data" / "00000001.sav", 60);
    hubemu::end_session(s);
    CHECK(read_text(battery / "title" / "00040000" / "00055d00" / "data" / "00000001.sav") == "SAVE-PC2");
    CHECK(fs::exists(vault / "_conflicts" / "3ds" / "3ds-0004000000055d00" / "battery" / "title" / "00040000" / "00055d00" / "data"));   // old copy kept
    /* homebrew has no title id: no mapping */
    GameEntry hb;
    hb.system = System::N3DS;
    hb.key = "3ds:file-homebrew";
    CHECK(!hubemu::begin_session(hb, ai, battery.string(), vault.string()).active);

    /* ---- Wii U managed: <install>\portable\mlc01\usr\save\00050000\<lo> ---- */
    fs::path instc = dir / "cemu";
    fs::create_directories(instc);
    hubemu::Install ci;
    ci.dir = instc.string();
    ci.exe = (instc / "Cemu.exe").string();
    ci.managed = true;
    GameEntry w;
    w.system = System::WiiU;
    w.key = "wiiu:000500001019e500";
    fs::path vb = vault / "wiiu" / "wiiu-000500001019e500" / "battery";
    fs::path save = instc / "portable" / "mlc01" / "usr" / "save" / "00050000" / "1019e500" / "user" / "80000001" / "slot.sav";
    write_text(save, "ZELDA1");
    s = hubemu::begin_session(w, ci, vb.string(), vault.string());
    CHECK(s.active && s.rels.size() == 1 && s.rels[0] == "usr/save/00050000/1019e500");
    hubemu::end_session(s);
    CHECK(read_text(vb / "usr" / "save" / "00050000" / "1019e500" / "user" / "80000001" / "slot.sav") == "ZELDA1");

    /* ---- Wii U in the user's own install: restore only into an empty save folder, never overwrite ---- */
    fs::path user_inst = dir / "usercemu";
    fs::create_directories(user_inst / "portable");   // portable user data inside their install
    hubemu::Install ui;
    ui.dir = user_inst.string();
    ui.exe = (user_inst / "Cemu.exe").string();
    ui.managed = false;
    fs::path usave = user_inst / "portable" / "mlc01" / "usr" / "save" / "00050000" / "1019e500" / "user" / "80000001" / "slot.sav";
    s = hubemu::begin_session(w, ui, vb.string(), vault.string());   // empty: restored from the vault
    CHECK(s.restore_only_if_missing && read_text(usave) == "ZELDA1");
    write_text(usave, "THEIR-OWN");   // now "their" newer progress exists
    set_mtime(vb / "usr" / "save" / "00050000" / "1019e500" / "user" / "80000001" / "slot.sav", 300);
    hubemu::end_session(s);
    CHECK(read_text(usave) == "THEIR-OWN");   // their save untouched
    CHECK(read_text(vb / "usr" / "save" / "00050000" / "1019e500" / "user" / "80000001" / "slot.sav") == "THEIR-OWN");   // backed up into the vault
    s = hubemu::begin_session(w, ui, vb.string(), vault.string());
    write_text(vb / "usr" / "save" / "00050000" / "1019e500" / "user" / "80000001" / "slot.sav", "VAULT-DIFFERENT");
    CHECK(read_text(usave) == "THEIR-OWN");   // a non-empty emulator folder is never overwritten before launch
    hubemu::end_session(s);

    /* ---- wud/wux: no readable title id; the changed folder during the session is picked up ---- */
    GameEntry wud;
    wud.system = System::WiiU;
    wud.key = "wiiu:wup-p-alze";
    fs::path vb2 = vault / "wiiu" / "wiiu-wup-p-alze" / "battery";
    set_mtime(save, 500);   // the other title (played earlier) must not look like it changed during this session
    s = hubemu::begin_session(wud, ci, vb2.string(), vault.string());
    CHECK(s.active && s.wiiu_unknown_title && s.rels.empty());
    write_text(instc / "portable" / "mlc01" / "usr" / "save" / "00050000" / "10aa0000" / "user" / "s.sav", "NEWGAME");
    hubemu::end_session(s);
    CHECK(read_text(vb2 / "usr" / "save" / "00050000" / "10aa0000" / "user" / "s.sav") == "NEWGAME");
    s = hubemu::begin_session(wud, ci, vb2.string(), vault.string());
    CHECK(s.rels.size() == 1 && s.rels[0] == "usr/save/00050000/10aa0000");   // next session knows the folder
}

static void test_config() {
    std::string ini = hubemu::azahar_config_patch("[Renderer]\nresolution_factor = 2\n");
    CHECK(ini.find("resolution_factor = 2") != std::string::npos);   // existing content kept
    CHECK(ini.find("[Controls]") != std::string::npos);
    CHECK(ini.find("button_a\\default = false") != std::string::npos);
    CHECK(ini.find("button_a = \"engine:sdl,api:controller,button:1,maptype:all,port:0\"") != std::string::npos);
    CHECK(ini.find("circle_pad = \"engine:sdl,api:controller,axis_x:0,axis_y:1") != std::string::npos);
    CHECK(ini.find("confirmClose = false") != std::string::npos && ini.find("check_for_update_on_start = false") != std::string::npos);
    CHECK(hubemu::azahar_config_patch(ini) == ini);   // idempotent

    std::string st = hubemu::cemu_default_settings_xml();
    CHECK(st.find("<content>") != std::string::npos && st.find("<check_update>false</check_update>") != std::string::npos);
    std::string pad = hubemu::cemu_gamepad_profile_xml();
    CHECK(pad.find("<type>Wii U GamePad</type>") != std::string::npos && pad.find("<api>XInput</api>") != std::string::npos);
    CHECK(pad.find("<uuid>0</uuid>") != std::string::npos);
    size_t entries = 0;
    for (size_t p = pad.find("<entry>"); p != std::string::npos; p = pad.find("<entry>", p + 1)) ++entries;
    CHECK(entries == 24);

    CHECK(hubemu::azahar_args("C:\\g\\x.3ds", true) == "-f \"C:\\g\\x.3ds\"");
    CHECK(hubemu::azahar_args("C:\\g\\x.3ds", false) == "-w \"C:\\g\\x.3ds\"");
    CHECK(hubemu::cemu_args("D:\\w\\z.wua", true) == "-g \"D:\\w\\z.wua\" -f");
    CHECK(hubemu::cemu_args("D:\\w\\z.wua", false) == "-g \"D:\\w\\z.wua\"");
}

static void test_prepare_and_find(const fs::path& dir) {
    fs::path az = dir / "root" / "azahar";
    hubemu::prepare_azahar(az.string());
    CHECK(fs::is_directory(az / "user" / "config") && fs::is_directory(az / "user" / "sdmc"));
    std::string cfg = read_text(az / "user" / "config" / "qt-config.ini");
    CHECK(cfg.find("button_start = ") != std::string::npos);
    write_text(az / "user" / "config" / "qt-config.ini", "[Controls]\nbutton_a = \"user-choice\"\n");
    hubemu::prepare_azahar(az.string());   // marker present: the player's own mapping stays
    CHECK(read_text(az / "user" / "config" / "qt-config.ini").find("user-choice") != std::string::npos);

    fs::path ce = dir / "root" / "cemu";
    hubemu::prepare_cemu_managed(ce.string());
    CHECK(fs::exists(ce / "portable" / "settings.xml") && fs::exists(ce / "portable" / "controllerProfiles" / "controller0.xml"));

    /* discovery: an install above the game file wins and is flagged unmanaged; otherwise RetroHub's own */
    fs::path user_cemu = dir / "Everything" / "Cemu_2.6";
    write_text(user_cemu / "Cemu.exe", "x");
    write_text(user_cemu / "game" / "TPHD.wua", "x");
    hubemu::Install i = hubemu::find(hubemu::Kind::Cemu, "", (user_cemu / "game" / "TPHD.wua").string(), (dir / "root").string());
    CHECK(i.exe == (user_cemu / "Cemu.exe").string() && !i.managed);
    write_text(ce / "Cemu.exe", "x");
    i = hubemu::find(hubemu::Kind::Cemu, (ce / "Cemu.exe").string(), "", (dir / "root").string());
    CHECK(i.exe == (ce / "Cemu.exe").string() && !i.managed);   // an explicit Settings path is honored
    i = hubemu::find(hubemu::Kind::Azahar, "", "", (dir / "root").string());
    CHECK(i.exe.empty());   // not downloaded yet
    write_text(az / "azahar.exe", "x");
    i = hubemu::find(hubemu::Kind::Azahar, "", "", (dir / "root").string());
    CHECK(i.managed && i.exe == (az / "azahar.exe").string());
}

static int test_download(const std::string& root) {
    int bad = 0;
    for (auto k : {hubemu::Kind::Azahar, hubemu::Kind::Cemu}) {
        std::string err;
        int last_phase = -1;
        bool ok = hubemu::install_blocking(k, root, &err, [&](int phase, uint64_t got, uint64_t total) {
            if (phase != last_phase) { std::printf("  %s: phase %d (%llu / %llu bytes)\n", hubemu::name(k), phase, (unsigned long long)got, (unsigned long long)total); last_phase = phase; }
        });
        hubemu::Install in;
        in.dir = hubemu::managed_dir(k, root);
        in.exe = in.dir + (k == hubemu::Kind::Azahar ? "\\azahar.exe" : "\\Cemu.exe");
        std::printf("%s install: %s%s%s\n  exe: %s\n", hubemu::name(k), ok ? "ok" : "FAILED ", ok ? "" : err.c_str(), "", in.exe.c_str());
        if (!ok || in.exe.empty() || !fs::exists(fs::path(in.exe))) { ++bad; continue; }
        if (k == hubemu::Kind::Azahar) hubemu::prepare_azahar(in.dir); else hubemu::prepare_cemu_managed(in.dir);
        std::printf("  sha256(retrohub-install.txt): %s", read_text(fs::path(in.dir) / "retrohub-install.txt").c_str());
        std::printf("  exe size: %llu bytes\n", (unsigned long long)fs::file_size(fs::path(in.exe)));
    }
    std::string again;
    CHECK(hubemu::install_blocking(hubemu::Kind::Cemu, root, &again, nullptr));   // second run: already installed, instant
    return bad;
}

int main(int argc, char** argv) {
    test_3ds_header();
    test_wiiu_formats();
    test_catalog();
    test_save_paths();
    test_config();

    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / "hub_emu_test_dir";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    test_library_ids(dir / "lib");
    test_tree_sync(dir / "tree");
    test_sessions(dir / "sess");
    test_prepare_and_find(dir / "prep");
    fs::remove_all(dir, ec);

    int dl_bad = 0;
    if (argc >= 3 && std::string(argv[1]) == "--download") dl_bad = test_download(argv[2]);

    if (g_fail == 0 && dl_bad == 0) {
        std::printf("hub_emu_test: all checks passed\n");
        return 0;
    }
    std::printf("hub_emu_test: %d check(s) failed, %d download(s) failed\n", g_fail, dl_bad);
    return 1;
}
