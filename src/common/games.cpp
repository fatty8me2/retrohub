#include "games.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace hub {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMinRomSize = 1024u * 1024u;       // 1 MB
constexpr size_t kMaxRomSize = 64u * 1024u * 1024u; // 64 MB

/* ---- UTF-8 <-> UTF-16 helpers (paths may contain non-ASCII) ---- */

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

fs::path utf8_path(const std::string& s) { return fs::path(to_wide(s)); }
std::string path_utf8(const fs::path& p) { return to_utf8(p.native()); }

std::string ascii_lower(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return s;
}

/* ---- MD5 via Windows BCrypt ---- */

std::string md5_bcrypt(const uint8_t* data, size_t len) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0)))
        return std::string();

    std::string result;
    BCRYPT_HASH_HANDLE h = nullptr;
    do {
        DWORD obj_len = 0, hash_len = 0, got = 0;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&obj_len), sizeof(obj_len), &got, 0))) break;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len), &got, 0))) break;

        std::vector<UCHAR> obj(obj_len), hash(hash_len);
        if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, obj.data(), obj_len, nullptr, 0, 0))) break;

        size_t off = 0;
        bool ok = true;
        while (off < len) {
            ULONG chunk = (ULONG)std::min<size_t>(len - off, size_t(1) << 30);
            if (!BCRYPT_SUCCESS(BCryptHashData(h, const_cast<PUCHAR>(data + off), chunk, 0))) {
                ok = false;
                break;
            }
            off += chunk;
        }
        if (!ok) break;
        if (!BCRYPT_SUCCESS(BCryptFinishHash(h, hash.data(), hash_len, 0))) break;

        static const char kHex[] = "0123456789ABCDEF";
        for (UCHAR b : hash) {
            result.push_back(kHex[b >> 4]);
            result.push_back(kHex[b & 0x0F]);
        }
    } while (false);

    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

/* ---- ROM byte order ---- */

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

/* Converts the image in place to z64 (big-endian) order. */
bool normalize_to_z64(std::vector<uint8_t>& img) {
    if (img.size() < 4) return false;
    switch (be32(img.data())) {
    case 0x80371240u:  // z64: already big-endian
        return true;
    case 0x37804012u:  // v64: byteswapped, swap each pair of bytes
        for (size_t i = 0; i + 1 < img.size(); i += 2) std::swap(img[i], img[i + 1]);
        return true;
    case 0x40123780u:  // n64: little-endian, reverse each 4-byte word
        for (size_t i = 0; i + 3 < img.size(); i += 4) {
            std::swap(img[i], img[i + 3]);
            std::swap(img[i + 1], img[i + 2]);
        }
        return true;
    default:
        return false;
    }
}

std::string header_name_of(const std::vector<uint8_t>& img) {
    if (img.size() < 0x34) return std::string();
    std::string s(reinterpret_cast<const char*>(img.data() + 0x20), 0x14);
    for (auto& c : s) {
        if (c == '\0') c = ' ';
    }
    size_t b = 0, e = s.size();
    while (b < e && s[b] == ' ') ++b;
    while (e > b && s[e - 1] == ' ') --e;
    return s.substr(b, e - b);
}

bool has_rom_extension(const fs::path& p) {
    std::string ext = ascii_lower(path_utf8(p.extension()));
    return ext == ".z64" || ext == ".v64" || ext == ".n64";
}

} // namespace

/* ---- catalog ---- */

const std::vector<std::string>& genres() {
    static const std::vector<std::string> g = {"Party", "Fighting", "Racing", "Shooter", "Platformer", "Adventure",
                                                "RPG", "Strategy", "Sports", "Puzzle", "Action"};
    return g;
}

namespace {
/* Genre of every catalog entry, by id (kept as a table so the long entries below stay readable). */
const char* genre_of(const std::string& id) {
    static const struct { const char* id; const char* genre; } kTable[] = {
        {"smash64", "Fighting"}, {"remix", "Fighting"}, {"ssb64rom", "Fighting"}, {"wwfnomercy", "Fighting"},
        {"mp1", "Party"}, {"mp2", "Party"}, {"mp3", "Party"}, {"pokestadium", "Party"}, {"pokestadium2", "Party"},
        {"wario", "Party"},
        {"mk64", "Racing"}, {"dkr", "Racing"}, {"fzerox", "Racing"}, {"wr64", "Racing"}, {"rush2049", "Racing"},
        {"mksc", "Racing"},
        {"goldeneye", "Shooter"}, {"perfectdark", "Shooter"}, {"sf64", "Shooter"},
        {"sm64", "Platformer"}, {"bk", "Platformer"}, {"bt", "Platformer"}, {"dk64", "Platformer"},
        {"kirby64", "Platformer"}, {"yoshisstory", "Platformer"}, {"toystory2", "Platformer"},
        {"sma4", "Platformer"}, {"kirbymirror", "Platformer"}, {"kirbynightmare", "Platformer"}, {"sa2", "Platformer"},
        {"oot", "Adventure"}, {"mm", "Adventure"}, {"ootmq", "Adventure"}, {"minishcap", "Adventure"},
        {"lttp", "Adventure"}, {"mzm", "Adventure"}, {"mfusion", "Adventure"}, {"pokesnap", "Adventure"},
        {"papermario", "RPG"}, {"pokeruby", "RPG"}, {"pokesapphire", "RPG"}, {"pokeemerald", "RPG"},
        {"pokefirered", "RPG"}, {"pokeleafgreen", "RPG"}, {"mlss", "RPG"}, {"goldensun", "RPG"},
        {"goldensunlostage", "RPG"}, {"bn3blue", "RPG"}, {"pmdred", "RPG"},
        {"aw1", "Strategy"}, {"aw2", "Strategy"}, {"fireemblem", "Strategy"}, {"ffta", "Strategy"},
        {"mariotennis", "Sports"}, {"mariogolf", "Sports"}, {"thps2", "Sports"},
        {"conker", "Action"}, {"bm64", "Action"}, {"aria", "Action"},
        {"pokered", "RPG"}, {"pokeyellow", "RPG"}, {"pokegold", "RPG"}, {"pokesilver", "RPG"}, {"pokecrystal", "RPG"},
    };
    for (const auto& r : kTable)
        if (id == r.id) return r.genre;
    return "";
}
} // namespace

const std::vector<GameInfo>& catalog() {
    static const std::vector<GameInfo> games = [] {
    std::vector<GameInfo> v = {
        /* ---- native port (no ROM serials: the N64 ROM entry "ssb64rom" owns NALE) ---- */
        {"smash64", "Super Smash Bros. (Smash Classic)", Engine::Native, 4, {},
         "games/smashClassic/BattleShip.exe", "n64", {}, true, 1999,
         "A native PC port of the N64 brawler, built for quick local and online matches.",
         "Super Smash Bros. (USA)", ""},

        /* ---- Mario Party series (N64, Mupen64Plus) ---- */
        {"mp1", "Mario Party", Engine::Mupen64Plus, 4,
         {"8BC2712139FBF0C56C8EA835802C52DC"}, "", "n64", {"CLBE", "CLBJ", "NLBP"}, true, 1998,
         "Roll the dice, race around the board, and settle the score in short minigames.",
         "Mario Party (USA)"},
        {"mp2", "Mario Party 2", Engine::Mupen64Plus, 4,
         {"04840612A35ECE222AFDB2DFBF926409"}, "", "n64", {"NMWE", "NMWJ", "NMWP"}, true, 2000,
         "A second board tour with new boards and a fresh set of minigames for four players.",
         "Mario Party 2 (USA)"},
        {"mp3", "Mario Party 3", Engine::Mupen64Plus, 4,
         {"76A8BBC81BC2060EC99C9645867237CC"}, "", "n64", {"NMVE", "NMVJ", "NMVP"}, true, 2001,
         "The third board game in the series, with new boards and a new cast of minigames.",
         "Mario Party 3 (USA)"},

        /* Smash Remix (fan ROM hack, 50+ fighters): no native port, so it runs
         * through the emulator path. MD5 of the 2.0.0 release .z64. No serials on
         * purpose: a hack shares the base game's header ID, which would mislabel it. */
        {"remix", "Smash Remix 2.0", Engine::Mupen64Plus, 4,
         {"43571020FA5DDB138BBD7F64A8C9C0D4"}, "", "n64", {}, true, 2025,
         "A fan-made overhaul of Super Smash Bros. with dozens of extra fighters.",
         "Super Smash Bros. (USA)"},

        /* ---- N64 retail library (Mupen64Plus). Serials: header ID at 0x3B, from the
         *      No-Intro DAT (libretro-database). USA first, then Europe / Japan. ---- */
        {"ssb64rom", "Super Smash Bros.", Engine::Mupen64Plus, 4, {}, "", "n64",
         {"NALE", "NALP", "NALJ"}, true, 1999,
         "The retail N64 brawler, played through the emulator for local four-player fights.",
         "Super Smash Bros. (USA)"},
        {"sm64", "Super Mario 64", Engine::Mupen64Plus, 1, {}, "", "n64",
         {"NSME", "NSMP", "NSMJ"}, false, 1996,
         "Mario explores painting worlds to collect stars in the 3D platforming classic.",
         "Super Mario 64 (USA)"},
        {"mk64", "Mario Kart 64", Engine::Mupen64Plus, 4, {}, "", "n64",
         {"NKTE", "NKTP", "NKTJ"}, true, 1997,
         "Kart racing with power-up items and shortcuts, split-screen for up to four racers.",
         "Mario Kart 64 (USA)"},
        {"goldeneye", "GoldenEye 007", Engine::Mupen64Plus, 4, {}, "", "n64",
         {"NGEE", "NGEP", "NGEJ"}, true, 1997,
         "Objective-based shooter whose split-screen multiplayer is still a favorite.",
         "GoldenEye 007 (USA)"},
        {"oot", "The Legend of Zelda: Ocarina of Time", Engine::Mupen64Plus, 1, {}, "", "n64",
         {"CZLE", "NZLP", "CZLJ"}, false, 1998,
         "Link moves between childhood and adulthood to stop Ganondorf.",
         "Legend of Zelda, The - Ocarina of Time (USA)"},
        {"mm", "The Legend of Zelda: Majora's Mask", Engine::Mupen64Plus, 1, {}, "", "n64",
         {"NZSE", "NZSP", "NZSJ"}, false, 2000,
         "Link has three days to stop the moon from falling, using masks that change his form.",
         "Legend of Zelda, The - Majora's Mask (USA)"},
        {"sf64", "Star Fox 64", Engine::Mupen64Plus, 4, {}, "", "n64",
         {"NFXE", "NFXP", "NFXJ"}, true, 1997,
         "Fox McCloud's squadron fights through rail-based space battles, with a four-player versus mode.",
         "Star Fox 64 (USA)"},
        {"bk", "Banjo-Kazooie", Engine::Mupen64Plus, 1, {}, "", "n64",
         {"NBKE", "NBKP", "NBKJ"}, false, 1998,
         "Banjo and Kazooie team up to collect jiggies across colorful worlds.",
         "Banjo-Kazooie (USA)"},
        {"bt", "Banjo-Tooie", Engine::Mupen64Plus, 1, {}, "", "n64",
         {"NB7E", "NB7P", "NB7J"}, false, 2000,
         "The bear and bird return for a bigger adventure with more worlds and new moves.",
         "Banjo-Tooie (USA)"},
        {"dk64", "Donkey Kong 64", Engine::Mupen64Plus, 4, {}, "", "n64",
         {"NDOE", "NDOP", "NDOJ"}, true, 1999,
         "Five playable Kongs explore huge levels collecting golden bananas, plus a four-player arena mode.",
         "Donkey Kong 64 (USA)"},
        {"dkr", "Diddy Kong Racing", Engine::Mupen64Plus, 4, {}, "", "n64",
         {"NDYE", "NDYP", "NDYJ"}, true, 1997,
         "Kart-style racing with planes and hovercraft, plus an adventure mode across worlds.",
         "Diddy Kong Racing (USA) (En,Fr)"},
        {"papermario", "Paper Mario", Engine::Mupen64Plus, 1, {}, "", "n64",
         {"NMQE", "NMQP"}, false, 2001,
         "Mario flattens into a paper world, gathering partners and fighting turn-based battles.",
         "Paper Mario (USA)"},
        {"perfectdark", "Perfect Dark", Engine::Mupen64Plus, 4, {}, "", "n64",
         {"NPDE", "NPDP", "NPDJ"}, true, 2000,
         "A spy shooter with a deep single-player campaign and a four-player multiplayer mode.",
         "Perfect Dark (USA)"},
        {"conker", "Conker's Bad Fur Day", Engine::Mupen64Plus, 4, {}, "", "n64",
         {"NFUE", "NFUP"}, true, 2001,
         "A squirrel's crude, profane adventure that also includes a four-player battle mode.",
         "Conker's Bad Fur Day (USA)"},
        {"pokestadium", "Pokemon Stadium", Engine::Mupen64Plus, 4, {}, "", "n64",
         {"NPOE", "NPOP", "CPSJ"}, true, 2000,
         "Battle with your Pokemon in 3D arenas, using Game Boy Pokemon from earlier games.",
         "Pokemon Stadium (USA)"},
        {"pokestadium2", "Pokemon Stadium 2", Engine::Mupen64Plus, 4, {}, "", "n64",
         {"NP3E", "NP3P", "CP2J"}, true, 2001,
         "Battle with Gold and Silver Pokemon in 3D, plus Pokemon-themed mini-games.",
         "Pokemon Stadium 2 (USA)"},
        {"mariotennis", "Mario Tennis", Engine::Mupen64Plus, 4, {}, "", "n64",
         {"NM8E", "NM8P", "NM8J"}, true, 2000,
         "Singles and doubles tennis with Mario characters, including four-player matches.",
         "Mario Tennis (USA)"},
        {"mariogolf", "Mario Golf", Engine::Mupen64Plus, 4, {}, "", "n64",
         {"NMFE", "NMFP", "NMFJ"}, true, 1999,
         "Golf with Mario characters and a four-player match mode.",
         "Mario Golf (USA)"},
        {"fzerox", "F-Zero X", Engine::Mupen64Plus, 4, {}, "", "n64",
         {"CFZE", "NFZP", "CFZJ"}, true, 1998,
         "High-speed hover racing with huge fields of racers and a four-player battle mode.",
         "F-Zero X (USA)"},
        {"wr64", "Wave Race 64", Engine::Mupen64Plus, 2, {}, "", "n64",
         {"NWRE", "NWRP", "NWRJ"}, true, 1996,
         "Jet ski racing across ocean waves, with two players on split screen.",
         "Wave Race 64 - Kawasaki Jet Ski (USA)"},
        {"kirby64", "Kirby 64: The Crystal Shards", Engine::Mupen64Plus, 1, {}, "", "n64",
         {"NK4E", "NK4P"}, false, 2000,
         "Kirby gathers crystal shards and blends copy abilities to power through the levels.",
         "Kirby 64 - The Crystal Shards (USA)"},
        {"yoshisstory", "Yoshi's Story", Engine::Mupen64Plus, 1, {}, "", "n64",
         {"NYSE", "NYSP"}, false, 1997,
         "A storybook platformer in which Yoshi carries baby Mario through pop-up levels.",
         "Yoshi's Story (USA) (En,Ja)"},
        {"toystory2", "Toy Story 2", Engine::Mupen64Plus, 1, {}, "", "n64",
         {"NTHE", "NTHP"}, false, 1999,
         "Buzz Lightyear explores rooms and streets from the film to rescue Woody.",
         "Toy Story 2 - Buzz Lightyear to the Rescue! (USA)"},
        /* Master Quest was released on GameCube; N64-format copies carry NZLE. Art: Ocarina's box. */
        {"ootmq", "Zelda: Ocarina of Time Master Quest", Engine::Mupen64Plus, 1, {}, "", "n64",
         {"NZLE"}, false, 2002,
         "Ocarina of Time with remixed, much harder dungeons.",
         "Legend of Zelda, The - Ocarina of Time (USA)"},
        {"wwfnomercy", "WWF No Mercy", Engine::Mupen64Plus, 4, {}, "", "n64",
         {"NW4E", "NW4P"}, true, 2000,
         "Pro wrestling with create-a-wrestler and a four-player bout option.",
         "WWF No Mercy (USA)"},
        {"pokesnap", "Pokemon Snap", Engine::Mupen64Plus, 1, {}, "", "n64",
         {"NPFE", "NPFP", "NPFJ"}, false, 1999,
         "Photograph Pokemon in their habitats on a guided rail tour.",
         "Pokemon Snap (USA)"},
        {"bm64", "Bomberman 64", Engine::Mupen64Plus, 4, {}, "", "n64",
         {"NBME", "NBMP"}, true, 1997,
         "Bomberman battles across colorful arenas with up to four players.",
         "Bomberman 64 (USA)"},
        {"rush2049", "San Francisco Rush 2049", Engine::Mupen64Plus, 2, {}, "", "n64",
         {"NRUE", "NRUP"}, true, 2000,
         "Stunt-filled arcade racing through city courses, with two-player split screen.",
         "San Francisco Rush 2049 (USA)"},

        /* ---- GBA library (Libretro). Link-cable games are online=false: the cable is
         *      not networked yet. Serials from the No-Intro GBA DAT. ---- */
        {"pokeruby", "Pokemon Ruby", Engine::Libretro, 2, {}, "", "gba",
         {"AXVE", "AXVJ"}, false, 2003,
         "Explore the Hoenn region, catch Pokemon, and trade or battle over a link cable.",
         "Pokemon - Ruby Version (USA, Europe)"},
        {"pokesapphire", "Pokemon Sapphire", Engine::Libretro, 2, {}, "", "gba",
         {"AXPE", "AXPJ"}, false, 2003,
         "The Hoenn adventure again, with a different set of version-exclusive Pokemon.",
         "Pokemon - Sapphire Version (USA, Europe)"},
        {"pokeemerald", "Pokemon Emerald", Engine::Libretro, 2, {}, "", "gba",
         {"BPEE", "BPEJ"}, false, 2005,
         "The expanded Hoenn story, with the Battle Frontier and more postgame content.",
         "Pokemon - Emerald Version (USA, Europe)"},
        {"pokefirered", "Pokemon FireRed", Engine::Libretro, 2, {}, "", "gba",
         {"BPRE", "BPRJ"}, false, 2004,
         "A remake of the first generation's Kanto adventure, with link-cable trading and battles.",
         "Pokemon - FireRed Version (USA, Europe)"},
        {"pokeleafgreen", "Pokemon LeafGreen", Engine::Libretro, 2, {}, "", "gba",
         {"BPGE", "BPGJ"}, false, 2004,
         "The Kanto remake in its Leaf Green form, with link-cable trading and battles.",
         "Pokemon - LeafGreen Version (USA, Europe)"},
        {"minishcap", "The Legend of Zelda: The Minish Cap", Engine::Libretro, 1, {}, "", "gba",
         {"BZME", "BZMP", "BZMJ"}, false, 2004,
         "Link shrinks to Minish size to restore a world, guided by a talking hat.",
         "Legend of Zelda, The - The Minish Cap (USA)"},
        {"lttp", "The Legend of Zelda: A Link to the Past & Four Swords", Engine::Libretro, 4, {}, "", "gba",
         {"AZLE", "AZLP", "AZLJ"}, false, 2002,
         "A Link to the Past and its four-player Four Swords mode on one cartridge.",
         "Legend of Zelda, The - A Link to the Past & Four Swords (USA)"},
        {"mzm", "Metroid: Zero Mission", Engine::Libretro, 1, {}, "", "gba",
         {"BMXE", "BMXP", "BMXJ"}, false, 2004,
         "A remake of the original Metroid with a new opening and expanded areas.",
         "Metroid - Zero Mission (USA)"},
        {"mfusion", "Metroid Fusion", Engine::Libretro, 1, {}, "", "gba",
         {"AMTE", "AMTP", "AMTJ"}, false, 2002,
         "Samus hunts a shape-shifting threat aboard a research station.",
         "Metroid Fusion (USA)"},
        {"mksc", "Mario Kart: Super Circuit", Engine::Libretro, 4, {}, "", "gba",
         {"AMKE", "AMKP", "AMKJ"}, false, 2001,
         "Kart racing with items on handheld tracks, with link-cable races for four.",
         "Mario Kart - Super Circuit (USA)"},
        {"mlss", "Mario & Luigi: Superstar Saga", Engine::Libretro, 1, {}, "", "gba",
         {"A88E", "A88P", "A88J"}, false, 2003,
         "Mario and Luigi battle through a kingdom with RPG combat and timed button presses.",
         "Mario & Luigi - Superstar Saga (USA)"},
        {"sma4", "Super Mario Advance 4: Super Mario Bros. 3", Engine::Libretro, 1, {}, "", "gba",
         {"AX4E", "AX4P", "AX4J"}, false, 2004,
         "Super Mario Bros. 3 with its original levels and power-ups, ported to the Game Boy Advance.",
         "Super Mario Advance 4 - Super Mario Bros. 3 (USA)"},
        {"aw1", "Advance Wars", Engine::Libretro, 4, {}, "", "gba",
         {"AWRE", "AWRP"}, false, 2001,
         "Turn-based tank battles on a grid, with link-cable matches for several commanders.",
         "Advance Wars (USA)"},
        {"aw2", "Advance Wars 2: Black Hole Rising", Engine::Libretro, 4, {}, "", "gba",
         {"AW2E", "AW2P"}, false, 2003,
         "Turn-based strategy with new units, new commanders, and link-cable battles.",
         "Advance Wars 2 - Black Hole Rising (USA)"},
        {"fireemblem", "Fire Emblem", Engine::Libretro, 1, {}, "", "gba",
         {"AE7E", "AE7X", "AE7Y", "AFEJ"}, false, 2003,
         "Tactical grid battles where fallen units stay gone for good.",
         "Fire Emblem (USA, Australia)"},
        {"goldensun", "Golden Sun", Engine::Libretro, 1, {}, "", "gba",
         {"AGSE"}, false, 2001,
         "Adepts use Psynergy to solve puzzles and fight in turn-based battles.",
         "Golden Sun (USA, Europe)"},
        {"goldensunlostage", "Golden Sun: The Lost Age", Engine::Libretro, 1, {}, "", "gba",
         {"AGFE"}, false, 2003,
         "A sequel that continues the Golden Sun story with a new party of Adepts.",
         "Golden Sun - The Lost Age (USA, Europe)"},
        {"aria", "Castlevania: Aria of Sorrow", Engine::Libretro, 1, {}, "", "gba",
         {"A2CE", "A2CP"}, false, 2003,
         "Soma Cruz explores a dark castle and gains powers by absorbing enemy souls.",
         "Castlevania - Aria of Sorrow (USA)"},
        {"wario", "WarioWare, Inc.: Mega Microgame$!", Engine::Libretro, 4, {}, "", "gba",
         {"AZWE", "AZWP"}, false, 2003,
         "Dozens of five-second microgames, with multiplayer rounds over the link cable.",
         "WarioWare, Inc. - Mega Microgame$! (USA)"},
        {"kirbymirror", "Kirby & The Amazing Mirror", Engine::Libretro, 4, {}, "", "gba",
         {"B8KE", "B8KP"}, false, 2004,
         "Kirby explores a sprawling map of mirror worlds, with multiplayer over the link cable.",
         "Kirby & The Amazing Mirror (USA)"},
        {"kirbynightmare", "Kirby: Nightmare in Dream Land", Engine::Libretro, 4, {}, "", "gba",
         {"A7KE", "A7KP"}, false, 2002,
         "Kirby copies enemy powers while exploring a classic platforming world.",
         "Kirby - Nightmare in Dream Land (USA)"},
        {"ffta", "Final Fantasy Tactics Advance", Engine::Libretro, 2, {}, "", "gba",
         {"AFXE", "AFXP", "AFXJ"}, false, 2003,
         "Job-based tactical battles on isometric maps, with a link-cable mode for several players.",
         "Final Fantasy Tactics Advance (USA)"},
        {"bn3blue", "Mega Man Battle Network 3 (Blue)", Engine::Libretro, 2, {}, "", "gba",
         {"A3XE", "A3XP"}, false, 2003,
         "Lan battles viruses on the network grid with battle chips, including link-cable battles.",
         "Mega Man Battle Network 3 - Blue Version (USA)"},
        {"pmdred", "Pokemon Mystery Dungeon: Red Rescue Team", Engine::Libretro, 1, {}, "", "gba",
         {"B24E", "B24P"}, false, 2006,
         "Become a Pokemon in a randomly generated dungeon alongside a partner Pokemon.",
         "Pokemon Mystery Dungeon - Red Rescue Team (USA, Australia)"},
        {"sa2", "Sonic Advance 2", Engine::Libretro, 4, {}, "", "gba",
         {"A2NE", "A2NP", "A2NJ"}, false, 2002,
         "Sonic and friends race through 2D stages with a multiplayer mode over the link cable.",
         "Sonic Advance 2 (USA) (En,Ja,Fr,De,Es,It)"},
        {"thps2", "Tony Hawk's Pro Skater 2", Engine::Libretro, 2, {}, "", "gba",
         {"ATHE"}, false, 2002,
         "Skate arena-style courses and chase high scores in a timed career mode.",
         "Tony Hawk's Pro Skater 2 (USA, Europe)"},

        /* ---- Game Boy / Game Boy Color (Libretro). Serials are the header manufacturer code
         *      (0x13F-0x142) where the cart has one; Red and Yellow have none. Link-cable
         *      trading and battles are not networked, so online=false. ---- */
        {"pokered", "Pokemon Red", Engine::Libretro, 2, {}, "", "gb",
         {}, false, 1998,
         "Explore Kanto, catch Pokemon, and trade or battle over a link cable.",
         "Pokemon - Red Version (USA, Europe) (SGB Enhanced)"},
        {"pokeyellow", "Pokemon Yellow", Engine::Libretro, 2, {}, "", "gbc",
         {}, false, 1999,
         "Follow Pikachu through Kanto as your partner, with the link cable for trades and battles.",
         "Pokemon - Yellow Version - Special Pikachu Edition (USA, Europe) (CGB+SGB Enhanced)"},
        {"pokegold", "Pokemon Gold", Engine::Libretro, 2, {}, "", "gbc",
         {"AAUE"}, false, 2000,
         "Catch Pokemon in Johto and Kanto, with day and night, held items and link-cable trades.",
         "Pokemon - Gold Version (USA, Europe) (SGB Enhanced) (GB Compatible)"},
        {"pokesilver", "Pokemon Silver", Engine::Libretro, 2, {}, "", "gbc",
         {"AAXE"}, false, 2000,
         "The Silver version of Johto and Kanto, with its own version-exclusive Pokemon.",
         "Pokemon - Silver Version (USA, Europe) (SGB Enhanced) (GB Compatible)"},
        {"pokecrystal", "Pokemon Crystal", Engine::Libretro, 2, {}, "", "gbc",
         {"BYTE"}, false, 2001,
         "Play as a girl or boy in Johto, with a roaming Raikou, Entei and Suicune and a day-night clock.",
         "Pokemon - Crystal Version (USA)"},
    };
    for (auto& g : v) g.genre = genre_of(g.id);
    return v;
    }();
    return games;
}

const GameInfo* find_game(const std::string& id) {
    for (const auto& g : catalog()) {
        if (g.id == id) return &g;
    }
    return nullptr;
}

int online_max_for(const GameInfo& g) {
    return g.online_max > 0 ? g.online_max : g.max_players;
}

const GameInfo* find_game_for(const std::string& system_short, const std::string& serial, const std::string& md5) {
    /* MD5 first: an exact dump match is the strongest identity. */
    if (!md5.empty()) {
        const std::string want = ascii_lower(md5);
        for (const auto& g : catalog()) {
            for (const auto& m : g.rom_md5s) {
                if (ascii_lower(m) == want) return &g;
            }
        }
    }
    /* Then the header game ID / code, scoped to the same system. */
    if (!serial.empty() && !system_short.empty()) {
        const std::string want = ascii_lower(serial);
        for (const auto& g : catalog()) {
            if (g.system != system_short) continue;
            for (const auto& s : g.serials) {
                if (ascii_lower(s) == want) return &g;
            }
        }
    }
    return nullptr;
}

/* ---- ROMs ---- */

bool load_rom(const std::string& path, std::vector<uint8_t>* z64_image) {
    if (!z64_image) return false;
    fs::path p = utf8_path(path);

    std::error_code ec;
    uintmax_t size = fs::file_size(p, ec);
    if (ec || size < kMinRomSize || size > kMaxRomSize) return false;

    std::ifstream f(p, std::ios::binary);
    if (!f) return false;

    std::vector<uint8_t> img((size_t)size);
    f.read(reinterpret_cast<char*>(img.data()), (std::streamsize)img.size());
    if (f.gcount() != (std::streamsize)img.size()) return false;

    if (!normalize_to_z64(img)) return false;
    *z64_image = std::move(img);
    return true;
}

std::string md5_hex(const uint8_t* data, size_t len) {
    return md5_bcrypt(data, len);
}

bool identify_rom(const std::string& path, RomFile* out) {
    if (!out) return false;
    std::vector<uint8_t> img;
    if (!load_rom(path, &img)) return false;

    RomFile r;
    r.path = path;
    r.md5 = md5_hex(img.data(), img.size());
    r.header_name = header_name_of(img);
    for (const auto& g : catalog()) {
        for (const auto& m : g.rom_md5s) {
            if (m == r.md5) {
                r.game_id = g.id;
                break;
            }
        }
        if (!r.game_id.empty()) break;
    }
    *out = std::move(r);
    return true;
}

std::vector<RomFile> scan_roms(const std::string& folder) {
    std::vector<RomFile> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(utf8_path(folder),
        fs::directory_options::skip_permission_denied, ec);
    if (ec) return out;
    const fs::recursive_directory_iterator end;

    while (it != end) {
        std::error_code fec;
        const fs::path p = it->path();
        if (has_rom_extension(p) && it->is_regular_file(fec) && !fec) {
            RomFile r;
            if (identify_rom(path_utf8(p), &r)) out.push_back(std::move(r));
        }
        it.increment(ec);
        if (ec) break;  // directory iteration became unreliable; keep what we have
    }
    return out;
}

namespace {

bool is_rom_like_file(const fs::path& p) {
    static const char* const kExts[] = {".z64", ".v64", ".n64", ".gba", ".gb", ".gbc", ".iso", ".rvz", ".zip"};
    std::string ext = ascii_lower(path_utf8(p.extension()));
    for (const char* e : kExts)
        if (ext == e) return true;
    return false;
}

bool is_library_folder_name(const fs::path& p) {
    std::string n = ascii_lower(path_utf8(p.filename()));
    return n == "roms" || n == "games" || n == "n64" || n == "gba";
}

/* True if dir (or a subfolder up to `depth` levels down) holds a ROM-like file.
 * `budget` caps the entries visited so a huge folder cannot stall the UI. */
bool holds_rom_files(const fs::path& dir, int depth, int& budget) {
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return false;
    const fs::directory_iterator end;
    while (it != end && budget > 0) {
        --budget;
        std::error_code fec;
        const fs::path p = it->path();
        if (it->is_regular_file(fec) && !fec && is_rom_like_file(p)) return true;
        if (depth > 0 && it->is_directory(fec) && !fec && holds_rom_files(p, depth - 1, budget)) return true;
        it.increment(ec);
        if (ec) break;
    }
    return false;
}

/* Library-named folders (ROMs, Games, N64, GBA) up to two levels below `dir`. */
void collect_library_folders(const fs::path& dir, int depth, std::vector<fs::path>& out) {
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return;
    const fs::directory_iterator end;
    int seen = 0;
    while (it != end && seen++ < 400) {
        std::error_code fec;
        const fs::path p = it->path();
        if (it->is_directory(fec) && !fec) {
            if (is_library_folder_name(p)) out.push_back(p);
            if (depth > 1) collect_library_folders(p, depth - 1, out);
        }
        it.increment(ec);
        if (ec) break;
    }
}

} // namespace

std::vector<std::string> suggest_rom_folders(size_t max_out) {
    std::vector<std::string> out;
    const wchar_t* up = _wgetenv(L"USERPROFILE");
    if (!up || !*up || max_out == 0) return out;
    const fs::path home(up);

    std::vector<fs::path> cands;
    for (const char* base : {"Desktop", "Documents", "Downloads"}) {
        const fs::path b = home / base;
        std::error_code ec;
        if (!fs::is_directory(b, ec)) continue;
        cands.push_back(b);
        collect_library_folders(b, 2, cands);
    }

    std::vector<std::string> seen;
    for (const fs::path& c : cands) {
        if (out.size() >= max_out) break;
        int budget = 4000;
        if (!holds_rom_files(c, 2, budget)) continue;
        std::string s = path_utf8(c);
        if (std::find(seen.begin(), seen.end(), s) != seen.end()) continue;
        seen.push_back(s);
        out.push_back(s);
    }
    return out;
}

/* ---- paths ---- */

std::string exe_dir() {
    static const std::string cached = [] {
        std::wstring buf(MAX_PATH, L'\0');
        for (;;) {
            DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
            if (n == 0) return std::string();
            if (n < buf.size()) {
                buf.resize(n);
                break;
            }
            buf.resize(buf.size() * 2);
        }
        size_t pos = buf.find_last_of(L"\\/");
        if (pos == std::wstring::npos) return std::string();
        buf.resize(pos + 1);
        std::string s = to_utf8(buf);
        if (!s.empty() && s.back() != '\\') s.push_back('\\');
        return s;
    }();
    return cached;
}

std::string data_dir() {
    std::string dir = exe_dir() + "userdata\\";
    std::error_code ec;
    fs::create_directories(utf8_path(dir), ec);
    return dir;
}

} // namespace hub
