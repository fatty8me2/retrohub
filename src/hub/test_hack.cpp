// Console test: ROM hack identity (hack_logic.h), hack.json, base finder, patched-image build and vault scan (hack_core.h).
#include "hack_core.h"
#include "hack_logic.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubui::hacks;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

static void put32(Bytes& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back((uint8_t)(v >> (8 * i))); }
static void varint(Bytes& o, uint64_t n) { for (;;) { uint8_t x = n & 0x7f; n >>= 7; if (!n) { o.push_back(x | 0x80); break; } o.push_back(x); --n; } }
/* Same-size-ish BPS: SourceRead for equal runs, TargetRead for changed bytes. */
static Bytes bps(const Bytes& src, const Bytes& tgt) {
    Bytes o = {'B', 'P', 'S', '1'};
    varint(o, src.size()); varint(o, tgt.size()); varint(o, 0);
    size_t i = 0;
    while (i < tgt.size()) {
        size_t j = i;
        bool same = i < src.size() && src[i] == tgt[i];
        while (j < tgt.size() && (j < src.size() && src[j] == tgt[j]) == same) ++j;
        varint(o, ((uint64_t)(j - i - 1) << 2) | (same ? 0 : 1));
        if (!same) o.insert(o.end(), tgt.begin() + (long)i, tgt.begin() + (long)j);
        i = j;
    }
    put32(o, hub::patch::crc32(src.data(), src.size()));
    put32(o, hub::patch::crc32(tgt.data(), tgt.size()));
    put32(o, hub::patch::crc32(o.data(), o.size()));
    return o;
}
static Bytes ips(const std::vector<std::pair<uint32_t, Bytes>>& recs) {
    Bytes o = {'P', 'A', 'T', 'C', 'H'};
    for (auto& r : recs) {
        o.push_back((uint8_t)(r.first >> 16)); o.push_back((uint8_t)(r.first >> 8)); o.push_back((uint8_t)r.first);
        o.push_back((uint8_t)(r.second.size() >> 8)); o.push_back((uint8_t)r.second.size());
        o.insert(o.end(), r.second.begin(), r.second.end());
    }
    for (char c : std::string("EOF")) o.push_back((uint8_t)c);
    return o;
}
static void wfile(const fs::path& p, const Bytes& b) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write((const char*)b.data(), (std::streamsize)b.size());
}
static Bytes noise(size_t n, uint32_t seed) {
    Bytes b(n);
    uint32_t x = seed;
    for (auto& c : b) { x = x * 1664525u + 1013904223u; c = (uint8_t)(x >> 24); }
    return b;
}
static Cand cand(const std::string& key, const std::string& title, const fs::path& p, hub::System s, uint64_t size) {
    Cand c; c.key = key; c.title = title; c.path = p.u8string(); c.system = s; c.size = size;
    return c;
}

/* qa\hacks_headless.ps1: hub_hack_test --add <base rom> <patch> <system> <vault dir> <cache dir>
 * Runs the add path (inspect, find the base, build the patched image, write hack.json into the vault) and prints KEY= IMAGE= GAMEDIR=. */
static int e2e_add(char** a) {
    namespace hk = hubui::hacks;
    hub::System sys = hub::system_from_short(a[4]);
    Cand c;
    c.path = a[2];
    c.system = sys;
    std::error_code ec;
    c.size = fs::file_size(fs::u8path(a[2]), ec);
    ImageCrcs ic;
    if (!image_crcs(c, &ic, nullptr)) { std::printf("ERR cannot read base\n"); return 2; }
    char b[16];
    std::snprintf(b, sizeof b, "%08x", (unsigned)ic.raw);
    c.key = std::string(a[4]) + ":qa-" + b;
    c.title = "QA Base";
    hub::patch::Info info;
    std::string err;
    if (!hub::patch::inspect_file(a[3], &info, &err)) { std::printf("ERR %s\n", err.c_str()); return 3; }
    FindResult fr = find_base({c}, info, fs::u8path(a[3]).filename().u8string(), 0);
    if (fr.status != FindStatus::Found) { std::printf("ERR %s\n", fr.message.c_str()); return 4; }
    fs::path tmp = fs::u8path(a[6]) / "_new" / patched_name(sys, c.path);
    BuildResult br = build_patched(c.path, fr.prep, a[3], tmp.u8string(), nullptr);
    if (!br.ok) { std::printf("ERR %s\n", br.message.c_str()); return 5; }
    HackDef d;
    d.base_key = c.key; d.system = a[4]; d.title = "QA Hack"; d.format = "bps"; d.prep = fr.prep;
    d.out_crc = br.out_crc; d.out_size = br.out_size; d.key = make_key(c.key, br.out_crc);
    d.patch_file = fs::u8path(a[3]).filename().u8string();
    fs::path dest = fs::u8path(a[6]) / cache_folder(d.key);
    fs::remove_all(dest, ec);
    fs::rename(tmp.parent_path(), dest, ec);
    fs::path gdir = fs::u8path(a[5]) / a[4] / cache_folder(d.key);
    if (!install_hack(gdir.u8string(), d, a[3], &err)) { std::printf("ERR %s\n", err.c_str()); return 6; }
    std::printf("KEY=%s\nIMAGE=%s\nGAMEDIR=%s\n", d.key.c_str(), (dest / patched_name(sys, c.path)).u8string().c_str(), gdir.u8string().c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 7 && std::string(argv[1]) == "--add") return e2e_add(argv);
    CHECK(make_key("gba:bpre-1a2b3c4d", 0x9f8e7d6cu) == "gba:bpre-1a2b3c4d+h9f8e7d6c");
    CHECK(make_key("snes:x", 0) == "snes:x+h00000000");
    std::string base;
    uint32_t crc = 0;
    CHECK(parse_key("gba:bpre-1a2b3c4d+h9f8e7d6c", &base, &crc) && base == "gba:bpre-1a2b3c4d" && crc == 0x9f8e7d6cu);
    CHECK(is_hack_key("n64:0123abcd+h00ff00ff"));
    CHECK(!is_hack_key("gba:bpre-1a2b3c4d") && !is_hack_key("+h12345678") && !is_hack_key("gba:x+h1234567") &&
          !is_hack_key("gba:x+h1234567g") && !is_hack_key("gba:x+h123456789") && !is_hack_key("gba:x.hack-12345678"));
    CHECK(parse_key(make_key("gc:file-mario+luigi", 0x12345678u), &base, &crc) && base == "gc:file-mario+luigi" && crc == 0x12345678u);   // last suffix wins

    /* hack.json round trip and strictness */
    HackDef d;
    d.key = make_key("gba:bpre-1a2b3c4d", 0x9f8e7d6cu); d.base_key = "gba:bpre-1a2b3c4d"; d.system = "gba"; d.title = "Radical Red \"v4\"";
    d.kind = "translation"; d.lang = "pt-br"; d.patch_file = "rr.bps"; d.format = "bps"; d.prep = "nohdr";
    d.patch_crc = 0xdeadbeefu; d.out_crc = 0x9f8e7d6cu; d.out_size = 16u << 20; d.added = 1700000000;
    HackDef b2;
    CHECK(decode(encode(d), &b2) && b2.key == d.key && b2.title == d.title && b2.kind == "translation" && b2.lang == "pt-br" && b2.prep == "nohdr" &&
          b2.patch_crc == 0xdeadbeefu && b2.out_size == (16u << 20) && b2.added == 1700000000);
    { HackDef x = d; x.patch_file = "..\\evil.bps"; CHECK(!decode(encode(x), nullptr)); }
    { HackDef x = d; x.patch_file = "a/b.bps"; CHECK(!decode(encode(x), nullptr)); }
    { HackDef x = d; x.out_crc = 1; CHECK(!decode(encode(x), nullptr)); }   // the key suffix must match
    { HackDef x = d; x.format = "exe"; CHECK(!decode(encode(x), nullptr)); }
    { HackDef x = d; x.prep = "../"; CHECK(!decode(encode(x), nullptr)); }
    CHECK(!decode("", nullptr) && !decode("[]", nullptr) && !decode("{\"version\":2}", nullptr));

    fs::path root = fs::temp_directory_path() / "retrohub_hack_test";
    std::error_code ec;
    fs::remove_all(root, ec);

    /* base finder: BPS by source CRC */
    Bytes gba = noise(64 * 1024, 1), gba_hack = gba;
    gba_hack[100] ^= 0xff; gba_hack[40000] ^= 0x55;
    Bytes other = noise(64 * 1024, 2);
    wfile(root / "g" / "a.gba", gba); wfile(root / "g" / "b.gba", other);
    std::vector<Cand> lib = {cand("gba:aaaa-1", "Alpha Quest", root / "g" / "a.gba", hub::System::GBA, gba.size()),
                             cand("gba:bbbb-2", "Beta Run", root / "g" / "b.gba", hub::System::GBA, other.size())};
    Bytes pbps = bps(gba, gba_hack);
    hub::patch::Info info;
    std::string err;
    CHECK(hub::patch::inspect(pbps.data(), pbps.size(), &info, &err));
    FindResult fr = find_base(lib, info, "alpha hack.bps", 0);
    CHECK(fr.status == FindStatus::Found && fr.base.key == "gba:aaaa-1" && fr.prep.empty());
    /* wrong version: right size, different CRC */
    Bytes wrong = bps(noise(64 * 1024, 9), gba_hack);
    hub::patch::inspect(wrong.data(), wrong.size(), &info, &err);
    fr = find_base(lib, info, "alpha hack.bps", 0);
    CHECK(fr.status == FindStatus::WrongVersion && fr.message == "This patch is for a different version of Alpha Quest.");
    /* no game of that size */
    Bytes big = bps(noise(200 * 1024, 3), noise(200 * 1024, 4));
    hub::patch::inspect(big.data(), big.size(), &info, &err);
    fr = find_base(lib, info, "x.bps", 0);
    CHECK(fr.status == FindStatus::NotFound && fr.message.find("original game") != std::string::npos);

    /* SNES copier header: patch made for the headerless image, the library file has the 512-byte header */
    Bytes snes = noise(256 * 1024, 5), snes_hack = snes;
    snes_hack[1000] ^= 0x11;
    Bytes headered(512, 0);
    headered.insert(headered.end(), snes.begin(), snes.end());
    wfile(root / "s" / "game.smc", headered);
    std::vector<Cand> slib = {cand("snes:1", "Super Test", root / "s" / "game.smc", hub::System::SNES, headered.size())};
    Bytes psn = bps(snes, snes_hack);
    hub::patch::inspect(psn.data(), psn.size(), &info, &err);
    fr = find_base(slib, info, "super test.bps", 0);
    CHECK(fr.status == FindStatus::Found && fr.prep == "nohdr");
    wfile(root / "s" / "fix.bps", psn);
    BuildResult br = build_patched(slib[0].path, fr.prep, (root / "s" / "fix.bps").u8string(), (root / "s" / "out" / "hack.smc").u8string(), nullptr);
    CHECK(br.ok && br.out_crc == hub::patch::crc32(snes_hack.data(), snes_hack.size()) && br.out_size == snes_hack.size());
    CHECK(!fs::exists(root / "s" / "out" / "hack.smc.src"));   // the temp image is removed
    /* a patch made for the headered dump matches the file as it is */
    Bytes hh = headered;
    hh[1500] ^= 1;
    Bytes pwh = bps(headered, hh);
    hub::patch::inspect(pwh.data(), pwh.size(), &info, &err);
    fr = find_base(slib, info, "super test.bps", 0);
    CHECK(fr.status == FindStatus::Found && fr.prep.empty());

    /* N64: a v64 file (byte-swapped pairs); the patch assumes z64 */
    Bytes z64 = noise(128 * 1024, 6);
    z64[0] = 0x80; z64[1] = 0x37; z64[2] = 0x12; z64[3] = 0x40;
    Bytes v64 = z64;
    for (size_t i = 0; i + 1 < v64.size(); i += 2) std::swap(v64[i], v64[i + 1]);
    Bytes z64_hack = z64;
    z64_hack[5000] ^= 0x77;
    wfile(root / "n" / "game.v64", v64);
    std::vector<Cand> nlib = {cand("n64:abc", "Test 64", root / "n" / "game.v64", hub::System::N64, v64.size())};
    Bytes pn = bps(z64, z64_hack);
    hub::patch::inspect(pn.data(), pn.size(), &info, &err);
    fr = find_base(nlib, info, "t.bps", 0);
    CHECK(fr.status == FindStatus::Found && fr.prep == "z64");
    wfile(root / "n" / "p.bps", pn);
    br = build_patched(nlib[0].path, "z64", (root / "n" / "p.bps").u8string(), (root / "n" / "hack.z64").u8string(), nullptr);
    CHECK(br.ok && br.out_crc == hub::patch::crc32(z64_hack.data(), z64_hack.size()));   // the key's suffix is the CRC of the output
    CHECK(default_prep(nlib[0]) == "z64");
    CHECK(make_key("n64:abc", br.out_crc) == "n64:abc+h" + hex8(br.out_crc));
    /* a patch for another file fails in plain words and leaves no output */
    br = build_patched(nlib[0].path, "", (root / "n" / "p.bps").u8string(), (root / "n" / "bad.z64").u8string(), nullptr);
    CHECK(!br.ok && !br.message.empty() && !fs::exists(root / "n" / "bad.z64"));

    /* IPS: the player chooses; size filter and name ranking */
    Bytes pips = ips({{0xF000, Bytes(8, 0xaa)}});
    CHECK(ips_extent(pips.data(), pips.size()) == 0xF008);
    CHECK(ips_extent(pips.data(), 4) == 0);
    hub::patch::inspect(pips.data(), pips.size(), &info, &err);
    lib.push_back(cand("gba:tiny-3", "Tiny Alpha", root / "g" / "a.gba", hub::System::GBA, 0x8000));
    fr = find_base(lib, info, "Beta Run (hack).ips", 0xF008);
    CHECK(fr.status == FindStatus::Choose && fr.choices.size() == 2 && fr.choices[0].key == "gba:bbbb-2");   // Tiny is too small; Beta is ranked first
    CHECK(find_base(lib, info, "x.ips", 0x7fffffff).status == FindStatus::NotFound);

    /* vault: install, scan with and without the base, remove */
    fs::path vault = root / "vault";
    HackDef hd;
    hd.base_key = "gba:aaaa-1"; hd.system = "gba"; hd.title = "Alpha Plus"; hd.patch_file = "alpha.bps"; hd.format = "bps";
    hd.out_crc = 0x11223344u; hd.key = make_key(hd.base_key, hd.out_crc); hd.out_size = 123; hd.patch_crc = 7;
    fs::path gdir = vault / "gba" / "gba-aaaa-1-h";
    CHECK(install_hack(gdir.u8string(), hd, (root / "s" / "fix.bps").u8string(), &err));
    wfile(vault / "gba" / "junk" / "patch" / "hack.json", Bytes{'{', '}'});   // a broken file is ignored
    std::vector<hub::GameEntry> scan(1);
    scan[0].key = "gba:aaaa-1"; scan[0].system = hub::System::GBA; scan[0].title = "Alpha Quest"; scan[0].path = lib[0].path; scan[0].serial = "AAAA"; scan[0].size = gba.size();
    scan.emplace_back();
    scan[1].key = "gba:zzzz-9"; scan[1].system = hub::System::GBA; scan[1].title = "Zed"; scan[1].path = lib[1].path; scan[1].size = other.size();
    CHECK(append_hacks(vault.u8string(), &scan) == 1 && scan.size() == 3);
    CHECK(scan[1].key == hd.key && scan[1].hack_patch.find("alpha.bps") != std::string::npos && scan[1].hack_base == "gba:aaaa-1" && scan[1].catalog_id.empty() &&
          scan[1].path == scan[0].path && scan[1].title == "Alpha Plus" && scan[1].serial == "AAAA");
    CHECK(append_hacks(vault.u8string(), &scan) == 0 && scan.size() == 3);   // idempotent
    std::vector<hub::GameEntry> without(1);
    without[0] = scan[2];   // the base game is missing from the scan
    CHECK(append_hacks(vault.u8string(), &without) == 0 && without.size() == 1);
    HackDef ld;
    CHECK(load_def_next_to(scan[1].hack_patch, &ld).empty() && ld.key == hd.key);
    wfile(gdir / "battery" / "save.sav", Bytes{1, 2, 3});
    CHECK(remove_hack_files(gdir.u8string()) && !fs::exists(gdir / "patch") && fs::exists(gdir / "battery" / "save.sav"));   // saves are kept
    CHECK(!remove_hack_files((vault / "nothing").u8string()));
    CHECK(candidates_from(scan).size() == 2);   // a hack is never a base

    fs::remove_all(root, ec);
    if (!fails) std::printf("hub_hack_test: all passed\n");
    return fails ? 1 : 0;
}
