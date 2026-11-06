/* Console test for cover-art logic. Usage: hub_art_test [--live <cache_dir> [rom_path]] */
#include "art.h"
#include "art_match.h"
#include "art_lru.h"
#include "library.h"
#include "games.h"
#include "gametdb.h"
#include "netgate.h"

#include <cstdio>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace hm = hubart_match;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
#define CHECK_EQ(a, b) do { std::string _a = (a), _b = (b); if (_a != _b) { std::printf("FAIL line %d: '%s' != '%s'\n", __LINE__, _a.c_str(), _b.c_str()); ++g_fail; } } while (0)

static const char* kSample = R"DAT(clrmamepro (
	name "Sample"
)

game (
	name "Super Mario 64 (Europe) (En,Fr,De)"
	region "Europe"
	serial "NSMP"
	rom ( name "Super Mario 64 (Europe) (En,Fr,De).z64" size 8388608 crc 1111AAAA md5 AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA1 sha1 00 serial "NSMP" )
)
game (
	name "Super Mario 64 (USA)"
	region "USA"
	serial "NSME"
	rom ( name "Super Mario 64 (USA).z64" size 8388608 crc 635A2BFF md5 20B854B239203BAF6C961B850A4A51A2 sha1 00 serial "NSME" )
)
game (
	name "Super Mario 64 (USA)"
	region "USA"
	serial "NSME"
	rom ( name "Super Mario 64 (USA).v64" size 8388608 crc 3A2B1C0D md5 BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB2 sha1 00 serial "NSME" )
)
game (
	name "Super Mario 64 (Japan) (Rev 1)"
	region "Japan"
	serial "NSMJ"
	rom ( name "Super Mario 64 (Japan) (Rev 1).z64" size 8388608 crc 0 md5 CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC3 serial "NSMJ" )
)
game (
	name "Pokemon - Emerald Version (USA, Europe)"
	region "USA"
	serial "BPEE"
	rom ( name "Pokemon - Emerald Version (USA, Europe).gba" size 16777216 crc 1F1C08FB md5 DDDD sha1 00 serial "BPEE" )
)
game (
	name "Pokemon - Emerald Version (Europe) (Proto)"
	serial "BPEE"
	rom ( name "x.gba" size 1 crc 12345678 md5 EEEE serial "BPEE" )
)
game (
	name "Pokemon - Red Version (USA, Europe) (SGB Enhanced)"
	region "USA"
	rom ( name "Pokemon - Red Version (USA, Europe) (SGB Enhanced).gb" size 1 crc 9F7F2B8F md5 FFFF )
)
game (
	name "Tetris (World) (Rev 1)"
	rom ( name "Tetris (World) (Rev 1).gb" size 1 crc 46DF91AD md5 1234 )
)
game (
	name "Tetris (Japan) (En)"
	rom ( name "Tetris (Japan) (En).gb" size 1 crc 1 md5 5678 )
)
game (
	name "Super Smash Bros. Melee (USA)"
	serial "DL-DOL-GALE-USA"
	rom ( name "Super Smash Bros. Melee (USA).iso" size 1 crc 1 md5 9999 serial "DL-DOL-GALE-USA" )
)
game (
	name "Super Smash Bros. Melee (USA) (Rev 1)"
	serial "DL-DOL-GALE-USA"
	rom ( name "Super Smash Bros. Melee (USA) (Rev 1).iso" size 1 crc 2 md5 9998 serial "DL-DOL-GALE-USA" )
)
)DAT";

static void test_lru() {
    hubart::Lru l;
    for (int i = 0; i < 10; ++i) { l.tick(); l.touch("a" + std::to_string(i)); }
    CHECK(l.victims(10).empty());
    l.tick(); l.tick();
    auto v = l.victims(7);
    CHECK(v.size() == 3 && v[0] == "a0" && v[1] == "a1" && v[2] == "a2");   // oldest first
    l.touch("a0");                                                           // drawn now: protected
    v = l.victims(7);
    CHECK(v.size() == 3 && v[0] == "a1");
    for (auto& id : v) l.remove(id);
    CHECK(l.size() == 7);
    hubart::Lru b;                                                           // everything drawn this frame: nothing evictable
    b.tick();
    for (int i = 0; i < 5; ++i) b.touch("x" + std::to_string(i));
    CHECK(b.victims(2).empty());
    b.tick();
    CHECK(b.victims(2).empty());                                             // still within kMinIdleFrames
    b.tick();
    CHECK(b.victims(2).size() == 3);
}

int main(int argc, char** argv) {
    test_lru();
    /* ---- naming / url ---- */
    CHECK_EQ(hm::thumb_name("Mario & Luigi - Superstar Saga (USA)"), "Mario _ Luigi - Superstar Saga (USA)");
    CHECK_EQ(hm::url_encode(hm::thumb_name("Mario & Luigi - Superstar Saga (USA)")),
             "Mario%20_%20Luigi%20-%20Superstar%20Saga%20(USA)");
    CHECK_EQ(hm::thumb_name("a&b*c/d:e`f<g>h?i\\j|k"), "a_b_c_d_e_f_g_h_i_j_k");
    CHECK_EQ(hm::url_encode("Pokémon, #1+2"), "Pok%C3%A9mon%2C%20%231%2B2");
    CHECK_EQ(hm::thumb_url("n64", 0, "Super Mario 64 (USA)"),
             "https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Nintendo_64/master/Named_Boxarts/Super%20Mario%2064%20(USA).png");
    CHECK_EQ(hm::thumb_url("gc", 1, "F-Zero GX (USA)"),
             "https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_GameCube/master/Named_Titles/F-Zero%20GX%20(USA).png");
    CHECK_EQ(hm::thumb_url("3ds", 0, "Pokemon X (USA) (En,Ja,Fr,De,Es,It,Ko)"),
             "https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Nintendo_3DS/master/Named_Boxarts/Pokemon%20X%20(USA)%20(En%2CJa%2CFr%2CDe%2CEs%2CIt%2CKo).png");
    CHECK_EQ(hm::thumb_url("wiiu", 0, "Legend of Zelda, The - Twilight Princess HD (USA) (En,Fr,Es) (Rev 2)").substr(0, 78),
             "https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Wii_U/master/");
    CHECK(hm::thumb_url("xx", 0, "a").empty());
    /* gbc art falls back to the Game Boy repo (dual-mode carts like Yellow are filed there) */
    {
        const std::string yellow = "Pokemon - Yellow Version - Special Pikachu Edition (USA, Europe) (CGB+SGB Enhanced)";
        auto u = hm::thumb_urls("gbc", 0, yellow);
        CHECK(u.size() == 2);
        if (u.size() == 2) {
            CHECK_EQ(u[0], hm::thumb_url("gbc", 0, yellow));
            CHECK_EQ(u[1], hm::thumb_url("gb", 0, yellow));
            CHECK(u[1].find("/Nintendo_-_Game_Boy/master/Named_Boxarts/") != std::string::npos);
        }
        CHECK(hm::thumb_urls("gb", 0, yellow).size() == 1);
        CHECK(hm::thumb_urls("n64", 0, "Super Mario 64 (USA)").size() == 1);
        CHECK(hm::thumb_urls("xx", 0, "a").empty());
    }

    /* ---- DAT parse ---- */
    auto dat = hm::parse_dat(kSample);
    CHECK(dat.size() == 11);
    CHECK_EQ(dat[1].name, "Super Mario 64 (USA)");
    CHECK_EQ(dat[1].serial, "NSME");
    CHECK_EQ(dat[1].md5, "20B854B239203BAF6C961B850A4A51A2");
    CHECK_EQ(dat[1].crc, "635A2BFF");
    CHECK_EQ(dat[6].serial, "");


    /* ---- ghost (friend-owned) entries: same resolution as an owned copy, even for systems the user owns nothing of ---- */
    {
        static const char* kPs = R"DAT(game (
	name "Woody Woodpecker Racing (Europe) (En,Fr,De,Es,It)"
	rom ( name "a.bin" size 1 crc FCF215DE md5 AA serial "SLES-02972" )
)
game (
	name "Woody Woodpecker Racing (USA)"
	rom ( name "b.bin" size 1 crc 670AEB6E md5 BB serial "SLUS-01188" )
)
game (
	name "Crash Bash (USA)"
	rom ( name "c.bin" size 1 crc 1 md5 CC serial "SCUS-94570" )
)
)DAT";
        auto pd = hm::parse_dat(kPs);
        std::string how;
        CHECK_EQ(hm::serial_from_key("ps1", "ps1:slus-01188"), "slus-01188");
        CHECK_EQ(hm::serial_from_key("ps1", "ps1:file-foo"), "");
        CHECK_EQ(hm::serial_from_key("gc", "gc:gm4e01-0"), "gm4e01");
        CHECK_EQ(hm::serial_from_key("nes", "nes:abcd1234"), "");
        hm::Query g; g.system = "ps1"; g.title = "Woody Woodpecker Racing"; g.serial = hm::serial_from_key("ps1", "ps1:slus-01188"); g.fuzzy = true;
        CHECK_EQ(hm::match_name(pd, g, &how), "Woody Woodpecker Racing (USA)");   // by serial
        CHECK_EQ(how, "serial");
        g.serial.clear();
        CHECK_EQ(hm::match_name(pd, g, &how), "Woody Woodpecker Racing (USA)");   // by title, USA release wins
        CHECK_EQ(how, "title");
        g.title = "Woody Woodpecker Racing - Special"; 
        CHECK_EQ(hm::match_name(pd, g, &how), "Woody Woodpecker Racing (USA)");     // fuzzy: DAT name is a prefix of the title
        g.title = "Totally Different Game";
        CHECK_EQ(hm::match_name(pd, g, &how), "");
        g.title = "Crash Bash Party Edition";
        CHECK_EQ(hm::match_name(pd, g, &how), "Crash Bash (USA)");                  // fuzzy: DAT name is a prefix
        CHECK_EQ(how, "fuzzy");
        g.fuzzy = false;
        CHECK_EQ(hm::match_name(pd, g, &how), "");
        /* every system the hub knows has a thumbnail repo and a DAT, so a ghost never depends on owned games */
        for (const char* s : {"n64", "gba", "gb", "gbc", "gc", "nds", "3ds", "ps1", "ps2", "nes", "snes", "wiiu"}) {
            CHECK(!hm::thumb_repo(s).empty());
            CHECK(!hm::dat_url(s).empty());
        }
    }

    /* ---- matching ---- */
    std::string how;
    hm::Query q;
    q.system = "n64"; q.md5 = "20b854b239203baf6c961b850a4a51a2"; q.serial = "NSMP";
    CHECK_EQ(hm::match_name(dat, q, &how), "Super Mario 64 (USA)");   // md5 beats serial
    CHECK_EQ(how, "md5");
    q = {}; q.system = "n64"; q.serial = "nsmp";
    CHECK_EQ(hm::match_name(dat, q, &how), "Super Mario 64 (Europe) (En,Fr,De)");
    CHECK_EQ(how, "serial");
    q = {}; q.system = "n64"; q.serial = "NSMJ";
    CHECK_EQ(hm::match_name(dat, q), "Super Mario 64 (Japan) (Rev 1)");
    q = {}; q.system = "gba"; q.serial = "BPEE"; q.crc = "deadbeef";   // crc miss -> serial; USA before proto
    CHECK_EQ(hm::match_name(dat, q, &how), "Pokemon - Emerald Version (USA, Europe)");
    CHECK_EQ(how, "serial");
    q.crc = "12345678";                                                // exact crc wins (even a proto)
    CHECK_EQ(hm::match_name(dat, q, &how), "Pokemon - Emerald Version (Europe) (Proto)");
    CHECK_EQ(how, "crc");
    q = {}; q.system = "gb"; q.title = "TETRIS";
    CHECK_EQ(hm::match_name(dat, q, &how), "Tetris (World) (Rev 1)");  // World preferred over Japan
    CHECK_EQ(how, "title");
    q = {}; q.system = "gb"; q.title = "POKEMON RED";                   // prefix on GB only
    CHECK_EQ(hm::match_name(dat, q), "Pokemon - Red Version (USA, Europe) (SGB Enhanced)");
    q.system = "n64";
    CHECK_EQ(hm::match_name(dat, q), "");
    q = {}; q.system = "gc"; q.serial = "GALE01";
    CHECK_EQ(hm::match_name(dat, q, &how), "Super Smash Bros. Melee (USA)");  // original over Rev 1
    q = {}; q.system = "n64"; q.title = "NOTHING HERE";
    CHECK_EQ(hm::match_name(dat, q), "");
    CHECK(hm::release_score("A (USA)") < hm::release_score("A (Europe)"));
    CHECK(hm::release_score("A (Europe)") < hm::release_score("A (World)"));
    CHECK(hm::release_score("A (World)") < hm::release_score("A (Japan)"));
    CHECK(hm::release_score("A (USA) (Proto)") > hm::release_score("A (Japan)"));
    CHECK_EQ(hm::normalize_title("Pokemon - Red Version (USA) [!]"), "pokemonredversion");

    /* ---- pack ghosts: hint, article placement, accents, region, aliases ---- */
    {
        std::vector<hm::DatRec> d = {{"Legend of Zelda, The - A Link to the Past (USA)", "", "", ""},
                                     {"Legend of Zelda, The - A Link to the Past (Europe)", "", "", ""},
                                     {"Pok" "é" "mon - Platinum Version (USA)", "", "", ""},
                                     {"Mario & Luigi - Superstar Saga (USA)", "", "", ""},
                                     {"GoldenEye 007 (USA)", "", "", ""}};
        hm::Query q; q.system = "snes"; q.title = "The Legend of Zelda - A Link to the Past";
        std::string how;
        CHECK_EQ(hm::match_name(d, q, &how), "Legend of Zelda, The - A Link to the Past (USA)");
        q.hint = "Legend of Zelda, The - A Link to the Past (Europe)";   // the exact name from the pack wins, USA preference does not
        CHECK_EQ(hm::match_name(d, q, &how), "Legend of Zelda, The - A Link to the Past (Europe)");
        CHECK_EQ(how, "hint");
        q.hint = "legend of zelda, the - a link to the past (europe)";    // case-insensitive
        CHECK_EQ(hm::match_name(d, q), "Legend of Zelda, The - A Link to the Past (Europe)");
        q.hint = "Legend of Zelda, The - A Link to the Past (Europe).sfc"; // not an exact name: falls back to the title, its region tag asks for Europe
        CHECK_EQ(hm::match_name(d, q), "Legend of Zelda, The - A Link to the Past (Europe)");
        q = {}; q.system = "gba"; q.title = "Pokemon - Platinum Version";   // accent folded
        CHECK_EQ(hm::match_name(d, q), "Pok" "é" "mon - Platinum Version (USA)");
        q = {}; q.system = "gba"; q.title = "Mario and Luigi - Superstar Saga";   // & = and
        CHECK_EQ(hm::match_name(d, q), "Mario & Luigi - Superstar Saga (USA)");
        CHECK_EQ(hm::title_key("Legend of Zelda, The - X (USA)"), hm::title_key("The Legend of Zelda - X"));
        CHECK_EQ(hm::region_of("A (Europe) (En,Fr)"), "Europe");
        CHECK_EQ(hm::region_of("A (Japan, USA)"), "Japan");
        CHECK_EQ(hm::region_of("Pikmin"), "");
        CHECK_EQ(hm::match_hint(d, "goldeneye 007 (usa)"), "GoldenEye 007 (USA)");
        CHECK_EQ(hm::match_hint(d, "GoldenEye 007"), "");
        CHECK_EQ(hm::title_alias("n64", "007 - GoldenEye"), "GoldenEye 007 (USA)");
        CHECK_EQ(hm::title_alias("gba", "007 - GoldenEye"), "");
        CHECK(hm::tree_url("gc").find("libretro-thumbnails/Nintendo_-_GameCube/git/trees/master") != std::string::npos);
        CHECK(hm::tree_url("bogus").empty());
    }

    /* ---- json map ---- */
    std::map<std::string, std::string> m{{"n64:abc", "Mario \"Kart\" 64 (USA)"}, {"k\\2", "line\nbreak\t\xC3\xA9"}}, m2;
    CHECK(hm::map_from_json(hm::map_to_json(m), &m2));
    CHECK(m == m2);
    CHECK(hm::map_from_json("{}", &m2) && m2.empty());
    CHECK(!hm::map_from_json("garbage", &m2));

    std::printf(g_fail ? "UNIT TESTS FAILED (%d)\n" : "unit tests passed\n", g_fail);

    /* ---- live check ---- */
    if (argc >= 3 && !std::strcmp(argv[1], "--live")) {
        hubart::detail::init_headless(argv[2]);
        struct L { const char* sys; const char* serial; const char* title; const char* expect; };
        const L cases[] = {
            {"n64", "NSME", "SUPER MARIO 64", "Super Mario 64 (USA)"},
            {"n64", "NKTE", "MARIOKART64", "Mario Kart 64 (USA)"},
            {"gba", "BPEE", "POKEMON EMER", "Pokemon - Emerald Version (USA, Europe)"},
            {"gba", "BZME", "ZELDA MC", "Legend of Zelda, The - The Minish Cap (USA)"},
            {"gc", "GALE01", "Super Smash Bros. Melee", "Super Smash Bros. Melee (USA)"},
        };
        for (const L& c : cases) {
            hub::GameEntry g;
            g.system = hub::system_from_short(c.sys);
            g.serial = c.serial;
            g.title = c.title;
            g.key = std::string(c.sys) + ":live-" + c.serial;
            std::string name, h;
            bool ok = hubart::detail::resolve_name(g, &name, &h);
            std::printf("[live] %s %s -> %s '%s' (%s) expected '%s'\n", c.sys, c.serial, ok ? "ok" : "NONE",
                        name.c_str(), h.c_str(), c.expect);
            if (!ok) { ++g_fail; continue; }
            for (auto kind : {hubart::Kind::Boxart, hubart::Kind::Title}) {
                int w = 0, hh = 0;
                std::string err, path;
                bool f = hubart::detail::fetch_decode(c.sys, name, kind, &w, &hh, &err, &path);
                std::printf("   %s: %s %dx%d %s%s\n", kind == hubart::Kind::Boxart ? "boxart" : "title ",
                            f ? "ok" : "FAIL", w, hh, f ? path.c_str() : err.c_str(), "");
                if (kind == hubart::Kind::Boxart && !f) ++g_fail;
            }
        }
        {   /* graceful miss: unknown name -> 404 recorded, second call is a known miss (no network) */
            int w = 0, hh = 0;
            std::string e1, e2;
            bool a = hubart::detail::fetch_decode("n64", "Smash Remix 2.0 (World) (Hack)", hubart::Kind::Boxart, &w, &hh, &e1, nullptr);
            bool b = hubart::detail::fetch_decode("n64", "Smash Remix 2.0 (World) (Hack)", hubart::Kind::Boxart, &w, &hh, &e2, nullptr);
            std::printf("[live] bogus name: %d '%s' then %d '%s'\n", a, e1.c_str(), b, e2.c_str());
            if (a || b || e2 != "known miss") ++g_fail;
        }
        if (argc >= 4) {
            hub::GameEntry g;
            if (!hub::identify_game(argv[3], &g)) {
                std::printf("[live] identify_game failed for %s\n", argv[3]);
            } else {
                std::string name, h, err;
                bool ok = hubart::detail::resolve_name(g, &name, &h);
                std::printf("[live] rom '%s' key=%s serial=%s title='%s' catalog='%s' -> %s '%s' (%s)\n", argv[3],
                            g.key.c_str(), g.serial.c_str(), g.title.c_str(), g.catalog_id.c_str(), ok ? "name" : "NO NAME",
                            name.c_str(), h.c_str());
                if (ok) {
                    int w = 0, hh = 0;
                    bool f = hubart::detail::fetch_decode(hub::system_short(g.system), name, hubart::Kind::Boxart, &w, &hh, &err, nullptr);
                    std::printf("   boxart: %s %dx%d %s\n", f ? "ok" : "no art", w, hh, err.c_str());
                }
            }
        }
    }
    /* ---- GameTDB: ids, regions, urls, database records (no network) ---- */
    {
        CHECK_EQ(gametdb::id_from_serial("gc", "gm4e01"), "GM4E01");
        CHECK_EQ(gametdb::id_from_serial("gc", "GM4E"), "");
        CHECK_EQ(gametdb::id_from_serial("wiiu", "ARDE"), "ARDE01");
        CHECK_EQ(gametdb::id_from_serial("wiiu", "ARDE01"), "ARDE01");
        CHECK_EQ(gametdb::id_from_serial("wiiu", "0005000010145C00"), "");   // a title id is not a product code
        CHECK_EQ(gametdb::id_from_serial("nds", "irbo"), "IRBO");
        CHECK_EQ(gametdb::id_from_serial("3ds", "ECLA"), "ECLA");
        CHECK_EQ(gametdb::id_from_serial("3ds", "0004000000055d00"), "");
        CHECK_EQ(gametdb::id_from_serial("n64", "NALE"), "");
        CHECK_EQ(gametdb::id_from_serial("gc", "GM4/01"), "");
        CHECK(gametdb::has_fullcover("gc") && gametdb::has_fullcover("wiiu") && gametdb::has_fullcover("nds") && gametdb::has_fullcover("3ds"));
        CHECK(!gametdb::has_fullcover("n64") && !gametdb::has_fullcover("switch") && !gametdb::has_fullcover("ps2"));
        CHECK_EQ(gametdb::db_name("gc"), "wii");
        CHECK_EQ(gametdb::db_url("wiiu"), "https://www.gametdb.com/wiiutdb.zip");
        CHECK_EQ(gametdb::db_url("nds"), "https://www.gametdb.com/dstdb.zip");
        CHECK(gametdb::db_url("n64").empty());

        /* region fallback: the id's own region, then the file name's hint, then US, EN, JA, the rest; no duplicates */
        auto r = gametdb::region_folders("GM4E01");
        CHECK(r.size() >= 8 && r[0] == "US" && r[1] == "EN" && r[2] == "JA");
        r = gametdb::region_folders("GM4P01");
        CHECK(r[0] == "EN" && r[1] == "US" && r[2] == "JA");
        r = gametdb::region_folders("GM4J01");
        CHECK(r[0] == "JA" && r[1] == "US" && r[2] == "EN");
        r = gametdb::region_folders("ECLA", "Japan");   // 'A' = all regions: the hint decides
        CHECK(r[0] == "JA" && r[1] == "US" && r[2] == "EN");
        r = gametdb::region_folders("ECLA");
        CHECK(r[0] == "US" && r[1] == "EN" && r[2] == "JA");
        r = gametdb::region_folders("GM4D01");
        CHECK(r[0] == "DE" && r[1] == "US" && r[2] == "EN");
        std::set<std::string> uniq(r.begin(), r.end());
        CHECK(uniq.size() == r.size());
        CHECK_EQ(gametdb::region_hint_from_name("Pokemon X (USA) (En,Ja,Fr)"), "USA");
        CHECK_EQ(gametdb::region_hint_from_name("Zelda (Europe) (En,Fr)"), "Europe");
        CHECK_EQ(gametdb::region_hint_from_name("Zelda (Japan)"), "Japan");
        CHECK_EQ(gametdb::region_hint_from_name("Plain Name"), "");
        CHECK_EQ(gametdb::region_hint_from_name("Name (En,Ja,Fr)"), "");

        /* urls: HQ first, then the standard size; GameCube is png and has no coverHQ; the others are jpg */
        auto u = gametdb::image_urls("gc", "GM4E01", gametdb::Img::CoverFull, "US");
        CHECK(u.size() == 2 && u[0] == "https://art.gametdb.com/wii/coverfullHQ/US/GM4E01.png" && u[1] == "https://art.gametdb.com/wii/coverfull/US/GM4E01.png");
        u = gametdb::image_urls("gc", "GM4E01", gametdb::Img::Cover, "US");
        CHECK(u.size() == 1 && u[0] == "https://art.gametdb.com/wii/cover/US/GM4E01.png");
        u = gametdb::image_urls("nds", "IRBO", gametdb::Img::CoverFull, "US");
        CHECK(u.size() == 2 && u[0] == "https://art.gametdb.com/ds/coverfullHQ/US/IRBO.jpg" && u[1] == "https://art.gametdb.com/ds/coverfull/US/IRBO.jpg");
        u = gametdb::image_urls("3ds", "ECLA", gametdb::Img::Cover, "EN");
        CHECK(u.size() == 2 && u[0] == "https://art.gametdb.com/3ds/coverHQ/EN/ECLA.jpg");
        u = gametdb::image_urls("wiiu", "ARDE01", gametdb::Img::CoverFull, "US");
        CHECK(u.size() == 2 && u[0] == "https://art.gametdb.com/wiiu/coverfullHQ/US/ARDE01.jpg");
        u = gametdb::image_urls("switch", "AAAAA", gametdb::Img::Cover, "US");
        CHECK(u.size() == 2 && u[0] == "https://art.gametdb.com/switch/coverHQ/US/AAAAA.jpg");
        CHECK(gametdb::image_urls("switch", "AAAAA", gametdb::Img::CoverFull, "US").empty());   // no full scans for Switch
        CHECK(gametdb::image_urls("n64", "NALE", gametdb::Img::Cover, "US").empty());
        CHECK(gametdb::image_urls("gc", "../../x", gametdb::Img::Cover, "US").empty());          // ids are alphanumeric only
        CHECK(gametdb::image_urls("gc", "GM4E01", gametdb::Img::Cover, "U/S").empty());

        /* the pseudo names the art worker passes around, and the url list they expand to */
        CHECK_EQ(hm::gametdb_name_for("GM4E01", "US"), "gametdb:GM4E01:US");
        CHECK_EQ(hm::gametdb_name_for("GM4E01", ""), "gametdb:GM4E01:*");
        CHECK_EQ(hm::gametdb_name_for("", "US"), "");
        std::string pid, preg;
        CHECK(hm::parse_gametdb_name("gametdb:ARDE01:US", &pid, &preg) && pid == "ARDE01" && preg == "US");
        CHECK(!hm::parse_gametdb_name("Super Mario 64 (USA)", &pid, &preg));
        auto tu = hm::thumb_urls("gc", 3, "gametdb:GM4E01:*");
        CHECK(tu.size() == 6 && tu[0] == "https://art.gametdb.com/wii/coverfullHQ/US/GM4E01.png" && tu[2].find("/EN/") != std::string::npos);   // 3 folders x (HQ, standard)
        tu = hm::thumb_urls("gc", 0, "gametdb:GM4E01:*");
        CHECK(tu.size() == 3 && tu[0] == "https://art.gametdb.com/wii/cover/US/GM4E01.png");
        tu = hm::thumb_urls("wiiu", 0, "gametdb:ARDE01:EN");
        CHECK(tu.size() == 6 && tu[0] == "https://art.gametdb.com/wiiu/coverHQ/EN/ARDE01.jpg" && tu[2].find("/US/") != std::string::npos);
        CHECK(hm::thumb_urls("gc", 1, "gametdb:GM4E01:*").empty() && hm::thumb_urls("gc", 2, "gametdb:GM4E01:*").empty());   // no title / snap from GameTDB
        CHECK(hm::thumb_urls("n64", 3, "Super Mario 64 (USA)").empty());   // a libretro name never has a full cover
        CHECK(hm::thumb_urls("n64", 0, "Super Mario 64 (USA)").size() == 1);
        CHECK(hm::thumb_urls("switch", 0, "gametdb:AAAAA:*").size() == 4);   // US, EN x (HQ, standard)
        tu = hm::thumb_urls("3ds", 3, "gametdb:BNEA:*");   // region 'A': 3 folders x (HQ, standard), then 5 more folders (HQ only), FR among them
        CHECK(tu.size() == 11 && tu[0] == "https://art.gametdb.com/3ds/coverfullHQ/US/BNEA.jpg" && tu.back().find("/NL/") != std::string::npos);
        bool has_fr = false;
        for (const auto& x : tu) if (x == "https://art.gametdb.com/3ds/coverfullHQ/FR/BNEA.jpg") has_fr = true;
        CHECK(has_fr);
        CHECK(hubart::Kind::FullCover != hubart::Kind::Boxart);

        /* database records: parse, tsv round trip, id / title lookup, region preference */
        const char* xml = R"X(<?xml version="1.0" encoding="UTF-8"?>
<datafile><WiiUTDB version="1" games="3"/>
	<game name="Super Mario 3D World (USA) (EN)">
		<id>ARDE01</id>
		<type>WiiU</type>
		<region>NTSC-U</region>
		<locale lang="FR">
			<title>SUPER MARIO 3D WORLD</title>
			<synopsis>Partez &amp; voyez.</synopsis>
		</locale>
		<locale lang="EN">
			<title>Super Mario 3D World</title>
			<synopsis>Leap into the first multiplayer Mario platformer!

Key Features:
Play as Mario &lt;or&gt; Luigi</synopsis>
		</locale>
		<developer>Nintendo EAD Tokyo</developer>
		<publisher>Nintendo</publisher>
		<date year="2013" month="11" day="22"/>
		<genre>action,platformer,3d platformer</genre>
		<rating type="ESRB" value="E"/>
		<rating type="PEGI" value="3"/>
		<input players="4"><control type="pad" required="false"/></input>
	</game>
	<game name="Super Mario 3D World (Europe) (EN,FR)">
		<id>ARDP01</id>
		<type>WiiU</type>
		<region>PAL</region>
		<locale lang="EN"><title>Super Mario 3D World</title><synopsis/></locale>
		<developer>Nintendo EAD Tokyo</developer>
		<publisher>Nintendo</publisher>
		<date year="2013" month="11" day="29"/>
	</game>
	<game name="Metroid (USA)"><id>GM8E01</id><type>GameCube</type><region>NTSC-U</region><locale lang="EN"><title>Metroid Prime</title><synopsis>Samus returns.</synopsis></locale></game>
	<game name="Broken"><type>WiiU</type></game>
</datafile>)X";
        auto recs = gametdb::parse_xml(xml);
        CHECK(recs.size() == 3);   // the entry without an id is dropped
        CHECK(recs[0].id == "ARDE01" && recs[0].title == "Super Mario 3D World" && recs[0].region == "NTSC-U");
        CHECK(recs[0].synopsis.find("Leap into") == 0 && recs[0].synopsis.find("<or>") != std::string::npos);   // EN locale chosen over FR; entities decoded
        CHECK(recs[0].developer == "Nintendo EAD Tokyo" && recs[0].publisher == "Nintendo" && recs[0].year == 2013 && recs[0].month == 11);
        CHECK(recs[0].esrb == "E" && recs[0].pegi == "PEGI 3" && recs[0].players == 4 && recs[0].genre.find("action") == 0);
        CHECK(recs[1].id == "ARDP01" && recs[1].synopsis.empty() && recs[1].esrb.empty());
        CHECK(gametdb::parse_xml(xml, "GameCube").size() == 1 && gametdb::parse_xml(xml, "GameCube")[0].id == "GM8E01");
        CHECK(gametdb::parse_xml("").empty() && gametdb::parse_xml("<game name=\"x\"><id>A").empty());   // truncated input: no crash, nothing
        auto back = gametdb::parse_tsv(gametdb::to_tsv(recs));
        CHECK(back.size() == 3 && back[0].synopsis == recs[0].synopsis && back[0].title == recs[0].title && back[0].year == 2013 && back[2].id == "GM8E01");
        gametdb::Index idx;
        idx.load(back);
        CHECK(idx.size() == 3 && idx.by_id("ARDP01") && idx.by_id("ARDP01")->region == "PAL" && !idx.by_id("NOPE01"));
        auto t = idx.by_title("Super Mario 3D World (USA) (En,Fr,Es) (Rev 2)");   // tags dropped, USA entry preferred
        CHECK(t && t->id == "ARDE01");
        t = idx.by_title("super mario 3d world", true);
        CHECK(t && t->id == "ARDE01" && !t->synopsis.empty());
        CHECK(!idx.by_title("Mario Party") && !idx.by_title(""));
        t = gametdb::find_record(idx, "wiiu", "0005000010145C00", {"Super Mario 3D World"});   // a Wii U title id finds it by title
        CHECK(t && t->id == "ARDE01");
        t = gametdb::find_record(idx, "wiiu", "ARDP", {"Super Mario 3D World"});             // the id's own entry has no synopsis: the same-title one is used
        CHECK(t && !t->synopsis.empty());
        t = gametdb::find_record(idx, "gc", "GM8E01", {"Wrong Title"});
        CHECK(t && t->id == "GM8E01");
        std::string xo;
        CHECK(!gametdb::unzip_xml("not a zip", &xo) && xo.empty());
    }

    /* ---- review fixes: bounded entity decoding, per-game searches, slim index, decode caps, one shared limiter ---- */
    {
        /* an '&' with no ';' within 10 characters is literal, and a hostile run of them is linear, not quadratic */
        std::string hostile = "<game name=\"x\"><id>ID1</id><type>WiiU</type><locale lang=\"EN\"><title>T</title><synopsis>";
        hostile.append(300000, '&');
        hostile += "&amp; ok;</synopsis></locale></game>";
        auto t0 = std::chrono::steady_clock::now();
        auto hr = gametdb::parse_xml(hostile);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(hr.size() == 1 && hr[0].synopsis.size() == 300000 + 1 + 4 && secs < 2.0);   // '&' x 300000, then "&amp;" -> '&', then " ok;"
        auto er = gametdb::parse_xml("<game name=\"x\"><id>A1</id><locale lang=\"EN\"><title>A &amp; B &#x41;&#66; &bogus; &lt</title><synopsis/></locale></game>");
        CHECK(er.size() == 1 && er[0].title == "A & B AB &bogus; &lt");

        /* every tag search stays inside its own <game>: the second game has no developer / no EN locale / no name and must not borrow the third's */
        auto sr = gametdb::parse_xml("<game name=\"One\"><id>A1</id><locale lang=\"FR\"><title>Un</title></locale></game>"
                                     "<game name=\"Two\"><id>B2</id></game>"
                                     "<game name=\"Three\"><id>C3</id><developer>Dev3</developer><locale lang=\"EN\"><title>Three EN</title><synopsis>s3</synopsis></locale>"
                                     "<date year=\"2001\" month=\"2\"/></game>");
        CHECK(sr.size() == 3 && sr[0].title == "Un" && sr[0].developer.empty() && sr[0].synopsis.empty());
        CHECK(sr[1].id == "B2" && sr[1].title == "Two" && sr[1].developer.empty() && sr[1].synopsis.empty() && sr[1].year == 0);
        CHECK(sr[2].title == "Three EN" && sr[2].developer == "Dev3" && sr[2].year == 2001);

        /* slim index: only id / region / title stay in memory, the rest is read from the file on demand (and a changed file is no record) */
        namespace fs2 = std::filesystem;
        fs2::path td = fs2::temp_directory_path() / "hub_art_slim_idx";
        fs2::create_directories(td);
        std::vector<gametdb::Rec> rs(3);
        rs[0].id = "AAAA"; rs[0].region = "NTSC-U"; rs[0].title = "Alpha Game"; rs[0].synopsis = "alpha text\nline two"; rs[0].developer = "DevA";
        rs[1].id = "BBBB"; rs[1].region = "PAL"; rs[1].title = "Alpha Game"; rs[1].publisher = "PubB";
        rs[2].id = "CCCC"; rs[2].region = "NTSC-J"; rs[2].title = "Gamma"; rs[2].synopsis = "gamma";
        { std::ofstream o(td / "t.tsv", std::ios::binary); std::string tsv = gametdb::to_tsv(rs); o.write(tsv.data(), (std::streamsize)tsv.size()); }
        gametdb::Index si;
        CHECK(si.load_file((td / "t.tsv").u8string()) && si.size() == 3);
        auto a = si.by_id("AAAA");
        CHECK(a && a->synopsis == "alpha text\nline two" && a->developer == "DevA");
        auto b = si.by_title("alpha game");
        CHECK(b && b->id == "AAAA");                       // USA before PAL
        b = si.by_title("Gamma (Japan)", true);
        CHECK(b && b->synopsis == "gamma");
        CHECK(!si.by_id("ZZZZ") && !si.load_file((td / "missing.tsv").u8string()));
        { std::ofstream o(td / "t.tsv", std::ios::binary | std::ios::trunc); o << "#changed\n"; }
        CHECK(!si.by_id("AAAA"));                          // the file changed under the index: nothing, not garbage
        std::error_code ec2;
        fs2::remove_all(td, ec2);

        /* image decode caps are checked from the header before any allocation */
        auto png_hdr = [](uint32_t w, uint32_t h) {
            std::string b("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR", 16);
            for (uint32_t v : {w, h}) for (int sh = 24; sh >= 0; sh -= 8) b += (char)((v >> sh) & 255);
            b += std::string("\x08\x06\0\0\0\0\0\0\0", 9);
            b += std::string("\0\0\0\0IDAT", 8);   // stb scans on to the first IDAT even for a header-only read
            return b;
        };
        int iw = 0, ih = 0;
        {   // positive control: a real 1x1 PNG
            static const unsigned char one[] = {0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x02,0x00,0x00,0x00,0x90,0x77,0x53,0xde,0x00,0x00,0x00,0x0c,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0xf8,0xcf,0xc0,0x00,0x00,0x03,0x01,0x01,0x00,0xc9,0xfe,0x92,0xef,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};
            CHECK(hubart::detail::image_header_ok(std::string((const char*)one, sizeof one), &iw, &ih) && iw == 1 && ih == 1);
        }
        CHECK(hubart::detail::image_header_ok(png_hdr(2048, 2048), &iw, &ih) && iw == 2048 && ih == 2048);
        CHECK(hubart::detail::image_header_ok(png_hdr(4000, 4000), &iw, &ih));          // 16 Mpx exactly
        CHECK(!hubart::detail::image_header_ok(png_hdr(4001, 4000), &iw, &ih));         // over 16 Mpx
        CHECK(!hubart::detail::image_header_ok(png_hdr(20000, 20000), &iw, &ih));       // a 400 Mpx bomb
        CHECK(!hubart::detail::image_header_ok(png_hdr(9000, 10), &iw, &ih));           // one side over 8192
        CHECK(!hubart::detail::image_header_ok(png_hdr(10, 8193), &iw, &ih));
        CHECK(hubart::detail::image_header_ok(png_hdr(8192, 100), &iw, &ih));
        CHECK(!hubart::detail::image_header_ok(png_hdr(0, 5), &iw, &ih));
        CHECK(!hubart::detail::image_header_ok("not an image at all", &iw, &ih) && !hubart::detail::image_header_ok("", &iw, &ih));

        /* one limiter for all gametdb.com traffic (art. and www. together), 1 s for the Wikimedia sites, per-site blocks from Retry-After */
        CHECK_EQ(netgate::site_of("art.gametdb.com"), "gametdb.com");
        CHECK_EQ(netgate::site_of("WWW.GameTDB.com"), "gametdb.com");
        CHECK_EQ(netgate::site_of("en.wikipedia.org"), "wikipedia.org");
        CHECK_EQ(netgate::site_of("raw.githubusercontent.com"), "raw.githubusercontent.com");
        CHECK_EQ(netgate::site_of("notgametdb.com"), "notgametdb.com");
        netgate::Gate g;
        using ms = std::chrono::milliseconds;
        const auto n0 = netgate::Clock::now();
        ms w{-1};
        CHECK(g.reserve("art.gametdb.com", n0, &w) && w.count() == 0);
        CHECK(g.reserve("www.gametdb.com", n0 + ms(100), &w) && w.count() == 400);          // same site: 0.5 s after the first, whichever host name
        CHECK(g.reserve("art.gametdb.com", n0 + ms(100), &w) && w.count() == 900);          // queued behind the second
        CHECK(g.reserve("art.gametdb.com", n0 + ms(5000), &w) && w.count() == 0);           // long idle: no wait
        CHECK(g.reserve("www.wikidata.org", n0, &w) && w.count() == 0);
        CHECK(g.reserve("en.wikipedia.org", n0, &w) && w.count() == 0);                      // separate site: independent
        CHECK(g.reserve("www.wikidata.org", n0 + ms(10), &w) && w.count() == 990);
        CHECK(g.reserve("raw.githubusercontent.com", n0, &w) && g.reserve("raw.githubusercontent.com", n0, &w) && w.count() == 0);   // no gap configured
        g.block("art.gametdb.com", n0 + ms(6000), 120);
        CHECK(!g.reserve("www.gametdb.com", n0 + ms(6000) + std::chrono::seconds(119), &w));      // blocked for the whole Retry-After, for every gametdb host
        CHECK(g.reserve("art.gametdb.com", n0 + ms(6000) + std::chrono::seconds(121), &w));
        CHECK(g.reserve("en.wikipedia.org", n0 + ms(6000), &w));                              // other sites unaffected
        g.block("art.gametdb.com", n0 + std::chrono::seconds(1000), 1);                       // a tiny Retry-After is raised to 30 s
        CHECK(!g.reserve("art.gametdb.com", n0 + std::chrono::seconds(1029), &w) && g.reserve("art.gametdb.com", n0 + std::chrono::seconds(1031), &w));
    }
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL OK\n", g_fail);
    return g_fail ? 1 : 0;
}
