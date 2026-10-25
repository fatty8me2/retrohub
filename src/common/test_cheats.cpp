// Cheat model: JSON round trip, per-format/per-system code validation, N64 conversion, retro strings, hash.
#include <cstdio>
#include <filesystem>
#include <string>

#include "cheats.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

using namespace cheats;

static Cheat mk(const char* id, Format f, std::vector<std::string> codes, bool en = true) {
    Cheat c; c.id = id; c.desc = id; c.format = f; c.codes = std::move(codes); c.enabled = en; return c;
}

int main() {
    // JSON
    CheatSet s; std::string err;
    CHECK(parse_json(R"({"version":1,"cheats":[
        {"id":"a","desc":"Unlock all characters","format":"gameshark","codes":["8033B21E 0008","8133B21F 0001"],"enabled":true},
        {"id":"b","desc":"x","format":"GG","codes":["abc-def-123"]},
        {"id":"a","desc":"dup","format":"raw","codes":[]},
        {"desc":"noid","format":"bogus","codes":["1"],"enabled":true}]})", &s, &err));
    CHECK(s.cheats.size() == 3);
    CHECK(s.cheats[0].enabled && s.cheats[0].codes.size() == 2 && s.cheats[0].format == Format::GameShark);
    CHECK(s.cheats[1].format == Format::GameGenie && !s.cheats[1].enabled);
    CHECK(s.cheats[2].id == "cheat4" && s.cheats[2].format == Format::Raw);
    CHECK(!parse_json("[1]", &s, &err));
    CheatSet r2;
    CHECK(parse_json(to_json(s), &r2, &err) && r2.cheats.size() == 3 && r2.cheats[0].codes[1] == "8133B21F 0001");
    std::string tmp = (std::filesystem::temp_directory_path() / "cheats_test.json").u8string();
    CHECK(save_file(tmp, s, &err));
    CheatSet r3; CHECK(load_file(tmp, &r3, &err) && r3.cheats.size() == 3);
    FileWatch w(tmp);
    CHECK(!w.changed());
    CHECK(save_file(tmp, r2, &err));
    std::filesystem::remove(tmp);
    CHECK(w.changed());
    CHECK(!load_file(tmp, &r3, &err));

    // N64
    std::vector<N64Code> n;
    CHECK(n64_codes(mk("n", Format::GameShark, {"8033b21e 0008", "81 33B21E"}), &n, &err) == false);   // bad second line
    CHECK(n64_codes(mk("n", Format::GameShark, {"8033b21e 0008", "D1330000:1234", "50000402 0000", "F1000000 00FF"}), &n, &err));
    CHECK(n.size() == 4 && n[0].address == 0x8033B21E && n[0].value == 8 && n[1].address == 0xD1330000 && n[1].value == 0x1234);
    CHECK(n64_codes(mk("n", Format::Raw, {"A0123456 0001"}), &n, &err));
    // device-control words are dropped, not fatal
    CHECK(n64_codes(mk("n", Format::GameShark, {"8033B21E 0008", "DE000000 0000", "EE000000 0000", "FF500000 0000", "00001300 0000"}), &n, &err) && n.size() == 1);
    CHECK(n64_codes(mk("n", Format::GameShark, {"DE000400 EE000000"}), &n, &err) && n.empty());
    CHECK(!n64_codes(mk("n", Format::GameShark, {"D0123456 0001", "DE000000 0000"}), &n, &err));   // would shift the guard
    CHECK(!n64_codes(mk("n", Format::GameShark, {"D899BE59 00A9"}), &n, &err) && err.find("D8") != std::string::npos);
    CHECK(!n64_codes(mk("n", Format::GameShark, {"8001A0E7 XXXX"}), &n, &err) && err.find("XXXX") != std::string::npos);
    // joined codes
    CHECK(n64_codes(mk("n", Format::GameShark, {"81103B66 0021;50000D01 0000;80118F38 0001"}), &n, &err) && n.size() == 3);
    CHECK(n64_codes(mk("n", Format::GameShark, {"D0120F70 0020,801A719D 00EF"}), &n, &err) && n.size() == 2);
    {   // 88/89: fire-once split, with the guarding conditional travelling along
        std::vector<N64Code> core, once;
        CHECK(n64_codes(mk("n", Format::GameShark, {"80000010 0001", "D1000020 0005", "89000030 1234", "88000040 0007"}), &n, &err));
        n64_split_button(n, &core, &once);
        CHECK(core.size() == 1 && core[0].address == 0x80000010u);
        CHECK(once.size() == 3 && (once[0].address >> 24) == 0xD1 && (once[1].address >> 24) == 0x89 && (once[2].address >> 24) == 0x88);
        CHECK(n64_is_button_cheat(mk("n", Format::GameShark, {"88000040 0007"})));
        CHECK(!n64_is_button_cheat(mk("n", Format::GameShark, {"80000040 0007"})));
        CHECK(!n64_codes(mk("n", Format::GameShark, {"50000201 0000", "88000040 0007"}), &n, &err));
        CheatSet fs; fs.cheats.push_back(mk("f", Format::GameShark, {"88000040 0007"})); fs.cheats[0].fire = 3;
        CheatSet f2; CHECK(parse_json(to_json(fs), &f2, &err) && f2.cheats[0].fire == 3);
    }
    CHECK(!n64_codes(mk("n", Format::GameGenie, {"ABC-DEF"}), &n, &err));
    CHECK(!n64_codes(mk("n", Format::GameShark, {}), &n, &err));
    CHECK(n64_codes(mk("n", Format::ActionReplay, {"88123456 0001"}), &n, &err) && n64_needs_gs_button(n));
    CHECK(n64_codes(mk("n", Format::GameShark, {"8012345 0001"}), &n, &err) == false);

    // libretro strings
    std::string o;
    CHECK(retro_code_string(Target::GBA, mk("g", Format::ActionReplay, {"82003e00 00000012", "1234abcd 00000001"}), &o, &err) && o == "82003E00 00000012+1234ABCD 00000001");
    CHECK(retro_code_string(Target::GBA, mk("g", Format::CodeBreaker, {"82003E00 0012"}), &o, &err) && o == "82003E00 0012");
    CHECK(!retro_code_string(Target::GBA, mk("g", Format::GameGenie, {"ABC-DEF"}), &o, &err));
    CHECK(!retro_code_string(Target::GBA, mk("g", Format::CodeBreaker, {"82003E00 00000012"}), &o, &err));
    CHECK(retro_code_string(Target::GB, mk("g", Format::GameShark, {"01ff00d0"}), &o, &err) && o == "01FF00D0");
    CHECK(retro_code_string(Target::GB, mk("g", Format::GameGenie, {"00a-17f-3b9", "abc-def"}), &o, &err) && o == "00A-17F-3B9+ABC-DEF");
    CHECK(!retro_code_string(Target::GB, mk("g", Format::GameGenie, {"00a17f3b9"}), &o, &err));
    CHECK(!retro_code_string(Target::GB, mk("g", Format::CodeBreaker, {"01ff00d0"}), &o, &err));
    CHECK(retro_code_string(Target::DS, mk("d", Format::ActionReplay, {"02000000 00000001"}), &o, &err) && o == "02000000 00000001");
    CHECK(retro_code_string(Target::PS1, mk("p", Format::GameShark, {"800A1B2C 03E8"}), &o, &err) && o == "800A1B2C 03E8");
    CHECK(!retro_code_string(Target::PS1, mk("p", Format::GameShark, {"800A1B2C 03E8 01"}), &o, &err));

    // systems + hash
    Target t;
    CHECK(target_for_system("GBC", &t) && t == Target::GB);
    CHECK(target_for_system("nds", &t) && t == Target::DS);
    CHECK(target_for_system("gc", &t) && t == Target::GC);
    CHECK(!target_for_system("wiiu", &t));
    CheatSet a, b;
    a.cheats = {mk("x", Format::GameShark, {"8033b21e 0008"}), mk("y", Format::GameShark, {"80000000 0001"}, false)};
    b.cheats = {mk("z", Format::ActionReplay, {"8033B21E:0008"}, true)};
    CHECK(set_hash(a, Target::N64) != kEmptyHash);
    CHECK(set_hash(a, Target::N64) != set_hash(b, Target::N64));   // format is part of the hash
    b.cheats[0].format = Format::GameShark;
    CHECK(set_hash(a, Target::N64) == set_hash(b, Target::N64));   // id, case, separators and disabled cheats do not matter
    a.cheats[1].enabled = true;
    CHECK(set_hash(a, Target::N64) != set_hash(b, Target::N64));
    CheatSet e;
    CHECK(set_hash(e, Target::N64) == kEmptyHash);
    CheatSet rev = a; std::swap(rev.cheats[0], rev.cheats[1]);
    CHECK(set_hash(a, Target::N64) == set_hash(rev, Target::N64));

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("cheats_test: ok\n");
    return 0;
}
