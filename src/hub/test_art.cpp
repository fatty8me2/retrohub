/* Console test for cover-art logic. Usage: hub_art_test [--live <cache_dir> [rom_path]] */
#include "art.h"
#include "art_match.h"
#include "art_lru.h"
#include "library.h"
#include "games.h"

#include <cstdio>
#include <cstring>
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
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL OK\n", g_fail);
    return g_fail ? 1 : 0;
}
