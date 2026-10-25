// Console unit test for hubcommon (games.cpp). Exit code 0 = all passed.
#include "games.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
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

static std::vector<uint8_t> make_z64() {
    std::vector<uint8_t> v(1024 * 1024);
    v[0] = 0x80; v[1] = 0x37; v[2] = 0x12; v[3] = 0x40;
    uint32_t x = 12345;
    for (size_t i = 4; i < v.size(); ++i) {
        x = x * 1103515245u + 12345u;
        v[i] = uint8_t(x >> 16);
    }
    for (size_t i = 0x20; i < 0x34; ++i) v[i] = ' ';
    std::memcpy(&v[0x20], "TEST ROM", 8);
    return v;
}

static std::vector<uint8_t> to_v64(const std::vector<uint8_t>& z) {
    std::vector<uint8_t> v = z;
    for (size_t i = 0; i + 1 < v.size(); i += 2) std::swap(v[i], v[i + 1]);
    return v;
}

static std::vector<uint8_t> to_n64(const std::vector<uint8_t>& z) {
    std::vector<uint8_t> v = z;
    for (size_t i = 0; i + 3 < v.size(); i += 4) {
        std::swap(v[i], v[i + 3]);
        std::swap(v[i + 1], v[i + 2]);
    }
    return v;
}

static bool write_file(const fs::path& p, const std::vector<uint8_t>& d) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(d.data()), (std::streamsize)d.size());
    return (bool)f;
}

static void test_md5() {
    CHECK(md5_hex(reinterpret_cast<const uint8_t*>("abc"), 3) == "900150983CD24FB0D6963F7D28E17F72");
    CHECK(md5_hex(nullptr, 0) == "D41D8CD98F00B204E9800998ECF8427E");
}

static void test_catalog() {
    const auto& c = catalog();
    {   // every entry has a genre from the allowed list
        const auto& gl = genres();
        for (const GameInfo& g : c) {
            bool ok = false;
            for (const auto& x : gl) ok = ok || x == g.genre;
            if (!ok) std::printf("bad genre '%s' for %s\n", g.genre.c_str(), g.id.c_str());
            CHECK(ok);
        }
        CHECK(find_game("smash64")->genre == "Fighting" && find_game("mp2")->genre == "Party");
        CHECK(find_game("pokegold") && find_game("pokegold")->genre == "RPG");
        CHECK(find_game("pokered") && find_game("pokered")->system == "gb");
        /* GB/GBC catalog serials are header manufacturer codes, scoped to the system */
        const GameInfo* gold = find_game_for("gbc", "AAUE", "");
        CHECK(gold && gold->id == "pokegold");
        CHECK(find_game_for("gb", "AAUE", "") == nullptr);
        CHECK(find_game_for("gb", "POKEMON RED", "") && find_game_for("gb", "POKEMON RED", "")->id == "pokered");
        CHECK(find_game_for("gbc", "POKEMON YEL", "") && find_game_for("gbc", "POKEMON YEL", "")->id == "pokeyellow");
        CHECK(find_game_for("gbc", "POKEMON_GLD", "") && find_game_for("gbc", "POKEMON_GLD", "")->id == "pokegold");
        CHECK(find_game_for("gbc", "PM_CRYSTAL", "") && find_game_for("gbc", "PM_CRYSTAL", "")->id == "pokecrystal");
        CHECK(find_game("mk64")->genre == "Racing" && find_game("pokeemerald")->genre == "RPG");
        CHECK(GameInfo().genre.empty());
    }
    CHECK(c.size() >= 5);  // smash64, mp1, mp2, mp3, remix, plus the curated N64/GBA additions

    const GameInfo* smash = find_game("smash64");
    CHECK(smash != nullptr);
    if (smash) {
        CHECK(smash->engine == Engine::Native);
        CHECK(smash->max_players == 4);
        CHECK(smash->rom_md5s.empty());
        CHECK(smash->native_exe == "games/smashClassic/BattleShip.exe");
        CHECK(smash->online_max == 0 && online_max_for(*smash) == 4);   // online rooms are up to 4 (no cap)
        CHECK(smash->max_players == 4);                                  // local play is still 4
    }

    const GameInfo* mp1 = find_game("mp1");
    CHECK(mp1 != nullptr);
    if (mp1) {
        CHECK(mp1->engine == Engine::Mupen64Plus);
        CHECK(mp1->max_players == 4);
        CHECK(mp1->rom_md5s.size() == 1 && mp1->rom_md5s[0] == "8BC2712139FBF0C56C8EA835802C52DC");
    }

    const GameInfo* mp2 = find_game("mp2");
    CHECK(mp2 != nullptr && mp2->rom_md5s.size() == 1 &&
          mp2->rom_md5s[0] == "04840612A35ECE222AFDB2DFBF926409");

    const GameInfo* mp3 = find_game("mp3");
    CHECK(mp3 != nullptr && mp3->rom_md5s.size() == 1 &&
          mp3->rom_md5s[0] == "76A8BBC81BC2060EC99C9645867237CC");

    /* PlayStation 1 / 2 entries: Redump serial format (AAAA-NNNNN), no duplicate ids; PS2 online off, PS1 online only for the 2+ player DualShock games. */
    {
        std::set<std::string> ids;
        for (const GameInfo& g : c) CHECK(ids.insert(g.id).second);   // ids unique (find_game returns the first match)
        for (const GameInfo& g : c) {
            if (g.system != "ps1" && g.system != "ps2") continue;
            if (g.system == "ps2") CHECK(!g.online);
            else CHECK(g.online == (g.max_players >= 2 && g.id != "buzz_junior_jungle"));   // Buzz! quiz buzzers are not DualShocks
            CHECK(g.engine == (g.system == "ps2" ? Engine::Pcsx2 : Engine::Libretro));
            CHECK(!g.serials.empty());
            for (const std::string& s : g.serials) {
                bool ok = s.size() == 10 && s[4] == '-';
                for (size_t i = 0; ok && i < 10; ++i) {
                    if (i == 4) continue;
                    ok = (i < 4) ? (s[i] >= 'A' && s[i] <= 'Z') : (s[i] >= '0' && s[i] <= '9');
                }
                if (!ok) std::printf("bad PS serial '%s' for %s\n", s.c_str(), g.id.c_str());
                CHECK(ok);
            }
        }
        const GameInfo* crash = find_game_for("ps1", "SCUS-94570", "");
        CHECK(crash && crash->id == "crashbash");
        CHECK(find_game_for("ps1", "scus-94570", "") == crash);   // case-insensitive, like the other systems
        CHECK(find_game_for("ps2", "SCUS-94570", "") == nullptr); // scoped to the system
    }

    CHECK(find_game("nope") == nullptr);
    CHECK(find_game("") == nullptr);
}

static void test_rom_files(const fs::path& dir) {
    const std::vector<uint8_t> z = make_z64();
    const std::vector<uint8_t> v = to_v64(z);
    const std::vector<uint8_t> n = to_n64(z);
    CHECK(v != z && n != z);  // the synthetic image really is in non-z64 order

    const fs::path pz = dir / "a.z64";
    const fs::path pv = dir / "b.V64";  // mixed-case extension on purpose
    const fs::path pn = dir / "sub" / "c.n64";
    fs::create_directories(dir / "sub");
    CHECK(write_file(pz, z));
    CHECK(write_file(pv, v));
    CHECK(write_file(pn, n));

    for (const fs::path& p : {pz, pv, pn}) {
        std::vector<uint8_t> out;
        CHECK(load_rom(p.string(), &out));
        CHECK(out == z);
    }

    // Too small (512 KB) and unknown magic must be rejected.
    std::vector<uint8_t> small(512 * 1024, 0);
    small[0] = 0x80; small[1] = 0x37; small[2] = 0x12; small[3] = 0x40;
    const fs::path ps = dir / "small.z64";
    CHECK(write_file(ps, small));
    std::vector<uint8_t> out;
    CHECK(!load_rom(ps.string(), &out));

    std::vector<uint8_t> bogus(1024 * 1024, 0x11);
    const fs::path pb = dir / "bogus.z64";
    CHECK(write_file(pb, bogus));
    CHECK(!load_rom(pb.string(), &out));

    // identify: hash is over the normalized image; header name trimmed.
    RomFile r;
    CHECK(identify_rom(pv.string(), &r));
    CHECK(r.md5 == md5_hex(z.data(), z.size()));
    CHECK(r.header_name == "TEST ROM");
    CHECK(r.game_id.empty());

    // scan: finds the three valid images (any case, recursive), skips the bad ones and .txt.
    CHECK(write_file(dir / "notes.txt", z));
    std::vector<RomFile> found = scan_roms(dir.string());
    CHECK(found.size() == 3);
}

static void test_paths() {
    std::string exe = exe_dir();
    CHECK(!exe.empty() && exe.back() == '\\');
    std::string data = data_dir();
    CHECK(data == exe + "userdata\\");
    CHECK(fs::is_directory(fs::u8path(data)));
}

static std::string lower(std::string s) {
    for (auto& ch : s) {
        if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
    }
    return s;
}

/* Every curated entry is well-formed, ids and per-system header serials are unique. */
/* online_max_for: 0 falls back to max_players; an explicit cap wins. */
static void test_online_max() {
    GameInfo g;
    g.max_players = 4;
    CHECK(g.online_max == 0 && online_max_for(g) == 4);
    g.online_max = 2;
    CHECK(online_max_for(g) == 2);
    const GameInfo* mp1 = find_game("mp1");
    CHECK(mp1 && online_max_for(*mp1) == mp1->max_players);
    /* every catalog entry: the online cap never exceeds the local count */
    for (const GameInfo& c : catalog()) CHECK(online_max_for(c) >= 1 && online_max_for(c) <= c.max_players);
}

static void test_catalog_integrity() {
    std::set<std::string> ids;
    std::set<std::string> serial_keys;
    for (const GameInfo& g : catalog()) {
        CHECK(!g.id.empty() && !g.title.empty() && !g.system.empty());
        CHECK(!g.nointro.empty());  // cover-art name; may be a base game's name for hacks
        CHECK(ids.insert(g.id).second);  // unique catalog id
        CHECK(g.system == "n64" || g.system == "gba" || g.system == "gb" || g.system == "gbc" || g.system == "nds" ||
              g.system == "3ds" || g.system == "wiiu" || g.system == "gc" || g.system == "ps1" || g.system == "ps2");
        CHECK(g.max_players >= 1 && g.max_players <= 4);
        CHECK(!g.online || g.max_players >= 2);  // online only makes sense for multiplayer
        for (const auto& s : g.serials) {
            if (g.system == "gb" || g.system == "gbc")
                CHECK(!s.empty() && s.size() <= 16);  // header title (<= 16) or 4-char CGB code
            else if (g.system == "wiiu")
                CHECK(s.size() == 16);  // Wii U title id
            else if (g.system == "gc")
                CHECK(s.size() == 6);   // GameCube disc ID
            else if (g.system == "ps1" || g.system == "ps2")
                CHECK(s.size() == 10);  // Redump serial, e.g. SCUS-94570 (format checked below)
            else
                CHECK(s.size() == 4);  // N64 game ID / GBA / 3DS game code
            CHECK(serial_keys.insert(g.system + ":" + lower(s)).second);  // unique per system
        }
        if (g.system == "gba") CHECK(g.engine == Engine::Libretro);
        if (g.system == "ps2") CHECK(g.engine == Engine::Pcsx2);
        if (g.system == "ps1") CHECK(g.engine == Engine::Libretro);
        if (g.system == "3ds") CHECK(g.engine == Engine::Azahar);
        if (g.system == "wiiu") CHECK(g.engine == Engine::Cemu);
        if (g.system == "gc") CHECK(g.engine == Engine::Dolphin);
        if (g.system == "n64" && g.id != "smash64" && g.id != "remix") CHECK(g.engine == Engine::Mupen64Plus);
    }
    CHECK(ids.count("smash64") == 1 && ids.count("mp1") == 1 && ids.count("remix") == 1);
}

static void test_find_game_for() {
    const GameInfo* g = find_game_for("gba", "BPEE", "");
    CHECK(g && g->id == "pokeemerald");
    g = find_game_for("gba", "bpee", "");  // case-insensitive
    CHECK(g && g->id == "pokeemerald");
    g = find_game_for("gba", "BPRE", "");
    CHECK(g && g->id == "pokefirered");
    g = find_game_for("n64", "NKTE", "");  // Mario Kart 64 (USA)
    CHECK(g && g->id == "mk64");
    g = find_game_for("n64", "NKTJ", "");  // region variant
    CHECK(g && g->id == "mk64");
    g = find_game_for("n64", "nalp", "");
    CHECK(g && g->id == "ssb64rom");
    /* Nintendo DS catalog: header codes as read from the USA dumps, scoped to system "nds". */
    {
        const struct { const char* code; const char* id; } ds[] = {
            {"ADAE", "pokediamond"}, {"APAE", "pokepearl"}, {"CPUE", "pokeplatinum"}, {"IPKE", "pokeheartgold"},
            {"IPGE", "pokesoulsilver"}, {"IRBO", "pokeblack"}, {"IRAO", "pokewhite"}, {"IREO", "pokeblack2"}, {"IRDO", "pokewhite2"}};
        for (const auto& d : ds) {
            const GameInfo* n = find_game_for("nds", d.code, "");
            CHECK(n && n->id == d.id && n->system == "nds" && n->max_players == 4 && n->online && online_max_for(*n) == 4 && n->genre == "RPG" &&
                  n->engine == Engine::Libretro && !n->nointro.empty() && n->year >= 2007);
            CHECK(find_game_for("gba", d.code, "") == nullptr);   // scoped by system
        }
    }
    g = find_game_for("n64", "CLBE", "");  // Mario Party (USA), the real header ID
    CHECK(g && g->id == "mp1");

    /* MD5 of mp1, lower-case accepted, no serial needed. */
    g = find_game_for("n64", "", "8BC2712139FBF0C56C8EA835802C52DC");
    CHECK(g && g->id == "mp1");
    g = find_game_for("n64", "", "8bc2712139fbf0c56c8ea835802c52dc");
    CHECK(g && g->id == "mp1");
    /* MD5 wins over a conflicting serial. */
    g = find_game_for("n64", "NKTE", "04840612A35ECE222AFDB2DFBF926409");
    CHECK(g && g->id == "mp2");

    /* No match cases: unknown serial, wrong system, empty inputs, remix has no serials. */
    CHECK(find_game_for("n64", "ZZZZ", "") == nullptr);
    CHECK(find_game_for("gba", "NKTE", "") == nullptr);  // N64 code is not a GBA code
    CHECK(find_game_for("n64", "", "") == nullptr);
    CHECK(find_game_for("", "NKTE", "") == nullptr);
    CHECK(find_game_for("n64", "NSMX", "00000000000000000000000000000000") == nullptr);
}

int main() {
    test_md5();
    test_catalog();
    test_catalog_integrity();
    test_online_max();
    /* link-cable games: online via retrorun --link, 2 players; GBA link stays offline-only (no in-process link available) */
    for (const char* id : {"pokered", "pokeyellow", "pokegold", "pokesilver", "pokecrystal"}) {
        const GameInfo* g = find_game(id);
        CHECK(g && g->online && online_max_for(*g) == 2 && link_cable_game(id));
    }
    for (const char* id : {"pokeruby", "pokesapphire", "pokeemerald", "pokefirered", "pokeleafgreen"}) {
        const GameInfo* g = find_game(id);   /* GBA link: mgba_link_libretro (2 GBAs in one simulation) */
        CHECK(g && g->online && online_max_for(*g) == 2 && link_cable_game(id));
    }
    test_find_game_for();

    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / "hubcommon_test_dir";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    test_rom_files(dir);
    fs::remove_all(dir, ec);

    test_paths();

    if (g_fail == 0) {
        std::printf("hubcommon_test: all checks passed\n");
        return 0;
    }
    std::printf("hubcommon_test: %d check(s) failed\n", g_fail);
    return 1;
}
