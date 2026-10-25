// GameCube cheats: format validation (gecko / ar_gc), Dolphin GameSettings ini patching, EnableCheats, file writer.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "cheat_store.h"
#include "cheats.h"
#include "dolphin_cheats.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

using namespace cheats;
namespace fs = std::filesystem;

static Cheat mk(const char* desc, Format f, std::vector<std::string> codes) {
    Cheat c; c.id = desc; c.desc = desc; c.format = f; c.codes = std::move(codes); c.enabled = true; return c;
}
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }
static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

int main() {
    std::string out, err;
    // ---- validation
    CHECK(normalize_code(Target::GC, Format::Gecko, "0445bf28 ffffffff", &out, &err) && out == "0445BF28 FFFFFFFF");
    CHECK(normalize_code(Target::GC, Format::Gecko, "0445BF28FFFFFFFF", &out, &err) && out == "0445BF28 FFFFFFFF");
    CHECK(!normalize_code(Target::GC, Format::Gecko, "0445BF28 FFFF", &out, &err));
    CHECK(!normalize_code(Target::GC, Format::Gecko, "0445BF28 FFFFFFFFF", &out, &err));
    CHECK(normalize_code(Target::GC, Format::ActionReplayGC, "0045C370 000000FF", &out, &err) && out == "0045C370 000000FF");
    CHECK(normalize_code(Target::GC, Format::ActionReplayGC, "n0rx-3tf0-d2f6k", &out, &err) && out == "N0RX-3TF0-D2F6K");
    CHECK(normalize_code(Target::GC, Format::ActionReplay, "0045C370 000000FF", &out, &err));
    CHECK(!normalize_code(Target::GC, Format::ActionReplayGC, "N0RX-3TF0-D2F6", &out, &err));
    CHECK(!normalize_code(Target::GC, Format::GameShark, "0445BF28 FFFFFFFF", &out, &err));
    CHECK(!normalize_code(Target::GC, Format::Raw, "0445BF28 FFFFFFFF", &out, &err));
    CHECK(!normalize_code(Target::N64, Format::Gecko, "8033B21E 0008", &out, &err));
    Format f; Target t;
    CHECK(parse_format("gecko", &f) && f == Format::Gecko && std::string(format_name(f)) == "gecko");
    CHECK(parse_format("AR_GC", &f) && f == Format::ActionReplayGC && std::string(format_name(f)) == "ar_gc");
    CHECK(target_for_system("gc", &t) && t == Target::GC);
    CHECK(cheatstore::detect_format(Target::GC, "0445BF28 FFFFFFFF", &f) && f == Format::Gecko);
    CHECK(cheatstore::detect_format(Target::GC, "N0RX-3TF0-D2F6K", &f) && f == Format::ActionReplayGC);
    CHECK(!cheatstore::detect_format(Target::GC, "8033B21E 0008", &f));
    CheatSet j; CHECK(parse_json(R"({"cheats":[{"id":"a","desc":"x","format":"gecko","codes":["0445BF28 FFFFFFFF"],"enabled":true}]})", &j, &err) && j.cheats[0].format == Format::Gecko);
    CHECK(has(to_json(j), "\"gecko\""));
    CheatSet j2; CHECK(parse_json(to_json(j), &j2, &err) && j2.cheats[0].format == Format::Gecko);
    CHECK(set_hash(j, Target::GC) != kEmptyHash);
    CheatSet j3 = j; j3.cheats[0].codes[0] = "0445bf28 ffffffff"; CHECK(set_hash(j3, Target::GC) == set_hash(j, Target::GC));   // peers agree regardless of case

    // ---- ini patching
    CheatSet on;
    on.cheats.push_back(mk("Unlock all characters", Format::Gecko, {"0445BF28 FFFFFFFF", "0445BF2C FFFFFFFF"}));
    on.cheats.push_back(mk("Items off", Format::ActionReplayGC, {"0045C370 000000FF"}));
    on.cheats.push_back(mk("Broken", Format::Gecko, {"nonsense"}));
    int added = 0;
    std::string a = dolphincheats::patch_game_ini("", on, &added);
    CHECK(added == 2);
    CHECK(has(a, "[Gecko]\n$RetroHub: Unlock all characters\n0445BF28 FFFFFFFF\n0445BF2C FFFFFFFF\n"));
    CHECK(has(a, "[Gecko_Enabled]\n$RetroHub: Unlock all characters\n"));
    CHECK(has(a, "[ActionReplay]\n$RetroHub: Items off\n0045C370 000000FF\n"));
    CHECK(has(a, "[ActionReplay_Enabled]\n$RetroHub: Items off\n"));
    CHECK(!has(a, "Broken"));
    CHECK(dolphincheats::count_owned(a) == 2);
    CHECK(dolphincheats::patch_game_ini(a, on) == a);   // idempotent

    // user's codes, comments, other sections, CRLF are preserved; owned entries replaced / removed
    std::string user =
        "[Core]\r\nCPUCore = 1\r\n\r\n[Gecko]\r\n$My code\r\n*note\r\n04000000 00000001\r\n$RetroHub: Stale\r\n04000004 00000002\r\n$After\r\n04000008 00000003\r\n"
        "\r\n[Gecko_Enabled]\r\n$My code\r\n$RetroHub: Stale\r\n[Video_Settings]\r\nwidth = 5\r\n";
    std::string p = dolphincheats::patch_game_ini(user, on, &added);
    CHECK(added == 2 && !has(p, "Stale"));
    CHECK(has(p, "$My code\r\n*note\r\n04000000 00000001\r\n$After\r\n04000008 00000003\r\n"));
    CHECK(has(p, "[Gecko_Enabled]\r\n$My code\r\n$RetroHub: Unlock all characters\r\n"));
    CHECK(has(p, "[Video_Settings]\r\nwidth = 5\r\n") && has(p, "[Core]\r\nCPUCore = 1\r\n"));
    CHECK(p.find('\n') != std::string::npos && p.find("\r\n") != std::string::npos && !has(p, "[Gecko]\n"));
    CheatSet none;
    std::string c = dolphincheats::patch_game_ini(p, none, &added);
    CHECK(added == 0 && dolphincheats::count_owned(c) == 0 && !has(c, "RetroHub") && has(c, "$My code") && has(c, "$After") && has(c, "width = 5"));
    CHECK(has(c, "[Gecko_Enabled]\r\n$My code\r\n[Video_Settings]"));
    // a name that tries to be a section / another entry cannot escape
    CheatSet evil; evil.cheats.push_back(mk("a\n[Core]\n$x", Format::Gecko, {"04000000 00000001"}));
    std::string e = dolphincheats::patch_game_ini("", evil, &added);
    CHECK(added == 1 && !has(e, "\n[Core]") );

    // ---- Dolphin.ini
    CHECK(dolphincheats::enable_cheats("") == "[Core]\nEnableCheats = True\n");
    CHECK(dolphincheats::enable_cheats("[Core]\nEnableCheats = False\nCPUCore = 1\n[Interface]\nx = 1\n") == "[Core]\nEnableCheats = True\nCPUCore = 1\n[Interface]\nx = 1\n");
    CHECK(dolphincheats::enable_cheats("[Core]\nCPUCore = 1\n\n[Interface]\nx = 1\n") == "[Core]\nCPUCore = 1\nEnableCheats = True\n\n[Interface]\nx = 1\n");
    CHECK(dolphincheats::enable_cheats("[Interface]\nx = 1\n") == "[Interface]\nx = 1\n[Core]\nEnableCheats = True\n");
    CHECK(dolphincheats::game_ini_name("GALE01") == "GALE01.ini" && dolphincheats::game_ini_name("../x").empty() && dolphincheats::game_ini_name("AB").empty());

    // ---- file writer
    fs::path root = fs::temp_directory_path() / "rh_dolphin_cheats_test";
    fs::remove_all(root);
    std::string user_dir = (root / "User").u8string();
    CHECK(dolphincheats::apply_for_game(user_dir, "GALE01", none, &err) == 0);          // nothing on, nothing there: no files
    CHECK(!fs::exists(root / "User" / "GameSettings" / "GALE01.ini"));
    CHECK(dolphincheats::apply_for_game(user_dir, "GALE01", on, &err) == 2);
    CHECK(has(slurp(root / "User" / "GameSettings" / "GALE01.ini"), "$RetroHub: Items off"));
    CHECK(has(slurp(root / "User" / "Config" / "Dolphin.ini"), "EnableCheats = True"));
    CHECK(dolphincheats::apply_for_game(user_dir, "GALE01", none, &err) == 0);          // turned off: the entries are removed
    CHECK(!has(slurp(root / "User" / "GameSettings" / "GALE01.ini"), "RetroHub"));
    CHECK(dolphincheats::apply_for_game(user_dir, "", on, &err) == -1);
    fs::remove_all(root);

    std::printf(g_fail ? "FAILED %d\n" : "dolphin_cheats_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
