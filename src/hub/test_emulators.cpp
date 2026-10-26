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
#include <process.h>
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

    /* CIA whose TMD header ends exactly before the 8-byte title id: must not read past the buffer */
    CHECK(!parse_3ds_header(cia.data(), tmd + 4 + 0x100 + 0x3C + 0x50, &i));

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
    CHECK(g && g->id == "pokex" && g->engine == Engine::Azahar && g->max_players == 2 && g->online && g->genre == "RPG");
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

    /* clock skew: with a manifest the side that changed since the last agreement wins, whatever its mtime says */
    {
        fs::path sa = dir / "sa", sb = dir / "sb";
        write_text(sa / "f.sav", "v1");
        SyncManifest man;
        CHECK(sync_tree_newest(sa.string(), sb.string(), "", "t", &man, "k/").copied == 1);
        write_text(sa / "f.sav", "v2-edited-on-skewed-pc");
        set_mtime(sa / "f.sav", 5000);   // its clock is far behind: older than sb's copy
        TreeSyncReport rr = sync_tree_newest(sa.string(), sb.string(), (dir / "skbk").string(), "local.20260101-000001", &man, "k/");
        CHECK(rr.copied == 1 && read_text(sb / "f.sav") == "v2-edited-on-skewed-pc");
        write_text(sa / "f.sav", "v1");   // without a manifest the older mtime loses
        set_mtime(sa / "f.sav", 9000);
        CHECK(sync_tree_newest(sa.string(), sb.string(), "", "t").kept == 1 && read_text(sb / "f.sav") == "v2-edited-on-skewed-pc");
        /* backups are pruned to the newest 10 per file; identical content makes no backup */
        fs::path pa = dir / "pa", pb = dir / "pb", pbk = dir / "pbk";
        write_text(pb / "g.sav", "start");
        for (int i = 0; i < 14; ++i) {
            write_text(pa / "g.sav", "ver" + std::to_string(i));
            set_mtime(pa / "g.sav", 100 - i);
            set_mtime(pb / "g.sav", 3000);
            char tg[40];
            std::snprintf(tg, sizeof tg, "local.20260101-0000%02d", i);
            sync_tree_newest(pa.string(), pb.string(), pbk.string(), tg);
        }
        int nb = 0;
        for (auto& de : fs::directory_iterator(pbk)) { (void)de; ++nb; }
        CHECK(nb == 10);
        CHECK(sync_tree_newest(pa.string(), pb.string(), pbk.string(), "local.zzz").identical == 1);
    }

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
    CHECK(s.active && s.rels.size() == 2 && s.rels[0] == "usr/save/00050000/1019e500" && s.rels[1] == "usr/save/0005000e/1019e500");
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

    /* Dolphin */
    CHECK(std::string(hubemu::release(hubemu::Kind::Dolphin).sha256).size() == 64 && hubemu::kind_for(hub::System::GC) == hubemu::Kind::Dolphin);
    CHECK(hubemu::dolphin_args("D:\\g\\m.iso", true, "C:\\c\\GCI") ==
          "-b -C Dolphin.Display.Fullscreen=True -C Dolphin.Core.SlotA=8 -C \"Dolphin.Core.GCIFolderAPathOverride=C:\\c\\GCI\" -e \"D:\\g\\m.iso\"");
    hubemu::DolphinPads none, two;
    two.xinput[1] = true;
    std::string di = hubemu::dolphin_dolphin_ini_patch("", none);
    CHECK(di.find("ConfirmStop = False") != std::string::npos && di.find("PermissionAsked = True") != std::string::npos);
    CHECK(di.find("SIDevice0 = 6") != std::string::npos && di.find("SIDevice1 = 0") != std::string::npos);
    CHECK(hubemu::dolphin_dolphin_ini_patch("", two).find("SIDevice1 = 6") != std::string::npos);
    hubemu::DolphinPads ad;
    ad.adapter = true;
    CHECK(hubemu::dolphin_dolphin_ini_patch("", ad).find("SIDevice3 = 12") != std::string::npos);
    CHECK(hubemu::dolphin_dolphin_ini_patch(di, none) == di);   // idempotent
    std::string pi = hubemu::dolphin_pad_ini("", none);
    CHECK(pi.find("[GCPad1]") != std::string::npos && pi.find("[GCPad4]") != std::string::npos);
    CHECK(pi.find("Device = XInput/3/Gamepad") != std::string::npos);
    CHECK(pi.find("Buttons/Z = `Shoulder R` | `DInput/0/Keyboard Mouse:D`") != std::string::npos);   // keyboard only on port 1
    CHECK(pi.find("Buttons/Z = `Shoulder R`\n") != std::string::npos || pi.find("Buttons/Z = `Shoulder R`\r\n") != std::string::npos);
    CHECK(hubemu::dolphin_gfx_ini_patch("").find("ShaderCompilationMode = 2") != std::string::npos);
    // per-game settings
    hub::gs::Values v = hub::gs::defaults_for(hub::System::GC);
    v["gc.res"] = "4"; v["gc.widescreen"] = "true";
    std::string gx = hubemu::dolphin_gfx_apply("[Settings]\nInternalResolution = 2\n", v);
    CHECK(gx.find("InternalResolution = 4") != std::string::npos && gx.find("wideScreenHack = True") != std::string::npos);
    hub::gs::Values a3 = hub::gs::defaults_for(hub::System::N3DS);
    a3["3ds.res"] = "3"; a3["3ds.layout"] = "side";
    std::string az = hubemu::azahar_config_apply("", a3);
    CHECK(az.find("resolution_factor=3") != std::string::npos || az.find("resolution_factor = 3") != std::string::npos);
    CHECK(az.find("layout_option=3") != std::string::npos || az.find("layout_option = 3") != std::string::npos);
    hub::gs::Values w = hub::gs::defaults_for(hub::System::WiiU);
    std::string cx = "<content><GraphicPack><Entry/></GraphicPack></content>";
    w["wiiu.graphic_packs"] = "false";
    std::string off = hubemu::cemu_settings_apply(cx, w);
    CHECK(off.find("<GraphicPack>") == std::string::npos && off.find("GraphicPack_off_by_retrohub") != std::string::npos);
    w["wiiu.graphic_packs"] = "true";
    CHECK(hubemu::cemu_settings_apply(off, w) == cx);
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
    CHECK(i.exe.empty());   // exe without the install marker = an incomplete install
    write_text(az / "retrohub-install.txt", "Azahar test");
    i = hubemu::find(hubemu::Kind::Azahar, "", "", (dir / "root").string());
    CHECK(i.managed && i.exe == (az / "azahar.exe").string());

    /* Dolphin: defaults once, user edits survive, per-launch ports follow the pads; old layout is tidied without touching saves */
    fs::path dd = dir / "dol";
    hubemu::DolphinPads none, two;
    two.xinput[1] = true;
    hubemu::prepare_dolphin(dd.string(), none);
    CHECK(fs::exists(dd / "portable.txt") && fs::exists(dd / "User" / "Config" / "GCPadNew.ini"));
    write_text(dd / "User" / "Config" / "GCPadNew.ini", "[GCPad1]\nButtons/A = custom\n");
    std::string di = read_text(dd / "User" / "Config" / "Dolphin.ini");
    write_text(dd / "User" / "Config" / "Dolphin.ini", hubui::ini_set(di, "Interface", "ConfirmStop", "True"));
    hubemu::prepare_dolphin(dd.string(), two);
    CHECK(read_text(dd / "User" / "Config" / "GCPadNew.ini").find("custom") != std::string::npos);
    std::string di2 = read_text(dd / "User" / "Config" / "Dolphin.ini");
    CHECK(di2.find("ConfirmStop = True") != std::string::npos && di2.find("SIDevice1 = 6") != std::string::npos);
    fs::path od = dir / "olddata";
    write_text(od / "dolphin-user" / "StateSaves" / "a.sav", "s");
    write_text(od / "saves" / "gc" / "g" / "battery" / "GCI" / "x.gci", "card");
    write_text(dir / "oldexe" / "dolphin" / "Dolphin.exe", "x");
    write_text(dir / "oldexe" / "dolphin" / "portable.txt", "");
    hubemu::dolphin_migrate_old(od.string(), (dir / "oldexe").string() + "\\", dd.string());
    CHECK(fs::exists(dd / "User" / "StateSaves" / "a.sav") && fs::exists(od / "dolphin-user.migrated") && !fs::exists(od / "dolphin-user"));
    CHECK(fs::exists(od / "saves" / "gc" / "g" / "battery" / "GCI" / "x.gci") && !fs::exists(dir / "oldexe" / "dolphin"));
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

/* ---- controller profiles: the default profiles must generate exactly what the old hardcoded tables did ---- */
namespace legacy {
static const char* kAzPad[][2] = {
    {"button_a", "engine:sdl,api:controller,button:1,maptype:all,port:0"},
    {"button_b", "engine:sdl,api:controller,button:0,maptype:all,port:0"},
    {"button_x", "engine:sdl,api:controller,button:3,maptype:all,port:0"},
    {"button_y", "engine:sdl,api:controller,button:2,maptype:all,port:0"},
    {"button_up", "engine:sdl,api:controller,button:11,maptype:all,port:0"},
    {"button_down", "engine:sdl,api:controller,button:12,maptype:all,port:0"},
    {"button_left", "engine:sdl,api:controller,button:13,maptype:all,port:0"},
    {"button_right", "engine:sdl,api:controller,button:14,maptype:all,port:0"},
    {"button_l", "engine:sdl,api:controller,button:9,maptype:all,port:0"},
    {"button_r", "engine:sdl,api:controller,button:10,maptype:all,port:0"},
    {"button_start", "engine:sdl,api:controller,button:6,maptype:all,port:0"},
    {"button_select", "engine:sdl,api:controller,button:4,maptype:all,port:0"},
    {"button_zl", "engine:sdl,api:controller,axis:4,direction:+,threshold:0.5,maptype:all,port:0"},
    {"button_zr", "engine:sdl,api:controller,axis:5,direction:+,threshold:0.5,maptype:all,port:0"},
    {"circle_pad", "engine:sdl,api:controller,axis_x:0,axis_y:1,deadzone:0.15,maptype:all,port:0"},
    {"c_stick", "engine:sdl,api:controller,axis_x:2,axis_y:3,deadzone:0.15,maptype:all,port:0"},
};
static std::string azahar(const std::string& in) {
    std::string t = in;
    auto set = [&](const char* sec, const std::string& key, const std::string& val) {
        t = hubui::ini_set(t, sec, key + "\\default", "false");
        t = hubui::ini_set(t, sec, key, val);
    };
    set("UI", "confirmClose", "false");
    set("UI", "firstStart", "false");
    set("UI", "enable_discord_presence", "false");
    set("Miscellaneous", "check_for_update_on_start", "false");
    for (auto& kv : kAzPad) set("Controls", kv[0], std::string("\"") + kv[1] + "\"");
    return t;
}
static std::string cemu() {
    static const int m[][2] = {
        {1, 13}, {2, 12}, {3, 15}, {4, 14}, {5, 8}, {6, 9}, {7, 42}, {8, 43}, {9, 4}, {10, 5}, {11, 0}, {12, 1}, {13, 2}, {14, 3},
        {15, 6}, {16, 7}, {17, 39}, {18, 45}, {19, 44}, {20, 38}, {21, 41}, {22, 47}, {23, 46}, {24, 40},
    };
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<emulated_controller>\n\t<type>Wii U GamePad</type>\n"
                    "\t<controller>\n\t\t<api>XInput</api>\n\t\t<uuid>0</uuid>\n\t\t<display_name>Controller 1</display_name>\n"
                    "\t\t<rumble>0</rumble>\n"
                    "\t\t<axis>\n\t\t\t<deadzone>0.25</deadzone>\n\t\t\t<range>1</range>\n\t\t</axis>\n"
                    "\t\t<rotation>\n\t\t\t<deadzone>0.25</deadzone>\n\t\t\t<range>1</range>\n\t\t</rotation>\n"
                    "\t\t<trigger>\n\t\t\t<deadzone>0.25</deadzone>\n\t\t\t<range>1</range>\n\t\t</trigger>\n"
                    "\t\t<mappings>\n";
    for (auto& e : m)
        x += "\t\t\t<entry>\n\t\t\t\t<mapping>" + std::to_string(e[0]) + "</mapping>\n\t\t\t\t<button>" + std::to_string(e[1]) +
             "</button>\n\t\t\t</entry>\n";
    x += "\t\t</mappings>\n\t</controller>\n</emulated_controller>\n";
    return x;
}
static std::string dolphin_pad(const std::string& in) {
    std::string t = in;
    const std::string kb = "DInput/0/Keyboard Mouse";
    struct M { const char* key; const char* xi; const char* key_kb; };
    static const M map[] = {
        {"Buttons/A", "`Button A`", "X"},        {"Buttons/B", "`Button X`", "Z"},      {"Buttons/X", "`Button B`", "C"},
        {"Buttons/Y", "`Button Y`", "S"},        {"Buttons/Z", "`Shoulder R`", "D"},    {"Buttons/Start", "Start", "RETURN"},
        {"Main Stick/Up", "`Left Y+`", "UP"},    {"Main Stick/Down", "`Left Y-`", "DOWN"},
        {"Main Stick/Left", "`Left X-`", "LEFT"}, {"Main Stick/Right", "`Left X+`", "RIGHT"},
        {"C-Stick/Up", "`Right Y+`", "I"},       {"C-Stick/Down", "`Right Y-`", "K"},
        {"C-Stick/Left", "`Right X-`", "J"},     {"C-Stick/Right", "`Right X+`", "L"},
        {"Triggers/L", "`Trigger L`", "Q"},      {"Triggers/R", "`Trigger R`", "W"},
        {"Triggers/L-Analog", "`Trigger L`", ""}, {"Triggers/R-Analog", "`Trigger R`", ""},
        {"D-Pad/Up", "`Pad N`", "T"},            {"D-Pad/Down", "`Pad S`", "G"},
        {"D-Pad/Left", "`Pad W`", "F"},          {"D-Pad/Right", "`Pad E`", "H"},
    };
    for (int i = 0; i < 4; ++i) {
        std::string sec = "GCPad" + std::to_string(i + 1);
        t = hubui::ini_set(t, sec, "Device", "XInput/" + std::to_string(i) + "/Gamepad");
        for (const auto& m : map) {
            std::string v = m.xi;
            if (i == 0 && m.key_kb[0]) v += " | `" + kb + ":" + m.key_kb + "`";
            t = hubui::ini_set(t, sec, m.key, v);
        }
        t = hubui::ini_set(t, sec, "Rumble/Motor", "`Motor L`|`Motor R`");
    }
    return t;
}
}  // namespace legacy

static void test_controls_emulators(const fs::path& dir) {
    hubemu::DolphinPads none;
    const char* sample = "[Renderer]\nresolution_factor = 2\n[Controls]\nbutton_a = \"x\"\n";
    CHECK(hubemu::azahar_config_patch("") == legacy::azahar(""));
    CHECK(hubemu::azahar_config_patch(sample) == legacy::azahar(sample));
    CHECK(hubemu::cemu_gamepad_profile_xml() == legacy::cemu());
    CHECK(hubemu::dolphin_pad_ini("", none) == legacy::dolphin_pad(""));
    CHECK(hubemu::dolphin_pad_ini("[GCPad1]\nButtons/A = q\n", none) == legacy::dolphin_pad("[GCPad1]\nButtons/A = q\n"));

    /* a changed profile changes the output; the marker logic rewrites only then */
    hubctl::Store st;
    CHECK(hubctl::set_mapping(st, "gc", "", "a", "b"));
    CHECK(hubctl::set_mapping(st, "wiiu", "", "zl", "leftshoulder"));
    CHECK(hubctl::set_mapping(st, "3ds", "", "a", "lefttrigger@8000"));
    CHECK(hubctl::set_analog(st, "3ds", "", "circle_pad", 0.3, 1.0));
    hubctl::Profile gc = hubctl::resolve(st, "gc"), wu = hubctl::resolve(st, "wiiu"), ds = hubctl::resolve(st, "3ds");
    CHECK(hubemu::dolphin_pad_ini("", none, gc).find("Buttons/A = `Button B` | `DInput/0/Keyboard Mouse:X`") != std::string::npos);
    CHECK(hubemu::cemu_gamepad_profile_xml(wu).find("<mapping>7</mapping>\n\t\t\t\t<button>8</button>") != std::string::npos);
    std::string az = hubemu::azahar_config_patch("", ds);
    CHECK(az.find("button_a = \"engine:sdl,api:controller,axis:4,direction:+,threshold:0.244141") != std::string::npos);
    CHECK(az.find("deadzone:0.3,") != std::string::npos);

    fs::path d = dir / "regen";
    fs::path azd = d / "az", ced = d / "ce", dod = d / "dol";
    hubemu::prepare_azahar(azd.string(), hubctl::default_profile("3ds"));
    write_text(azd / "user" / "config" / "qt-config.ini", "[Controls]\nbutton_a = \"user-choice\"\n");
    hubemu::prepare_azahar(azd.string(), hubctl::default_profile("3ds"));   // same profile: user edit stays
    CHECK(read_text(azd / "user" / "config" / "qt-config.ini").find("user-choice") != std::string::npos);
    hubemu::prepare_azahar(azd.string(), ds);                               // profile changed: regenerated
    CHECK(read_text(azd / "user" / "config" / "qt-config.ini").find("user-choice") == std::string::npos);
    write_text(azd / "user" / "config" / "qt-config.ini", "[Controls]\nbutton_a = \"user-choice\"\n");
    hubemu::prepare_azahar(azd.string(), ds);                               // same changed profile again: stays
    CHECK(read_text(azd / "user" / "config" / "qt-config.ini").find("user-choice") != std::string::npos);
    /* an install from an older RetroHub (marker without a hash) counts as the default profile */
    write_text(azd / "user" / "config" / "retrohub-pad.txt", "old marker\n");
    hubemu::prepare_azahar(azd.string(), hubctl::default_profile("3ds"));
    CHECK(read_text(azd / "user" / "config" / "qt-config.ini").find("user-choice") != std::string::npos);

    hubemu::prepare_cemu_managed(ced.string(), hubctl::default_profile("wiiu"));
    CHECK(read_text(ced / "portable" / "controllerProfiles" / "controller0.xml") == legacy::cemu());
    write_text(ced / "portable" / "controllerProfiles" / "controller0.xml", "user xml");
    hubemu::prepare_cemu_managed(ced.string(), hubctl::default_profile("wiiu"));
    CHECK(read_text(ced / "portable" / "controllerProfiles" / "controller0.xml") == "user xml");
    hubemu::prepare_cemu_managed(ced.string(), wu);
    CHECK(read_text(ced / "portable" / "controllerProfiles" / "controller0.xml") == hubemu::cemu_gamepad_profile_xml(wu));

    hubemu::prepare_dolphin(dod.string(), none, hubctl::default_profile("gc"));
    CHECK(read_text(dod / "User" / "Config" / "GCPadNew.ini") == legacy::dolphin_pad(""));
    write_text(dod / "User" / "Config" / "GCPadNew.ini", "[GCPad1]\nButtons/A = custom\n");
    hubemu::prepare_dolphin(dod.string(), none, hubctl::default_profile("gc"));
    CHECK(read_text(dod / "User" / "Config" / "GCPadNew.ini").find("custom") != std::string::npos);
    hubemu::prepare_dolphin(dod.string(), none, gc);
    CHECK(read_text(dod / "User" / "Config" / "GCPadNew.ini").find("custom") == std::string::npos);
}

static void test_controls2_emulators(const fs::path& dir) {
    using hubemu::PadDesc;
    PadDesc xb{"Xbox Controller", "guidX", "guidX#1", true}, ds{"DualSense Wireless Controller", "guidD", "guidD#a", false},
            ds2{"DualSense Wireless Controller", "guidD", "guidD#b", false}, bd{"8BitDo Pro 2", "guidB", "guidB", false};
    hubemu::DolphinPads base;
    /* all XInput in order, or no pads: exactly the old output */
    CHECK(hubemu::dolphin_pads_with(base, {xb}, {xb}).dev[0].empty());
    CHECK(hubemu::dolphin_pad_ini("", hubemu::dolphin_pads_with(base, {xb}, {xb})) == hubemu::dolphin_pad_ini("", base));
    /* DualSense on port 1, Xbox on port 2, a second DualSense on port 3 */
    hubemu::DolphinPads mix = hubemu::dolphin_pads_with(base, {ds, xb, ds2}, {xb, ds, ds2});
    CHECK(mix.dev[0] == "SDL/0/DualSense Wireless Controller" && mix.dev[1] == "XInput/0/Gamepad" && mix.dev[2] == "SDL/1/DualSense Wireless Controller");
    std::string pi = hubemu::dolphin_pad_ini("", mix);
    CHECK(pi.find("[GCPad1]\nDevice = SDL/0/DualSense Wireless Controller") != std::string::npos);
    CHECK(pi.find("Buttons/A = `Button S` | `DInput/0/Keyboard Mouse:X`") != std::string::npos);
    CHECK(pi.find("Buttons/B = `Button W`") != std::string::npos);   // GC B = SDL x = west
    CHECK(pi.find("Main Stick/Up = `Left Y-` | `DInput/0/Keyboard Mouse:UP`") != std::string::npos);
    CHECK(pi.find("Device = XInput/0/Gamepad") != std::string::npos);
    CHECK(hubemu::dolphin_ini_ports("", mix).find("SIDevice2 = 6") != std::string::npos);   // SDL pad on port 3 counts as present
    hubemu::DolphinPads bdp = hubemu::dolphin_pads_with(base, {bd}, {bd});
    CHECK(hubemu::dolphin_pad_ini("", bdp).find("Device = SDL/0/8BitDo Pro 2") != std::string::npos);

    /* keyboard remap reaches GCPad1 only */
    hubctl::Store st;
    CHECK(hubctl::set_key(st, "gc", "", "a", "space|n"));
    std::string kp = hubemu::dolphin_pad_ini("", base, hubctl::resolve(st, "gc"));
    CHECK(kp.find("Buttons/A = `Button A` | `DInput/0/Keyboard Mouse:SPACE` | `DInput/0/Keyboard Mouse:N`") != std::string::npos);
    CHECK(kp.find("[GCPad2]") != std::string::npos && kp.find("Keyboard Mouse:SPACE", kp.find("[GCPad2]")) == std::string::npos);
    CHECK(hubemu::dolphin_key_name("return") == "RETURN" && hubemu::dolphin_key_name("f5") == "F5" && hubemu::dolphin_key_name("kp3") == "NUMPAD3");

    /* Azahar follows the first pad of the port order */
    hubemu::AzPad az;
    CHECK(!hubemu::azahar_pad_select({}, {xb}, &az) && az.guid.empty());
    CHECK(hubemu::azahar_pad_select({ds2, ds}, {xb, ds, ds2}, &az) && az.guid == "guidD" && az.port == 1);
    hubctl::Profile p3 = hubctl::default_profile("3ds");
    std::string with = hubemu::azahar_config_patch("", p3, az);
    CHECK(with.find("button_a = \"engine:sdl,api:controller,button:1,guid:guidD,port:1\"") != std::string::npos);
    CHECK(with.find("maptype:all") == std::string::npos);
    CHECK(hubemu::azahar_config_patch("", p3, hubemu::AzPad()) == hubemu::azahar_config_patch("", p3));
    /* choosing another pad regenerates; the same choice keeps user edits */
    fs::path azd = dir / "pad_az";
    fs::path ini = azd / "user" / "config" / "qt-config.ini";
    hubemu::prepare_azahar(azd.string(), p3, hubemu::AzPad());
    write_text(ini, "[Controls]\nbutton_a = \"user-choice\"\n");
    hubemu::prepare_azahar(azd.string(), p3, hubemu::AzPad());
    CHECK(read_text(ini).find("user-choice") != std::string::npos);
    hubemu::prepare_azahar(azd.string(), p3, az);
    CHECK(read_text(ini).find("user-choice") == std::string::npos && read_text(ini).find("guid:guidD,port:1") != std::string::npos);

    if (const char* dump = std::getenv("RETROHUB_DUMP_DIR")) {   // QA aid: write the fake-pad configs (Xbox + DualSense + Xbox) for a readback launch
        hubemu::DolphinPads fk = hubemu::dolphin_pads_with(base, {xb, ds, bd}, {xb, ds, bd});
        hubemu::prepare_dolphin((fs::path(dump) / "dolphin").string(), fk, hubctl::default_profile("gc"));
        hubemu::prepare_cemu_managed((fs::path(dump) / "cemu").string(), hubctl::default_profile("wiiu"), hubemu::cemu_pads_with({xb, xb, xb}));
        hubemu::prepare_cemu_managed((fs::path(dump) / "cemu_dsu").string(), hubctl::default_profile("wiiu"), hubemu::cemu_pads_with_dsu(hubemu::CemuPads(), 3));   // remote play: P1 local, P2-4 DSU
        hubemu::prepare_azahar((fs::path(dump) / "azahar").string(), hubctl::default_profile("3ds"), az);
    }
    /* Cemu: players follow the port order's XInput pads; GamePad for player 1, Pro Controllers after */
    hubemu::CemuPads cp = hubemu::cemu_pads_with({ds, xb, bd});   // non-XInput skipped
    CHECK(cp.xinput.size() == 1 && cp.xinput[0] == 0);
    PadDesc xb2{"Xbox Controller", "guidX", "guidX#2", true};
    cp = hubemu::cemu_pads_with({xb, bd, xb2});
    CHECK(cp.xinput.size() == 2 && cp.xinput[1] == 1);
    hubctl::Profile wup = hubctl::default_profile("wiiu");
    CHECK(hubemu::cemu_gamepad_profile_xml(wup, 0) == hubemu::cemu_gamepad_profile_xml(wup));
    std::string pro = hubemu::cemu_pro_profile_xml(wup, 1, 1);
    CHECK(pro.find("<type>Wii U Pro Controller</type>") != std::string::npos && pro.find("<uuid>1</uuid>") != std::string::npos &&
          pro.find("<display_name>Controller 2</display_name>") != std::string::npos);
    fs::path ced = dir / "pad_cemu";
    fs::path cdir = ced / "portable" / "controllerProfiles";
    hubemu::prepare_cemu_managed(ced.string(), wup, hubemu::CemuPads());
    CHECK(!fs::exists(cdir / "controller1.xml"));
    hubemu::prepare_cemu_managed(ced.string(), wup, cp);
    CHECK(read_text(cdir / "controller1.xml") == hubemu::cemu_pro_profile_xml(wup, 1, 1));
    CHECK(read_text(cdir / "controller0.xml") == hubemu::cemu_gamepad_profile_xml(wup, 0));
    hubemu::prepare_cemu_managed(ced.string(), wup, hubemu::CemuPads());   // fewer pads: stale Pro file goes
    CHECK(!fs::exists(cdir / "controller1.xml"));
    /* Remote play: player 1 local pad, players 2-4 = DSU slots 0-2 */
    {
        hubemu::CemuPads dp = hubemu::cemu_pads_with_dsu(hubemu::CemuPads(), 3);
        hubemu::prepare_cemu_managed(ced.string(), wup, dp);
        std::string c0 = read_text(cdir / "controller0.xml"), c1 = read_text(cdir / "controller1.xml"), c3 = read_text(cdir / "controller3.xml");
        CHECK(c0.find("<api>XInput</api>") != std::string::npos && c0.find("Wii U GamePad") != std::string::npos);
        CHECK(c1.find("<api>DSUController</api>") != std::string::npos && c1.find("<uuid>0</uuid>") != std::string::npos && c1.find("Wii U Pro Controller") != std::string::npos);
        CHECK(c3.find("<uuid>2</uuid>") != std::string::npos);
        CHECK(c1 == hubemu::cemu_pro_profile_xml_dsu(wup, 0, 1));
        CHECK(c1.find("<button>13</button>") != std::string::npos);   // Wii U A/B land on DSU face bits
        hubemu::prepare_cemu_managed(ced.string(), wup, hubemu::cemu_pads_with_dsu(hubemu::CemuPads(), 1));
        CHECK(fs::exists(cdir / "controller1.xml") && !fs::exists(cdir / "controller2.xml"));
        hubemu::prepare_cemu_managed(ced.string(), wup, hubemu::CemuPads());
        CHECK(!fs::exists(cdir / "controller1.xml"));
    }
    /* A user's own Cemu: backup once, write only when managing, restore, SDL pads */
    {
        fs::path ud = dir / "own_cemu_data";
        fs::create_directories(ud / "controllerProfiles");
        write_text(ud / "controllerProfiles" / "controller0.xml", "users own");
        write_text(ud / "controllerProfiles" / "mine.xml", "keep");
        hubemu::prepare_cemu_own(ud.string(), wup, hubemu::CemuPads(), false);   // setting OFF: nothing touched, no backup
        CHECK(read_text(ud / "controllerProfiles" / "controller0.xml") == "users own" && !fs::exists(ud / "controllerProfiles.retrohub-backup"));
        hubemu::prepare_cemu_own(ud.string(), wup, hubemu::CemuPads(), true);
        CHECK(read_text(ud / "controllerProfiles.retrohub-backup" / "controller0.xml") == "users own");
        CHECK(read_text(ud / "controllerProfiles.retrohub-backup" / "mine.xml") == "keep");
        CHECK(read_text(ud / "controllerProfiles" / "controller0.xml") == hubemu::cemu_gamepad_profile_xml(wup, 0));
        write_text(ud / "controllerProfiles" / "controller0.xml", "edited later");   // backup is once only; our marker keeps edits
        hubemu::prepare_cemu_own(ud.string(), wup, hubemu::CemuPads(), true);
        CHECK(read_text(ud / "controllerProfiles.retrohub-backup" / "controller0.xml") == "users own");
        CHECK(hubemu::cemu_restore_controls(ud.string()));
        CHECK(read_text(ud / "controllerProfiles" / "controller0.xml") == "users own" && read_text(ud / "controllerProfiles" / "mine.xml") == "keep" &&
              !fs::exists(ud / "controllerProfiles" / "retrohub-pad.txt"));
        CHECK(!hubemu::cemu_restore_controls((dir / "no_such_cemu").string()));
        /* no profiles at all: an empty backup restores to none */
        fs::path ud2 = dir / "own_cemu_data2";
        hubemu::prepare_cemu_own(ud2.string(), wup, hubemu::CemuPads(), true);
        CHECK(fs::is_directory(ud2 / "controllerProfiles.retrohub-backup") && fs::exists(ud2 / "controllerProfiles" / "controller0.xml"));
        hubemu::cemu_restore_controls(ud2.string());
        CHECK(!fs::exists(ud2 / "controllerProfiles" / "controller0.xml"));
        /* data dir: portable\ folder, else settings.xml beside the exe, else %APPDATA%\Cemu */
        fs::path ex = dir / "own_cemu_exe";
        fs::create_directories(ex);
        CHECK(hubemu::cemu_user_data_dir(ex.string(), "C:\\AD") == "C:\\AD\\Cemu");
        write_text(ex / "settings.xml", "<content/>");
        CHECK(fs::path(hubemu::cemu_user_data_dir(ex.string(), "C:\\AD")) == ex);
        fs::create_directories(ex / "portable");
        CHECK(fs::path(hubemu::cemu_user_data_dir(ex.string(), "C:\\AD")) == ex / "portable");
        /* SDL: Cemu uuid = "<index>_<guid>", index among same-GUID pads */
        PadDesc s1{"DualSense", "guidDS", "guidDS", false}, s2{"DualSense", "guidDS", "guidDS#2", false};
        hubemu::CemuPads sp = hubemu::cemu_pads_with_sdl({xb, s2, s1}, {s1, s2, xb});
        CHECK(sp.xinput.size() == 3 && sp.xinput[0] == 0 && sp.xinput[1] == -1 && sp.sdl[1] == "1_guidDS" && sp.sdl[2] == "0_guidDS" && sp.sdl[0].empty());
        std::string sx = hubemu::cemu_pro_profile_xml_sdl(wup, "0_guidDS", 1);
        CHECK(sx.find("<api>SDLController</api>") != std::string::npos && sx.find("<uuid>0_guidDS</uuid>") != std::string::npos &&
              (hubemu::cemu_pro_profile_xml(wup, 1, 1).find("<mapping>1</mapping>\n\t\t\t\t<button>12</button>") != std::string::npos
                   ? sx.find("<mapping>1</mapping>\n\t\t\t\t<button>0</button>") : sx.find("<mapping>1</mapping>\n\t\t\t\t<button>1</button>")) != std::string::npos);
        fs::path ud3 = dir / "own_cemu_data3";
        hubemu::prepare_cemu_own(ud3.string(), wup, sp, true);
        CHECK(read_text(ud3 / "controllerProfiles" / "controller1.xml") == hubemu::cemu_pro_profile_xml_sdl(wup, "1_guidDS", 1));
        CHECK(read_text(ud3 / "controllerProfiles" / "controller0.xml").find("<api>XInput</api>") != std::string::npos);
    }
    /* Xbox pad is the default for every console: GC/3DS/Wii U default profiles all bind the XInput names */
    CHECK(hubemu::dolphin_pad_ini("", base).find("Device = XInput/0/Gamepad") != std::string::npos);

    /* Dolphin: the pad file is rewritten when the device assignment changes, kept otherwise */
    fs::path dod = dir / "pad_dol";
    fs::path gcp = dod / "User" / "Config" / "GCPadNew.ini";
    hubctl::Profile gcd = hubctl::default_profile("gc");
    hubemu::prepare_dolphin(dod.string(), base, gcd);
    write_text(gcp, "[GCPad1]\nButtons/A = user\n");
    hubemu::prepare_dolphin(dod.string(), base, gcd);
    CHECK(read_text(gcp).find("Buttons/A = user") != std::string::npos);
    hubemu::prepare_dolphin(dod.string(), bdp, gcd);
    CHECK(read_text(gcp).find("Device = SDL/0/8BitDo Pro 2") != std::string::npos);
    write_text(gcp, "[GCPad1]\nButtons/A = user\n");
    hubemu::prepare_dolphin(dod.string(), bdp, gcd);
    CHECK(read_text(gcp).find("Buttons/A = user") != std::string::npos);
}

int main(int argc, char** argv) {
    test_3ds_header();
    test_wiiu_formats();
    test_catalog();
    test_save_paths();
    test_config();

    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / ("hub_emu_test_dir_" + std::to_string(_getpid()));   // per-process: concurrent runs (agents, parallel ctest) must not share/wipe one dir
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    test_library_ids(dir / "lib");
    test_tree_sync(dir / "tree");
    test_sessions(dir / "sess");
    test_prepare_and_find(dir / "prep");
    test_controls_emulators(dir / "ctl");
    test_controls2_emulators(dir / "ctl2");
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
