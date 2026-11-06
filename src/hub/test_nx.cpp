// Console test for Nintendo Switch / Switch 2 identification (src/common/nx.h): PFS0 / XCI / NRO / NPDM parsers on synthetic headers, file front
// ends, loose exefs dumps through scan_library, the cache round trip (nx2) and that other systems are untouched. No network, no emulator, no keys.
#include <windows.h>
#include "library.h"
#include "nx.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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
    write_file(games / "Real Cart.gba", Bytes(0x200, 0));                    // a non-game GBA stays unlisted

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
    CHECK(sw == 8);   // Super Game, Tagged Only, 2 xci, 2 nro, 2 loose dumps
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

int main() {
    test_parsers();
    test_garbage();
    test_files_and_scan();
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("hub_nx_test: all passed\n");
    return 0;
}
