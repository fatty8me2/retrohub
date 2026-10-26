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

    // ---- value cheats (placeholder digits) ----
    {
        Cheat v = mk("v", Format::GameShark, {"8001A0E7 XXXX"}); v.desc = "Play As";
        CHECK(value_has_placeholder(v));
        CHECK(!value_has_placeholder(mk("p", Format::GameShark, {"8001A0E7 00FF"})));
        std::vector<N64Code> nc; std::string e;
        CHECK(!n64_codes(v, &nc, &e));                       // still rejected raw
        ValueSpec sp = value_spec(v, Target::N64);
        CHECK(sp.valid && sp.digits == 4 && sp.min == 0 && sp.max == 0xFF);   // 80 = 8-bit write
        Cheat f;
        CHECK(value_apply(v, 0x2A, Target::N64, &f) && f.codes[0] == "8001A0E7 002A");
        CHECK(value_apply(v, 0x1234, Target::N64, &f) && f.codes[0] == "8001A0E7 00FF");   // clamped
        CHECK(n64_codes(f, &nc, &e) && nc.size() == 1 && nc[0].value == 0xFF);
        Cheat w = mk("w", Format::GameShark, {"8101A0E8 xxxx"});
        sp = value_spec(w, Target::N64);
        CHECK(sp.max == 0xFFFF);
        CHECK(value_apply(w, 0xBEEF, Target::N64, &f) && f.codes[0] == "8101A0E8 BEEF");
        // several lines / mixed with a plain guard, one value for all slots
        Cheat m = mk("m", Format::GameShark, {"D01C6A7D 0010;8011895F XXXX", "80118960 ??"});
        CHECK(value_has_placeholder(m));
        sp = value_spec(m, Target::N64);
        CHECK(sp.digits == 4);
        CHECK(value_apply(m, 0x0C, Target::N64, &f) && f.codes[0] == "D01C6A7D 0010;8011895F 000C" && f.codes[1] == "80118960 0C");
        // GB: 01VVAAAA
        Cheat g = mk("g", Format::GameShark, {"01??DCC4"}); g.desc = "Level Modifier (00-0F)";
        sp = value_spec(g, Target::GB);
        CHECK(sp.digits == 2 && sp.min == 0 && sp.max == 0x0F && sp.prefer_hex);
        CHECK(value_apply(g, 9, Target::GB, &f) && f.codes[0] == "0109DCC4");
        std::string o;
        CHECK(retro_code_string(Target::GB, f, &o, nullptr) && o == "0109DCC4");
        Cheat g1 = mk("g1", Format::GameShark, {"010?07C0"});
        CHECK(value_spec(g1, Target::GB).digits == 1 && value_spec(g1, Target::GB).max == 15);
        // PS1 / GBA pairs
        Cheat ps = mk("ps", Format::GameShark, {"800A0F34 000?"});
        CHECK(value_spec(ps, Target::PS1).max == 15);
        CHECK(value_apply(ps, 7, Target::PS1, &f) && f.codes[0] == "800A0F34 0007" && retro_code_string(Target::PS1, f, &o, nullptr));
        Cheat ps8 = mk("ps8", Format::GameShark, {"300D1368 00??"});
        CHECK(value_spec(ps8, Target::PS1).max == 0xFF);
        Cheat ps16 = mk("ps16", Format::GameShark, {"800A0F34 ????"});
        CHECK(value_spec(ps16, Target::PS1).max == 0xFFFF);
        // hints
        Cheat h1 = mk("h1", Format::GameShark, {"8011BB25 XXXX"}); h1.desc = "Lives You Start With (0-63 Hexidecimal)";
        sp = value_spec(h1, Target::N64);
        CHECK(sp.min == 0 && sp.max == 0x63 && sp.prefer_hex);
        h1.desc = "Level (00-03)";
        sp = value_spec(h1, Target::N64);
        CHECK(sp.max == 3 && !sp.prefer_hex);
        h1.desc = "Level (enter 1 to 9)";
        sp = value_spec(h1, Target::N64);
        CHECK(sp.min == 1 && sp.max == 9);
        h1.desc = "Money - enter 0-99 decimal";
        sp = value_spec(h1, Target::N64);
        CHECK(sp.min == 0 && sp.max == 99);
        h1.desc = "Catch Pokemon 152-251 (97-FB)";
        sp = value_spec(h1, Target::N64);
        CHECK(sp.min == 0x97 && sp.max == 0xFB);
        h1.desc = "Item (Can still be killed)";
        sp = value_spec(h1, Target::N64);
        CHECK(sp.max == 0xFF && sp.options.empty());
        h1.desc = "Hold item: 01=Potion 02=Antidote, 0A=Elixir";
        sp = value_spec(h1, Target::N64);
        CHECK(sp.options.size() == 3 && sp.options[0].value == 1 && sp.options[0].label == "Potion" && sp.options[2].value == 0x0A && sp.options[2].label == "Elixir");
        CHECK(sp.min == 1 && sp.max == 0x0A);
        CHECK(value_clamp(sp, 0) == 1 && value_clamp(sp, 99) == 0x0A);
        // not a value cheat: other junk in the code
        CHECK(!value_has_placeholder(mk("j", Format::GameShark, {"8001A0E7 XXXX", "GARBAGE"})));
        CHECK(!value_has_placeholder(mk("gg", Format::GameGenie, {"0AF-4E1-3C1"})));
        CHECK(!value_apply(mk("p", Format::GameShark, {"8001A0E7 00FF"}), 5, Target::N64, &f));
        // number text
        uint32_t n;
        CHECK(value_parse("99", true, &n) && n == 99);
        CHECK(value_parse("99", false, &n) && n == 0x99);
        CHECK(value_parse("0x63", true, &n) && n == 0x63);
        CHECK(value_parse("FFh", true, &n) && n == 255);
        CHECK(!value_parse("", true, &n) && !value_parse("12a", true, &n) && !value_parse("FFFFFFFFFF", false, &n));
        CHECK(value_format(99, true, 4) == "99" && value_format(99, false, 4) == "0063");
        // the net hash follows the substituted value
        CheatSet a, b;
        Cheat e1 = v; e1.enabled = true; Cheat e2 = v; e2.enabled = true;
        value_apply(v, 3, Target::N64, &e1); e1.enabled = true;
        value_apply(v, 4, Target::N64, &e2); e2.enabled = true;
        a.cheats.push_back(e1); b.cheats.push_back(e2);
        CHECK(set_hash(a, Target::N64) != set_hash(b, Target::N64));
    }

    std::printf("cheats_test: ok\n");
    return 0;
}
