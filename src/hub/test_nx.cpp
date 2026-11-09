// Console test for Nintendo Switch / Switch 2 identification (src/common/nx.h): PFS0 / XCI / NRO / NPDM parsers on synthetic headers, file front
// ends, loose exefs dumps through scan_library, the cache round trip (nx2) and that other systems are untouched. No network, no emulator, no keys.
#include <windows.h>
#include "library.h"
#include "nx.h"
#include "dump_check.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace hub;

static int g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

typedef std::vector<uint8_t> Bytes;

static void le32(Bytes& b, size_t at, uint32_t v) { if (b.size() < at + 4) b.resize(at + 4); for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); }
static void le64(Bytes& b, size_t at, uint64_t v) { le32(b, at, (uint32_t)v); le32(b, at + 4, (uint32_t)(v >> 32)); }
static void put(Bytes& b, size_t at, const std::string& s) { if (b.size() < at + s.size()) b.resize(at + s.size()); std::memcpy(&b[at], s.data(), s.size()); }
static bool write_file(const fs::path& p, const Bytes& d) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(d.data()), (std::streamsize)d.size());
    return (bool)f;
}
static std::string U8(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }

static Bytes make_pfs0(const std::vector<std::string>& names) {
    std::string strtab;
    std::vector<uint32_t> off;
    for (auto& n : names) { off.push_back((uint32_t)strtab.size()); strtab += n; strtab.push_back(0); }
    Bytes b(16 + names.size() * 0x18 + strtab.size(), 0);
    put(b, 0, "PFS0");
    le32(b, 4, (uint32_t)names.size());
    le32(b, 8, (uint32_t)strtab.size());
    for (size_t i = 0; i < names.size(); ++i) { le64(b, 16 + i * 0x18, 0); le64(b, 16 + i * 0x18 + 8, 0); le32(b, 16 + i * 0x18 + 16, off[i]); }
    put(b, 16 + names.size() * 0x18, strtab);
    return b;
}
static std::string rights(uint64_t tid) { char b[40]; std::snprintf(b, sizeof b, "%016llx%016llx.tik", (unsigned long long)tid, 0x0ull); return b; }

static Bytes make_xci(size_t head_at) { Bytes b(head_at + 0x200, 0); put(b, head_at, "HEAD"); return b; }

static Bytes make_nro(const std::string& nacp_title, bool aset) {
    Bytes b(0x80, 0);
    put(b, 0x10, "NRO0");
    le32(b, 0x18, 0x80);   // nro size
    if (aset) {
        b.resize(0x80 + 0x38 + 0x4000, 0);
        put(b, 0x80, "ASET");
        le64(b, 0x80 + 0x18, 0x38);       // nacp offset (relative to ASET)
        le64(b, 0x80 + 0x20, 0x4000);     // nacp size
        put(b, 0x80 + 0x38, nacp_title);
    }
    return b;
}

static Bytes make_npdm(uint64_t tid, bool nx2, const std::string& name) {
    Bytes b(0x1000, 0);
    put(b, 0, "META");
    put(b, 0x20, name);
    le32(b, 0x70, 0x80);  le32(b, 0x74, 0x40);
    put(b, 0x80, "ACI0");
    le64(b, 0x80 + 0x10, tid);
    const uint32_t acid = 0x100;
    le32(b, 0x78, acid); le32(b, 0x7C, 0x400);
    put(b, acid + (nx2 ? 0x300 : 0x200), "ACID");
    return b;
}

/* NUP (Lichii build 88 nup_container.cpp): superblock 0x80, TOC at 0x80 = 0x30 header + 0x40-byte package records, then each package's meta TLVs. */
struct NupPkg {
    uint64_t tid = 0;
    uint8_t kind = 0;       // 0 base, 1 update, 2 add-on
    uint8_t flags = 0;      // 1 = standalone update
    uint32_t version = 0;
    bool exefs = true;
    Bytes meta;
    bool empty_exefs = false;   // an ExeFS dir with no children: Lichii's GetExeFSDir returns null
};
static Bytes tlv(uint16_t type, const Bytes& payload) {
    Bytes b(8, 0);
    b[0] = uint8_t(type); b[1] = uint8_t(type >> 8);
    le32(b, 4, (uint32_t)payload.size());
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}
static Bytes tlv_s(uint16_t type, const std::string& s) { return tlv(type, Bytes(s.begin(), s.end())); }
static Bytes localized(const std::vector<std::pair<std::string, std::string>>& v) {
    Bytes b;
    for (auto& lt : v) {
        b.push_back((uint8_t)lt.first.size());
        b.insert(b.end(), lt.first.begin(), lt.first.end());
        size_t at = b.size();
        le32(b, at, (uint32_t)lt.second.size());
        b.insert(b.end(), lt.second.begin(), lt.second.end());
    }
    return b;
}
static Bytes cat(std::vector<Bytes> parts) { Bytes o; for (auto& p : parts) o.insert(o.end(), p.begin(), p.end()); return o; }
static Bytes make_nup(const std::vector<NupPkg>& pk) {
    /* each package i owns dirs 3i (root), 3i+1 (exefs), 3i+2 (romfs); dir records (0x18) follow the package records */
    const size_t toc_off = 0x80, n = pk.size(), dirs = 3 * n, dir_at = toc_off + 0x30 + n * 0x40, toc_size = 0x30 + n * 0x40 + dirs * 0x18;
    Bytes b(toc_off + toc_size, 0);
    put(b, 0, std::string("NUP\x01", 4));
    b[4] = 0x80; b[6] = 1;
    le64(b, 0x08, toc_off);
    le64(b, 0x10, toc_size);
    le32(b, 0x48, (uint32_t)n);
    put(b, toc_off, std::string("TOC\x01", 4));
    le32(b, toc_off + 4, (uint32_t)n);
    le64(b, toc_off + 8, dirs);
    for (size_t i = 0; i < n; ++i) {
        const size_t r = toc_off + 0x30 + i * 0x40;
        le64(b, r, pk[i].tid);
        le32(b, r + 0x08, (uint32_t)(3 * i));
        le32(b, r + 0x0C, pk[i].exefs ? (uint32_t)(3 * i + 1) : 0xFFFFFFFFu);
        le32(b, r + 0x10, (uint32_t)(3 * i + 2));
        le32(b, r + 0x14, pk[i].version);
        b[r + 0x18] = pk[i].kind;
        b[r + 0x19] = pk[i].flags;
        le32(b, dir_at + (3 * i) * 0x18 + 0x0C, 2);                                    // root: exefs + romfs
        le32(b, dir_at + (3 * i + 1) * 0x18 + 0x0C, pk[i].empty_exefs ? 0u : 2u);        // exefs: main + main.npdm
        le32(b, dir_at + (3 * i + 2) * 0x18 + 0x0C, 1);
    }
    for (size_t i = 0; i < n; ++i) {
        if (pk[i].meta.empty()) continue;
        while (b.size() % 0x10) b.push_back(0);
        le64(b, toc_off + 0x30 + i * 0x40 + 0x20, b.size());
        le32(b, toc_off + 0x30 + i * 0x40 + 0x28, (uint32_t)pk[i].meta.size());
        b.insert(b.end(), pk[i].meta.begin(), pk[i].meta.end());
    }
    while (b.size() % 0x10) b.push_back(0);
    le64(b, 0x38, b.size());   // blob offset (no member files here)
    le64(b, 0x40, b.size());   // file size
    return b;
}
static bool nup_ident(const Bytes& b, size_t n, nx::Ident* id) {
    *id = nx::Ident();
    return nx::parse_nup([&](uint64_t off, size_t len, Bytes* o) {
        if (off > n) return false;
        size_t k = (size_t)std::min<uint64_t>(len, n - off);
        o->assign(b.begin() + (size_t)off, b.begin() + (size_t)off + k);
        return true;
    }, n, id);
}

static void test_nup() {
    nx::Ident id;
    // a base package: title TLV + a Switch 2 NPDM
    Bytes meta = cat({tlv(1, make_npdm(0x0400AAAA00000000ull, true, "Application")), tlv_s(4, "1.0.0"), tlv_s(5, "Base Game")});
    Bytes b = make_nup({{0x0400AAAA00000000ull, 0, 0, 0, true, meta}});
    CHECK(nup_ident(b, b.size(), &id) && id.format == "nup" && id.title_id == 0x0400AAAA00000000ull && id.nx2 && !id.update_or_dlc && id.title == "Base Game");

    // the user's dump: one standalone update, no title TLV, localized titles (English wins), no NPDM -> nx2 from the 04xx id
    meta = tlv(9, localized({{"ja", "JP Title"}, {"en-US", "Ocarina of Time"}, {"fr", "FR"}}));
    b = make_nup({{0x0400486024DEC000ull, 1, 1, 0x20000, true, meta}});
    CHECK(nup_ident(b, b.size(), &id) && id.title_id == 0x0400486024DEC000ull && !id.update_or_dlc && id.nx2 && id.title == "Ocarina of Time");
    // no English: the first localized title
    b = make_nup({{0x0400486024DEC000ull, 1, 1, 1, true, tlv(9, localized({{"ja", "JP Title"}, {"de", "DE"}}))}});
    CHECK(nup_ident(b, b.size(), &id) && id.title == "JP Title");

    // an update's own id (base + 0x800) is reported as the base id
    b = make_nup({{0x0400BBBB00000800ull, 1, 0, 1, true, {}}});
    CHECK(nup_ident(b, b.size(), &id) && id.title_id == 0x0400BBBB00000000ull && !id.update_or_dlc && id.title.empty());

    // two updates: the newest one is the game (like Lichii's FindBestUpdatePackage)
    b = make_nup({{0x0400CCCC00000800ull, 1, 0, 1, true, tlv_s(5, "Old")}, {0x0400CCCC00000800ull, 1, 0, 7, true, tlv_s(5, "New")},
                  {0x0400CCCC00000800ull, 1, 0, 3, true, tlv_s(5, "Mid")}});
    CHECK(nup_ident(b, b.size(), &id) && id.title == "New");

    // base + update + add-on: the base wins
    b = make_nup({{0x0400DDDD00000800ull, 1, 0, 9, true, tlv_s(5, "Update")}, {0x0400DDDD00001001ull, 2, 0, 0, true, tlv_s(5, "Addon")},
                  {0x0400DDDD00000000ull, 0, 0, 0, true, tlv_s(5, "Base")}});
    CHECK(nup_ident(b, b.size(), &id) && id.title == "Base" && id.title_id == 0x0400DDDD00000000ull);

    // a base without an ExeFS cannot boot: the update with one is used
    b = make_nup({{0x0400EEEE00000000ull, 0, 0, 0, false, tlv_s(5, "Base")}, {0x0400EEEE00000800ull, 1, 0, 1, true, tlv_s(5, "Patched")}});
    CHECK(nup_ident(b, b.size(), &id) && id.title == "Patched" && !id.update_or_dlc);

    // add-on only / nothing with an ExeFS: not a game
    b = make_nup({{0x0400FFFF00001001ull, 2, 0, 0, true, tlv_s(5, "DLC")}});
    CHECK(nup_ident(b, b.size(), &id) && id.update_or_dlc);
    b = make_nup({{0x0400FFFF00000000ull, 0, 0, 0, false, {}}, {0x0400FFFF00000800ull, 1, 0, 1, false, {}}});
    CHECK(nup_ident(b, b.size(), &id) && id.update_or_dlc);

    // an empty ExeFS cannot boot (Lichii's GetExeFSDir returns null): the update is used, or nothing
    b = make_nup({{0x0400ACAC00000000ull, 0, 0, 0, true, tlv_s(5, "Base"), true}, {0x0400ACAC00000800ull, 1, 0, 1, true, tlv_s(5, "Patched")}});
    CHECK(nup_ident(b, b.size(), &id) && id.title == "Patched");
    b = make_nup({{0x0400ACAC00000800ull, 1, 1, 1, true, tlv_s(5, "Patched"), true}});
    CHECK(nup_ident(b, b.size(), &id) && id.update_or_dlc);
    // only the FIRST base counts (FindBasePackage); the newest update must itself have an ExeFS (no fallback to an older one)
    b = make_nup({{0x0400ADAD00000000ull, 0, 0, 0, false, tlv_s(5, "Base 1")}, {0x0400ADAD00000000ull, 0, 0, 0, true, tlv_s(5, "Base 2")}});
    CHECK(nup_ident(b, b.size(), &id) && id.update_or_dlc);
    b = make_nup({{0x0400AEAE00000800ull, 1, 0, 1, true, tlv_s(5, "v1")}, {0x0400AEAE00000800ull, 1, 0, 2, false, tlv_s(5, "v2")}});
    CHECK(nup_ident(b, b.size(), &id) && id.update_or_dlc);

    // a Switch 1 program in a NUP
    b = make_nup({{0x0100ABAB00000000ull, 0, 0, 0, true, tlv(1, make_npdm(0x0100ABAB00000000ull, false, "x"))}});
    CHECK(nup_ident(b, b.size(), &id) && !id.nx2 && id.title_id == 0x0100ABAB00000000ull);

    // not a NUP / too short
    Bytes bad = b;
    bad[3] = 2;
    CHECK(!nup_ident(bad, bad.size(), &id));
    CHECK(!nup_ident(b, 0x7F, &id));
    CHECK(!nx::parse_nup(nx::ReadFn(), 0, &id));

    // every truncation and hostile fields: never crashes, never reads out of bounds
    Bytes full = make_nup({{0x0400486024DEC000ull, 1, 1, 1, true, cat({tlv(1, make_npdm(0x0400486024DEC000ull, true, "x")), tlv(9, localized({{"en", "T"}}))})}});
    for (size_t n = 0; n <= full.size(); ++n) nup_ident(full, n, &id);
    CHECK(nup_ident(full, full.size(), &id) && !id.update_or_dlc);
    for (size_t n = 0x80; n < full.size(); ++n) CHECK(!nup_ident(full, n, &id));   // truncated: the declared size no longer matches
    Bytes h = full;
    le32(h, 0x48, 0xFFFFFFFF);                   // package count
    CHECK(!nup_ident(h, h.size(), &id));
    h = full;
    le64(h, 0x08, ~0ull);                        // toc offset
    CHECK(!nup_ident(h, h.size(), &id));
    h = full;
    le64(h, 0x40, h.size() + 16);                // declared file size
    CHECK(!nup_ident(h, h.size(), &id));
    h = full;
    le32(h, 0x4C, 1);                            // superblock flags
    CHECK(!nup_ident(h, h.size(), &id));
    h = full;
    le32(h, 0x80 + 4, 2);                        // TOC package count != superblock count
    CHECK(!nup_ident(h, h.size(), &id));
    h = full;
    h[0x80 + 0x30 + 0x18] = 3;                   // unknown package kind
    CHECK(!nup_ident(h, h.size(), &id));
    h = full;
    h[0x80 + 0x30 + 0x19] = 2;                   // unknown package flag
    CHECK(!nup_ident(h, h.size(), &id));
    h = full;
    le32(h, 0x80 + 0x30 + 0x0C, 3);              // exefs dir past dir_count
    CHECK(!nup_ident(h, h.size(), &id));
    h = full;
    le64(h, 0x80 + 8, ~0ull);                    // dir count
    CHECK(!nup_ident(h, h.size(), &id));
    h = full;
    le32(h, 0x80 + 0x30 + 0x28, 0xFFFFFFF0);     // meta size
    CHECK(nup_ident(h, h.size(), &id) && !id.update_or_dlc && id.title.empty());
    h = full;
    {   // a TLV whose size runs past the meta block, and a localized record whose length does
        const size_t meta_at = (size_t)(h[0x80 + 0x30 + 0x20] | (h[0x80 + 0x30 + 0x21] << 8) | (h[0x80 + 0x30 + 0x22] << 16));
        le32(h, meta_at + 4, 0x7FFFFFFF);
        CHECK(nup_ident(h, h.size(), &id) && !id.update_or_dlc);
    }
    h = make_nup({{0x0400AAAA00000000ull, 0, 0, 0, true, tlv(9, Bytes{2, 'e', 'n', 0xFF, 0xFF, 0xFF, 0x7F, 'x'})}});
    CHECK(nup_ident(h, h.size(), &id) && id.title.empty());
}

static void test_parsers() {
    nx::Ident id;
    Bytes p = make_pfs0({"aaaa.nca", rights(0x0100ABCD12340000ull), "bbbb.cnmt.nca"});
    CHECK(nx::parse_pfs0(p.data(), p.size(), &id));
    CHECK(id.title_id == 0x0100ABCD12340000ull && !id.update_or_dlc && id.format == "nsp");

    id = nx::Ident();
    p = make_pfs0({"aaaa.nca", "bbbb.nca"});
    CHECK(nx::parse_pfs0(p.data(), p.size(), &id) && id.title_id == 0);

    id = nx::Ident();
    p = make_pfs0({rights(0x0100ABCD12340800ull)});
    CHECK(nx::parse_pfs0(p.data(), p.size(), &id) && id.update_or_dlc);
    id = nx::Ident();
    p = make_pfs0({rights(0x0100ABCD12341001ull)});
    CHECK(nx::parse_pfs0(p.data(), p.size(), &id) && id.update_or_dlc);
    CHECK(nx::is_base_title(0x0100ABCD12340000ull) && !nx::is_base_title(0x0100ABCD12340800ull) && !nx::is_base_title(0x0100ABCD12341001ull) && !nx::is_base_title(0));

    id = nx::Ident();
    Bytes x = make_xci(0x100);
    CHECK(nx::parse_xci(x.data(), x.size(), &id) && id.format == "xci");
    x = make_xci(0x1100);
    CHECK(nx::parse_xci(x.data(), x.size(), &id));
    x = Bytes(0x2000, 0);
    CHECK(!nx::parse_xci(x.data(), x.size(), &id));

    id = nx::Ident();
    Bytes n = make_nro("Homebrew Thing", true);
    CHECK(nx::parse_nro(n.data(), n.size(), n.size(), &id) && id.title == "Homebrew Thing" && id.format == "nro");
    id = nx::Ident();
    n = make_nro("", false);
    CHECK(nx::parse_nro(n.data(), n.size(), n.size(), &id) && id.title.empty());

    id = nx::Ident();
    Bytes m1 = make_npdm(0x0100ABCD12340000ull, false, "Application");
    CHECK(nx::parse_npdm(m1.data(), m1.size(), &id) && id.title_id == 0x0100ABCD12340000ull && !id.nx2 && id.loose && id.title.empty());
    id = nx::Ident();
    Bytes m2 = make_npdm(0x0400ABCD12340000ull, true, "Some Game");
    CHECK(nx::parse_npdm(m2.data(), m2.size(), &id) && id.title_id == 0x0400ABCD12340000ull && id.nx2 && id.title == "Some Game");

    CHECK(nx::title_id_from_name("Super Game [0100ABCD12340000][v0] (USA)") == 0x0100ABCD12340000ull);
    CHECK(nx::title_id_from_name("nothing [1234]") == 0);
    CHECK(nx::clean_title("Super Game [0100ABCD12340000][v0] (USA)") == "Super Game");
    CHECK(nx::clean_title("A_B  C") == "A B C");
    CHECK(nx::key_for(0x0100ABCD12340000ull, "x") == "switch:0100abcd12340000");
    CHECK(nx::key_for(0, "my-game") == "switch:file-my-game");
    CHECK(nx::title_id_from_key("switch:0100abcd12340000") == 0x0100ABCD12340000ull);
    CHECK(nx::title_id_from_key("switch:file-x") == 0 && nx::title_id_from_key("n64:abc") == 0);
}

static void test_garbage() {
    nx::Ident id;
    Bytes junk(64, 0xFF);
    for (size_t n = 0; n <= 64; ++n) {
        CHECK(!nx::parse_pfs0(junk.data(), n, &id) && !nx::parse_xci(junk.data(), n, &id) && !nx::parse_nro(junk.data(), n, 1000, &id) && !nx::parse_npdm(junk.data(), n, &id));
    }
    // valid magics with hostile fields, every truncation length
    Bytes p = make_pfs0({"x.nca", rights(0x0100ABCD12340000ull)});
    le32(p, 4, 0xFFFFFFFF);
    for (size_t n = 0; n <= p.size(); ++n) nx::parse_pfs0(p.data(), n, &id);
    Bytes p2 = make_pfs0({"x.nca", rights(0x0100ABCD12340000ull)});
    le32(p2, 16 + 16, 0xFFFFFFF0);
    for (size_t n = 0; n <= p2.size(); ++n) nx::parse_pfs0(p2.data(), n, &id);
    Bytes nro = make_nro("T", true);
    le64(nro, 0x80 + 0x18, ~0ull);
    for (size_t n = 0; n <= nro.size(); n += 7) nx::parse_nro(nro.data(), n, nro.size(), &id);
    Bytes m = make_npdm(1, true, "x");
    le32(m, 0x70, 0xFFFFFFFF); le32(m, 0x78, 0xFFFFFFFF);
    for (size_t n = 0; n <= m.size(); n += 3) nx::parse_npdm(m.data(), n, &id);
    nx::parse_pfs0(nullptr, 0, &id);

    // file front ends on missing / truncated files
    fs::path d = fs::temp_directory_path() / "nx_test_garbage";
    std::error_code ec;
    fs::remove_all(d, ec);
    write_file(d / "a.nsp", Bytes{'P', 'F', 'S', '0'});
    write_file(d / "b.xci", Bytes(10, 0));
    write_file(d / "c.nro", Bytes(0x12, 0));
    write_file(d / "exefs" / "main", Bytes(4, 0));
    write_file(d / "exefs" / "main.npdm", Bytes(8, 0));
    CHECK(!nx::identify_file(U8(d / "a.nsp"), &id));
    CHECK(!nx::identify_file(U8(d / "b.xci"), &id));
    CHECK(!nx::identify_file(U8(d / "c.nro"), &id));
    CHECK(!nx::identify_file(U8(d / "exefs" / "main"), &id));
    CHECK(!nx::identify_file(U8(d / "missing.nsp"), &id));
    fs::remove_all(d, ec);
}

static void test_files_and_scan() {
    fs::path d = fs::temp_directory_path() / "nx_test_scan";
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::path games = d / "games";
    write_file(games / "Super Game [0100ABCD12340000][v0].nsp", make_pfs0({"a.nca", rights(0x0100ABCD12340000ull)}));
    write_file(games / "Tagged Only [0100AAAA00000000].nsp", make_pfs0({"a.nca"}));
    write_file(games / "Update [0100ABCD12340800][v65536].nsp", make_pfs0({rights(0x0100ABCD12340800ull)}));
    write_file(games / "Dlc [0100ABCD12341001].nsp", make_pfs0({rights(0x0100ABCD12341001ull)}));
    write_file(games / "Cart [0100BBBB00000000].xci", make_xci(0x100));
    write_file(games / "Cart2 [0100CCCC00000000].xci", make_xci(0x1100));
    write_file(games / "brew.nro", make_nro("Cool Homebrew", true));
    write_file(games / "plain.nro", make_nro("", false));
    write_file(games / "Mario Kart World" / "exefs" / "main", Bytes(64, 1));
    write_file(games / "Mario Kart World" / "exefs" / "main.npdm", make_npdm(0x0400ABCD12340000ull, true, "Application"));
    write_file(games / "Old Dump" / "EXEFS" / "main", Bytes(64, 1));
    write_file(games / "Old Dump" / "EXEFS" / "main.npdm", make_npdm(0x0100DDDD00000000ull, false, "Whatever"));
    write_file(games / "Orphan" / "exefs" / "main", Bytes(64, 1));          // no npdm: not a game
    write_file(games / "Other" / "main", Bytes(64, 1));                      // main outside exefs: ignored
    write_file(games / "Update Dump" / "exefs" / "main", Bytes(64, 1));
    write_file(games / "Update Dump" / "exefs" / "main.npdm", make_npdm(0x0100EEEE00000800ull, false, "x"));
    write_file(games / "Real Cart.gba", Bytes(0x200, 0));
    write_file(games / "Switch2" / "Zelda OoT [04004860224DEC000].nup",   // the user's case: a standalone update, a 17-digit name tag
               make_nup({{0x0400486024DEC000ull, 1, 1, 0x20000, true, tlv(9, localized({{"en-US", "Ocarina of Time"}}))}}));
    write_file(games / "Bare [0400ABAB00000000].NUP", make_nup({{0x0400ABAB00000000ull, 0, 0, 0, true, {}}}));
    write_file(games / "Addon Pack.nup", make_nup({{0x0400ABAB00001001ull, 2, 0, 0, true, {}}}));
    write_file(games / "broken.nup", Bytes(0x200, 0x5A));                    // a non-game GBA stays unlisted

    nx::Ident id;
    CHECK(nx::identify_file(U8(games / "Super Game [0100ABCD12340000][v0].nsp"), &id) && id.title == "Super Game" && id.title_id == 0x0100ABCD12340000ull);
    CHECK(nx::identify_file(U8(games / "Tagged Only [0100AAAA00000000].nsp"), &id) && id.title_id == 0x0100AAAA00000000ull);
    CHECK(nx::identify_file(U8(games / "Update [0100ABCD12340800][v65536].nsp"), &id) && id.update_or_dlc);
    CHECK(nx::identify_file(U8(games / "brew.nro"), &id) && id.title == "Cool Homebrew");
    CHECK(nx::identify_file(U8(games / "plain.nro"), &id) && id.title == "plain");
    CHECK(nx::identify_file(U8(games / "Mario Kart World" / "exefs" / "main"), &id) && id.nx2 && id.loose && id.title == "Mario Kart World");

    library_set_data_dir(U8(d / "userdata"));
    auto lib = scan_library({U8(games)}, nullptr);
    auto find = [&](const std::string& key) -> const GameEntry* { for (auto& g : lib) if (g.key == key) return &g; return nullptr; };
    int sw = 0;
    for (auto& g : lib) if (g.system == System::Switch) ++sw;
    CHECK(sw == 10);   // Super Game, Tagged Only, 2 xci, 2 nro, 2 loose dumps, 2 nup
    const GameEntry* mk = find("switch:0400abcd12340000");
    CHECK(mk && mk->system == System::Switch && mk->nx2 && mk->title == "Mario Kart World" && mk->serial == "0400ABCD12340000");
    CHECK(mk && mk->path.size() > 4 && mk->path.compare(mk->path.size() - 4, 4, "main") == 0);
    const GameEntry* od = find("switch:0100dddd00000000");
    CHECK(od && !od->nx2 && od->title == "Old Dump");
    const GameEntry* sg = find("switch:0100abcd12340000");
    CHECK(sg && sg->title == "Super Game" && !sg->nx2 && sg->serial == "0100ABCD12340000");
    CHECK(!find("switch:0100abcd12340800") && !find("switch:0100abcd12341001") && !find("switch:0100eeee00000800"));
    CHECK(find("switch:0100aaaa00000000") && find("switch:0100bbbb00000000") && find("switch:0100cccc00000000"));
    for (auto& g : lib) CHECK(g.path.find("Orphan") == std::string::npos && g.path.find("Other") == std::string::npos);
    const GameEntry* oot = find("switch:0400486024dec000");
    CHECK(oot && oot->nx2 && oot->title == "Ocarina of Time" && oot->serial == "0400486024DEC000");
    CHECK(oot && oot->path.size() > 4 && oot->path.compare(oot->path.size() - 4, 4, ".nup") == 0);
    const GameEntry* bare = find("switch:0400abab00000000");
    CHECK(bare && bare->title == "Bare" && bare->nx2);
    for (auto& g : lib) CHECK(g.path.find("Addon Pack") == std::string::npos && g.path.find("broken.nup") == std::string::npos);
    CHECK(nx::identify_file(U8(games / "Switch2" / "Zelda OoT [04004860224DEC000].nup"), &id) && id.format == "nup" && !id.loose);
    CHECK(nx::identify_file(U8(games / "broken.nup"), &id) == false);

    // cache round trip: a second scan reuses the cache and keeps nx2
    auto lib2 = scan_library({U8(games)}, nullptr);
    const GameEntry* mk2 = nullptr;
    for (auto& g : lib2) if (g.key == "switch:0400abcd12340000") mk2 = &g;
    CHECK(mk2 && mk2->nx2 && mk2->system == System::Switch);
    CHECK(lib2.size() == lib.size());

    // an older cache with no nx2 field still loads
    {
        std::ifstream f(d / "userdata" / "library_cache.json", std::ios::binary);
        std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        size_t at;
        while ((at = t.find(",\"nx2\":true")) != std::string::npos) t.erase(at, 11);
        std::ofstream o(d / "userdata" / "library_cache.json", std::ios::binary | std::ios::trunc);
        o << t;
    }
    auto lib3 = scan_library({U8(games)}, nullptr);
    CHECK(lib3.size() == lib.size());
    library_set_data_dir("");
    fs::remove_all(d, ec);
}

/* hub_nx_test --probe <file or folder>: read-only check of real dumps. A file is identified (header reads only); a folder is scanned with
 * scan_library into a throwaway data dir (the player's library cache is never touched). Prints what the library would list. */
static int probe(const std::string& target) {
    std::error_code ec;
    fs::path t = fs::u8path(target);
    if (fs::is_regular_file(t, ec)) {
        nx::Ident id;
        bool ok = nx::identify_file(target, &id);
        std::printf("identify_file: %s\n  format=%s title_id=%016llX nx2=%d loose=%d update_or_dlc=%d title=\"%s\"\n", ok ? "ok" : "NOT A SWITCH FILE",
                    id.format.c_str(), (unsigned long long)id.title_id, id.nx2, id.loose, id.update_or_dlc, id.title.c_str());
        return ok && !id.update_or_dlc ? 0 : 1;
    }
    fs::path data = fs::temp_directory_path() / "nx_probe_data";
    fs::remove_all(data, ec);
    library_set_data_dir(U8(data));
    auto lib = scan_library({target}, nullptr);
    int sw = 0;
    for (auto& g : lib) {
        if (g.system != System::Switch) continue;
        ++sw;
        std::printf("  %s | %s | nx2=%d | serial=%s | %s\n", g.key.c_str(), g.title.c_str(), g.nx2, g.serial.c_str(), g.path.c_str());
    }
    std::printf("scan_library: %d game(s), %d Switch\n", (int)lib.size(), sw);
    library_set_data_dir("");
    fs::remove_all(data, ec);
    return sw ? 0 : 1;
}

/* NUP icon extraction (meta TLV 3 / localized TLV 10): English wins, bounded, hostile sizes ignored, file front end. */
static Bytes icon_localized(const std::vector<std::pair<std::string, Bytes>>& v) {
    Bytes b;
    for (auto& li : v) {
        b.push_back((uint8_t)li.first.size());
        b.insert(b.end(), li.first.begin(), li.first.end());
        size_t at = b.size();
        le32(b, at, (uint32_t)li.second.size());
        b.insert(b.end(), li.second.begin(), li.second.end());
    }
    return b;
}
static bool nup_icon_of(const Bytes& b, Bytes* icon, nx::Ident* id) {
    *id = nx::Ident();
    return nx::parse_nup([&](uint64_t off, size_t len, Bytes* o) {
        if (off > b.size()) return false;
        size_t k = (size_t)std::min<uint64_t>(len, b.size() - off);
        o->assign(b.begin() + (size_t)off, b.begin() + (size_t)off + k);
        return true;
    }, b.size(), id, icon);
}
static void test_nup_icon() {
    nx::Ident id;
    Bytes icon;
    const Bytes jpg = {0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 4}, png = {0x89, 'P', 'N', 'G', 9, 9}, ja = {0xFF, 0xD8, 7, 7}, en = {0xFF, 0xD8, 8, 8, 8};
    // TLV 3 only
    Bytes b = make_nup({{0x0400AAAA00000000ull, 0, 0, 0, true, cat({tlv_s(5, "G"), tlv(3, jpg)})}});
    CHECK(nup_icon_of(b, &icon, &id) && icon == jpg && id.title == "G");
    // localized icons: English beats the others and beats TLV 3
    b = make_nup({{0x0400AAAA00000000ull, 0, 0, 0, true, cat({tlv(3, jpg), tlv(10, icon_localized({{"ja", ja}, {"en-US", en}}))})}});
    CHECK(nup_icon_of(b, &icon, &id) && icon == en);
    // no English entry: TLV 3 (the package default) before the first localized one
    b = make_nup({{0x0400AAAA00000000ull, 0, 0, 0, true, cat({tlv(10, icon_localized({{"ja", ja}})), tlv(3, png)})}});
    CHECK(nup_icon_of(b, &icon, &id) && icon == png);
    // only a Japanese icon: the first localized one
    b = make_nup({{0x0400AAAA00000000ull, 0, 0, 0, true, tlv(10, icon_localized({{"ja", ja}, {"fr", en}}))}});
    CHECK(nup_icon_of(b, &icon, &id) && icon == ja);
    // no icon at all
    b = make_nup({{0x0400AAAA00000000ull, 0, 0, 0, true, tlv_s(5, "G")}});
    CHECK(nup_icon_of(b, &icon, &id) && icon.empty());
    // hostile: an icon over the 4 MB bound is ignored (the title still parses), a localized record that overruns its TLV stops cleanly
    Bytes huge((4u << 20) + 1, 0xAB);
    b = make_nup({{0x0400AAAA00000000ull, 0, 0, 0, true, cat({tlv_s(5, "Big"), tlv(3, huge)})}});
    CHECK(nup_icon_of(b, &icon, &id) && icon.empty() && id.title == "Big");
    Bytes bad = {2, 'e', 'n', 0xFF, 0xFF, 0xFF, 0x7F, 1, 2};   // claims a 2 GB icon inside a 9 byte TLV
    b = make_nup({{0x0400AAAA00000000ull, 0, 0, 0, true, cat({tlv(10, bad), tlv_s(5, "Ok")})}});
    CHECK(nup_icon_of(b, &icon, &id) && icon.empty() && id.title == "Ok");
    Bytes cut = {5, 'e', 'n'};                               // language longer than the record
    b = make_nup({{0x0400AAAA00000000ull, 0, 0, 0, true, tlv(10, cut)}});
    CHECK(nup_icon_of(b, &icon, &id) && icon.empty());
    // a TLV whose size runs past the meta block: nothing read, no crash
    Bytes lie = tlv(3, jpg);
    lie[4] = 0xFF; lie[5] = 0xFF; lie[6] = 0xFF; lie[7] = 0x00;
    b = make_nup({{0x0400AAAA00000000ull, 0, 0, 0, true, lie}});
    CHECK(nup_icon_of(b, &icon, &id) && icon.empty());
    // not a NUP: icon cleared, false
    Bytes junk(256, 0x41);
    icon = jpg;
    CHECK(!nup_icon_of(junk, &icon, &id) && icon.empty());
    // file front end
    fs::path dir = fs::temp_directory_path() / "hub_nx_icon_test";
    fs::create_directories(dir);
    b = make_nup({{0x0400486024DEC000ull, 1, 1, 0x20000, true, cat({tlv(1, make_npdm(0x0400486024DEC000ull, true, "Application")), tlv(3, jpg)})}});
    fs::path f = dir / "game.nup";
    { std::ofstream o(f, std::ios::binary); o.write((const char*)b.data(), (std::streamsize)b.size()); }
    Bytes got;
    CHECK(nx::nup_icon_file(f.u8string(), &got) && got == jpg);
    Bytes none = make_nup({{0x0400486024DEC000ull, 1, 1, 0x20000, true, tlv_s(5, "x")}});
    { std::ofstream o(f, std::ios::binary | std::ios::trunc); o.write((const char*)none.data(), (std::streamsize)none.size()); }
    CHECK(!nx::nup_icon_file(f.u8string(), &got) && got.empty());
    CHECK(!nx::nup_icon_file((dir / "missing.nup").u8string(), &got));
    std::error_code ec;
    fs::remove_all(dir, ec);
}


/* ---- dump_check.h: incomplete / cut-short dumps ---- */
namespace dcx = hub::dumpcheck;
static dcx::ReadFn bytes_reader(const Bytes& b) {
    return [&b](uint64_t off, size_t len, std::vector<uint8_t>* o) {
        o->clear();
        if (off < b.size()) { size_t n = (size_t)std::min<uint64_t>(len, b.size() - off); o->assign(b.begin() + (long)off, b.begin() + (long)off + (long)n); }
        return true;
    };
}
static void test_dump_check() {
    const uint64_t kOot = 0x0400486024DEC000ull;
    dcx::Result r;
    CHECK(dcx::format_gb(25020741061ull) == "23.30 GB" && dcx::format_gb(25620000000ull) == "23.86 GB");
    CHECK(dcx::warning_text(dcx::Result{}).empty());

    // PFS0: header says two entries, 100 + 50 bytes of data
    Bytes p = make_pfs0({"a.nca", "b.nca"});
    le64(p, 16, 0); le64(p, 24, 100); le64(p, 16 + 0x18, 100); le64(p, 16 + 0x18 + 8, 50);
    const uint64_t hdr = p.size(), full = hdr + 150;
    CHECK(dcx::check_structure(bytes_reader(p), full, ".nsp").v == dcx::Verdict::Ok);
    CHECK(dcx::check_structure(bytes_reader(p), full + 7, ".nsz").v == dcx::Verdict::Ok);
    r = dcx::check_structure(bytes_reader(p), full - 1, ".nsp");
    CHECK(r.v == dcx::Verdict::Truncated && r.have == full - 1 && r.expected == full && !dcx::warning_text(r).empty());
    CHECK(dcx::warning_text(r).find("cut short") != std::string::npos && dcx::warning_text(r).find("re-dump your own copy") != std::string::npos);
    CHECK(dcx::warning_text(r).find("Water Temple") == std::string::npos && dcx::warning_text(r).find("Complete copies") == std::string::npos);
    CHECK(dcx::check_structure(bytes_reader(p), hdr, ".nsp").v == dcx::Verdict::Truncated);
    CHECK(dcx::check_structure(bytes_reader(p), full - 1, ".xcz").v == dcx::Verdict::Ok);   // compressed: can't tell
    CHECK(dcx::check_structure(bytes_reader(p), 3, ".txt").v == dcx::Verdict::Ok);

    // XCI: valid data ends at unit 0x1F -> 0x4000 bytes
    Bytes x = make_xci(0x100);
    le32(x, 0x118, 0x1F);
    CHECK(dcx::check_structure(bytes_reader(x), 0x4000, ".xci").v == dcx::Verdict::Ok);
    r = dcx::check_structure(bytes_reader(x), 0x3FFF, ".xci");
    CHECK(r.v == dcx::Verdict::Truncated && r.expected == 0x4000);
    le32(x, 0x118, 0);
    CHECK(dcx::check_structure(bytes_reader(x), 10, ".xci").v == dcx::Verdict::Ok);   // no size declared

    // NUP superblock
    Bytes n(0x80, 0);
    put(n, 0, std::string("NUP\x01", 4));
    le64(n, 0x40, 1000);
    CHECK(dcx::check_structure(bytes_reader(n), 1000, ".nup").v == dcx::Verdict::Ok);
    CHECK(dcx::check_structure(bytes_reader(n), 999, ".nup").v == dcx::Verdict::Truncated);
    // the owner's Ocarina of Time (Switch 2) file: header declares exactly its own size, so no warning (no "typical size" rule exists)
    Bytes owner = n; le64(owner, 0x40, 25020741061ull);
    CHECK(dcx::check_structure(bytes_reader(owner), 25020741061ull, ".nup").v == dcx::Verdict::Ok);
    CHECK(dcx::check_structure(bytes_reader(owner), 25020741062ull, ".nup").v == dcx::Verdict::Ok);   // longer than declared is not truncation
    r = dcx::check_structure(bytes_reader(owner), 25020741060ull, ".nup");
    CHECK(r.v == dcx::Verdict::Truncated && r.have == 25020741060ull && r.expected == 25020741061ull);
    // a PFS0 entry beyond the end of the file and an XCI end beyond the file are the other two proofs (above); a NUP with no data at all
    CHECK(dcx::check_structure(bytes_reader(n), 0, ".nup").v == dcx::Verdict::Truncated);

    // fuzz: short, random and mutated headers never crash and never claim a huge file is missing for nonsense
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    const char* exts[] = {".nsp", ".nsz", ".xci", ".xcz", ".nup", ""};
    for (int it = 0; it < 3000; ++it) {
        Bytes f(rnd() % 0x400);
        for (auto& c : f) c = (uint8_t)rnd();
        const int kind = (int)(rnd() % 4);
        if (kind == 0 && f.size() >= 16) put(f, 0, "PFS0");
        if (kind == 1 && f.size() >= 0x120) put(f, 0x100, "HEAD");
        if (kind == 2 && f.size() >= 0x80) put(f, 0, std::string("NUP\x01", 4));
        if (kind == 3) f = p, f.resize(f.size() - rnd() % 8);
        const uint64_t fsz = (rnd() & 1) ? f.size() : ((uint64_t)rnd() << 20);
        for (const char* e : exts) { dcx::Result q = dcx::check_structure(bytes_reader(f), fsz, e); CHECK(q.v == dcx::Verdict::Ok || q.expected > q.have); }
    }
    CHECK(dcx::check_structure(nullptr, 1, ".nsp").v == dcx::Verdict::Ok);
    dcx::ReadFn failing = [](uint64_t, size_t, std::vector<uint8_t>*) { return false; };
    CHECK(dcx::check_structure(failing, 100, ".nsp").v == dcx::Verdict::Ok && dcx::check_structure(failing, 100, ".nup").v == dcx::Verdict::Ok);

    // real files + cache
    std::error_code ec;
    fs::path d = fs::temp_directory_path() / "retrohub_dumpcheck_test";
    fs::remove_all(d, ec);
    Bytes cut = p; cut.resize(cut.size() + 100);   // 100 of 150 data bytes
    CHECK(write_file(d / "cut.nsp", cut));
    CHECK(dcx::check_file(U8(d / "cut.nsp"), 0).v == dcx::Verdict::Truncated);
    CHECK(dcx::check_file(U8(d / "missing.nsp"), 0).v == dcx::Verdict::Ok);
    CHECK(write_file(d / "small.nup", n));   // 128 bytes, declares 1000
    CHECK(dcx::check_file(U8(d / "small.nup"), kOot).v == dcx::Verdict::Truncated);
    Bytes okn = n; le64(okn, 0x40, 0x80);
    CHECK(write_file(d / "ok.nup", okn));
    CHECK(dcx::check_file(U8(d / "ok.nup"), kOot).v == dcx::Verdict::Ok);   // header declares exactly its own size: complete, however small
    CHECK(dcx::check_file(U8(d / "ok.nup"), 0).v == dcx::Verdict::Ok);

    dcx::Cache cache;
    dcx::Result cr;
    const std::string cp = U8(d / "cut.nsp");
    CHECK(!cache.get(cp, 0, &cr));   // first ask: not known yet, never blocks
    cache.wait_idle();
    CHECK(cache.get(cp, 0, &cr) && cr.v == dcx::Verdict::Truncated && cache.runs() == 1);
    cache.get(cp, 0, &cr);
    cache.wait_idle();
    CHECK(cache.runs() == 1);   // same path, size and time: no second read

    // a changed size, and separately a changed modified time, re-runs the check (recheck interval 0 = look every time)
    {
        dcx::Cache c2(0);
        dcx::Result q;
        const fs::path fp = d / "change.nsp";
        CHECK(write_file(fp, cut));
        const std::string fs8 = U8(fp);
        c2.get(fs8, 0, &q); c2.wait_idle();
        CHECK(c2.get(fs8, 0, &q) && c2.runs() == 1);   // first run done
        c2.wait_idle();
        CHECK(c2.runs() == 1);                          // nothing changed: still one
        Bytes bigger = cut; bigger.resize(bigger.size() + 10);
        CHECK(write_file(fp, bigger));                  // size change
        c2.get(fs8, 0, &q); c2.wait_idle();
        CHECK(c2.runs() == 2);
        std::error_code e2;
        fs::last_write_time(fp, fs::last_write_time(fp, e2) - std::chrono::hours(5), e2);   // time change only
        CHECK(!e2);
        c2.get(fs8, 0, &q); c2.wait_idle();
        CHECK(c2.runs() == 3);
    }

    fs::remove_all(d, ec);
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--probe") return probe(argv[2]);
    test_parsers();
    test_garbage();
    test_nup();
    test_nup_icon();
    test_dump_check();
    test_files_and_scan();
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("hub_nx_test: all passed\n");
    return 0;
}
