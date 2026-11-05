#include "transferpak_logic.h"
#include <cstdio>
using namespace hubui::tpak;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    CHECK(uses_transfer_pak("pokestadium") && uses_transfer_pak("pokestadium2") && uses_transfer_pak("mariogolf") && uses_transfer_pak("mariotennis") && uses_transfer_pak("perfectdark"));
    CHECK(!uses_transfer_pak("remix") && !uses_transfer_pak(""));

    std::vector<GbGame> lib = {
        {"gbc:a", "Pokemon Red", "pokered", false}, {"gbc:b", "POKEMON_SLV", "", true}, {"gbc:c", "PM_CRYSTAL", "pokecrystal", true},
        {"gb:d", "Pokemon Blue Version (USA)", "", false}, {"gbc:e", "POKEMON PINBALL", "", true}, {"gbc:f", "MARIO GOLF", "", true},
        {"gbc:g", "Mario Tennis", "", true}, {"gbc:h", "PERFECT DARK", "", true}, {"gb:i", "Tetris", "", false}, {"gbc:j", "Pokemon Trading Card Game", "", true}};
    auto s1 = compatible("pokestadium", lib);
    CHECK(s1.size() == 2 && s1[0].key == "gb:d" && s1[1].key == "gbc:a");   // Red, Blue only; sorted by title
    auto s2 = compatible("pokestadium2", lib);
    CHECK(s2.size() == 4 && s2[0].key == "gbc:c" && s2[1].key == "gbc:b");   // gen 2 first, gen 1 after
    CHECK(s2[2].key == "gb:d" && s2[3].key == "gbc:a");
    CHECK(compatible("mariogolf", lib).size() == 1 && compatible("mariogolf", lib)[0].key == "gbc:f");
    CHECK(compatible("mariotennis", lib).size() == 1 && compatible("perfectdark", lib).size() == 1);
    CHECK(compatible("remix", lib).empty());
    CHECK(pokemon_gen("", "POKEMON YEL") == 1 && pokemon_gen("", "Pokemon - Gold Version (USA, Europe)") == 2 && pokemon_gen("", "Pokemon Puzzle Challenge") == 0);

    // default / stored choice
    CHECK(resolve_choice(nullptr, compatible("mariogolf", lib)) == "gbc:f");   // exactly one: auto
    CHECK(resolve_choice(nullptr, s2).empty());                                 // several: none
    std::string none, pick = "gbc:b", gone = "gbc:zzz";
    CHECK(resolve_choice(&none, compatible("mariogolf", lib)).empty());         // explicit None wins over auto
    CHECK(resolve_choice(&pick, s2) == "gbc:b");
    CHECK(resolve_choice(&gone, s2).empty() && resolve_choice(&gone, compatible("mariogolf", lib)) == "gbc:f");
    CHECK(resolve_choice(nullptr, {}).empty());

    // save file name (retrorun rules)
    CHECK(save_file_name("C:/r/Pokemon Gold.gbc", "", {}) == "Pokemon Gold.srm");
    CHECK(save_file_name("C:/r/rom.gbc", "Inner Name", {"Inner Name.srm"}) == "Inner Name.srm");
    CHECK(save_file_name("C:/r/New.gbc", "", {"Old.srm", "Old.rtc"}) == "Old.srm");                 // lone .srm
    CHECK(save_file_name("C:/r/New.gbc", "", {"A.srm", "B.srm"}) == "New.srm");                      // ambiguous: its own
    CHECK(save_file_name("C:/r/Gold.gbc", "", {"GOLD.SRM"}) == "GOLD.SRM");
    CHECK(path_stem("a/b/c.d.gb") == "c.d");

    // expected size
    std::vector<uint8_t> h(0x150, 0);
    h[0x149] = 3; CHECK(expected_ram_size(h) == 32768);
    h[0x149] = 2; CHECK(expected_ram_size(h) == 8192);
    h[0x149] = 0; h[0x147] = 5; CHECK(expected_ram_size(h) == 512);
    h[0x147] = 0; CHECK(expected_ram_size(h) == 0);
    CHECK(expected_ram_size({}) == 0);
    CHECK(save_size_ok(32768, false, 0) && save_size_ok(32768, true, 32768) && !save_size_ok(32768, true, 100) && !save_size_ok(8192, true, 8240) && save_size_ok(0, true, 5));

    // backups: keep 3
    std::vector<std::string> b = {"G.srm.20260101000000.bak", "G.srm.20260103000000.bak", "G.srm.20260102000000.bak", "G.srm.20260105000000.bak", "G.srm.20260104000000.bak", "notes.txt"};
    auto del = backups_to_prune(b);
    CHECK(del.size() == 2 && del[0] == "G.srm.20260101000000.bak" && del[1] == "G.srm.20260102000000.bak");
    CHECK(backups_to_prune({"a.bak", "b.bak"}).empty());
    CHECK(backup_name("G.srm", "20260101000000") == "G.srm.20260101000000.bak");

    // args
    auto q = [](const std::string& s) { return "\"" + s + "\""; };
    CHECK(launch_args("C:/g.gbc", "C:/v/g.srm", q) == " --tpak-rom \"C:/g.gbc\" --tpak-save \"C:/v/g.srm\"");
    CHECK(launch_args("", "x", q).empty());
    CHECK(args_allowed("--rom x") && !args_allowed("--rom x --net ws://a"));

    std::printf("%s (%d failure(s))\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
