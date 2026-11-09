/* nuprepair_test: damaged-member repair for Lichii NUPs (nup_repair.h). Synthetic NUPs built in-test (superblock, TOC with a real SHA-256,
 * packages, dirs, files, children, pool, blobs; zstd members are hand-built RAW-block frames, no compressor), all under a scratch folder in %TEMP%.
 * Real-file QA, never run by ctest:  nuprepair_test --scan <file.nup>  |  --apply <file.nup> <user_dir>  |  --ensure <file.nup> <user_dir> */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "nup_repair.h"
#include "winutil.h"

namespace np = hub::nuprepair;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; if (!((a) == (b))) { ++g_fail; std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

// ---------------------------------------------------------------- helpers
static std::string sha_hex(const void* d, size_t n) {
    uint8_t h[32] = {};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)d, (ULONG)n, h, 32);
    static const char* x = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 32; ++i) { s += x[h[i] >> 4]; s += x[h[i] & 15]; }
    return s;
}
static std::string sha_hex(const Bytes& b) { return sha_hex(b.data(), b.size()); }

static fs::path FP(const std::string& utf8) { return fs::path(winutil::to_wide(utf8)); }
static bool exists(const std::string& p) { std::error_code ec; return fs::exists(FP(p), ec); }
static bool read_all(const std::string& p, Bytes* out) { return winutil::read_file(p, out); }
static bool write_all(const std::string& p, const Bytes& b) {
    std::error_code ec;
    fs::create_directories(FP(p).parent_path(), ec);
    return winutil::write_file(p, b.data(), b.size());
}
static bool write_text(const std::string& p, const std::string& t) { return write_all(p, Bytes(t.begin(), t.end())); }

static std::vector<std::string> list_files(const std::string& dir) {   // relative, '/' separated, sorted
    std::vector<std::string> v;
    std::error_code ec;
    const fs::path root = FP(dir);
    if (!fs::exists(root, ec)) return v;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) v.push_back(winutil::to_utf8(it->path().lexically_relative(root).generic_wstring()));
    std::sort(v.begin(), v.end());
    return v;
}

static bool set_mtime(const std::string& p, uint64_t ticks) {
    HANDLE h = CreateFileW(winutil::to_wide(p).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFFu);
    ft.dwHighDateTime = (DWORD)(ticks >> 32);
    const BOOL ok = SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    return ok != 0;
}

// ---------------------------------------------------------------- synthetic NUP writer
static void put32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = (uint8_t)(v >> (8 * i)); }
static void put64(Bytes& b, size_t at, uint64_t v) { for (int i = 0; i < 8; ++i) b[at + i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const Bytes& b, size_t at) { return (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8) | ((uint32_t)b[at + 2] << 16) | ((uint32_t)b[at + 3] << 24); }
static uint64_t get64(const Bytes& b, size_t at) { return (uint64_t)get32(b, at) | ((uint64_t)get32(b, at + 4) << 32); }

/* One zstd frame holding `c` as a single RAW block (single segment, 4-byte content size). */
static Bytes zstd_raw(const Bytes& c) {
    Bytes f = {0x28, 0xB5, 0x2F, 0xFD, 0xA0};
    for (int i = 0; i < 4; ++i) f.push_back((uint8_t)(c.size() >> (8 * i)));
    const uint32_t bh = 1u | ((uint32_t)c.size() << 3);   // last block, type raw
    f.push_back((uint8_t)bh); f.push_back((uint8_t)(bh >> 8)); f.push_back((uint8_t)(bh >> 16));
    f.insert(f.end(), c.begin(), c.end());
    return f;
}

static const uint8_t kMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};
static Bytes good_body(size_t n, uint8_t fill) { Bytes b(n, fill); std::memcpy(b.data(), kMagic, 4); return b; }   // a .zs file: starts with the zstd magic
static Bytes zero_body(size_t n) { return Bytes(n, 0); }                                                            // a damaged one

struct TFile {
    std::string name;
    Bytes content;                 // uncompressed bytes (what the TOC hash covers)
    int codec = 1;
    bool bad_sha = false;          // the TOC hash is wrong
    bool use_stored = false;       // stored bytes given directly (garbage frames)
    Bytes stored;
};
struct TDir { std::string name; std::vector<TDir> dirs; std::vector<TFile> files; };
struct TPkg { uint64_t tid = 0; uint8_t kind = 0; uint8_t flags = 0; uint32_t version = 0; bool has_romfs = true; TDir romfs; };
struct Layout { uint64_t toc_off = 0, toc_size = 0, dir_rec = 0, file_rec = 0, children = 0, pool = 0, dir_count = 0, file_count = 0; uint32_t pkgs = 0; };

static TFile F(const std::string& n, const Bytes& c, int codec = 1) { TFile f; f.name = n; f.content = c; f.codec = codec; return f; }
static TDir D(const std::string& n, std::vector<TFile> files, std::vector<TDir> dirs = {}) { TDir d; d.name = n; d.files = std::move(files); d.dirs = std::move(dirs); return d; }

static Bytes build_nup(const std::vector<TPkg>& pkgs, Layout* lay = nullptr) {
    struct DR { uint32_t no, nl, first, count; };
    struct FR { uint32_t no, nl; uint64_t size; uint64_t eoff, esize; uint8_t sha[32]; uint8_t codec; Bytes blob; };
    std::vector<DR> dirs;
    std::vector<FR> files;
    std::vector<std::pair<bool, uint32_t>> kids_all;
    std::string pool;
    auto add_name = [&](const std::string& s) { const uint32_t o = (uint32_t)pool.size(); pool += s; return o; };
    std::function<uint32_t(const TDir&)> emit = [&](const TDir& d) {
        const uint32_t idx = (uint32_t)dirs.size();
        dirs.push_back({add_name(d.name), (uint32_t)d.name.size(), 0, 0});
        std::vector<std::pair<bool, uint32_t>> kids;
        for (const TDir& s : d.dirs) kids.push_back({true, emit(s)});
        for (const TFile& f : d.files) {
            FR r{};
            r.no = add_name(f.name);
            r.nl = (uint32_t)f.name.size();
            r.size = f.content.size();
            const std::string h = sha_hex(f.content);
            for (int i = 0; i < 32; ++i) r.sha[i] = (uint8_t)std::stoi(h.substr((size_t)i * 2, 2), nullptr, 16);
            if (f.bad_sha) r.sha[0] ^= 0xFF;
            r.codec = (uint8_t)f.codec;
            if (!f.content.empty()) r.blob = f.use_stored ? f.stored : (f.codec == 1 ? zstd_raw(f.content) : f.content);
            r.esize = r.blob.size();
            kids.push_back({false, (uint32_t)files.size()});
            files.push_back(std::move(r));
        }
        dirs[idx].first = (uint32_t)kids_all.size();
        dirs[idx].count = (uint32_t)kids.size();
        kids_all.insert(kids_all.end(), kids.begin(), kids.end());
        return idx;
    };
    struct PR { uint64_t tid; uint32_t root, romfs, version; uint8_t kind, flags; };
    std::vector<PR> prs;
    for (const TPkg& p : pkgs) {
        PR r{};
        r.tid = p.tid; r.kind = p.kind; r.flags = p.flags; r.version = p.version;
        r.root = (uint32_t)dirs.size();
        dirs.push_back({add_name(""), 0, 0, 0});
        std::vector<std::pair<bool, uint32_t>> kids;
        r.romfs = 0xFFFFFFFFu;
        if (p.has_romfs) { r.romfs = emit(p.romfs); kids.push_back({true, r.romfs}); }
        dirs[r.root].first = (uint32_t)kids_all.size();
        dirs[r.root].count = (uint32_t)kids.size();
        kids_all.insert(kids_all.end(), kids.begin(), kids.end());
        prs.push_back(r);
    }
    const uint64_t nd = dirs.size(), nf = files.size();
    const uint64_t toc_size = 0x30 + pkgs.size() * 0x40 + nd * 0x18 + nf * 0x50 + kids_all.size() * 4 + pool.size();
    const uint64_t blob = (0x80 + toc_size + 15) & ~15ull;
    uint64_t at = blob;
    for (FR& f : files) if (!f.blob.empty()) { f.eoff = at; at += (f.blob.size() + 15) & ~15ull; }
    uint64_t end = blob;
    for (const FR& f : files) if (!f.blob.empty()) end = std::max<uint64_t>(end, f.eoff + f.blob.size());
    Bytes nup(end, 0);
    for (const FR& f : files) if (!f.blob.empty()) std::memcpy(nup.data() + f.eoff, f.blob.data(), f.blob.size());
    Bytes toc(toc_size, 0);
    put32(toc, 0, 0x01434F54u);
    put32(toc, 4, (uint32_t)pkgs.size());
    put64(toc, 8, nd);
    put64(toc, 0x10, nf);
    const uint64_t pool_off = toc_size - pool.size();
    put64(toc, 0x18, pool_off);
    put64(toc, 0x20, pool.size());
    size_t pos = 0x30;
    for (const PR& r : prs) {
        put64(toc, pos, r.tid); put32(toc, pos + 8, r.root); put32(toc, pos + 0xC, 0xFFFFFFFFu); put32(toc, pos + 0x10, r.romfs); put32(toc, pos + 0x14, r.version);
        toc[pos + 0x18] = r.kind; toc[pos + 0x19] = r.flags;
        pos += 0x40;
    }
    const uint64_t dir_rec = pos;
    for (const DR& d : dirs) { put32(toc, pos, d.no); put32(toc, pos + 4, d.nl); put32(toc, pos + 8, d.first); put32(toc, pos + 12, d.count); pos += 0x18; }
    const uint64_t file_rec = pos;
    for (const FR& f : files) {
        put32(toc, pos, f.no); put32(toc, pos + 4, f.nl); put64(toc, pos + 8, f.size); put64(toc, pos + 0x10, f.eoff); put64(toc, pos + 0x18, f.esize);
        std::memcpy(toc.data() + pos + 0x20, f.sha, 32);
        toc[pos + 0x40] = f.codec;
        pos += 0x50;
    }
    const uint64_t children = pos;
    for (const auto& k : kids_all) { put32(toc, pos, k.first ? k.second : (uint32_t)(nd + k.second)); pos += 4; }
    std::memcpy(toc.data() + pos, pool.data(), pool.size());
    std::memcpy(nup.data() + 0x80, toc.data(), toc.size());
    std::memcpy(nup.data(), "NUP\x01", 4);
    nup[4] = 0x80; nup[6] = 1;
    put64(nup, 0x08, 0x80);
    put64(nup, 0x10, toc_size);
    const std::string th = sha_hex(toc);
    for (int i = 0; i < 32; ++i) nup[0x18 + i] = (uint8_t)std::stoi(th.substr((size_t)i * 2, 2), nullptr, 16);
    put64(nup, 0x38, blob);
    put64(nup, 0x40, nup.size());
    put32(nup, 0x48, (uint32_t)pkgs.size());
    put32(nup, 0x4C, 0);
    if (lay) { lay->toc_off = 0x80; lay->toc_size = toc_size; lay->dir_rec = dir_rec; lay->file_rec = file_rec; lay->children = children; lay->pool = pool_off; lay->dir_count = nd; lay->file_count = nf; lay->pkgs = (uint32_t)pkgs.size(); }
    return nup;
}

static void reseal(Bytes& nup) {   // recompute the superblock's TOC hash after a TOC patch
    const uint64_t off = get64(nup, 8), size = get64(nup, 0x10);
    const std::string h = sha_hex(nup.data() + off, (size_t)size);
    for (int i = 0; i < 32; ++i) nup[0x18 + i] = (uint8_t)std::stoi(h.substr((size_t)i * 2, 2), nullptr, 16);
}

static TPkg base_pkg(uint64_t tid, TDir romfs) { TPkg p; p.tid = tid; p.kind = 0; p.romfs = std::move(romfs); return p; }
static TPkg update_pkg(uint64_t tid, uint32_t ver, TDir romfs, uint8_t flags = 0) { TPkg p; p.tid = tid; p.kind = 1; p.flags = flags; p.version = ver; p.romfs = std::move(romfs); return p; }

// ---------------------------------------------------------------- fixtures
static const uint64_t kTid = 0x0400486024DEC000ull;
static const char* kWater = "Tex/Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs";
static const char* kWaterSI = "Tex/Rock_WaterTemple_Gate_P_01_Nrm.bntx.zs";
static const char* kSoil = "Tex/CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm.bntx.zs";
static const char* kSoilSI = "Tex/CmnTex_Map_Sand_FairyFountain_Sand_A_Nrm.bntx.zs";

static Bytes water_body() { return good_body(710, 'p'); }
static Bytes soil_body() { return good_body(420, 's'); }

/* An OoT-like copy: Water + Soil damaged with stand-ins, Lone_Alb damaged without one; 6 codec-1 .zs members checked. */
static Bytes make_oot(uint64_t tid, bool soil_damaged = true, bool water_damaged = true, Layout* lay = nullptr) {
    TDir tex = D("Tex", {
        F("Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs", water_damaged ? zero_body(700) : good_body(700, 'w')),
        F("Rock_WaterTemple_Gate_P_01_Nrm.bntx.zs", water_body()),
        F("CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm.bntx.zs", soil_damaged ? zero_body(400) : good_body(400, 'o')),
        F("CmnTex_Map_Sand_FairyFountain_Sand_A_Nrm.bntx.zs", soil_body()),
        F("Lone_Alb.bntx.zs", zero_body(300)),
        F("Fine_Nrm.bntx.zs", good_body(64, 'f')),
    });
    TDir model = D("Model", {F("Other.bin", zero_body(64)), F("Raw.zs", zero_body(64), 0)});   // not codec-1 .zs: never checked
    return build_nup({base_pkg(tid, D("romfs", {F("root.bin", Bytes{1, 2, 3}, 0)}, {tex, model}))}, lay);
}

// ---------------------------------------------------------------- scratch
static std::string g_root;
static int g_n = 0;
static std::string fresh(const std::string& stem) { return g_root + "\\" + stem + std::to_string(++g_n); }
static std::string put_nup(const std::string& name, const Bytes& b) { const std::string p = g_root + "\\nups\\" + name + ".nup"; CHECK(write_all(p, b)); return p; }
static std::string mod_of(const std::string& user, uint64_t tid) { char t[24]; std::snprintf(t, sizeof t, "%016llX", (unsigned long long)tid); return user + "\\load\\" + t + "\\" + np::kModFolder; }

static const np::Repair* find_repair(const np::Scan& s, const std::string& dmg) { for (const np::Repair& r : s.repairs) if (r.damaged.path == dmg) return &r; return nullptr; }

static std::string expected_manifest(const np::Scan& s, const std::vector<std::vector<std::string>>& e /* sha, size, dmg, stand */) {
    std::string t = "RetroHub damaged-file repair 1\nnup " + std::to_string(s.file_size) + " " + std::to_string(s.mtime) + " " + s.toc_sha256 + " rule " + std::to_string(np::kRuleVersion) + "\n";
    for (const auto& x : e) t += "repair " + x[0] + " " + x[1] + "\t" + x[2] + "\t" + x[3] + "\n";
    return t;
}

// ---------------------------------------------------------------- tests
static void test_healthy() {
    TDir tex = D("Tex", {F("A_Nrm.bntx.zs", good_body(100, 'a')), F("B_Nrm.bntx.zs", good_body(50, 'b')), F("C.zs", zero_body(20), 0), F("D.bin", zero_body(20))});
    const std::string p = put_nup("healthy", build_nup({base_pkg(kTid, D("romfs", {}, {tex}))}));
    np::Scan s = np::scan(p);
    CHECK(s.ok);
    CHECK_EQ(s.title_id, kTid);
    CHECK_EQ(s.checked, 2u);
    CHECK(s.repairs.empty());
    CHECK_EQ(s.toc_sha256.size(), 64u);
    const std::string user = fresh("user");
    np::Applied a = np::apply(p, s, user);
    CHECK(a.ok && a.written == 0 && a.kept == 0 && a.removed == 0 && a.unrepaired == 0);
    CHECK(!exists(user + "\\load"));
    np::Ensure e = np::ensure(p, user, true);
    CHECK(e.scanned && e.scan.ok && e.applied.ok);
    CHECK(e.summary.find("nothing to repair") != std::string::npos);
    CHECK(!exists(user + "\\load"));
    // member contents: raw and zstd extents come back byte-exact
    Bytes b;
    std::string err;
    CHECK(np::extract(p, "Tex/A_Nrm.bntx.zs", &b, &err) && b == good_body(100, 'a'));
    CHECK(np::extract(p, "Tex/C.zs", &b, &err) && b == zero_body(20));
    CHECK(!np::extract(p, "Tex/Nope.zs", &b, &err) && !err.empty());
}

static void test_standin_choice() {
    TDir tex = D("Tex", {
        F("Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs", zero_body(500)),                       // damaged
        F("Rock_WaterTemple_Gate_M_02_Nrm.bntx.zs", good_body(900, 'A'), 0),               // prefix 25, codec 0: the winner
        F("Rock_WaterTemple_Gate_P_01_Nrm.bntx.zs", good_body(505, 'B')),                  // prefix 22, closer size: loses to the longer prefix
        F("Rock_Water_Other_Nrm.bntx.zs", good_body(500, 'C')),                            // shorter prefix, exact size
        F("Rock_WaterTemple_Gate_M_01_X_Nrm.bntx.zs", zero_body(500)),                     // longer prefix but no magic: skipped
        F("Rock_WaterTemple_Gate_M_01_Alb.bntx.zs", good_body(500, 'D')),                  // other last token: skipped
        F("Rock_WaterTemple_Gate_M_01xNrm.bntx.zs", good_body(500, 'E')),                  // no '_' before the token: skipped
    });
    TDir other = D("Other", {F("Rock_WaterTemple_Gate_M_01_Q_Nrm.bntx.zs", good_body(500, 'F'))});   // another directory: ignored
    const std::string p = put_nup("choice", build_nup({base_pkg(kTid, D("romfs", {}, {tex, other}))}));
    np::Scan s = np::scan(p);
    CHECK(s.ok);
    const np::Repair* r = find_repair(s, "Tex/Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs");
    CHECK(r && r->stand_in.path == "Tex/Rock_WaterTemple_Gate_M_02_Nrm.bntx.zs" && r->stand_in.size == 900 && r->damaged.size == 500);
    const np::Repair* x = find_repair(s, "Tex/Rock_WaterTemple_Gate_M_01_X_Nrm.bntx.zs");   // damaged too; its own stand-in: M_01_Nrm is damaged, so the next best
    CHECK(x && x->stand_in.path == "Tex/Rock_WaterTemple_Gate_M_02_Nrm.bntx.zs");
    CHECK_EQ(s.repairs.size(), 2u);
    const std::string user = fresh("user");
    np::Applied a = np::apply(p, s, user);
    CHECK(a.ok && a.written == 2 && a.unrepaired == 0);
    Bytes got;
    CHECK(read_all(mod_of(user, kTid) + "\\romfs\\Tex\\Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs", &got) && got == good_body(900, 'A'));

    // equal prefix -> closest size; equal gap -> first by name; a codec-1 stand-in; a garbage codec-1 frame counts as damaged
    TFile garbage = F("Aa_Zz_Nrm.bntx.zs", good_body(1000, 'z'));
    garbage.use_stored = true;
    garbage.stored = Bytes(40, 0x55);
    TDir t2 = D("Tex", {
        F("Aa_Bb_Nrm.bntx.zs", zero_body(1000)),
        F("Aa_Cc_Nrm.bntx.zs", good_body(3000, 'c')),
        F("Aa_Dd_Nrm.bntx.zs", good_body(1100, 'd')),
        F("Aa_Ee_Nrm.bntx.zs", good_body(900, 'e')),
        garbage,
    });
    const std::string p2 = put_nup("choice2", build_nup({base_pkg(kTid, D("romfs", {}, {t2}))}));
    np::Scan s2 = np::scan(p2);
    CHECK(s2.ok);
    CHECK_EQ(s2.repairs.size(), 2u);   // Bb and the garbage one
    const np::Repair* r2 = find_repair(s2, "Tex/Aa_Bb_Nrm.bntx.zs");
    CHECK(r2 && r2->stand_in.path == "Tex/Aa_Dd_Nrm.bntx.zs");
    const np::Repair* r3 = find_repair(s2, "Tex/Aa_Zz_Nrm.bntx.zs");
    CHECK(r3 && r3->stand_in.path == "Tex/Aa_Dd_Nrm.bntx.zs");   // Zz size 1000: Dd and Ee... Bb is damaged; Dd gap 100, Ee gap 100 -> Dd by name
}

static void test_no_standin() {
    TDir tex = D("Tex", {F("Lone_Alb.bntx.zs", zero_body(300)), F("Fine_Nrm.bntx.zs", good_body(64, 'f'))});
    const std::string p = put_nup("nostand", build_nup({base_pkg(kTid, D("romfs", {}, {tex}))}));
    np::Scan s = np::scan(p);
    CHECK(s.ok && s.repairs.size() == 1 && s.repairs[0].stand_in.path.empty() && s.repairs[0].damaged.path == "Tex/Lone_Alb.bntx.zs");
    const std::string user = fresh("user");
    np::Applied a = np::apply(p, s, user);
    CHECK(a.ok && a.written == 0 && a.unrepaired == 1);
    CHECK(!exists(user + "\\load"));
    np::Ensure e = np::ensure(p, user, true);
    CHECK(e.scanned && e.applied.ok && e.applied.unrepaired == 1 && e.summary.find("without a stand-in") != std::string::npos);
    CHECK(!exists(user + "\\load"));
}

static void test_merge() {
    // base: D damaged, H healthy, V healthy, S1 stand-in only in the base; update v2: D healthy, H damaged, U damaged (stand-in in the base's dir);
    // a lower update v1 would damage V, but the best update (highest version) wins and has no V.
    TDir bt = D("Tex", {
        F("D_Nrm.bntx.zs", zero_body(300)),
        F("H_Nrm.bntx.zs", good_body(310, 'h')),
        F("V_Nrm.bntx.zs", good_body(320, 'v')),
        F("U_Y_Nrm.bntx.zs", good_body(330, 'y')),
    });
    TDir ut = D("Tex", {
        F("D_Nrm.bntx.zs", good_body(305, 'd')),
        F("H_Nrm.bntx.zs", zero_body(310)),
        F("U_X_Nrm.bntx.zs", zero_body(330)),
    });
    TDir lt = D("Tex", {F("V_Nrm.bntx.zs", zero_body(320))});
    const uint64_t upd = kTid + 0x800;
    const std::string p = put_nup("merge", build_nup({update_pkg(upd, 65536, D("romfs", {}, {lt})), base_pkg(kTid, D("romfs", {}, {bt})), update_pkg(upd, 131072, D("romfs", {}, {ut}))}));
    np::Scan s = np::scan(p);
    CHECK(s.ok);
    CHECK_EQ(s.title_id, kTid);   // the base supplies the program, whatever the order
    CHECK(!find_repair(s, "Tex/D_Nrm.bntx.zs"));   // damaged only in the base, replaced by the update
    CHECK(!find_repair(s, "Tex/V_Nrm.bntx.zs"));   // the lower update is not the best one
    const np::Repair* h = find_repair(s, "Tex/H_Nrm.bntx.zs");   // the update's damaged copy wins over the base's healthy one; all candidates share prefix 0, so the closest size: the update's D (305)
    CHECK(h && h->stand_in.path == "Tex/D_Nrm.bntx.zs");
    const np::Repair* u = find_repair(s, "Tex/U_X_Nrm.bntx.zs");
    CHECK(u && u->stand_in.path == "Tex/U_Y_Nrm.bntx.zs");   // candidates come from the merged directory (base file)
    CHECK_EQ(s.repairs.size(), 2u);
    // the extracted stand-in for H is the update's D (merged) and the update path wins for D itself
    Bytes b;
    std::string err;
    CHECK(np::extract(p, "Tex/D_Nrm.bntx.zs", &b, &err) && b == good_body(305, 'd'));
    CHECK(np::extract(p, "Tex/V_Nrm.bntx.zs", &b, &err) && b == good_body(320, 'v'));

    // a standalone update alone
    TDir st = D("Tex", {F("K_Nrm.bntx.zs", zero_body(100)), F("L_Nrm.bntx.zs", good_body(100, 'l'))});
    const std::string p2 = put_nup("standalone", build_nup({update_pkg(upd, 65536, D("romfs", {}, {st}), 1)}));
    np::Scan s2 = np::scan(p2);
    CHECK(s2.ok);
    CHECK_EQ(s2.title_id, upd);
    CHECK(s2.repairs.size() == 1 && s2.repairs[0].stand_in.path == "Tex/L_Nrm.bntx.zs");
    const std::string user = fresh("user");
    CHECK(np::apply(p2, s2, user).ok);
    CHECK(exists(mod_of(user, upd) + "\\romfs\\Tex\\K_Nrm.bntx.zs"));
    CHECK(!exists(mod_of(user, kTid)));

    // only add-on packages: no program
    TPkg addon = base_pkg(kTid, D("romfs", {}, {}));
    addon.kind = 2;
    np::Scan s3 = np::scan(put_nup("addon", build_nup({addon})));
    CHECK(!s3.ok && !s3.error.empty());
}

static void test_malformed() {
    const Bytes good = make_oot(kTid);
    std::vector<std::pair<std::string, Bytes>> bad;
    bad.push_back({"empty", Bytes()});
    bad.push_back({"tiny", Bytes(10, 7)});
    bad.push_back({"garbage", Bytes(1 << 20, 0xAB)});
    { Bytes g(0x200, 0); std::memcpy(g.data(), "NUP\x01", 4); bad.push_back({"magic only", g}); }
    { Bytes t(good.begin(), good.begin() + good.size() / 2); bad.push_back({"truncated", t}); }
    { Bytes t(good); t.push_back(0); bad.push_back({"extra byte", t}); }
    { Bytes t(good); t[0x90] ^= 1; bad.push_back({"toc hash mismatch", t}); }
    { Bytes t(good); put32(t, 0x4C, 1); bad.push_back({"flags", t}); }
    { Bytes t(good); put32(t, 0x48, 2); bad.push_back({"package count", t}); }
    { Bytes t(good); put64(t, 0x08, 0x40); bad.push_back({"toc offset", t}); }
    { Bytes t(good); put64(t, 0x10, 0xFFFFFFFFFFFFFFF0ull); bad.push_back({"absurd toc size", t}); }
    { Bytes t(good); put64(t, 0x38, 0x7FFFFFFFFFFFFFF0ull); bad.push_back({"blob offset", t}); }
    Layout L;
    make_oot(kTid, true, true, &L);
    auto mut = [&](const std::string& name, const std::function<void(Bytes&, const Layout&)>& f) { Bytes t(good); f(t, L); reseal(t); bad.push_back({name, t}); };
    mut("absurd dir count", [](Bytes& t, const Layout& l) { put64(t, l.toc_off + 8, 0xFFFFFFFFFFFFull); });
    mut("absurd file count", [](Bytes& t, const Layout& l) { put64(t, l.toc_off + 0x10, 0x7FFFFFFFFFFFFFFFull); });
    mut("counts past toc", [](Bytes& t, const Layout& l) { put64(t, l.toc_off + 0x10, l.file_count + 1); });
    mut("pool offset huge", [](Bytes& t, const Layout& l) { put64(t, l.toc_off + 0x18, l.toc_size + 100); });
    mut("pool size huge", [](Bytes& t, const Layout& l) { put64(t, l.toc_off + 0x20, 0xFFFFFFFFFFFFFFFFull); });
    mut("pool offset misaligned", [](Bytes& t, const Layout& l) { put64(t, l.toc_off + 0x18, l.pool + 1); });
    mut("bad toc magic", [](Bytes& t, const Layout& l) { put32(t, l.toc_off, 0); });
    mut("package kind", [](Bytes& t, const Layout& l) { t[l.toc_off + 0x30 + 0x18] = 9; });
    mut("package flags", [](Bytes& t, const Layout& l) { t[l.toc_off + 0x30 + 0x19] = 2; });
    mut("package romfs dir", [](Bytes& t, const Layout& l) { put32(t, l.toc_off + 0x30 + 0x10, 0x7FFFFFFF); });
    mut("dir children range", [](Bytes& t, const Layout& l) { put32(t, l.toc_off + l.dir_rec + 8, 0x7FFFFFFF); });
    mut("dir name range", [](Bytes& t, const Layout& l) { put32(t, l.toc_off + l.dir_rec + 4, 0x7FFFFFFF); });
    mut("file name range", [](Bytes& t, const Layout& l) { put32(t, l.toc_off + l.file_rec + 0, 0x7FFFFFFF); });
    mut("extent before blob", [](Bytes& t, const Layout& l) { put64(t, l.toc_off + l.file_rec + 0x10, 0x80); });
    mut("extent unaligned", [](Bytes& t, const Layout& l) { put64(t, l.toc_off + l.file_rec + 0x10, get64(t, l.toc_off + l.file_rec + 0x10) + 1); });
    mut("extent past end", [](Bytes& t, const Layout& l) { put64(t, l.toc_off + l.file_rec + 0x18, 0x7FFFFFFFFFFFull); });
    mut("extent huge offset", [](Bytes& t, const Layout& l) { put64(t, l.toc_off + l.file_rec + 0x10, 0xFFFFFFFFFFFFFFF0ull); });
    mut("codec 2", [](Bytes& t, const Layout& l) { t[l.toc_off + l.file_rec + 0x40] = 2; });
    mut("codec flags", [](Bytes& t, const Layout& l) { t[l.toc_off + l.file_rec + 0x41] = 1; });
    mut("child index", [](Bytes& t, const Layout& l) { put32(t, l.toc_off + l.children, 0x7FFFFFFF); });
    mut("dir cycle", [](Bytes& t, const Layout& l) { for (uint64_t i = 0; i < 3; ++i) put32(t, l.toc_off + l.children + i * 4, 0); });
    for (const auto& b : bad) {
        const std::string p = put_nup("bad_" + std::to_string(++g_n), b.second);
        np::Scan s = np::scan(p);
        if (s.ok) std::printf("  (accepted: %s)\n", b.first.c_str());
        CHECK(!s.ok);
        CHECK(!s.error.empty());
        Bytes out;
        std::string err;
        CHECK(!np::extract(p, kWater, &out, &err));
        const std::string user = fresh("user");
        np::Ensure e = np::ensure(p, user, true);
        CHECK(!e.applied.ok);
        CHECK(!e.summary.empty());
        CHECK(!exists(user + "\\load"));
        CHECK(!np::apply(p, s, user).ok);
        CHECK(!exists(user + "\\load"));
    }
    CHECK(!np::scan(g_root + "\\nups\\does_not_exist.nup").ok);
    CHECK(!np::scan(g_root + "\\nups").ok);   // a directory
    CHECK(!np::scan("").ok);

    // every truncation length, and random TOC byte flips (hash re-sealed): never a crash; truncations are never ok
    const std::string tp = g_root + "\\nups\\trunc.nup";
    for (size_t n = 0; n < 0x300; n += 7) {
        Bytes t(good.begin(), good.begin() + (std::ptrdiff_t)std::min(n, good.size()));
        write_all(tp, t);
        CHECK(!np::scan(tp).ok);
    }
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    int accepted = 0;
    for (int i = 0; i < 400; ++i) {
        Bytes t(good);
        for (int k = 0; k < 1 + (int)(rnd() % 3); ++k) t[L.toc_off + rnd() % L.toc_size] = (uint8_t)rnd();
        reseal(t);
        write_all(tp, t);
        np::Scan s = np::scan(tp);
        accepted += s.ok ? 1 : 0;
        Bytes out;
        std::string err;
        np::extract(tp, kWaterSI, &out, &err);
    }
    std::printf("  fuzz: %d of 400 mutated TOCs still parse\n", accepted);
}

static void test_extract_sha() {
    TFile bad = F("Bad_Nrm.bntx.zs", good_body(100, 'x'));
    bad.bad_sha = true;
    TFile ok = F("Ok_Nrm.bntx.zs", good_body(100, 'y'), 0);
    TFile trunc = F("Trunc_Nrm.bntx.zs", good_body(100, 'z'));
    trunc.use_stored = true;
    trunc.stored = zstd_raw(good_body(100, 'z'));
    trunc.stored.resize(trunc.stored.size() - 10);
    TFile big = F("Big_Nrm.bntx.zs", good_body(100, 'q'));   // a frame that decompresses to more than the TOC size says
    big.use_stored = true;
    big.stored = zstd_raw(good_body(200, 'q'));
    const std::string p = put_nup("sha", build_nup({base_pkg(kTid, D("romfs", {}, {D("Tex", {bad, ok, trunc, big})}))}));
    Bytes b;
    std::string err;
    CHECK(!np::extract(p, "Tex/Bad_Nrm.bntx.zs", &b, &err) && !err.empty());
    CHECK(np::extract(p, "Tex/Ok_Nrm.bntx.zs", &b, &err) && b == good_body(100, 'y'));
    CHECK(!np::extract(p, "Tex/Trunc_Nrm.bntx.zs", &b, &err));
    CHECK(!np::extract(p, "Tex/Big_Nrm.bntx.zs", &b, &err));

    // a stand-in whose TOC hash is wrong: apply fails and writes nothing
    TDir tex = D("Tex", {F("A_Nrm.bntx.zs", zero_body(100)), bad});
    const std::string p2 = put_nup("sha2", build_nup({base_pkg(kTid, D("romfs", {}, {tex}))}));
    np::Scan s = np::scan(p2);
    CHECK(s.ok && s.repairs.size() == 1 && s.repairs[0].stand_in.path == "Tex/Bad_Nrm.bntx.zs");
    const std::string user = fresh("user");
    np::Applied a = np::apply(p2, s, user);
    CHECK(!a.ok && !a.error.empty() && a.written == 0);
    CHECK(list_files(user).empty());
}

static void test_apply() {
    const std::string nup = put_nup("oot", make_oot(kTid));
    np::Scan s = np::scan(nup);
    CHECK(s.ok);
    CHECK_EQ(s.checked, 6u);
    CHECK_EQ(s.repairs.size(), 3u);
    const np::Repair* w = find_repair(s, kWater);
    CHECK(w && w->stand_in.path == kWaterSI && w->stand_in.size == 710);
    const np::Repair* so = find_repair(s, kSoil);
    CHECK(so && so->stand_in.path == kSoilSI);
    const np::Repair* lone = find_repair(s, "Tex/Lone_Alb.bntx.zs");
    CHECK(lone && lone->stand_in.path.empty());
    CHECK(s.repairs[0].damaged.path < s.repairs[1].damaged.path && s.repairs[1].damaged.path < s.repairs[2].damaged.path);   // sorted

    const std::string user = fresh("user");
    const std::string mod = mod_of(user, kTid);
    // foreign files: another mod of the same title, another title, a file inside RetroHub's folder
    write_text(user + "\\load\\" + "0400486024DEC000" + "\\Other mod\\romfs\\Tex\\keep.bin", "other");
    write_text(user + "\\load\\0100000000001000\\x\\romfs\\a.bin", "x");
    write_text(mod + "\\readme.txt", "mine");
    write_text(mod + "\\romfs\\Tex\\Foreign.bin", "foreign");

    np::Applied a = np::apply(nup, s, user);
    CHECK(a.ok && a.written == 2 && a.kept == 0 && a.removed == 0 && a.unrepaired == 1);
    Bytes got;
    CHECK(read_all(mod + "\\romfs\\Tex\\Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs", &got) && got == water_body());
    CHECK(read_all(mod + "\\romfs\\Tex\\CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm.bntx.zs", &got) && got == soil_body());
    CHECK(!exists(mod + "\\romfs\\Tex\\Lone_Alb.bntx.zs"));
    std::string mf;
    CHECK(winutil::read_file(mod + "\\" + np::kManifestName, &mf));
    CHECK_EQ(mf, expected_manifest(s, {{sha_hex(soil_body()), "420", kSoil, kSoilSI}, {sha_hex(water_body()), "710", kWater, kWaterSI}}));
    {   // sorted by damaged path: CmnTex... < Rock...
        np::Manifest m;
        CHECK(np::read_manifest(user, kTid, &m));
        CHECK(m.entries.size() == 2 && m.file_size == s.file_size && m.mtime == s.mtime && m.toc_sha256 == s.toc_sha256 && m.rule == np::kRuleVersion);
        CHECK(m.entries[0].damaged_path == kSoil && m.entries[0].stand_in_path == kSoilSI && m.entries[0].size == 420 && m.entries[0].sha256 == sha_hex(soil_body()));
    }
    // nothing else touched, no temp files left
    CHECK_EQ(list_files(user).size(), 7u);   // keep.bin, a.bin, readme.txt, Foreign.bin, 2 stand-ins, manifest
    CHECK(exists(user + "\\load\\0400486024DEC000\\Other mod\\romfs\\Tex\\keep.bin") && exists(user + "\\load\\0100000000001000\\x\\romfs\\a.bin"));

    np::Applied a2 = np::apply(nup, s, user);
    CHECK(a2.ok && a2.written == 0 && a2.kept == 2 && a2.removed == 0 && a2.unrepaired == 1);

    // an altered file is rewritten, the intact one is kept
    write_text(mod + "\\romfs\\Tex\\Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs", "tampered");
    np::Applied a3 = np::apply(nup, s, user);
    CHECK(a3.ok && a3.written == 1 && a3.kept == 1);
    CHECK(read_all(mod + "\\romfs\\Tex\\Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs", &got) && got == water_body());

    // a changed game file between scan and apply is refused
    const std::string other = put_nup("oot_other", make_oot(kTid, false));
    CHECK(!np::apply(other, s, user).ok);

    // stale entries: a copy with only the water damage -> the soil file is deleted (and its listing dropped), foreign files stay
    const std::string nup2 = put_nup("oot2", make_oot(kTid, false, true));
    np::Scan s2 = np::scan(nup2);
    CHECK(s2.ok && s2.repairs.size() == 2);
    np::Applied a4 = np::apply(nup2, s2, user);
    CHECK(a4.ok && a4.removed == 1 && a4.written == 0 && a4.kept == 1);
    CHECK(!exists(mod + "\\romfs\\Tex\\CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm.bntx.zs"));
    CHECK(exists(mod + "\\romfs\\Tex\\Foreign.bin") && exists(mod + "\\readme.txt"));
    { np::Manifest m; CHECK(np::read_manifest(user, kTid, &m) && m.entries.size() == 1 && m.entries[0].damaged_path == kWater); }

    // empty result: RetroHub's files and manifest go, the folder stays because of the foreign files
    const std::string nup3 = put_nup("oot3", make_oot(kTid, false, false));
    np::Scan s3 = np::scan(nup3);
    CHECK(s3.ok && s3.repairs.size() == 1 && s3.repairs[0].stand_in.path.empty());   // Lone only
    np::Applied a5 = np::apply(nup3, s3, user);
    CHECK(a5.ok && a5.removed == 1 && a5.written == 0 && a5.unrepaired == 1);
    CHECK(!exists(mod + "\\" + np::kManifestName) && !exists(mod + "\\romfs\\Tex\\Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs"));
    CHECK(exists(mod + "\\romfs\\Tex\\Foreign.bin") && exists(mod + "\\readme.txt"));
    CHECK(exists(user + "\\load\\0400486024DEC000\\Other mod\\romfs\\Tex\\keep.bin"));

    // no foreign files: the whole mod folder goes, nothing else
    const std::string user2 = fresh("user");
    CHECK(np::apply(nup, s, user2).ok);
    CHECK(exists(mod_of(user2, kTid)));
    write_text(user2 + "\\load\\0400486024DEC000\\Other mod\\romfs\\k.bin", "k");
    np::Applied a6 = np::apply(nup3, s3, user2);
    CHECK(a6.ok && a6.removed == 2);
    CHECK(!exists(mod_of(user2, kTid)));
    CHECK(exists(user2 + "\\load\\0400486024DEC000\\Other mod\\romfs\\k.bin"));

    // a hand-made folder without a manifest is adopted (identical file kept, listed), then remove() deletes only the listed files
    const std::string user3 = fresh("user");
    const std::string mod3 = mod_of(user3, kTid);
    write_all(mod3 + "\\romfs\\Tex\\Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs", water_body());
    write_text(mod3 + "\\romfs\\Tex\\Mine.bin", "mine");
    CHECK(!exists(mod3 + "\\" + np::kManifestName));
    CHECK_EQ(np::remove(user3, kTid), 0);   // no manifest: nothing is touched
    CHECK(exists(mod3 + "\\romfs\\Tex\\Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs"));
    np::Applied a7 = np::apply(nup, s, user3);
    CHECK(a7.ok && a7.kept == 1 && a7.written == 1);
    CHECK_EQ(np::remove(user3, kTid), 2);
    CHECK(!exists(mod3 + "\\" + np::kManifestName) && !exists(mod3 + "\\romfs\\Tex\\Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs"));
    CHECK(exists(mod3 + "\\romfs\\Tex\\Mine.bin"));
    CHECK_EQ(np::remove(user3, kTid), 0);

    // apply refuses a scan that was not ok and an empty user folder
    np::Scan notok;
    CHECK(!np::apply(nup, notok, user3).ok);
    CHECK(!np::apply(nup, s, "").ok);
}

static void test_ensure() {
    const std::string nup = put_nup("ensure", make_oot(kTid));
    const std::string user = fresh("user");
    const std::string mod = mod_of(user, kTid);
    np::Ensure e1 = np::ensure(nup, user, true);
    CHECK(e1.scanned && e1.scan.ok && e1.applied.ok && e1.applied.written == 2 && e1.applied.unrepaired == 1);
    CHECK(e1.summary.find("nup repair: 2 repaired (2 written, 0 kept), 6 checked") == 0);
    np::Ensure e2 = np::ensure(nup, user, true);
    CHECK(!e2.scanned && e2.applied.ok && e2.summary.find("unchanged") != std::string::npos);
    CHECK(list_files(user).size() == 3);

    // the NUP changes time -> rescan, files kept
    np::Manifest m;
    CHECK(np::read_manifest(user, kTid, &m));
    CHECK(set_mtime(nup, m.mtime + 30000000ull));
    np::Ensure e3 = np::ensure(nup, user, true);
    CHECK(e3.scanned && e3.applied.ok && e3.applied.kept == 2 && e3.applied.written == 0);
    CHECK(!np::ensure(nup, user, true).scanned);

    // a listed file deleted / altered -> rescan and rewrite
    DeleteFileW(winutil::to_wide(mod + "\\romfs\\Tex\\" + "Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs").c_str());
    np::Ensure e4 = np::ensure(nup, user, true);
    CHECK(e4.scanned && e4.applied.written == 1 && e4.applied.kept == 1);
    write_text(mod + "\\romfs\\Tex\\CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm.bntx.zs", "x");
    np::Ensure e5 = np::ensure(nup, user, true);
    CHECK(e5.scanned && e5.applied.written == 1 && e5.applied.kept == 1);
    Bytes got;
    CHECK(read_all(mod + "\\romfs\\Tex\\CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm.bntx.zs", &got) && got == soil_body());
    CHECK(!np::ensure(nup, user, true).scanned);

    // another rule version in the manifest -> rescan
    {
        std::string mf;
        CHECK(winutil::read_file(mod + "\\" + np::kManifestName, &mf));
        const size_t at = mf.find(" rule 1\n");
        CHECK(at != std::string::npos);
        mf.replace(at, 8, " rule 0\n");
        write_text(mod + "\\" + np::kManifestName, mf);
        CHECK(np::ensure(nup, user, true).scanned);
    }

    // another copy at the same path (different size / hash): rescan, stale file dropped
    write_all(nup, make_oot(kTid, false, true));
    np::Ensure e6 = np::ensure(nup, user, true);
    CHECK(e6.scanned && e6.applied.ok && e6.applied.removed == 1 && e6.applied.kept == 1);
    CHECK(!exists(mod + "\\romfs\\Tex\\CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm.bntx.zs"));

    // a damaged manifest is just rescanned
    write_text(mod + "\\" + np::kManifestName, "garbage\n");
    CHECK(np::ensure(nup, user, true).scanned);

    // the setting off removes what is listed and nothing else
    write_text(mod + "\\readme.txt", "mine");
    np::Ensure off = np::ensure(nup, user, false);
    CHECK(!off.scanned && off.applied.ok && off.applied.removed == 1 && off.summary.find("off") != std::string::npos);
    CHECK(exists(mod + "\\readme.txt") && !exists(mod + "\\" + np::kManifestName) && !exists(mod + "\\romfs\\Tex\\Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs"));
    CHECK_EQ(np::ensure(nup, user, false).applied.removed, 0);
    // and back on
    CHECK(np::ensure(nup, user, true).scanned);

    // a non-NUP path / a missing file: an error line, never a throw
    np::Ensure bad = np::ensure(g_root + "\\nope.nup", user, true);
    CHECK(!bad.scanned && !bad.applied.ok && bad.summary.find("error") != std::string::npos);
    np::Ensure bad2 = np::ensure(g_root + "\\nope.nup", user, false);
    CHECK(!bad2.applied.ok && !bad2.summary.empty());
}

static void test_unicode() {
    const std::wstring wdir = L"Zelda – テスト";
    const std::string dir = g_root + "\\" + winutil::to_utf8(wdir);
    const std::string nup = dir + "\\" + winutil::to_utf8(wdir) + ".nup";
    CHECK(write_all(nup, make_oot(kTid)));
    const std::string user = g_root + "\\" + winutil::to_utf8(L"ユーザー é") + "\\user";
    np::Scan s = np::scan(nup);
    CHECK(s.ok && s.repairs.size() == 3);
    np::Ensure e = np::ensure(nup, user, true);
    CHECK(e.scanned && e.applied.ok && e.applied.written == 2);
    CHECK(exists(mod_of(user, kTid) + "\\romfs\\Tex\\Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs"));
    CHECK(!np::ensure(nup, user, true).scanned);
    Bytes b;
    std::string err;
    CHECK(np::extract(nup, kWaterSI, &b, &err) && b == water_body());
    CHECK_EQ(np::ensure(nup, user, false).applied.removed, 2);
}

static np::Manifest mf_of(std::vector<std::string> dmg) {
    np::Manifest m;
    for (const std::string& d : dmg) { np::ManifestEntry e; e.damaged_path = d; e.stand_in_path = "Tex/x"; e.sha256 = std::string(64, 'a'); e.size = 1; m.entries.push_back(e); }
    return m;
}

static void test_text_and_manifest() {
    CHECK_EQ(np::page_text(np::Manifest()), std::string());
    CHECK_EQ(np::page_text(mf_of({"Tex/CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm.bntx.zs", "Tex/RBake_Scene_GanonStage_G_2_1_d0_t0.bntx.zs", "Tex/Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs"})),
             std::string("This copy of the game has 3 damaged files, so some rooms would be invisible (the Water Temple, the fairy fountains, Ganon's Castle). RetroHub fills each one in with a matching file from the same copy when the game starts, so they show up. Only a fresh dump of your own copy restores them exactly."));
    const std::string one = np::page_text(mf_of({"Tex/Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs"}));
    CHECK(one.find("has 1 damaged file,") != std::string::npos && one.find("(the Water Temple)") != std::string::npos && one.find("fills it in") != std::string::npos && one.find("restores it exactly") != std::string::npos);
    const std::string unk = np::page_text(mf_of({"Tex/Whatever_Nrm.bntx.zs", "Tex/Other_Nrm.bntx.zs"}));
    CHECK(unk.find("2 damaged files, so some rooms would be invisible.") != std::string::npos && unk.find('(') == std::string::npos);
    CHECK(one.find(".zs") == std::string::npos && one.find("Rock_") == std::string::npos);   // no file names

    // read_manifest: malformed variants
    const std::string user = fresh("user");
    const std::string mp = mod_of(user, kTid) + "\\" + np::kManifestName;
    np::Manifest m;
    CHECK(!np::read_manifest(user, kTid, &m));   // missing
    const std::string sha = std::string(64, 'b'), toc = std::string(64, 'c');
    const std::string head = "RetroHub damaged-file repair 1\nnup 100 200 " + toc + " rule 1\n";
    write_text(mp, head + "repair " + sha + " 5\tTex/a.zs\tTex/b.zs\n");
    CHECK(np::read_manifest(user, kTid, &m) && m.entries.size() == 1 && m.file_size == 100 && m.mtime == 200 && m.rule == 1 && m.toc_sha256 == toc && m.entries[0].size == 5 && m.entries[0].stand_in_path == "Tex/b.zs");
    write_text(mp, "RetroHub damaged-file repair 2\nnup 100 200 " + toc + " rule 1\n");
    CHECK(!np::read_manifest(user, kTid, &m));
    write_text(mp, head + "repair " + sha + " 5\tTex/../a.zs\tTex/b.zs\n");
    CHECK(!np::read_manifest(user, kTid, &m));
    write_text(mp, head + "repair " + std::string(63, 'b') + " 5\tTex/a.zs\tTex/b.zs\n");
    CHECK(!np::read_manifest(user, kTid, &m));
    write_text(mp, head + "repair " + sha + " x\tTex/a.zs\tTex/b.zs\n");
    CHECK(!np::read_manifest(user, kTid, &m));
    write_text(mp, head + "repair " + sha + " 5\tTex/a.zs\n");
    CHECK(!np::read_manifest(user, kTid, &m));
    write_text(mp, "RetroHub damaged-file repair 1\nnup 100 " + toc + " rule 1\n");
    CHECK(!np::read_manifest(user, kTid, &m));
    write_text(mp, head + "repair " + sha + " 5\tC:\\evil.zs\tTex/b.zs\n");
    CHECK(!np::read_manifest(user, kTid, &m));
    // a manifest naming a path outside the mod folder never makes remove() delete it
    write_text(user + "\\outside.txt", "keep");
    write_text(mp, head + "repair " + sha + " 5\t../../../../outside.txt\tTex/b.zs\n");
    CHECK_EQ(np::remove(user, kTid), 0);
    CHECK(exists(user + "\\outside.txt"));
}

// ---------------------------------------------------------------- real-file QA modes
static std::string file_sha(const std::string& p) { Bytes b; return read_all(p, &b) ? sha_hex(b) : std::string("(unreadable)"); }

static void print_written(const std::string& user, uint64_t tid) {
    np::Manifest m;
    if (!np::read_manifest(user, tid, &m)) { std::printf("  (no manifest)\n"); return; }
    for (const np::ManifestEntry& e : m.entries) {
        std::string rel = e.damaged_path;
        std::replace(rel.begin(), rel.end(), '/', '\\');
        std::printf("  %s  %s  (%llu bytes, from %s)\n", file_sha(mod_of(user, tid) + "\\romfs\\" + rel).c_str(), e.damaged_path.c_str(), (unsigned long long)e.size, e.stand_in_path.c_str());
    }
}

static void print_scan(const np::Scan& s) {
    std::printf("scan ok=%d%s%s  title %016llX  checked %llu  repairs %zu\n", s.ok ? 1 : 0, s.ok ? "" : " error=", s.error.c_str(), (unsigned long long)s.title_id, (unsigned long long)s.checked, s.repairs.size());
    for (const np::Repair& r : s.repairs)
        std::printf("  damaged %s <- %s\n", r.damaged.path.c_str(), r.stand_in.path.empty() ? "(no stand-in)" : r.stand_in.path.c_str());
}

static int real_mode(int argc, char** argv) {
    const std::string mode = argv[1];
    if (argc < 3) { std::printf("usage: nuprepair_test --scan <file.nup> | --apply <file.nup> <user_dir> | --ensure <file.nup> <user_dir>\n"); return 2; }
    const std::string nup = argv[2];
    const auto t0 = std::chrono::steady_clock::now();
    auto secs = [&]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    if (mode == "--scan") {
        np::Scan s = np::scan(nup);
        const double t = secs();
        print_scan(s);
        std::printf("time %.2f s\n", t);
        return s.ok ? 0 : 1;
    }
    if (argc < 4) { std::printf("%s needs <user_dir>\n", mode.c_str()); return 2; }
    const std::string user = argv[3];
    if (mode == "--apply") {
        np::Scan s = np::scan(nup);
        const double ts = secs();
        print_scan(s);
        np::Applied a = np::apply(nup, s, user);
        std::printf("apply ok=%d written %d kept %d removed %d unrepaired %d%s%s\n", a.ok ? 1 : 0, a.written, a.kept, a.removed, a.unrepaired, a.ok ? "" : " error=", a.error.c_str());
        print_written(user, s.title_id);
        std::printf("time scan %.2f s, total %.2f s\n", ts, secs());
        return a.ok ? 0 : 1;
    }
    if (mode == "--ensure") {
        np::Ensure e = np::ensure(nup, user, true);
        const double t = secs();
        std::printf("ensure scanned=%d ok=%d written %d kept %d removed %d unrepaired %d\n  summary: %s\n", e.scanned ? 1 : 0, e.applied.ok ? 1 : 0, e.applied.written, e.applied.kept, e.applied.removed, e.applied.unrepaired, e.summary.c_str());
        if (e.scanned) print_scan(e.scan);
        np::Scan quick;
        quick.title_id = e.scan.title_id;
        if (!e.scanned) {   // title id for the listing: read from the manifests the folder holds
            std::error_code ec;
            for (fs::directory_iterator it(FP(user + "\\load"), ec), end; !ec && it != end; it.increment(ec)) {
                const std::string name = winutil::to_utf8(it->path().filename().wstring());
                if (name.size() == 16) quick.title_id = std::stoull(name, nullptr, 16);
            }
        }
        print_written(user, quick.title_id);
        std::printf("time %.2f s\n", t);
        return e.applied.ok ? 0 : 1;
    }
    std::printf("unknown mode %s\n", mode.c_str());
    return 2;
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == '-') return real_mode(argc, argv);
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    g_root = winutil::to_utf8(std::wstring(tmp)) + "nuprepair_test_" + std::to_string(GetCurrentProcessId());
    while (!g_root.empty() && g_root.back() == '\\') g_root.pop_back();
    std::error_code ec;
    fs::remove_all(FP(g_root), ec);
    fs::create_directories(FP(g_root + "\\nups"), ec);

    test_healthy();
    test_standin_choice();
    test_no_standin();
    test_merge();
    test_malformed();
    test_extract_sha();
    test_apply();
    test_ensure();
    test_unicode();
    test_text_and_manifest();

    fs::remove_all(FP(g_root), ec);
    std::printf("nuprepair_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
