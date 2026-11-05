// Console test for X2 (GameCube <-> GBA): game list / match, choice store, Dolphin.ini writing, save round trip, coexistence with prepare_dolphin.
#include "emulators.h"
#include "gba_link_logic.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static std::string rd(const fs::path& p) { std::string s; hubgba::read_all(p.u8string(), &s); return s; }
static void wr(const fs::path& p, const std::string& s) { hubgba::write_atomic(p.u8string(), s); }

int main() {
    using namespace hubgba;
    fs::path dir = fs::temp_directory_path() / ("gbalink_test_" + std::to_string((long long)std::time(nullptr)));
    fs::remove_all(dir);
    fs::create_directories(dir);

    /* targets */
    CHECK(target_for("GM8E01") && target_for("GM8E01")->need == Need::Rom);
    CHECK(target_for("gm8p01") != nullptr);
    CHECK(target_for("G4SE01") && target_for("G4SE01")->need == Need::Controller && target_for("G4SE01")->ports == 3);
    CHECK(target_for("GZLE01") && target_for("GCCE01") && target_for("GC6E01") && target_for("GXXE01"));
    CHECK(!target_for("GALE01") && !target_for("") && !target_for("GM8"));   // Melee has no GBA link; a short ID is not a match
    std::vector<Cand> lib = {{"k1", "Zelda Minish Cap", "BZMP01", "a"}, {"k2", "Pokemon Ruby", "AXVE01", "b"}, {"k3", "Metroid Fusion", "AMTE01", "c"},
                             {"k4", "Pokemon Emerald", "BPEE01", "d"}, {"k5", "Advance Wars", "AWRE01", "e"}};
    size_t nm = 0;
    auto o = ordered(target_for("GC6E01"), lib, &nm);
    CHECK(nm == 2 && lib[o[0]].key == "k4" && lib[o[1]].key == "k2");   // best matches first, by title
    CHECK(lib[o[2]].key == "k5" && lib[o[3]].key == "k3" && lib[o[4]].key == "k1");
    o = ordered(target_for("GM8E01"), lib, &nm);
    CHECK(nm == 1 && lib[o[0]].key == "k3");
    o = ordered(target_for("G4SE01"), lib, &nm);
    CHECK(nm == 0 && o.size() == 5);

    /* choices + store */
    CHECK(parse_choice("").kind == Kind::Off && parse_choice("ctl").kind == Kind::Controller);
    Choice c = parse_choice(game_choice("gba:axve-1234abcd"));
    CHECK(c.kind == Kind::Game && c.key == "gba:axve-1234abcd");
    std::map<std::string, Ports> st;
    Ports p; p[1] = "ctl"; p[2] = game_choice("gba:x-1"); p[0] = "ctl";
    st["gc:g4se01-0"] = p;
    st["gc:off-0"] = Ports();   // all Off: not stored
    std::string js = store_to_json(st);
    CHECK(js.find("gc:off-0") == std::string::npos);
    std::map<std::string, Ports> back;
    CHECK(store_from_json(js, &back) && back.size() == 1 && back["gc:g4se01-0"][1] == "ctl" && back["gc:g4se01-0"][2] == game_choice("gba:x-1") && back["gc:g4se01-0"][0].empty());
    CHECK(!store_from_json("not json", &back) && back.empty());
    CHECK(store_from_json("{}", &back) && back.empty());

    /* ini */
    IniPlan plan;
    plan.on[1] = true; plan.on[3] = true;
    plan.rom[3] = "D:\\roms\\Pokemon Ruby.gba";
    plan.bios = "D:\\bios\\gba_bios.bin";
    plan.saves_dir = "C:\\RH\\dolphin\\User\\GBA\\Saves";
    std::string ini = ini_apply("[Core]\nSIDevice0 = 6\nSIDevice1 = 6\n[GBA]\nRom4 = old\n", plan);
    CHECK(hubui::ini_get(ini, "Core", "SIDevice1") == "13" && hubui::ini_get(ini, "Core", "SIDevice3") == "13");
    CHECK(hubui::ini_get(ini, "Core", "SIDevice0") == "6");                    // port 1 keeps the pad
    CHECK(hubui::ini_get(ini, "Core", "SIDevice2").empty());                   // an Off port is not written
    CHECK(hubui::ini_get(ini, "GBA", "BIOS") == "D:/bios/gba_bios.bin");
    CHECK(hubui::ini_get(ini, "GBA", "Rom4") == "D:/roms/Pokemon Ruby.gba" && hubui::ini_get(ini, "GBA", "Rom2") == "");
    CHECK(hubui::ini_get(ini, "GBA", "SavesPath") == "C:/RH/dolphin/User/GBA/Saves/" && hubui::ini_get(ini, "GBA", "SavesInRomPath") == "False");
    CHECK(ini.find("SIDevice1 = 13") != std::string::npos);

    /* coexistence with prepare_dolphin: the next launch's pads win again, [GBA] stays harmless */
    fs::path dol = dir / "dol";
    hubemu::DolphinPads pads;
    pads.xinput[0] = true;
    hubemu::prepare_dolphin(dol.string(), pads);
    fs::path ini_path = dol / "User" / "Config" / "Dolphin.ini";
    std::string before = rd(ini_path);
    wr(ini_path, ini_apply(before, plan));
    CHECK(hubui::ini_get(rd(ini_path), "Core", "SIDevice1") == "13");
    hubemu::prepare_dolphin(dol.string(), pads);                              // the following launch (or netplay)
    std::string after = rd(ini_path);
    CHECK(hubui::ini_get(after, "Core", "SIDevice1") == "0" && hubui::ini_get(after, "Core", "SIDevice3") == "0" && hubui::ini_get(after, "Core", "SIDevice0") == "6");
    CHECK(hubui::ini_get(after, "Core", "SkipIPL") == "True" && hubui::ini_get(after, "Core", "SlotA") == "8");   // the pads writer is intact
    hubemu::DolphinPads two = pads; two.xinput[1] = true;
    wr(ini_path, ini_apply(after, plan));
    hubemu::prepare_dolphin(dol.string(), two);
    CHECK(hubui::ini_get(rd(ini_path), "Core", "SIDevice1") == "6");

    /* names: retrorun's vault file and Dolphin's save file */
    CHECK(vault_file("C:\\v\\gba\\k\\battery\\", "D:\\r\\Pokemon Ruby.gba", "") == "C:\\v\\gba\\k\\battery\\Pokemon Ruby.srm");
    CHECK(vault_file("C:\\v\\battery", "C:\\cache\\rom.gba", "Inner Name") == "C:\\v\\battery\\Inner Name.srm");   // zipped ROM: the save stem
    CHECK(dolphin_save_file("C:\\s\\", "D:\\r\\Pokemon Ruby.gba", 1) == "C:\\s\\Pokemon Ruby-2.sav");
    CHECK(dolphin_save_file("C:/s", "D:/r/Fusion.v2.gba", 3) == "C:/s\\Fusion.v2-4.sav");

    /* save round trip */
    Staged s;
    s.port = 1; s.game_key = "k"; s.system_short = "gba"; s.title = "T";
    s.vault = (dir / "vault" / "battery" / "Ruby.srm").u8string();
    s.dolphin = (dir / "dol" / "User" / "GBA" / "Saves" / "Ruby-2.sav").u8string();
    s.backup_dir = (dir / "bak").u8string();
    std::string save1(32768, 'A');
    // no vault save, nothing from Dolphin: nothing happens
    std::string err;
    CHECK(stage_save(s, &err) && !fs::exists(s.dolphin));
    CHECK(finish_save(s, &err) == Back::Nothing);
    // a blank 0xFF save Dolphin made on its own never creates a vault entry
    wr(s.dolphin, std::string(32768, '\xFF'));
    CHECK(finish_save(s, &err) == Back::Nothing && !fs::exists(s.vault));
    fs::remove(s.dolphin);
    // vault save -> Dolphin
    wr(s.vault, save1);
    CHECK(stage_save(s, &err) && rd(s.dolphin) == save1);
    CHECK(finish_save(s, &err) == Back::Nothing);                              // unchanged: no copy, no backup
    CHECK(!fs::exists(s.backup_dir));
    // played: Dolphin changed the save -> vault (backup of the old vault copy first)
    std::string save2(32768, 'B');
    wr(s.dolphin, save2);
    CHECK(finish_save(s, &err) == Back::Copied && rd(s.vault) == save2);
    CHECK(rd(fs::path(s.backup_dir) / "Ruby.srm.1.bak") == save1);
    // clock block (Emerald / Ruby): 16 bytes after the save are cut off
    wr(s.dolphin, save2 + std::string(16, 'R'));
    CHECK(finish_save(s, &err) == Back::Nothing);                              // same save, only the clock differs from nothing
    std::string save3(131072, 'C');
    wr(s.dolphin, save3 + std::string(16, 'R'));
    CHECK(finish_save(s, &err) == Back::Copied && rd(s.vault) == save3);
    // Dolphin file left from an earlier session that never came back: backed up before the next launch replaces it
    wr(s.dolphin, std::string(32768, 'Z'));
    CHECK(stage_save(s, &err) && rd(s.dolphin) == save3);
    bool found_z = false;
    for (auto& f : fs::directory_iterator(s.backup_dir)) if (rd(f.path()) == std::string(32768, 'Z')) found_z = true;
    CHECK(found_z);
    // oversized / empty files are ignored
    wr(s.dolphin, "");
    CHECK(finish_save(s, &err) == Back::Nothing && rd(s.vault) == save3);
    wr(s.dolphin, std::string((1u << 20) + 1, 'x'));
    CHECK(finish_save(s, &err) == Back::Nothing && rd(s.vault) == save3);
    // raw bytes of a vault save and the Dolphin save are the same format: no header, no conversion
    CHECK(normalize_save(std::string(512, 'e')).size() == 512 && normalize_save(std::string(8192 + 16, 'e')).size() == 8192 && normalize_save(std::string(100, 'e')).size() == 100);

    fs::remove_all(dir);
    std::printf(fails ? "gba_link_test: %d FAILED\n" : "gba_link_test: all passed\n", fails);
    return fails ? 1 : 0;
}
