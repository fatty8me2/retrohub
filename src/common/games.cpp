#include "winutil.h"
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

std::wstring to_wide(const std::string& s) { return winutil::to_wide(s); }

std::string to_utf8(const std::wstring& s) { return winutil::to_utf8(s); }

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
        {"pokefirered", "RPG"}, {"kanto", "RPG"}, {"pokeleafgreen", "RPG"}, {"mlss", "RPG"}, {"goldensun", "RPG"},
        {"goldensunlostage", "RPG"}, {"bn3blue", "RPG"}, {"pmdred", "RPG"},
        {"aw1", "Strategy"}, {"aw2", "Strategy"}, {"fireemblem", "Strategy"}, {"ffta", "Strategy"},
        {"mariotennis", "Sports"}, {"mariogolf", "Sports"}, {"thps2", "Sports"},
        {"conker", "Action"}, {"bm64", "Action"}, {"aria", "Action"},
        {"pokex", "RPG"}, {"pokey", "RPG"}, {"pokeor", "RPG"}, {"pokeas", "RPG"},
        {"pokesun", "RPG"}, {"pokemoon", "RPG"}, {"pokeusun", "RPG"}, {"pokeumoon", "RPG"},
        {"tphd", "Adventure"},
        {"gc_ww", "Adventure"}, {"gc_tp", "Adventure"}, {"mkdd", "Racing"}, {"gc_mp6", "Party"}, {"pokexd", "RPG"},
        {"sunshine", "Platformer"}, {"melee", "Fighting"},
        {"pokered", "RPG"}, {"pokeyellow", "RPG"}, {"pokegold", "RPG"}, {"pokesilver", "RPG"}, {"pokecrystal", "RPG"},
        {"pokediamond", "RPG"}, {"pokepearl", "RPG"}, {"pokeplatinum", "RPG"}, {"pokeheartgold", "RPG"}, {"pokesoulsilver", "RPG"}, {"pokeblack", "RPG"}, {"pokewhite", "RPG"}, {"pokeblack2", "RPG"}, {"pokewhite2", "RPG"},   // nds
        {"crashbash", "Party"}, {"buzz_junior_jungle", "Party"}, {"buzz_hollywood", "Party"},
        {"ctr", "Racing"}, {"chocobo_racing", "Racing"}, {"crashnitro", "Racing"}, {"crashtag", "Racing"}, {"burnout3", "Racing"},
        {"ps1thps2", "Sports"}, {"tm2", "Action"}, {"spyro", "Platformer"}, {"ratchet", "Platformer"}, {"jak1", "Platformer"},
        {"tekken3", "Fighting"}, {"tekken5", "Fighting"}, {"bloodyroar2", "Fighting"}, {"digimon_rumble", "Fighting"},
        {"bomberman_world", "Action"}, {"bomberman_fantasy", "Action"},
        {"ff7", "RPG"}, {"ff9", "RPG"}, {"kh", "RPG"}, {"kh2", "RPG"},
        {"mgs", "Action"}, {"re2", "Adventure"}, {"sotc", "Adventure"}, {"gtasa", "Action"}, {"timesplitters2", "Shooter"},
        {"nes_smb", "Platformer"}, {"nes_smb2", "Platformer"}, {"nes_smb3", "Platformer"}, {"nes_contra", "Shooter"}, {"nes_superc", "Shooter"}, {"nes_tecmobowl", "Sports"},
        {"nes_tecmosb", "Sports"}, {"nes_drmario", "Puzzle"}, {"nes_iceclimber", "Platformer"}, {"nes_bomberman2", "Action"}, {"nes_kirby", "Platformer"}, {"nes_zelda", "Adventure"},
        {"nes_zelda2", "Adventure"}, {"nes_metroid", "Adventure"}, {"nes_rcproam", "Racing"}, {"nes_micromachines", "Racing"}, {"nes_superoffroad", "Racing"}, {"nes_gauntlet2", "Action"},
        {"nes_nwc", "Sports"}, {"nes_punchout", "Sports"}, {"nes_dd2", "Action"}, {"nes_battletoads", "Action"}, {"nes_btdd", "Action"}, {"nes_tmnt2", "Action"},
        {"nes_rivercity", "Action"}, {"nes_supdodge", "Sports"}, {"nes_ddribble", "Sports"}, {"nes_tetris", "Puzzle"}, {"nes_megaman2", "Platformer"}, {"nes_castlevania3", "Action"},
        {"nes_spikevb", "Sports"}, {"nes_dk", "Platformer"}, {"snes_smw", "Platformer"}, {"snes_allstars", "Platformer"}, {"snes_smk", "Racing"}, {"snes_sf2t", "Fighting"},
        {"snes_ssf2", "Fighting"}, {"snes_sbomber", "Action"}, {"snes_sbomber2", "Action"}, {"snes_sbomber3", "Action"}, {"snes_sbomber4", "Action"}, {"snes_sbomber5", "Action"},
        {"snes_som", "RPG"}, {"snes_dkc", "Platformer"}, {"snes_dkc2", "Platformer"}, {"snes_dkc3", "Platformer"}, {"snes_lttp", "Adventure"}, {"snes_smetroid", "Adventure"},
        {"snes_ct", "RPG"}, {"snes_kss", "Platformer"}, {"snes_nbajam", "Sports"}, {"snes_nbajamte", "Sports"}, {"snes_mk2", "Fighting"}, {"snes_tetrisattack", "Puzzle"},
        {"snes_contra3", "Shooter"}, {"snes_tmnt4", "Action"}, {"snes_zamn", "Action"}, {"snes_yoshi", "Platformer"}, {"snes_mariorpg", "RPG"}, {"snes_ff6", "RPG"},
        {"snes_earthbound", "RPG"}, {"snes_fzero", "Racing"}, {"snes_punchout", "Sports"}, {"snes_megamanx", "Platformer"}, {"snes_pocky", "Shooter"}, {"snes_killerinstinct", "Fighting"},
        {"snes_ff2", "Action"}, {"snes_kdc", "Sports"}, {"snes_supertennis", "Sports"},
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

        /* ---- GBA library (Libretro). Link-cable games are online via mgba_link_libretro
         *      (two GBAs in one deterministic simulation, retrorun --link). Serials from the No-Intro GBA DAT. ---- */
        {"pokeruby", "Pokemon Ruby", Engine::Libretro, 2, {}, "", "gba",
         {"AXVE", "AXVJ"}, true, 2003,
         "Explore the Hoenn region, catch Pokemon, and trade or battle over a link cable.",
         "Pokemon - Ruby Version (USA, Europe)"},
        {"pokesapphire", "Pokemon Sapphire", Engine::Libretro, 2, {}, "", "gba",
         {"AXPE", "AXPJ"}, true, 2003,
         "The Hoenn adventure again, with a different set of version-exclusive Pokemon.",
         "Pokemon - Sapphire Version (USA, Europe)"},
        {"pokeemerald", "Pokemon Emerald", Engine::Libretro, 2, {}, "", "gba",
         {"BPEE", "BPEJ"}, true, 2005,
         "The expanded Hoenn story, with the Battle Frontier and more postgame content.",
         "Pokemon - Emerald Version (USA, Europe)"},
        {"pokefirered", "Pokemon FireRed", Engine::Libretro, 2, {}, "", "gba",
         {"BPRE", "BPRJ"}, true, 2004,
         "A remake of the first generation's Kanto adventure, with link-cable trading and battles.",
         "Pokemon - FireRed Version (USA, Europe)"},
        /* Kanto Together: a friend's 2-player co-op FireRed hack (src/common/kanto.h). No ROM serial on purpose: the library entry is made
         * from the user's own package zip, never matched from a ROM header. Art is FireRed's cover. */
        {"kanto", "Kanto Together (FireRed co-op)", Engine::Libretro, 2, {}, "", "gba",
         {}, true, 2026,
         "A friend's two-player co-op version of Pokemon FireRed: walk the story together online and trade Pokemon.",
         "Pokemon - FireRed Version (USA, Europe)"},
        {"pokeleafgreen", "Pokemon LeafGreen", Engine::Libretro, 2, {}, "", "gba",
         {"BPGE", "BPGJ"}, true, 2004,
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

        /* ---- Nintendo 3DS (Azahar). Serials = the 4-char game code of the NCCH product code (CTR-P-xxxx). ---- */
        {"pokex", "Pokemon X", Engine::Azahar, 2, {}, "", "3ds",
         {"EKJA"}, true, 2013,
         "Explore the Kalos region, collect Pokemon and meet Mega Evolution in the first fully 3D mainline Pokemon game.",
         "Pokemon X (USA) (En,Ja,Fr,De,Es,It,Ko)"},
        {"pokey", "Pokemon Y", Engine::Azahar, 2, {}, "", "3ds",
         {"EK2A"}, true, 2013,
         "The Kalos adventure with its own Pokemon, a Mega Evolution twist and a mysterious team to stop.",
         "Pokemon Y (USA) (En,Ja,Fr,De,Es,It,Ko)"},
        {"pokeor", "Pokemon Omega Ruby", Engine::Azahar, 2, {}, "", "3ds",
         {"ECRA"}, true, 2014,
         "A remake of the Hoenn classic with Mega Evolution, soaring Primal Groudon and a new Delta Episode.",
         "Pokemon Omega Ruby (USA) (En,Ja,Fr,De,Es,It,Ko) (Rev 2)"},
        {"pokeas", "Pokemon Alpha Sapphire", Engine::Azahar, 2, {}, "", "3ds",
         {"ECLA"}, true, 2014,
         "A remake of the Hoenn classic with Mega Evolution, Primal Kyogre and a new Delta Episode.",
         "Pokemon Alpha Sapphire (USA) (En,Ja,Fr,De,Es,It,Ko) (Rev 2)"},
        {"pokesun", "Pokemon Sun", Engine::Azahar, 2, {}, "", "3ds",
         {"BNDA"}, true, 2016,
         "Island hopping through Alola with Island Challenges, Z-Moves and regional forms.",
         "Pokemon Sun (USA) (En,Ja,Fr,De,Es,It,Zh,Ko)"},
        {"pokemoon", "Pokemon Moon", Engine::Azahar, 2, {}, "", "3ds",
         {"BNEA"}, true, 2016,
         "Island hopping through Alola with Island Challenges, Z-Moves and regional forms.",
         "Pokemon Moon (USA) (En,Ja,Fr,De,Es,It,Zh,Ko)"},
        {"pokeusun", "Pokemon Ultra Sun", Engine::Azahar, 2, {}, "", "3ds",
         {"A2AA"}, true, 2017,
         "A new take on Alola with extra story, Ultra Beasts and Totem-sized surprises.",
         "Pokemon Ultra Sun (USA) (En,Ja,Fr,De,Es,It,Zh,Ko)"},
        {"pokeumoon", "Pokemon Ultra Moon", Engine::Azahar, 2, {}, "", "3ds",
         {"A2BA"}, true, 2017,
         "A new take on Alola with extra story, Ultra Beasts and Totem-sized surprises.",
         "Pokemon Ultra Moon (USA) (En,Ja,Fr,De,Es,It,Zh,Ko)"},

        /* ---- Wii U (Cemu). Serial = the 16-hex title id of the base game. ---- */
        {"tphd", "The Legend of Zelda: Twilight Princess HD", Engine::Cemu, 1, {}, "", "wiiu",
         {"000500001019E500"}, false, 2016,
         "Link crosses between Hyrule and the Twilight Realm in the remastered GameCube adventure.",
         "Legend of Zelda, The - Twilight Princess HD (USA) (En,Fr,Es) (Rev 2)"},

        /* ---- GameCube (Dolphin). Serial = the 6-char disc ID. Online play (Dolphin NetPlay) is wired for the online-capable titles. ---- */
        {"gc_ww", "Zelda: The Wind Waker", Engine::Dolphin, 1, {}, "", "gc",
         {"GZLE01", "GZLP01", "GZLJ01"}, false, 2002,
         "Link sails a drowned Hyrule on the King of Red Lions in a cel-shaded adventure.",
         "Legend of Zelda, The - The Wind Waker (USA)"},
        {"gc_tp", "The Legend of Zelda: Twilight Princess (GameCube)", Engine::Dolphin, 1, {}, "", "gc",
         {"GZ2E01", "GZ2P01", "GZ2J01"}, false, 2006,
         "Link shifts between Hyrule and the Twilight Realm as a wolf and a hero.",
         "Legend of Zelda, The - Twilight Princess (USA)"},
        {"mkdd", "Mario Kart: Double Dash!!", Engine::Dolphin, 4, {}, "", "gc",
         {"GM4E01", "GM4P01", "GM4J01"}, true, 2003,
         "Two racers share each kart, one driving and one throwing items.",
         "Mario Kart - Double Dash!! (USA)"},
        {"gc_mp6", "Mario Party 6", Engine::Dolphin, 4, {}, "", "gc",
         {"GP6E01", "GP6P01", "GP6J01"}, true, 2004,
         "Day and night change the board and the minigames, with a microphone for some.",
         "Mario Party 6 (USA)"},
        {"pokexd", "Pokemon XD: Gale of Darkness", Engine::Dolphin, 1, {}, "", "gc",
         {"GXXE01", "GXXP01", "GXXJ01"}, false, 2005,
         "Snag Shadow Pokemon from Cipher in a 3D role-playing adventure.",
         "Pokemon XD - Gale of Darkness (USA)"},
        {"sunshine", "Super Mario Sunshine", Engine::Dolphin, 1, {}, "", "gc",
         {"GMSE01", "GMSP01", "GMSJ01"}, false, 2002,
         "Mario cleans up Isle Delfino with the water-powered F.L.U.D.D.",
         "Super Mario Sunshine (USA)"},
        {"melee", "Super Smash Bros. Melee", Engine::Dolphin, 4, {}, "", "gc",
         {"GALE01", "GALP01", "GALJ01"}, true, 2001,
         "The fast GameCube Smash with a huge roster and a tournament scene of its own.",
         "Super Smash Bros. Melee (USA) (En,Ja) (Rev 2)"},
        /* ---- Game Boy / Game Boy Color (Libretro). Serials are what library.cpp reads from the
         *      header: the manufacturer code (0x13F-0x142) on CGB carts that have one, otherwise
         *      the header title (CGB titles are 11 chars, DMG 16). Link-cable play is not
         *      networked in-core: online=true means retrorun --link (both consoles in one SameBoy
         *      simulation, see docs/RETRORUN.md). ---- */
        {"pokered", "Pokemon Red", Engine::Libretro, 2, {}, "", "gb",
         {"POKEMON RED"}, true, 1998,
         "Explore Kanto, catch Pokemon, and trade or battle over a link cable.",
         "Pokemon - Red Version (USA, Europe) (SGB Enhanced)"},
        {"pokeyellow", "Pokemon Yellow", Engine::Libretro, 2, {}, "", "gbc",
         {"POKEMON YEL"}, true, 1999,
         "Follow Pikachu through Kanto as your partner, with the link cable for trades and battles.",
         "Pokemon - Yellow Version - Special Pikachu Edition (USA, Europe) (CGB+SGB Enhanced)"},
        {"pokegold", "Pokemon Gold", Engine::Libretro, 2, {}, "", "gbc",
         {"AAUE", "POKEMON_GLD"}, true, 2000,
         "Catch Pokemon in Johto and Kanto, with day and night, held items and link-cable trades.",
         "Pokemon - Gold Version (USA, Europe) (SGB Enhanced) (GB Compatible)"},
        {"pokesilver", "Pokemon Silver", Engine::Libretro, 2, {}, "", "gbc",
         {"AAXE", "POKEMON_SLV"}, true, 2000,
         "The Silver version of Johto and Kanto, with its own version-exclusive Pokemon.",
         "Pokemon - Silver Version (USA, Europe) (SGB Enhanced) (GB Compatible)"},
        {"pokecrystal", "Pokemon Crystal", Engine::Libretro, 2, {}, "", "gbc",
         {"BYTE", "PM_CRYSTAL"}, true, 2001,
         "Play as a girl or boy in Johto, with a roaming Raikou, Entei and Suicune and a day-night clock.",
         "Pokemon - Crystal Version (USA)"},

        /* ---- Nintendo DS library (Libretro, melonDS DS). Local wireless (trades, battles, Union Room) runs online through retrorun --link, up to 4 PCs; serials = header game code (0x0C). ---- */
        {"pokediamond", "Pokemon Diamond", Engine::Libretro, 4, {}, "", "nds",
         {"ADAE", "ADAJ", "ADAP", "ADAD", "ADAF", "ADAI", "ADAS", "ADAK"}, true, 2007,
         "Explore the Sinnoh region, catch Pokemon, and take on Team Galactic.",
         "Pokemon - Diamond Version (USA) (Rev 5)", "", 4},
        {"pokepearl", "Pokemon Pearl", Engine::Libretro, 4, {}, "", "nds",
         {"APAE", "APAJ", "APAP", "APAD", "APAF", "APAI", "APAS", "APAK"}, true, 2007,
         "The Sinnoh adventure again, with a different set of version-exclusive Pokemon.",
         "Pokemon - Pearl Version (USA) (Rev 5)", "", 4},
        {"pokeplatinum", "Pokemon Platinum", Engine::Libretro, 4, {}, "", "nds",
         {"CPUE", "CPUJ", "CPUP", "CPUD", "CPUF", "CPUI", "CPUS", "CPUK"}, true, 2009,
         "The enhanced Sinnoh story, with the Distortion World and the Battle Frontier.",
         "Pokemon - Platinum Version (USA)", "", 4},
        {"pokeheartgold", "Pokemon HeartGold", Engine::Libretro, 4, {}, "", "nds",
         {"IPKE", "IPKJ", "IPKP", "IPKD", "IPKF", "IPKI", "IPKS", "IPKK"}, true, 2010,
         "A remake of Gold with Johto and Kanto to explore, and a Pokemon following you around.",
         "Pokemon - HeartGold Version (USA)", "", 4},
        {"pokesoulsilver", "Pokemon SoulSilver", Engine::Libretro, 4, {}, "", "nds",
         {"IPGE", "IPGJ", "IPGP", "IPGD", "IPGF", "IPGI", "IPGS", "IPGK"}, true, 2010,
         "A remake of Silver with Johto and Kanto to explore, and a Pokemon following you around.",
         "Pokemon - SoulSilver Version (USA)", "", 4},
        {"pokeblack", "Pokemon Black", Engine::Libretro, 4, {}, "", "nds",
         {"IRBE", "IRBJ", "IRBP", "IRBD", "IRBF", "IRBI", "IRBS", "IRBK", "IRBO"}, true, 2011,
         "Journey through the Unova region, home to entirely new Pokemon.",
         "Pokemon - Black Version (USA, Europe) (NDSi Enhanced)", "", 4},
        {"pokewhite", "Pokemon White", Engine::Libretro, 4, {}, "", "nds",
         {"IRAE", "IRAJ", "IRAP", "IRAD", "IRAF", "IRAI", "IRAS", "IRAK", "IRAO"}, true, 2011,
         "The Unova adventure with its own exclusive Pokemon and a city to explore.",
         "Pokemon - White Version (USA, Europe) (NDSi Enhanced)", "", 4},
        {"pokeblack2", "Pokemon Black 2", Engine::Libretro, 4, {}, "", "nds",
         {"IREE", "IREJ", "IREP", "IRED", "IREF", "IREI", "IRES", "IREK", "IREO"}, true, 2012,
         "A direct sequel set in Unova two years later, with new areas and a new story.",
         "Pokemon - Black Version 2 (USA, Europe) (NDSi Enhanced)", "", 4},
        {"pokewhite2", "Pokemon White 2", Engine::Libretro, 4, {}, "", "nds",
         {"IRDE", "IRDJ", "IRDP", "IRDD", "IRDF", "IRDI", "IRDS", "IRDK", "IRDO"}, true, 2012,
         "The Unova sequel with its own version-exclusive Pokemon and challenges.",
         "Pokemon - White Version 2 (USA, Europe) (NDSi Enhanced)", "", 4},

        /* ---- PlayStation 1 (Libretro, retrorun). Serials = Redump header IDs (SLUS-/SCUS-/SCES-), region variants included.
         * Local player counts and multitap needs are from the game manuals. online = true for the games with 2+ DualShock players (retrorun rollback through
         * the room; 3-4 players use the emulated multitap); Buzz! quiz controllers and the single-player games stay offline. ---- */
        {"crashbash", "Crash Bash", Engine::Libretro, 4, {}, "", "ps1",
         {"SCUS-94570", "SCES-02834"}, true, 2000,
         "Minigame party with up to four players on one console; needs a multitap for 3-4 players.",
         "Crash Bash (USA)"},
        {"ctr", "Crash Team Racing", Engine::Libretro, 4, {}, "", "ps1",
         {"SCUS-94426", "SCES-02105"}, true, 1999,
         "Kart racing with power-ups and a split-screen race for up to four; multitap needed for 3-4.",
         "CTR - Crash Team Racing (USA)"},
        {"ps1thps2", "Tony Hawk's Pro Skater 2", Engine::Libretro, 2, {}, "", "ps1",
         {"SLUS-01066"}, true, 2000,
         "Skate courses with a timed career and split-screen head-to-head for two players.",
         "Tony Hawk's Pro Skater 2 (USA)"},
        {"tm2", "Twisted Metal 2", Engine::Libretro, 2, {}, "", "ps1",
         {"SCUS-94306"}, true, 1996,
         "Vehicular combat in destructible arenas, with split-screen duels for two players.",
         "Twisted Metal 2 (USA)"},
        {"spyro", "Spyro the Dragon", Engine::Libretro, 1, {}, "", "ps1",
         {"SCUS-94228"}, false, 1998,
         "A young dragon glides and charges through colorful worlds to free captured dragons.",
         "Spyro the Dragon (USA)"},
        {"tekken3", "Tekken 3", Engine::Libretro, 2, {}, "", "ps1",
         {"SLUS-00402"}, true, 1998,
         "The third Tekken tournament, with a large roster and two-player versus battles.",
         "Tekken 3 (USA)"},
        {"bomberman_world", "Bomberman World", Engine::Libretro, 4, {}, "", "ps1",
         {"SLUS-00680"}, true, 1998,
         "Classic bomb-laying mazes with a multiplayer battle mode; multitap for more than two players.",
         "Bomberman World (USA)"},
        {"bomberman_fantasy", "Bomberman Fantasy Race", Engine::Libretro, 4, {}, "", "ps1",
         {"SLUS-00823", "SLES-01712"}, true, 1998,
         "Bomberman goes kart racing with item-based combat, with multiplayer races for up to four.",
         "Bomberman Fantasy Race (USA)"},
        {"bloodyroar2", "Bloody Roar II", Engine::Libretro, 2, {}, "", "ps1",
         {"SCUS-94424"}, true, 1999,
         "Animal-transforming fighters in a 3D arena, with two-player versus matches.",
         "Bloody Roar II (USA)"},
        {"digimon_rumble", "Digimon Rumble Arena", Engine::Libretro, 2, {}, "", "ps1",
         {"SLUS-01404", "SLES-03841"}, true, 2001,
         "Digimon brawl in 3D arenas, with two-player versus fights.",
         "Digimon Rumble Arena (USA)"},
        {"chocobo_racing", "Chocobo Racing", Engine::Libretro, 4, {}, "", "ps1",
         {"SLUS-00844", "SLES-02079"}, true, 1999,
         "Final Fantasy-style kart racing with Chocobos and split-screen races for up to four.",
         "Chocobo Racing (USA)"},
        {"buzz_junior_jungle", "Buzz! Junior - Jungle Party", Engine::Libretro, 4, {}, "", "ps1",
         {"SCUS-97596", "SCES-54220"}, false, 2004,
         "Kid-friendly quiz and minigames for up to four, played with Buzz buzzer controllers.",
         "Buzz! Junior - Jungle Party (USA) (En,Fr,Es)"},
        {"ff7", "Final Fantasy VII", Engine::Libretro, 1, {}, "", "ps1",
         {"SCUS-94163", "SCES-00867"}, false, 1997,
         "Cloud and friends fight to stop Shinra in a sprawling three-disc RPG.",
         "Final Fantasy VII (USA) (Disc 1)"},
        {"ff9", "Final Fantasy IX", Engine::Libretro, 1, {}, "", "ps1",
         {"SLUS-01251", "SLUS-01295", "SLUS-01296", "SLUS-01297", "SLES-02965"}, false, 2000,
         "Zidane and Garnet travel a fantasy world in a classic-style four-disc RPG.",
         "Final Fantasy IX (USA, Canada) (Disc 1)"},
        {"mgs", "Metal Gear Solid", Engine::Libretro, 1, {}, "", "ps1",
         {"SLUS-00594", "SLUS-00776"}, false, 1998,
         "Solid Snake infiltrates a nuclear facility in a stealth action game.",
         "Metal Gear Solid (USA) (Disc 1)"},
        {"re2", "Resident Evil 2", Engine::Libretro, 1, {}, "", "ps1",
         {"SLUS-00421", "SLUS-00592", "SLUS-00748", "SLUS-00756"}, false, 1998,
         "Leon and Claire survive a zombie-infested Raccoon City in survival horror.",
         "Resident Evil 2 (USA) (Disc 1)"},

        /* ---- PlayStation 2 (PCSX2 engine). Serials = Redump header IDs (SLUS-/SCUS-), region variants included.
         * Engine::Pcsx2 runs the PCSX2 core offline; online=true games are 2-player versus/race titles played in rooms through retrorun + LRPS2
         * (input-delay lockstep, 2 ports: LRPS2 has no multitap, see online_max_for). ---- */
        {"ratchet", "Ratchet & Clank", Engine::Pcsx2, 1, {}, "", "ps2",
         {"SCUS-97199"}, false, 2002,
         "Ratchet and the robot Clank blast through planets in a 3D action platformer.",
         "Ratchet & Clank (USA) (En,Fr,De,Es,It)"},
        {"jak1", "Jak and Daxter: The Precursor Legacy", Engine::Pcsx2, 1, {}, "", "ps2",
         {"SCUS-97124"}, false, 2001,
         "Jak and Daxter explore an open world in a platformer that blends action and exploration.",
         "Jak and Daxter - The Precursor Legacy (USA) (En,Fr,De,Es,It)"},
        {"kh", "Kingdom Hearts", Engine::Pcsx2, 1, {}, "", "ps2",
         {"SLUS-20370"}, false, 2002,
         "Sora wields a Keyblade and fights Heartless in a Disney-Square Enix action RPG.",
         "Kingdom Hearts (USA)"},
        {"kh2", "Kingdom Hearts II", Engine::Pcsx2, 1, {}, "", "ps2",
         {"SLUS-21005"}, false, 2005,
         "The Kingdom Hearts sequel continues Sora's journey through more Disney worlds.",
         "Kingdom Hearts II (USA)"},
        {"gtasa", "Grand Theft Auto: San Andreas", Engine::Pcsx2, 1, {}, "", "ps2",
         {"SLUS-20946"}, false, 2004,
         "Carl Johnson returns to Los Santos in an open-world crime sandbox.",
         "Grand Theft Auto - San Andreas (USA, Canada) (v3.00)"},
        {"tekken5", "Tekken 5", Engine::Pcsx2, 2, {}, "", "ps2",
         {"SLUS-21059"}, true, 2005,
         "The fifth Tekken tournament, with two-player versus fights and a large roster.",
         "Tekken 5 (USA)"},
        {"burnout3", "Burnout 3: Takedown", Engine::Pcsx2, 2, {}, "", "ps2",
         {"SLUS-21050"}, true, 2004,
         "Crash-focused racing with Takedown mode, and split-screen races for two players.",
         "Burnout 3 - Takedown (USA, Canada)"},
        {"crashnitro", "Crash Nitro Kart", Engine::Pcsx2, 4, {}, "", "ps2",
         {"SLUS-20649"}, true, 2003,
         "Kart racing with power-ups and a split-screen race for up to four; multitap needed for 3-4.",
         "Crash Nitro Kart (USA)"},
        {"crashtag", "Crash Tag Team Racing", Engine::Pcsx2, 4, {}, "", "ps2",
         {"SLUS-21191"}, true, 2005,
         "Crash and friends race in tag teams, with split-screen races for up to four; multitap for 3-4.",
         "Crash Tag Team Racing (USA)"},
        {"timesplitters2", "TimeSplitters 2", Engine::Pcsx2, 4, {}, "", "ps2",
         {"SLUS-20314"}, true, 2002,
         "Time-hopping shooter with split-screen deathmatch for up to four; multitap for 3-4.",
         "TimeSplitters 2 (USA)"},
        {"sotc", "Shadow of the Colossus", Engine::Pcsx2, 1, {}, "", "ps2",
         {"SCUS-97472"}, false, 2005,
         "Wander alone across a vast land and defeat sixteen colossi.",
         "Shadow of the Colossus (USA)"},
        {"buzz_hollywood", "Buzz! The Hollywood Quiz", Engine::Pcsx2, 4, {}, "", "ps2",
         {"SCUS-97594", "SCUS-97633"}, false, 2005,
         "Movie-themed quiz for up to four, played with Buzz buzzer controllers.",
         "Buzz! The Hollywood Quiz (USA)"},

        /* ---- NES library (Libretro). Titles are No-Intro names, verified against libretro-database metadat/no-intro; no header serials exist, so ROMs match by name. 3-4 player games need the Four Score (emulated by the core). ---- */
        {"nes_smb", "Super Mario Bros.", Engine::Libretro, 2, {}, "", "nes",
         {}, false, 1985,
         "The original run-and-jump classic; a second player takes over on the next life.",
         "Super Mario Bros. (World)"},
        {"nes_smb2", "Super Mario Bros. 2", Engine::Libretro, 1, {}, "", "nes",
         {}, false, 1988,
         "Pick Mario, Luigi, Toad or Peach and pull up vegetables to beat Wart.",
         "Super Mario Bros. 2 (USA)"},
        {"nes_smb3", "Super Mario Bros. 3", Engine::Libretro, 2, {}, "", "nes",
         {}, false, 1990,
         "Eight worlds, power-up suits and an overworld map; two players alternate turns.",
         "Super Mario Bros. 3 (USA)"},
        {"nes_contra", "Contra", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1988,
         "Run-and-gun co-op through jungle and base stages; two players share the screen.",
         "Contra (USA)"},
        {"nes_superc", "Super C", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1990,
         "The Contra sequel, with overhead stages and two-player co-op.",
         "Super C (USA)"},
        {"nes_tecmobowl", "Tecmo Bowl", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1989,
         "Fast arcade football with real players and head-to-head play.",
         "Tecmo Bowl (USA)"},
        {"nes_tecmosb", "Tecmo Super Bowl", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1991,
         "Tecmo Bowl with a full season, playoffs and every team.",
         "Tecmo Super Bowl (USA)"},
        {"nes_drmario", "Dr. Mario", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1990,
         "Match colored capsules to clear viruses, with a head-to-head two-player mode.",
         "Dr. Mario (Japan, USA) (En)"},
        {"nes_iceclimber", "Ice Climber", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1985,
         "Popo and Nana climb icy mountains together or race each other.",
         "Ice Climber (USA, Europe, Asia) (En)"},
        {"nes_bomberman2", "Bomberman II", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1991,
         "Maze bombing with a two-player battle mode.",
         "Bomberman II (USA)"},
        {"nes_kirby", "Kirby's Adventure", Engine::Libretro, 1, {}, "", "nes",
         {}, false, 1993,
         "Kirby inhales enemies and copies their powers across Dream Land.",
         "Kirby's Adventure (USA)"},
        {"nes_zelda", "The Legend of Zelda", Engine::Libretro, 1, {}, "", "nes",
         {}, false, 1986,
         "The first overhead quest through Hyrule and its dungeons.",
         "Legend of Zelda, The (USA)"},
        {"nes_zelda2", "Zelda II: The Adventure of Link", Engine::Libretro, 1, {}, "", "nes",
         {}, false, 1988,
         "Side-scrolling combat and an overworld map in a darker Zelda.",
         "Zelda II - The Adventure of Link (USA)"},
        {"nes_metroid", "Metroid", Engine::Libretro, 1, {}, "", "nes",
         {}, false, 1987,
         "Samus explores Zebes, gathering upgrades to reach new areas.",
         "Metroid (USA)"},
        {"nes_rcproam", "R.C. Pro-Am", Engine::Libretro, 1, {}, "", "nes",
         {}, false, 1988,
         "Isometric remote-control car racing with weapons and upgrades.",
         "R.C. Pro-Am (USA)"},
        {"nes_micromachines", "Micro Machines", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1991,
         "Tiny cars on tabletop tracks, with a head-to-head two-player race.",
         "Micro Machines (USA)"},
        {"nes_superoffroad", "Ivan 'Ironman' Stewart's Super Off Road", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1990,
         "Truck racing on dirt tracks with upgrades; two players race at once.",
         "Ivan 'Ironman' Stewart's Super Off Road (USA)"},
        {"nes_gauntlet2", "Gauntlet II", Engine::Libretro, 4, {}, "", "nes",
         {}, true, 1990,
         "Dungeon crawling for up to four heroes; three or four players need the NES Four Score.",
         "Gauntlet II (USA)"},
        {"nes_nwc", "Nintendo World Cup", Engine::Libretro, 4, {}, "", "nes",
         {}, true, 1990,
         "Cartoon soccer with national teams; extra players need the NES Four Score.",
         "Nintendo World Cup (USA)"},
        {"nes_punchout", "Mike Tyson's Punch-Out!!", Engine::Libretro, 1, {}, "", "nes",
         {}, false, 1987,
         "Learn each boxer's patterns and climb the circuit to Tyson.",
         "Mike Tyson's Punch-Out!! (Japan, USA) (En)"},
        {"nes_dd2", "Double Dragon II: The Revenge", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1989,
         "Co-op street brawling with a hurricane-kick finisher.",
         "Double Dragon II - The Revenge (USA)"},
        {"nes_battletoads", "Battletoads", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1991,
         "Punishing beat-'em-up and racing stages for one or two toads.",
         "Battletoads (USA)"},
        {"nes_btdd", "Battletoads & Double Dragon", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1993,
         "Both brawler series team up for a two-player fight.",
         "Battletoads-Double Dragon (USA)"},
        {"nes_tmnt2", "Teenage Mutant Ninja Turtles II: The Arcade Game", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1990,
         "Arcade turtle brawler with two-player co-op.",
         "Teenage Mutant Ninja Turtles II - The Arcade Game (USA)"},
        {"nes_rivercity", "River City Ransom", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1990,
         "Street brawling with shops, stat upgrades and a two-player co-op mode.",
         "River City Ransom (USA)"},
        {"nes_supdodge", "Super Dodge Ball", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1989,
         "Arcade dodgeball with national teams and special throws.",
         "Super Dodge Ball (USA)"},
        {"nes_ddribble", "Double Dribble", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1987,
         "Early basketball with cinematic dunks and a two-player match.",
         "Double Dribble (USA)"},
        {"nes_tetris", "Tetris", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1989,
         "Nintendo's falling-block puzzler, with a head-to-head mode.",
         "Tetris (USA)"},
        {"nes_megaman2", "Mega Man 2", Engine::Libretro, 1, {}, "", "nes",
         {}, false, 1989,
         "Pick any of eight robot masters and steal their weapons.",
         "Mega Man 2 (USA)"},
        {"nes_castlevania3", "Castlevania III: Dracula's Curse", Engine::Libretro, 1, {}, "", "nes",
         {}, false, 1990,
         "A branching quest with party members Trevor can team up with.",
         "Castlevania III - Dracula's Curse (USA)"},
        {"nes_spikevb", "Super Spike V'Ball", Engine::Libretro, 2, {}, "", "nes",
         {}, true, 1990,
         "Beach volleyball for one or two players.",
         "Super Spike V'Ball (USA)"},
        {"nes_dk", "Donkey Kong", Engine::Libretro, 2, {}, "", "nes",
         {}, false, 1986,
         "The arcade classic; two players alternate turns.",
         "Donkey Kong (World) (Rev 1)"},
        /* ---- SNES library (Libretro). Titles are No-Intro names, verified against libretro-database metadat/no-intro; no header serials, so ROMs match by name. Multitap games (3-4 players) need the Super Multitap (emulated by the core). Player counts above 4 (Super Bomberman 5-player) are capped at 4 here and unverified. ---- */
        {"snes_smw", "Super Mario World", Engine::Libretro, 2, {}, "", "snes",
         {}, false, 1991,
         "Yoshi's debut across Dinosaur Land; two players alternate turns.",
         "Super Mario World (USA)"},
        {"snes_allstars", "Super Mario All-Stars", Engine::Libretro, 2, {}, "", "snes",
         {}, false, 1993,
         "Remade Super Mario Bros. 1-3 and The Lost Levels, with alternating two-player turns.",
         "Super Mario All-Stars (USA)"},
        {"snes_smk", "Super Mario Kart", Engine::Libretro, 2, {}, "", "snes",
         {}, true, 1992,
         "The first Mario Kart: Mode 7 racing with a split-screen two-player grand prix and battle mode.",
         "Super Mario Kart (USA)"},
        {"snes_sf2t", "Street Fighter II Turbo: Hyper Fighting", Engine::Libretro, 2, {}, "", "snes",
         {}, true, 1993,
         "Faster Street Fighter II with all twelve fighters, for head-to-head matches.",
         "Street Fighter II Turbo (USA)"},
        {"snes_ssf2", "Super Street Fighter II", Engine::Libretro, 2, {}, "", "snes",
         {}, true, 1994,
         "Sixteen fighters including four new challengers.",
         "Super Street Fighter II (USA)"},
        {"snes_sbomber", "Super Bomberman", Engine::Libretro, 4, {}, "", "snes",
         {}, true, 1993,
         "Maze battles for up to four; three or four players need the Super Multitap.",
         "Super Bomberman (USA)"},
        {"snes_sbomber2", "Super Bomberman 2", Engine::Libretro, 4, {}, "", "snes",
         {}, true, 1994,
         "More stages and arenas; four-player battles need the Super Multitap.",
         "Super Bomberman 2 (USA)"},
        {"snes_sbomber3", "Super Bomberman 3", Engine::Libretro, 4, {}, "", "snes",
         {}, true, 1995,
         "Rideable Louie creatures and new battle maps; multitap for more than two. Japan-only release.",
         "Super Bomberman 3 (Japan)"},
        {"snes_sbomber4", "Super Bomberman 4", Engine::Libretro, 4, {}, "", "snes",
         {}, true, 1996,
         "Japan-only entry with a story mode and multitap battles for up to four.",
         "Super Bomberman 4 (Japan)"},
        {"snes_sbomber5", "Super Bomberman 5", Engine::Libretro, 4, {}, "", "snes",
         {}, true, 1997,
         "The final SNES Bomberman; Japan-only, with multitap battles (unverified whether 5-player mode applies).",
         "Super Bomberman 5 (Japan)"},
        {"snes_som", "Secret of Mana", Engine::Libretro, 3, {}, "", "snes",
         {}, true, 1993,
         "Real-time action RPG; a second and third player (Super Multitap) join as Randi's allies.",
         "Secret of Mana (USA)"},
        {"snes_dkc", "Donkey Kong Country", Engine::Libretro, 2, {}, "", "snes",
         {}, false, 1994,
         "Pre-rendered visuals and Donkey and Diddy swapping; two players alternate.",
         "Donkey Kong Country (USA)"},
        {"snes_dkc2", "Donkey Kong Country 2: Diddy's Kong Quest", Engine::Libretro, 2, {}, "", "snes",
         {}, false, 1995,
         "Diddy and Dixie hunt for Donkey Kong across Crocodile Isle.",
         "Donkey Kong Country 2 - Diddy's Kong Quest (USA) (En,Fr)"},
        {"snes_dkc3", "Donkey Kong Country 3: Dixie Kong's Double Trouble!", Engine::Libretro, 2, {}, "", "snes",
         {}, false, 1996,
         "Dixie and Kiddy Kong search a northern landscape for the Kongs.",
         "Donkey Kong Country 3 - Dixie Kong's Double Trouble! (USA) (En,Fr)"},
        {"snes_lttp", "The Legend of Zelda: A Link to the Past", Engine::Libretro, 1, {}, "", "snes",
         {}, false, 1991,
         "Light and Dark World dungeons, the series' reference 2D entry.",
         "Legend of Zelda, The - A Link to the Past (USA)"},
        {"snes_smetroid", "Super Metroid", Engine::Libretro, 1, {}, "", "snes",
         {}, false, 1994,
         "Samus returns to Zebes in a huge interconnected map.",
         "Super Metroid (Japan, USA) (En,Ja)"},
        {"snes_ct", "Chrono Trigger", Engine::Libretro, 1, {}, "", "snes",
         {}, false, 1995,
         "A time-travelling RPG with multiple endings.",
         "Chrono Trigger (USA)"},
        {"snes_kss", "Kirby Super Star", Engine::Libretro, 2, {}, "", "snes",
         {}, true, 1996,
         "Eight games in one cartridge; a helper character is controlled by the second player.",
         "Kirby Super Star (USA)"},
        {"snes_nbajam", "NBA Jam", Engine::Libretro, 4, {}, "", "snes",
         {}, true, 1994,
         "Two-on-two arcade basketball; four players need the Super Multitap.",
         "NBA Jam (USA)"},
        {"snes_nbajamte", "NBA Jam: Tournament Edition", Engine::Libretro, 4, {}, "", "snes",
         {}, true, 1995,
         "Updated rosters and hot spots; four players need the Super Multitap.",
         "NBA Jam - Tournament Edition (USA)"},
        {"snes_mk2", "Mortal Kombat II", Engine::Libretro, 2, {}, "", "snes",
         {}, true, 1994,
         "Digitized fighters, fatalities and a two-player versus mode.",
         "Mortal Kombat II (USA)"},
        {"snes_tetrisattack", "Tetris Attack", Engine::Libretro, 2, {}, "", "snes",
         {}, true, 1996,
         "Swap panels to clear blocks, with a versus mode for two players.",
         "Tetris Attack (USA) (En,Ja)"},
        {"snes_contra3", "Contra III: The Alien Wars", Engine::Libretro, 2, {}, "", "snes",
         {}, true, 1992,
         "Run-and-gun co-op with Mode 7 boss fights.",
         "Contra III - The Alien Wars (USA)"},
        {"snes_tmnt4", "Teenage Mutant Ninja Turtles IV: Turtles in Time", Engine::Libretro, 2, {}, "", "snes",
         {}, true, 1992,
         "Scrolling arcade brawler across time periods, with two-player co-op.",
         "Teenage Mutant Ninja Turtles IV - Turtles in Time (USA)"},
        {"snes_zamn", "Zombies Ate My Neighbors", Engine::Libretro, 2, {}, "", "snes",
         {}, true, 1993,
         "Horror-movie run-and-gun for one or two players.",
         "Zombies Ate My Neighbors (USA)"},
        {"snes_yoshi", "Super Mario World 2: Yoshi's Island", Engine::Libretro, 1, {}, "", "snes",
         {}, false, 1995,
         "Yoshi carries baby Mario through hand-drawn levels.",
         "Super Mario World 2 - Yoshi's Island (USA)"},
        {"snes_mariorpg", "Super Mario RPG: Legend of the Seven Stars", Engine::Libretro, 1, {}, "", "snes",
         {}, false, 1996,
         "Mario's RPG debut with timed-button combat.",
         "Super Mario RPG - Legend of the Seven Stars (USA)"},
        {"snes_ff6", "Final Fantasy III", Engine::Libretro, 1, {}, "", "snes",
         {}, false, 1994,
         "The SNES release of Final Fantasy VI, with a large party and an opera.",
         "Final Fantasy III (USA)"},
        {"snes_earthbound", "EarthBound", Engine::Libretro, 1, {}, "", "snes",
         {}, false, 1995,
         "Modern-day RPG with rolling HP meters.",
         "EarthBound (USA)"},
        {"snes_fzero", "F-Zero", Engine::Libretro, 1, {}, "", "snes",
         {}, false, 1990,
         "Hover-car racing with Mode 7 tracks.",
         "F-Zero (USA)"},
        {"snes_punchout", "Super Punch-Out!!", Engine::Libretro, 1, {}, "", "snes",
         {}, false, 1994,
         "Boxing circuit against caricatured opponents.",
         "Super Punch-Out!! (USA)"},
        {"snes_megamanx", "Mega Man X", Engine::Libretro, 1, {}, "", "snes",
         {}, false, 1993,
         "Dash, wall-jump and take on eight Mavericks.",
         "Mega Man X (USA)"},
        {"snes_pocky", "Pocky & Rocky", Engine::Libretro, 2, {}, "", "snes",
         {}, true, 1993,
         "Top-down shooter with cooperative play.",
         "Pocky & Rocky (USA)"},
        {"snes_killerinstinct", "Killer Instinct", Engine::Libretro, 2, {}, "", "snes",
         {}, true, 1995,
         "Combo-driven fighter with pre-rendered fighters.",
         "Killer Instinct (USA)"},
        {"snes_ff2", "Final Fight 2", Engine::Libretro, 2, {}, "", "snes",
         {}, true, 1993,
         "Beat-'em-up with two-player co-op.",
         "Final Fight 2 (USA)"},
        {"snes_kdc", "Kirby's Dream Course", Engine::Libretro, 2, {}, "", "snes",
         {}, false, 1994,
         "Isometric golf-style game; two players alternate turns.",
         "Kirby's Dream Course (USA)"},
        {"snes_supertennis", "Super Tennis", Engine::Libretro, 2, {}, "", "snes",
         {}, true, 1991,
         "Tennis with singles and doubles for up to two players.",
         "Super Tennis (USA)"},
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

bool link_cable_game(const std::string& id) {
    static const char* ids[] = {"pokered", "pokeyellow", "pokegold", "pokesilver", "pokecrystal",
                                 "pokeruby", "pokesapphire", "pokeemerald", "pokefirered", "pokeleafgreen"};
    for (const char* x : ids) if (id == x) return true;
    return false;
}

int online_max_for(const GameInfo& g) {
    int n = g.online_max > 0 ? g.online_max : g.max_players;
    if (g.system == "ps2") n = std::min(n, 2);   // the LRPS2 core has two controller ports and no multitap option
    return n;
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
    static const char* const kExts[] = {".z64", ".v64", ".n64", ".gba", ".gb", ".gbc", ".iso", ".rvz", ".zip", ".nds", ".3ds", ".cci", ".cue", ".chd", ".pbp", ".m3u"};
    std::string ext = ascii_lower(path_utf8(p.extension()));
    for (const char* e : kExts)
        if (ext == e) return true;
    return false;
}

bool is_library_folder_name(const fs::path& p) {
    std::string n = ascii_lower(path_utf8(p.filename()));
    return n == "roms" || n == "games" || n == "n64" || n == "gba" || n == "gbc" || n == "gb" || n == "nds" || n == "3ds" || n == "ps1" || n == "psx" || n == "playstation" || n.rfind("games", 0) == 0;
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
        collect_library_folders(b, 2, cands);
        // never the bare Desktop/Documents/Downloads: a stray .zip there is not a game library; only named library folders below them
    }

    std::vector<std::string> seen;
    for (const fs::path& c : cands) {
        if (out.size() >= max_out) break;
        int budget = 4000;
        if (!holds_rom_files(c, 2, budget)) continue;
        std::string s = path_utf8(c);
        if (std::find(seen.begin(), seen.end(), s) != seen.end()) continue;
        bool nested = false;   // a folder inside one already suggested adds nothing (the parent scan covers it)
        for (const std::string& q : seen)
            if (s.size() > q.size() && s.compare(0, q.size(), q) == 0 && s[q.size()] == '\\') nested = true;
        if (nested) continue;
        seen.push_back(s);
        out.push_back(s);
    }
    return out;
}

/* ---- paths ---- */

std::string exe_dir() { return winutil::exe_dir(); }

std::string data_dir() {
    std::string dir = exe_dir() + "userdata\\";
    std::error_code ec;
    fs::create_directories(utf8_path(dir), ec);
    return dir;
}

} // namespace hub
