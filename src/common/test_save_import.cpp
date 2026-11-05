/* Unit tests for "Bring your saves" (src/common/save_import.*): detection, every source format, matching, newer/older preview rules
 * and the backup-before-overwrite rule. All fixtures are tiny fake saves written into a scratch folder. */
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "save_import.h"

using namespace hub::saveimport;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static std::string U8(const fs::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }
static void wr(const fs::path& p, const std::string& d, int64_t age_s = 0) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(d.data(), (std::streamsize)d.size());
    f.close();
    if (age_s) fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::seconds(age_s), ec);
}
static std::string rd(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static std::string fill(size_t n, char c) { return std::string(n, c); }

static std::string sk(std::string k) { for (char& c : k) if (c == ':') c = '_'; return k; }
static fs::path g_root;
static fs::path vault() { return g_root / "vault"; }
static std::string bat(const GameRef& g) { return U8(vault() / g.system / sk(g.key) / "battery") + "\\"; }   // test vault layout (key chars are filename-safe here)

static GameRef game(const std::string& key, const std::string& sys, const std::string& title, const std::string& stem, const std::string& rom = "", const std::string& serial = "") {
    GameRef g; g.key = key; g.system = sys; g.title = title; g.save_stem = stem; g.rom_path = rom; g.serial = serial;
    return g;
}
static Found found(Source s, const fs::path& p) {
    std::error_code ec;
    Found f; f.src = s; f.path = U8(p); f.name = U8(p.filename()); f.ext = U8(p.extension());
    for (char& c : f.ext) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    f.size = (int64_t)fs::file_size(p, ec);
    auto ft = fs::last_write_time(p, ec);
    f.mtime = (int64_t)std::chrono::system_clock::to_time_t(std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now()));
    return f;
}

static const char* kIni =
    "; test ini\n[3E3F1A31AAAAAAAAAAAAAAAAAAAAAAAA]\nGoodName=Super Mario 64 (U) [!]\nCRC=1 2\n\n"
    "[44444444BBBBBBBBBBBBBBBBBBBBBBBB]\nRefMD5=3E3F1A31AAAAAAAAAAAAAAAAAAAAAAAA\nGoodName=Mario Hack (U)\n\n"
    "[55555555CCCCCCCCCCCCCCCCCCCCCCCC]\nRefMD5=3E3F1A31AAAAAAAAAAAAAAAAAAAAAAAA\n";

static std::string z64_header(const std::string& name) {
    std::string h(0x40, '\0');
    h[0] = (char)0x80; h[1] = 0x37; h[2] = 0x12; h[3] = 0x40;
    std::string n = name;
    n.resize(20, ' ');
    std::memcpy(&h[0x20], n.data(), 20);
    return h;
}

static std::string raw_card() {   // a 16-block GameCube card with one 2-block save of game "GALE"
    std::string c(16 * 0x2000, '\xFF');
    auto be16 = [&](size_t o, uint16_t v) { c[o] = char(v >> 8); c[o + 1] = char(v & 0xFF); };
    std::memset(&c[0x2000], 0xFF, 0x2000);
    size_t e = 0x2000;
    std::memcpy(&c[e], "GALE", 4); std::memcpy(&c[e + 4], "01", 2); c[e + 6] = (char)0xFF; c[e + 7] = 0;
    std::string nm(32, '\0'); std::memcpy(&nm[0], "smash_save", 10); std::memcpy(&c[e + 8], nm.data(), 32);
    be16(e + 0x36, 5); be16(e + 0x38, 2);
    be16(0x2000 + 0x1FFA, 2); be16(0x4000 + 0x1FFA, 1);   // active dir = block 1
    be16(3 * 0x2000 + 4, 2); be16(4 * 0x2000 + 4, 1);     // active BAT = block 3
    be16(3 * 0x2000 + 10, 6); be16(3 * 0x2000 + 12, 0xFFFF);
    std::memset(&c[5 * 0x2000], 0xA5, 0x2000);
    std::memset(&c[6 * 0x2000], 0x5A, 0x2000);
    return c;
}

int main() {
    std::error_code ec;
    g_root = fs::temp_directory_path() / ("hub_saveimport_test_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root, ec);

    /* ---- names and small helpers ---- */
    CHECK(norm_name("Pokemon - Emerald (USA) [!]") == "pokemonemerald");
    CHECK(norm_name("(only brackets)") == "onlybrackets");
    int ctl = 0;
    CHECK(strip_save_suffix("Mario Kart 64_Cont_3", &ctl) == "Mario Kart 64" && ctl == 2);
    CHECK(strip_save_suffix("Mario Kart 64 (U) [!]-3A67D998") == "Mario Kart 64 (U) [!]");
    CHECK(strip_save_suffix("Plain Name") == "Plain Name");
    CHECK(pcsx2_safe_key("ps2:slus-20312") == "ps2_slus-20312");
    CHECK(ini_goodname(kIni, "3e3f1a31aaaaaaaaaaaaaaaaaaaaaaaa") == "Super Mario 64 (U) [!]");
    CHECK(ini_goodname(kIni, "44444444BBBBBBBBBBBBBBBBBBBBBBBB") == "Mario Hack (U)");
    CHECK(ini_goodname(kIni, "55555555CCCCCCCCCCCCCCCCCCCCCCCC") == "Super Mario 64 (U) [!]");   // RefMD5 inherits
    CHECK(ini_goodname(kIni, "00000000000000000000000000000000").empty());
    CHECK(n64_save_base("3E3F1A31AAAA", "Super Mario 64 (U) [!]", "") == "Super Mario 64 (U) [!]-3E3F1A31");
    CHECK(n64_save_base("ABCDEF0123456789", "A very long good name that is more than 32 chars", "") == "A very long good name that is mo-ABCDEF01");
    CHECK(n64_save_base("abcdef0123456789", "", "MY GAME") == "MY GAME-ABCDEF01");
    CHECK(n64_save_base("abcdef0123456789", "a:b", "") == "a_b-ABCDEF01");
    CHECK(n64_save_base("abcdef0123456789", "", "").empty());
    {
        std::string z = z64_header("SUPER MARIO 64");
        CHECK(n64_header_name(z) == "SUPER MARIO 64");
        std::string v = z;
        for (size_t i = 0; i + 1 < v.size(); i += 2) std::swap(v[i], v[i + 1]);
        CHECK(n64_header_name(v) == "SUPER MARIO 64");
        std::string n = z;
        for (size_t i = 0; i + 3 < n.size(); i += 4) { std::swap(n[i], n[i + 3]); std::swap(n[i + 1], n[i + 2]); }
        CHECK(n64_header_name(n) == "SUPER MARIO 64");
        CHECK(n64_header_name("garbage").empty());
    }
    {
        std::string blank = blank_mempak("seed");
        CHECK(blank.size() == 32768 && !mempak_has_notes(blank));
        CHECK(blank_mempak("seed") == blank && blank_mempak("other") != blank);
        std::string used = blank;
        used[256 + 2 * 7] = 0; used[256 + 2 * 7 + 1] = 9;   // an inode points at page 9: a note exists
        CHECK(mempak_has_notes(used));
        CHECK(!mempak_has_notes(std::string(32768, '\0')));
        CHECK(blank.substr(32, 32) == blank.substr(96, 32) && blank.substr(256, 256) == blank.substr(512, 256));
    }
    {
        RaCfg c = parse_retroarch_cfg("savefile_directory = \":\\saves\"\nsavestate_directory = \"D:\\ra\\st\"\nsavefiles_in_content_dir = \"true\"\n", "C:\\RA");
        CHECK(c.savefile_dir == "C:\\RA\\saves" && c.savestate_dir == "D:\\ra\\st" && c.saves_in_content_dir);
        c = parse_retroarch_cfg("savefile_directory = \"default\"\nsavestate_directory = \"states2\"\n", "C:\\RA");
        CHECK(c.savefile_dir == "C:\\RA\\saves" && c.savestate_dir == "C:\\RA\\states2");
        c = parse_retroarch_cfg("", "C:\\RA");
        CHECK(c.savefile_dir == "C:\\RA\\saves" && c.savestate_dir == "C:\\RA\\states" && !c.saves_in_content_dir);
        CHECK(parse_mgba_savegame_path("[ports.qt]\nsavegamePath=D:\\saves\\gba\n") == "D:\\saves\\gba");
        CHECK(parse_mgba_savegame_path("[x]\n").empty());
    }

    /* ---- GameCube card, Controller Pak and EEPROM building blocks ---- */
    {
        std::string card = raw_card();
        std::vector<GcEntry> es;
        CHECK(gc_card_entries(card, &es) && es.size() == 1 && es[0].gamecode == "GALE" && es[0].filename == "smash_save" && es[0].count == 2);
        std::string gci;
        CHECK(gc_card_export_gci(card, es[0], &gci) && gci.size() == 0x40 + 2 * 0x2000 && gci[0x40] == (char)0xA5 && gci[0x40 + 0x2000] == 0x5A);
        CHECK(!gc_card_entries(std::string(1000, 'x'), &es));
    }

    /* ---- detection with a fake profile ---- */
    {
        fs::path pr = g_root / "profile";
        Env env;
        env.appdata = U8(pr / "AppData" / "Roaming");
        env.localappdata = U8(pr / "AppData" / "Local");
        env.userprofile = U8(pr);
        env.documents = U8(pr / "Documents");
        env.programfiles = U8(pr / "PF");
        env.programfiles_x86 = U8(pr / "PF86");
        env.system_drive = U8(pr / "C");
        env.own_data_dir = U8(pr / "AppData" / "Local" / "RetroHub");
        fs::create_directories(pr / "C" / "Emu" / "x");
        wr(pr / "AppData" / "Roaming" / "RetroArch" / "retroarch.cfg", "savefile_directory = \"D:\\nonexistent_ra_saves\"\n");
        fs::create_directories(pr / "AppData" / "Roaming" / "RetroArch" / "saves");
        fs::create_directories(pr / "PF86" / "Project64 2.3" / "Save");
        fs::create_directories(pr / "Documents" / "Dolphin Emulator" / "GC");
        fs::create_directories(pr / "AppData" / "Roaming" / "PCSX2" / "memcards");
        fs::create_directories(pr / "Documents" / "mGBA");
        fs::create_directories(pr / "C" / "VisualBoyAdvance-M");
        fs::create_directories(pr / "AppData" / "Roaming" / "melonDS");
        fs::create_directories(pr / "AppData" / "Local" / "RetroHub" / "emulators" / "PCSX2" / "memcards");   // RetroHub's own: never offered
        auto fl = detect_folders(env);
        auto has = [&](Source s) { for (auto& f : fl) if (f.src == s) return true; return false; };
        CHECK(has(Source::RetroArch) && has(Source::Project64) && has(Source::Dolphin) && has(Source::PCSX2) && has(Source::MGBA) && has(Source::VBA) && has(Source::MelonDS));
        for (auto& f : fl) CHECK(f.dir.find("RetroHub") == std::string::npos);
        CHECK(detect_folders(Env()).empty());
    }

    /* ---- RetroArch .srm: stem match, title match, newer/older ---- */
    {
        fs::path ra = g_root / "ra" / "saves";
        wr(ra / "Pokemon Emerald (USA).srm", fill(131072, 'E'), 100);
        wr(ra / "mGBA" / "Metroid Fusion (USA).srm", fill(65536, 'M'), 100);          // core sub folder
        wr(ra / "Unrelated Game.srm", fill(100, 'U'));
        std::vector<Found> fs_ = scan_folder({Source::RetroArch, U8(ra)});
        CHECK(fs_.size() == 3);
        std::vector<GameRef> gs = {game("gba:bpee-1", "gba", "Pokemon Emerald", "Pokemon Emerald (USA)", "C:\\roms\\Pokemon Emerald (USA).gba"),
                                   game("gba:amtj-2", "gba", "Metroid Fusion", "Metroid Fusion (Europe)", "C:\\roms\\Metroid Fusion (Europe).gba")};
        Ctx ctx; ctx.battery_dir = bat;
        auto rows = build_rows(fs_, gs, ctx);
        CHECK(rows.size() == 2);   // the unrelated one has no row
        const Row* emerald = nullptr; const Row* fusion = nullptr;
        for (auto& r : rows) { if (r.game == 0) emerald = &r; else fusion = &r; }
        CHECK(emerald && emerald->how == How::Stem && emerald->selected && emerald->newer == Row::NoVault && emerald->problem.empty());
        CHECK(emerald && emerald->out_rel.size() == 1 && emerald->out_rel[0] == "Pokemon Emerald (USA).srm");
        CHECK(fusion && fusion->how == How::Title && fusion->out_rel[0] == "Metroid Fusion (Europe).srm");   // named like the runner expects: the vault game's stem

        /* vault already holds a NEWER save: nothing pre-selected, both sides described */
        wr(U8(vault() / "gba" / "gba_bpee-1" / "battery" / "Pokemon Emerald (USA).srm"), fill(131072, 'V'));   // now
        rows = build_rows(fs_, gs, ctx);
        for (auto& r : rows) if (r.game == 0) {
            CHECK(r.vault_has && r.newer == Row::VaultNewer && !r.selected && r.vault_size == 131072 && r.src_size == 131072);
        }
        /* vault OLDER than the source: pre-selected */
        wr(U8(vault() / "gba" / "gba_bpee-1" / "battery" / "Pokemon Emerald (USA).srm"), fill(131072, 'V'), 5000);
        rows = build_rows(fs_, gs, ctx);
        for (auto& r : rows) if (r.game == 0) CHECK(r.newer == Row::SourceNewer && r.selected);
        /* identical content: nothing to do */
        wr(U8(vault() / "gba" / "gba_bpee-1" / "battery" / "Pokemon Emerald (USA).srm"), fill(131072, 'E'), 5000);
        rows = build_rows(fs_, gs, ctx);
        for (auto& r : rows) if (r.game == 0) CHECK(r.same_as_vault && r.newer == Row::Same && !r.selected);
    }

    /* ---- mGBA clock block: retrorun's core keeps the bare save, so the 16 byte RTC footer is cut off (and nothing else) ---- */
    {
        Ctx ctx; ctx.battery_dir = bat;
        GameRef g = game("gba:rtc-1", "gba", "Pokemon Ruby", "Pokemon Ruby", "C:\\r\\Pokemon Ruby.gba");
        struct { size_t in, out; } cases[] = {{131072 + 16, 131072}, {32768 + 16, 32768}, {65536 + 16, 65536}, {8192 + 16, 8192}, {512 + 16, 512}, {32768, 32768}, {131072, 131072}, {32768 + 17, 32768 + 17}, {100, 100}};
        for (auto& c : cases) {
            fs::path src = g_root / "rtc" / std::to_string(c.in) / "Pokemon Ruby.sav";
            wr(src, fill(c.in, 'R'));
            auto rows = build_rows({found(Source::MGBA, src)}, {g}, ctx);
            std::vector<OutFile> o; std::string why;
            CHECK(!rows.empty() && convert(rows[0], g, ctx, &o, &why) && o.size() == 1 && o[0].rel == "Pokemon Ruby.srm" && o[0].data.size() == c.out && o[0].data == fill(c.out, 'R'));
        }
    }

    /* ---- backup before overwrite ---- */
    {
        GameRef g = game("gba_bk-1", "gba", "Backup Game", "Backup Game");
        fs::path src = g_root / "bk" / "Backup Game.sav";
        wr(src, fill(1024, 'N'), 10);
        Ctx ctx; ctx.battery_dir = bat;
        fs::path target = vault() / "gba" / "gba_bk-1" / "battery" / "Backup Game.srm";
        wr(target, fill(1024, 'O'), 4000);
        auto rows = build_rows({found(Source::MGBA, src)}, {g}, ctx);
        CHECK(rows.size() == 1 && rows[0].selected && rows[0].how == How::Stem);
        ApplyReport rep = apply_row(rows[0], g, ctx, U8(vault()), 1700000000);
        CHECK(rep.ok && rep.written == 1 && rep.backed_up == 1);
        CHECK(rd(target) == fill(1024, 'N'));
        fs::path bk = vault() / "_conflicts" / "gba" / "gba_bk-1" / "battery" / "Backup Game.srm.import.20231114-221320";
        CHECK(fs::exists(bk) && rd(bk) == fill(1024, 'O'));
        /* a second run with the same content changes nothing and makes no new backup */
        {   // a stale preview (the vault file changed after it was built) is refused, nothing is written
            fs::path t2 = vault() / "gba" / "gba_bk-1" / "battery" / "Backup Game.srm";
            auto stale = rows[0];
            ApplyReport sr = apply_row(stale, g, ctx, U8(vault()), 1700000050);
            CHECK(!sr.ok && sr.written == 0 && rd(t2) == fill(1024, 'N'));
        }
        rows = build_rows({found(Source::MGBA, src)}, {g}, ctx);
        rep = apply_row(rows[0], g, ctx, U8(vault()), 1700000100);
        CHECK(rep.ok && rep.written == 0 && rep.unchanged == 1 && rep.backed_up == 0);
        /* another different import backs up again (separate file, nothing overwritten) */
        wr(src, fill(1024, 'P'), 10);
        rows = build_rows({found(Source::MGBA, src)}, {g}, ctx);
        rep = apply_row(rows[0], g, ctx, U8(vault()), 1700000200);
        CHECK(rep.ok && rep.backed_up == 1 && rd(target) == fill(1024, 'P'));
        CHECK(rd(vault() / "_conflicts" / "gba" / "gba_bk-1" / "battery" / "Backup Game.srm.import.20231114-221320") == fill(1024, 'O'));
        /* if the backup cannot be made, the save is NOT overwritten */
        fs::remove_all(vault() / "_conflicts", ec);
        wr(vault() / "_conflicts", "i am a file, so no folder can be made here");
        wr(src, fill(1024, 'Q'), 10);
        rows = build_rows({found(Source::MGBA, src)}, {g}, ctx);
        rep = apply_row(rows[0], g, ctx, U8(vault()), 1700000300);
        CHECK(!rep.ok && rep.written == 0 && rd(target) == fill(1024, 'P'));
        fs::remove(vault() / "_conflicts", ec);
    }

    /* ---- sources: VBA / melonDS .sav, next-to-ROM ---- */
    {
        Ctx ctx; ctx.battery_dir = bat;
        fs::path d = g_root / "vba";
        wr(d / "Zelda Minish Cap.sav", fill(32768, 'Z'));
        wr(d / "Pokemon Black.sav", fill(524288, 'B'));
        wr(d / "notes.txt", "ignore me");
        auto f1 = scan_folder({Source::VBA, U8(d)});
        CHECK(f1.size() == 2);
        std::vector<GameRef> gs = {game("gba:zmc", "gba", "The Legend of Zelda: The Minish Cap", "Zelda Minish Cap", "C:\\r\\Zelda Minish Cap.gba"),
                                   game("nds:pb", "nds", "Pokemon Black", "Pokemon Black", "C:\\r\\Pokemon Black.nds"),
                                   game("n64:00112233445566778899aabbccddeeff", "n64", "Zelda Minish Cap", "", "C:\\r\\x.z64")};
        auto rows = build_rows(f1, gs, ctx);
        CHECK(rows.size() == 2);   // .sav never matches the N64 game of the same name
        for (auto& r : rows) CHECK(r.problem.empty() && r.selected && r.out_rel[0].size() > 4 && r.out_rel[0].substr(r.out_rel[0].size() - 4) == ".srm");
        /* next to the ROM */
        fs::path rom = g_root / "romdir" / "Pokemon Black.nds";
        wr(rom, "rom");
        wr(g_root / "romdir" / "Pokemon Black.sav", fill(100, 'S'));
        wr(g_root / "romdir" / "saves" / "Pokemon Black.srm", fill(100, 'R'));
        auto nb = scan_next_to_rom(U8(rom), "Pokemon Black");
        CHECK(nb.size() == 2);
        rows = build_rows(nb, {game("nds:pb2", "nds", "Pokemon Black", "Pokemon Black", U8(rom))}, ctx);
        CHECK(rows.size() == 2 && rows[0].how == How::Beside);
        CHECK(rows[0].selected != rows[1].selected || rows[0].src_mtime == rows[1].src_mtime);   // one vault file: not both pre-selected unless tied...
        int sel = 0; for (auto& r : rows) sel += r.selected;
        CHECK(sel == 1);
        /* ambiguous: two library games fit the same file */
        std::vector<GameRef> two = {game("gb:a", "gb", "Tetris", "Tetris (World)"), game("gb:b", "gb", "Tetris", "Tetris (Japan)")};
        wr(g_root / "amb" / "Tetris.sav", fill(100, 'T'));
        rows = build_rows(scan_folder({Source::MGBA, U8(g_root / "amb")}), two, ctx);
        CHECK(rows.size() == 1 && rows[0].ambiguous && !rows[0].selected && rows[0].how == How::Title);
    }

    /* ---- N64: Project64 files, mupen names ---- */
    {
        fs::path rom = g_root / "n64" / "Super Mario 64.z64";
        wr(rom, z64_header("SUPER MARIO 64"));
        const std::string md5 = "3E3F1A31AAAAAAAAAAAAAAAAAAAAAAAA";
        GameRef g = game("n64:3e3f1a31aaaaaaaaaaaaaaaaaaaaaaaa", "n64", "Super Mario 64", "", U8(rom));
        Ctx ctx; ctx.battery_dir = bat; ctx.n64_ini = kIni;
        fs::path pj = g_root / "pj64" / "Save";
        wr(pj / "SUPER MARIO 64.eep", fill(512, 'e'));
        wr(pj / "SUPER MARIO 64.sra", fill(32768, 's'));
        wr(pj / "SUPER MARIO 64.fla", fill(131072, 'f'));
        wr(pj / "SUPER MARIO 64_Cont_2.mpk", blank_mempak("pj"));
        wr(pj / "SUPER MARIO 64.eep.bak", "junk");
        auto fs_ = scan_folder({Source::Project64, U8(pj)});
        CHECK(fs_.size() == 4);
        auto rows = build_rows(fs_, {g}, ctx);
        CHECK(rows.size() == 4);
        std::string base = "Super Mario 64 (U) [!]-3E3F1A31";
        int checked = 0;
        for (auto& r : rows) {
            CHECK(r.problem.empty() && r.selected && r.how == How::Stem && r.out_rel.size() == 1);   // the ROM's file stem equals the header name here
            std::vector<OutFile> o; std::string why;
            CHECK(convert(r, g, ctx, &o, &why) && o.size() == 1);
            if (r.files[0].ext == ".eep") { CHECK(o[0].rel == base + ".eep" && o[0].data.size() == 2048 && o[0].data.substr(0, 512) == fill(512, 'e') && o[0].data[512] == '\xFF'); ++checked; }
            if (r.files[0].ext == ".sra") { CHECK(o[0].rel == base + ".sra" && o[0].data == fill(32768, 's')); ++checked; }
            if (r.files[0].ext == ".fla") { CHECK(o[0].rel == base + ".fla" && o[0].data == fill(131072, 'f')); ++checked; }
            if (r.files[0].ext == ".mpk") {
                CHECK(o[0].rel == base + ".mpk" && o[0].data.size() == 131072 && r.controller == 1);
                CHECK(o[0].data.substr(32768, 32768) == blank_mempak("pj") && o[0].data.substr(0, 32768) == blank_mempak(g.key + "#0"));   // controller 2 slot
                ++checked;
            }
        }
        CHECK(checked == 4);
        /* an existing vault mempak file keeps the other slots */
        std::string existing = blank_mempak("a") + blank_mempak("b") + blank_mempak("c") + blank_mempak("d");
        wr(U8(vault() / "n64" / sk(g.key) / "battery" / (base + ".mpk")), existing, 4000);
        for (auto& r : build_rows(fs_, {g}, ctx)) if (r.files[0].ext == ".mpk") {
            std::vector<OutFile> o; std::string why;
            CHECK(convert(r, g, ctx, &o, &why) && o[0].data.substr(0, 32768) == blank_mempak("a") && o[0].data.substr(32768, 32768) == blank_mempak("pj") &&
                  o[0].data.substr(65536) == blank_mempak("c") + blank_mempak("d"));
        }
        /* wrong sizes are refused with a reason, not imported */
        wr(g_root / "pj64b" / "SUPER MARIO 64.sra", fill(1234, 'x'));
        rows = build_rows(scan_folder({Source::Project64, U8(g_root / "pj64b")}), {g}, ctx);
        CHECK(rows.size() == 1 && !rows[0].problem.empty() && !rows[0].selected);
        /* unknown to the ini: "<header name>-<md5>" */
        GameRef g2 = game("n64:99999999aaaaaaaaaaaaaaaaaaaaaaaa", "n64", "Super Mario 64", "", U8(rom));
        rows = build_rows(scan_folder({Source::Project64, U8(pj)}), {g2}, ctx);
        std::vector<OutFile> o; std::string why;
        for (auto& r : rows) if (r.files[0].ext == ".sra") CHECK(convert(r, g2, ctx, &o, &why) && o[0].rel == "SUPER MARIO 64-99999999.sra");
        /* an old-style "<goodname> (unknown rom).sra" already in the vault keeps its name (the core would keep using it) */
        wr(U8(vault() / "n64" / sk(g2.key) / "battery" / "SUPER MARIO 64 (unknown rom).sra"), fill(32768, 'k'));
        for (auto& r : build_rows(scan_folder({Source::Project64, U8(pj)}), {g2}, ctx)) if (r.files[0].ext == ".sra") CHECK(convert(r, g2, ctx, &o, &why) && o[0].rel == "SUPER MARIO 64 (unknown rom).sra");
        /* nothing known about the name (no ini entry, no readable ROM): refuse instead of guessing */
        GameRef g3 = game("n64:77777777aaaaaaaaaaaaaaaaaaaaaaaa", "n64", "Super Mario 64", "", "C:\\no\\such\\rom.z64");
        for (auto& r : build_rows(scan_folder({Source::Project64, U8(pj)}), {g3}, ctx)) CHECK(!r.problem.empty());
        (void)md5;
    }

    /* ---- N64: RetroArch single .srm ---- */
    {
        GameRef g = game("n64:3e3f1a31aaaaaaaaaaaaaaaaaaaaaaaa", "n64", "Super Mario 64", "", "");
        Ctx ctx; ctx.battery_dir = bat; ctx.n64_ini = kIni;
        std::string srm(0x48800, '\xFF');
        std::memset(&srm[0], 0x11, 0x800); srm[7] = 0x12;                    // eeprom (a uniform block counts as blank)
        std::string pak = blank_mempak("ra");
        pak[256 + 2 * 9 + 1] = 5;                                              // a note on pak 1
        const std::string zpak = blank_mempak("z");
        for (int i = 0; i < 4; ++i) std::memcpy(&srm[0x800 + (size_t)i * 0x8000], i == 0 ? pak.data() : zpak.data(), 0x8000);
        std::memset(&srm[0x20800], 0x22, 0x8000); srm[0x20800 + 9] = 0x23;     // sram
        // flash stays erased (0xFF): not imported
        fs::path f = g_root / "ra64" / "Super Mario 64.srm";
        wr(f, srm);
        Found fd = found(Source::RetroArch, f);
        auto rows = build_rows({fd}, {g}, ctx);
        CHECK(rows.size() == 1 && rows[0].problem.empty() && rows[0].out_rel.size() == 3);
        std::vector<OutFile> o; std::string why;
        CHECK(convert(rows[0], g, ctx, &o, &why) && o.size() == 3);
        std::string base = "Super Mario 64 (U) [!]-3E3F1A31";
        CHECK(o[0].rel == base + ".eep" && o[0].data == srm.substr(0, 0x800));
        CHECK(o[1].rel == base + ".mpk" && o[1].data.size() == 131072 && o[1].data.substr(0, 32768) == pak);
        CHECK(o[2].rel == base + ".sra" && o[2].data == srm.substr(0x20800, 0x8000));
        /* a .srm of another size is not an N64 save */
        wr(g_root / "ra64b" / "Super Mario 64.srm", fill(1000, 'x'));
        CHECK(build_rows({found(Source::RetroArch, g_root / "ra64b" / "Super Mario 64.srm")}, {g}, ctx).empty());
        /* a blank one has nothing to import */
        wr(g_root / "ra64c" / "Super Mario 64.srm", std::string(0x48800, '\xFF'));
        rows = build_rows({found(Source::RetroArch, g_root / "ra64c" / "Super Mario 64.srm")}, {g}, ctx);
        CHECK(rows.size() == 1 && !rows[0].problem.empty());
        /* apply writes all of them */
        ApplyReport rep = apply_row(build_rows({fd}, {g}, ctx)[0], g, ctx, U8(vault()), 1700001000);
        CHECK(rep.ok && rep.written == 3 && fs::exists(vault() / "n64" / sk(g.key) / "battery" / (base + ".sra")));
    }

    /* ---- GameCube: Dolphin raw card and GCI folder ---- */
    {
        Ctx ctx; ctx.battery_dir = bat;
        GameRef g = game("gc:gale01-0", "gc", "Super Smash Bros. Melee", "", "", "GALE01");
        GameRef other = game("gc:gm4e01-0", "gc", "Mario Kart", "", "", "GM4E01");
        fs::path card = g_root / "dolphin" / "GC" / "MemoryCardA.USA.raw";
        wr(card, raw_card());
        wr(g_root / "dolphin" / "GC" / "RetroHub" / "x" / "GCI" / "01-GALE-own.gci", "must not be scanned");
        auto fs_ = scan_folder({Source::Dolphin, U8(g_root / "dolphin" / "GC")});
        CHECK(fs_.size() == 1);
        auto rows = build_rows(fs_, {g, other}, ctx);
        CHECK(rows.size() == 1 && rows[0].game == 0 && rows[0].how == How::Content && rows[0].gc_game == "GALE" && rows[0].problem.empty());
        CHECK(rows[0].out_rel.size() == 1 && rows[0].out_rel[0] == "GCI/01-GALE-smash_save.gci");
        ApplyReport rep = apply_row(rows[0], g, ctx, U8(vault()), 1700002000);
        CHECK(rep.ok && rep.written == 1);
        std::string gci = rd(vault() / "gc" / sk(g.key) / "battery" / "GCI" / "01-GALE-smash_save.gci");
        CHECK(gci.size() == 0x40 + 0x4000 && gci.substr(0, 4) == "GALE");
        /* GCI folder: grouped per game */
        fs::path gf = g_root / "dolphin2" / "GC" / "USA" / "Card A";
        std::string gci1 = gci, gci2 = gci;
        wr(gf / "01-GALE-a.gci", gci1);
        wr(gf / "01-GALE-b.gci", gci2);
        std::string mk = gci; std::memcpy(&mk[0], "GM4E", 4);
        wr(gf / "01-GM4E-m.gci", mk);
        rows = build_rows(scan_folder({Source::Dolphin, U8(g_root / "dolphin2" / "GC")}), {g, other}, ctx);
        CHECK(rows.size() == 2);
        for (auto& r : rows) if (r.game == 0) CHECK(r.files.size() == 2 && r.out_rel.size() == 2); else CHECK(r.files.size() == 1);
    }

    /* ---- PS2: PCSX2 card ---- */
    {
        Ctx ctx; ctx.battery_dir = bat;
        GameRef g = game("ps2:slus-20312", "ps2", "Some PS2 Game", "", "", "");
        GameRef other = game("ps2:slus-99999", "ps2", "Other", "", "", "");
        std::string card(8388608, '\0');
        std::memcpy(&card[4096], "BASLUS-20312DATA", 16);
        fs::path c = g_root / "pcsx2" / "memcards" / "Mcd001.ps2";
        wr(c, card);
        wr(g_root / "pcsx2" / "memcards" / "small.ps2", fill(1000, 'x'));
        wr(g_root / "pcsx2" / "memcards" / "RetroHub" / "ps2_x" / "Mcd001.ps2", card);   // own cards never offered
        auto fs_ = scan_folder({Source::PCSX2, U8(g_root / "pcsx2" / "memcards")});
        CHECK(fs_.size() == 2);
        auto rows = build_rows(fs_, {g, other}, ctx);
        CHECK(rows.size() == 1 && rows[0].game == 0 && rows[0].out_rel[0] == "memcards/RetroHub/ps2_slus-20312/Mcd001.ps2");
        ApplyReport rep = apply_row(rows[0], g, ctx, U8(vault()), 1700003000);
        CHECK(rep.ok && fs::file_size(vault() / "ps2" / sk(g.key) / "battery" / "memcards" / "RetroHub" / "ps2_slus-20312" / "Mcd001.ps2") == 8388608);
    }

    /* ---- dedupe: two sources for one vault file, newest pre-selected ---- */
    {
        Ctx ctx; ctx.battery_dir = bat;
        GameRef g = game("gba:dd", "gba", "Dup Game", "Dup Game");
        wr(g_root / "dd1" / "Dup Game.sav", fill(100, '1'), 3000);
        wr(g_root / "dd2" / "Dup Game.srm", fill(100, '2'), 10);
        auto rows = build_rows({found(Source::MGBA, g_root / "dd1" / "Dup Game.sav"), found(Source::RetroArch, g_root / "dd2" / "Dup Game.srm")}, {g}, ctx);
        CHECK(rows.size() == 2);
        int sel = 0; for (auto& r : rows) if (r.selected) { ++sel; CHECK(r.src() == Source::RetroArch); }
        CHECK(sel == 1);
    }

    fs::remove_all(g_root, ec);
    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("save_import_test OK\n");
    return 0;
}
