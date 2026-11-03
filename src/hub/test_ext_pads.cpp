// Console test: the pad configs of the EXTERNAL emulators for 1-4 fake pads.
//   PCSX2 (PS2: Pad1/Pad2 or the multitap's Pad1/Pad3/Pad4/Pad5), Dolphin (GCPad1-4 + SIDevice), Cemu (controller0-3.xml: GamePad then Pro
//   Controllers, XInput slot / SDL uuid), Azahar (player 1's guid + port), PPSSPP (device 20 + XInput slot).
// Every scenario renders the files from fake PadDesc lists (no SDL, no emulator) and asserts: each player maps to a distinct, correct device,
// the full button set is bound, and nothing is bound twice. Offline, no windows.
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "controls.h"
#include "emulators.h"
#include "logic.h"
#include "pcsx2.h"
#include "ppsspp.h"

namespace fs = std::filesystem;
using hubemu::PadDesc;

static int g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

static std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/* ---- fake pads ---- */
static PadDesc xb(int slot, const char* key = nullptr) {   // an Xbox pad on XInput slot `slot` (-1 = SDL gave no index)
    PadDesc p;
    p.name = "Xbox Controller";
    p.guid = "03000000x";
    p.key = key ? key : ("03000000x#" + std::to_string(slot));
    p.xinput = true;
    p.player = slot;
    return p;
}
static PadDesc ds(int player, const char* key) {   // a DualSense (SDL/HIDAPI, not XInput); `player` = the index SDL handed out (-1 none)
    PadDesc p;
    p.name = "DualSense Wireless Controller";
    p.guid = "030000d";
    p.key = key;
    p.xinput = false;
    p.player = player;
    return p;
}
static PadDesc bd(const char* key) {
    PadDesc p;
    p.name = "8BitDo Pro 2";
    p.guid = "030000b";
    p.key = key;
    return p;
}

typedef std::vector<PadDesc> Pads;

/* port order = the first n of `order` (indexes into `sdl`), the way list_pads(port_order) returns them */
static Pads by_port(const Pads& sdl, const std::vector<int>& order) {
    Pads out;
    for (int i : order) out.push_back(sdl[(size_t)i]);
    return out;
}

static std::string ini_get(const std::string& t, const std::string& sec, const std::string& key) { return hubui::ini_get(t, sec, key, ""); }

/* ------------------------------------------------------------------ PCSX2 */

static void check_ps2(const char* what, const Pads& sdl, const std::vector<int>& order, const std::vector<int>& expect_ids, int max_players = 4) {
    using namespace hubemu;
    const Pads port = by_port(sdl, order);
    const Ps2Pads pads = pcsx2_pads_with(port, sdl, max_players);
    const int n = (int)expect_ids.size();
    if (pads.count != n) std::printf("  [%s] ps2 count %d want %d\n", what, pads.count, n);
    CHECK(pads.count == n);
    CHECK(pads.multitap == (n > 2));
    for (int i = 0; i < n && i < pads.count; ++i) CHECK(pads.sdl[i] == expect_ids[(size_t)i]);
    const hubctl::Profile prof = hubctl::default_profile("ps2");
    const std::string ini = pcsx2_ini_pads("", pads, prof);
    CHECK(ini_get(ini, "Pad", "MultitapPort1") == (n > 2 ? "true" : "false"));
    CHECK(ini_get(ini, "Pad", "MultitapPort2") == "false");
    static const int kPlain[4] = {1, 2, 0, 0}, kTap[4] = {1, 3, 4, 5};
    std::set<std::string> seen_dev;
    bool active[9] = {};
    for (int i = 0; i < n; ++i) {
        const int sec_n = n > 2 ? kTap[i] : kPlain[i];
        active[sec_n] = true;
        const std::string sec = "Pad" + std::to_string(sec_n), dev = "SDL-" + std::to_string(expect_ids[(size_t)i]) + "/";
        CHECK(ini_get(ini, sec, "Type") == "DualShock2");
        CHECK(seen_dev.insert(dev).second);   // no SDL pad on two players
        for (const char* k : {"Up", "Right", "Down", "Left", "Triangle", "Circle", "Cross", "Square", "Select", "Start", "L1", "L2", "R1", "R2", "L3", "R3",
                              "LUp", "LRight", "LDown", "LLeft", "RUp", "RRight", "RDown", "RLeft", "LargeMotor", "SmallMotor"}) {
            const std::string v = ini_get(ini, sec, k);
            CHECK(!v.empty());
            CHECK(v.compare(0, dev.size(), dev) == 0);   // this player's own pad, nobody else's
        }
    }
    if (n == 0) active[1] = true;   // no pads: the keyboard sits on Pad1
    for (int s = 1; s <= 8; ++s) if (!active[s]) CHECK(ini_get(ini, "Pad" + std::to_string(s), "Type") == "None");
    if (n == 0) CHECK(ini_get(ini, "Pad1", "Cross") == "Keyboard/K" && ini_get(ini, "Pad2", "Type") == "None");
}

static void test_ps2() {
    using namespace hubemu;
    // SDL ids: an XInput pad is its user slot, the others take free numbers
    CHECK((pcsx2_sdl_ids({xb(0), xb(1)}) == std::vector<int>{0, 1}));
    CHECK((pcsx2_sdl_ids({ds(-1, "d0"), xb(0)}) == std::vector<int>{1, 0} || pcsx2_sdl_ids({ds(-1, "d0"), xb(0)}) == std::vector<int>{0, 1}));
    CHECK((pcsx2_sdl_ids({xb(2), xb(0)}) == std::vector<int>{2, 0}));          // not "position in the list"
    CHECK((pcsx2_sdl_ids({xb(-1), xb(-1, "b")}) == std::vector<int>{0, 1}));   // no indices: list order
    CHECK((pcsx2_sdl_ids({xb(1), xb(1, "again")}) == std::vector<int>{1, 0}));   // a taken index: lowest free
    CHECK((pcsx2_sdl_ids({ds(-1, "d0"), ds(-1, "d1")}) == std::vector<int>{0, 1}));
    {   // DualSense listed first without an index, then an Xbox pad on slot 0: the DualSense is opened first and takes 0, the Xbox pad's index is then taken -> 1
        std::vector<int> v = pcsx2_sdl_ids({ds(-1, "d0"), xb(0)});
        CHECK(v.size() == 2 && v[0] != v[1]);
    }

    // 1-4 players, all XInput in order
    Pads x4 = {xb(0), xb(1), xb(2), xb(3)};
    check_ps2("1 xinput", x4, {0}, {0});
    check_ps2("2 xinput", x4, {0, 1}, {0, 1});
    check_ps2("3 xinput multitap", x4, {0, 1, 2}, {0, 1, 2});
    check_ps2("4 xinput multitap", x4, {0, 1, 2, 3}, {0, 1, 2, 3});
    // port order differs from slots: player 1 = the pad on slot 3
    check_ps2("4 xinput reordered", x4, {3, 2, 1, 0}, {3, 2, 1, 0});
    // gaps in the slots: the pads are on slots 1 and 3 only
    Pads gap = {xb(1), xb(3)};
    check_ps2("2 xinput gaps", gap, {0, 1}, {1, 3});
    check_ps2("2 xinput gaps reversed", gap, {1, 0}, {3, 1});
    // two identical non-XInput pads (same GUID, keys differ): distinct ids
    Pads twin = {ds(-1, "030000d"), ds(-1, "030000d#dup2")};
    check_ps2("2 identical", twin, {0, 1}, {0, 1});
    check_ps2("2 identical swapped", twin, {1, 0}, {1, 0});
    // mix: DualSense (SDL), Xbox on slot 0, 8BitDo, Xbox on slot 1: SDL order is the enumeration order
    Pads mix = {xb(0), ds(-1, "d0"), bd("b0"), xb(1)};
    {
        const std::vector<int> ids = pcsx2_sdl_ids(mix);
        CHECK(ids[0] == 0 && ids[1] == 1 && ids[2] == 2 && ids[3] == 3);   // opened in SDL order: Xbox(slot 0)=0, DualSense takes the free 1, 8BitDo 2, and the Xbox pad on slot 1 finds its number taken -> 3 (as PCSX2 does)
        check_ps2("4 mixed", mix, {1, 0, 3, 2}, {ids[1], ids[0], ids[3], ids[2]});
        check_ps2("3 mixed", mix, {2, 3, 0}, {ids[2], ids[3], ids[0]});
        check_ps2("2 mixed", mix, {3, 1}, {ids[3], ids[1]});
    }
    // a 2-player game never gets the multitap, even with 4 pads connected
    check_ps2("2-player game, 4 pads", x4, {0, 1, 2, 3}, {0, 1}, 2);
    check_ps2("1-player game", x4, {2, 0}, {2}, 1);
    check_ps2("no pads", x4, {}, {});
    // the same pad is never bound twice, even if the port list repeats it
    {
        Ps2Pads dup = pcsx2_pads_with({x4[0], x4[0], x4[1]}, x4);
        CHECK(dup.count == 2 && dup.sdl[0] == 0 && dup.sdl[1] == 1);
    }
    // Gauntlet: Dark Legacy is a 4-player multitap game: 2, 3 and 4 pads through the real prepare_pcsx2 path
    for (int np = 2; np <= 4; ++np) {
        fs::path d = fs::temp_directory_path() / ("hub_ext_pads_ps2_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(np));
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        std::vector<int> order;
        for (int i = 0; i < np; ++i) order.push_back(i);
        Ps2Pads pads = pcsx2_pads_with(by_port(x4, order), x4, 4);
        std::string err = prepare_pcsx2(d.string(), pads, hubctl::default_profile("ps2"), Bios{"C:\\bios", "scph39001.bin"}, "ps2:slus-20047", hub::gs::defaults_for(hub::System::PS2));
        CHECK(err.empty());
        const std::string ini = read_text(d / "inis" / "PCSX2.ini");
        CHECK(ini_get(ini, "Pad", "MultitapPort1") == (np > 2 ? "true" : "false"));
        CHECK(ini_get(ini, "Pad1", "Cross") == "SDL-0/A");
        CHECK(ini_get(ini, np > 2 ? "Pad3" : "Pad2", "Cross") == "SDL-1/A");
        if (np >= 3) CHECK(ini_get(ini, "Pad4", "Cross") == "SDL-2/A" && ini_get(ini, "Pad2", "Type") == "None");
        if (np >= 4) CHECK(ini_get(ini, "Pad5", "Cross") == "SDL-3/A");
        // pads changed (4 -> 2): the file follows
        Ps2Pads two = pcsx2_pads_with(by_port(x4, {0, 1}), x4, 4);
        CHECK(prepare_pcsx2(d.string(), two, hubctl::default_profile("ps2"), Bios{"C:\\bios", "scph39001.bin"}, "ps2:slus-20047", hub::gs::defaults_for(hub::System::PS2)).empty());
        const std::string ini2 = read_text(d / "inis" / "PCSX2.ini");
        CHECK(ini_get(ini2, "Pad", "MultitapPort1") == "false" && ini_get(ini2, "Pad2", "Cross") == "SDL-1/A" && ini_get(ini2, "Pad3", "Type") == "None");
        fs::remove_all(d, ec);
    }
}

/* ------------------------------------------------------------------ Dolphin */

static void check_gc(const char* what, const Pads& sdl, const std::vector<int>& order, const std::vector<std::string>& expect_dev) {
    using namespace hubemu;
    const Pads port = by_port(sdl, order);
    DolphinPads base;
    // a connected XInput pad on every slot would, before the fix, switch ports on that had no player: the detection must not decide
    for (const PadDesc& p : sdl) if (p.xinput && p.player >= 0 && p.player < 4) base.xinput[p.player] = true;
    const DolphinPads pads = dolphin_pads_with(base, port, sdl);
    const hubctl::Profile prof = hubctl::default_profile("gc");
    const std::string pad_ini = dolphin_pad_ini("", pads, prof), ports_ini = dolphin_ini_ports("", pads);
    std::set<std::string> devs;
    for (int i = 0; i < 4; ++i) {
        const std::string sec = "GCPad" + std::to_string(i + 1);
        const std::string dev = ini_get(pad_ini, sec, "Device");
        const bool present = (size_t)i < expect_dev.size();
        if (present) {
            if (dev != expect_dev[(size_t)i]) std::printf("  [%s] GCPad%d device '%s' want '%s'\n", what, i + 1, dev.c_str(), expect_dev[(size_t)i].c_str());
            CHECK(dev == expect_dev[(size_t)i]);
            CHECK(devs.insert(dev).second);   // no device on two ports
            for (const char* k : {"Buttons/A", "Buttons/B", "Buttons/X", "Buttons/Y", "Buttons/Z", "Buttons/Start", "Main Stick/Up", "Main Stick/Down",
                                  "Main Stick/Left", "Main Stick/Right", "C-Stick/Up", "C-Stick/Down", "C-Stick/Left", "C-Stick/Right", "Triggers/L", "Triggers/R",
                                  "Triggers/L-Analog", "Triggers/R-Analog", "D-Pad/Up", "D-Pad/Down", "D-Pad/Left", "D-Pad/Right"})
                CHECK(!ini_get(pad_ini, sec, k).empty());
            const std::string a = ini_get(pad_ini, sec, "Buttons/A");
            CHECK(dev.compare(0, 4, "SDL/") == 0 ? a.find("Button S") != std::string::npos : a.find("Button A") != std::string::npos);   // the backend's own names
        }
        CHECK(ini_get(ports_ini, "Core", "SIDevice" + std::to_string(i)) == (present || (i == 0) ? "6" : "0"));   // port 1 always has the keyboard fallback
    }
    CHECK(ini_get(ports_ini, "Core", "SIDevice0") == "6");
}

static void test_gc() {
    using namespace hubemu;
    Pads x4 = {xb(0), xb(1), xb(2), xb(3)};
    check_gc("1", x4, {0}, {"XInput/0/Gamepad"});
    check_gc("2", x4, {0, 1}, {"XInput/0/Gamepad", "XInput/1/Gamepad"});
    check_gc("3", x4, {0, 1, 2}, {"XInput/0/Gamepad", "XInput/1/Gamepad", "XInput/2/Gamepad"});
    check_gc("4", x4, {0, 1, 2, 3}, {"XInput/0/Gamepad", "XInput/1/Gamepad", "XInput/2/Gamepad", "XInput/3/Gamepad"});
    check_gc("4 reordered", x4, {3, 2, 1, 0}, {"XInput/3/Gamepad", "XInput/2/Gamepad", "XInput/1/Gamepad", "XInput/0/Gamepad"});
    // pads on slots 1 and 3 only: player 1 = XInput/1, player 2 = XInput/3 (before: XInput/0 and XInput/1 = dead ports), and ports 3-4 stay empty
    Pads gap = {xb(1), xb(3)};
    check_gc("gaps", gap, {0, 1}, {"XInput/1/Gamepad", "XInput/3/Gamepad"});
    // slots 0 and 2 connected, two players: port 3 must not turn on and share slot 2's pad with port 2
    Pads s02 = {xb(0), xb(2)};
    check_gc("0+2", s02, {0, 1}, {"XInput/0/Gamepad", "XInput/2/Gamepad"});
    // SDL pads: Dolphin numbers same-named devices in SDL order; two identical DualSense get /0 and /1
    Pads twin = {ds(0, "030000d"), ds(1, "030000d#dup2")};
    check_gc("2 identical sdl", twin, {0, 1}, {"SDL/0/DualSense Wireless Controller", "SDL/1/DualSense Wireless Controller"});
    check_gc("2 identical sdl swapped", twin, {1, 0}, {"SDL/1/DualSense Wireless Controller", "SDL/0/DualSense Wireless Controller"});
    // mixed: DualSense, Xbox on slot 0, 8BitDo, Xbox on slot 1
    Pads mix = {xb(0), ds(-1, "d0"), bd("b0"), xb(1)};
    check_gc("4 mixed", mix, {1, 0, 2, 3}, {"SDL/0/DualSense Wireless Controller", "XInput/0/Gamepad", "SDL/0/8BitDo Pro 2", "XInput/1/Gamepad"});
    check_gc("3 mixed", mix, {3, 2, 0}, {"XInput/1/Gamepad", "SDL/0/8BitDo Pro 2", "XInput/0/Gamepad"});
    // a GameCube adapter: all four ports belong to it
    {
        DolphinPads b;
        b.adapter = true;
        std::string p = dolphin_ini_ports("", dolphin_pads_with(b, by_port(x4, {0, 1}), x4));
        for (int i = 0; i < 4; ++i) CHECK(ini_get(p, "Core", "SIDevice" + std::to_string(i)) == "12");
    }
    // the pad file is rewritten when the assignment changes
    {
        fs::path d = fs::temp_directory_path() / ("hub_ext_pads_gc_" + std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        const hubctl::Profile prof = hubctl::default_profile("gc");
        prepare_dolphin(d.string(), dolphin_pads_with(DolphinPads(), by_port(gap, {0, 1}), gap), prof);
        CHECK(ini_get(read_text(d / "User" / "Config" / "GCPadNew.ini"), "GCPad1", "Device") == "XInput/1/Gamepad");
        prepare_dolphin(d.string(), dolphin_pads_with(DolphinPads(), by_port(x4, {0, 1}), x4), prof);
        CHECK(ini_get(read_text(d / "User" / "Config" / "GCPadNew.ini"), "GCPad1", "Device") == "XInput/0/Gamepad");
        fs::remove_all(d, ec);
    }
}

/* ------------------------------------------------------------------ Cemu */

static std::string xml_tag(const std::string& x, const std::string& tag, size_t from = 0, size_t* end = nullptr) {
    size_t a = x.find("<" + tag + ">", from);
    if (a == std::string::npos) return "";
    a += tag.size() + 2;
    size_t b = x.find("</" + tag + ">", a);
    if (end) *end = b;
    return b == std::string::npos ? "" : x.substr(a, b - a);
}

/* mapping id -> button id of every <entry> */
static std::vector<std::pair<int, int>> xml_entries(const std::string& x) {
    std::vector<std::pair<int, int>> v;
    size_t at = 0;
    for (;;) {
        size_t e = x.find("<entry>", at);
        if (e == std::string::npos) break;
        size_t endpos = 0;
        std::string m = xml_tag(x, "mapping", e, &endpos);
        std::string b = xml_tag(x, "button", endpos);
        v.push_back({std::atoi(m.c_str()), std::atoi(b.c_str())});
        at = x.find("</entry>", e) + 8;
    }
    return v;
}

struct CemuWant { const char* api; std::string uuid; };

static void check_cemu(const char* what, const Pads& sdl, const std::vector<int>& order, const std::vector<CemuWant>& want) {
    using namespace hubemu;
    const Pads port = by_port(sdl, order);
    const CemuPads cp = cemu_pads_with_sdl(port, sdl);
    const hubctl::Profile prof = hubctl::default_profile("wiiu");
    fs::path d = fs::temp_directory_path() / ("hub_ext_pads_cemu_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(d, ec);
    prepare_cemu_managed(d.string(), prof, cp);
    std::set<std::string> ids;
    const size_t n = want.size() > 4 ? 4 : want.size();
    for (size_t i = 0; i < 4; ++i) {
        const fs::path f = d / "portable" / "controllerProfiles" / ("controller" + std::to_string(i) + ".xml");
        if (i >= n && i > 0) { CHECK(!fs::exists(f)); continue; }
        const std::string x = read_text(f);
        if (i >= n) { CHECK(!x.empty()); continue; }   // controller0.xml always exists
        CHECK(xml_tag(x, "type") == (i == 0 ? "Wii U GamePad" : "Wii U Pro Controller"));
        if (xml_tag(x, "api") != want[i].api || xml_tag(x, "uuid") != want[i].uuid)
            std::printf("  [%s] player %zu api=%s uuid=%s want %s %s\n", what, i + 1, xml_tag(x, "api").c_str(), xml_tag(x, "uuid").c_str(), want[i].api, want[i].uuid.c_str());
        CHECK(xml_tag(x, "api") == want[i].api && xml_tag(x, "uuid") == want[i].uuid);
        CHECK(ids.insert(xml_tag(x, "api") + "/" + xml_tag(x, "uuid")).second);   // no device on two players
        // the full button set, ids valid for this controller type, none twice
        std::vector<std::pair<int, int>> en = xml_entries(x);
        CHECK(en.size() == 24);
        std::set<int> maps;
        for (auto& e : en) {
            CHECK(maps.insert(e.first).second);
            if (i == 0) CHECK(e.first >= 1 && e.first <= 24);                                   // GamePad: A..Minus, d-pad, sticks, then Mic/Screen/Home
            else CHECK(e.first >= 1 && e.first <= 25 && e.first != 11);                         // Pro: Home sits at 11
            CHECK(e.second >= 0 && e.second <= 47);
        }
        if (i > 0) CHECK(maps.count(12) && maps.count(25) && !maps.count(11));   // d-pad up .. right stick right
        else CHECK(maps.count(11) && maps.count(24) && !maps.count(25));
    }
    fs::remove_all(d, ec);
}

static void test_cemu() {
    using namespace hubemu;
    {   // no stored port order: the XInput pads first in slot order, then the others; player 1 stays on XInput slot 0 for an Xbox + DualSense/8BitDo user
        Pads sdl_first_ds = {ds(-1, "d0"), bd("b0"), xb(1), xb(0)};   // SDL lists the DualSense first
        Pads ord = default_player_order(sdl_first_ds);
        CHECK(ord.size() == 4 && ord[0].player == 0 && ord[1].player == 1 && ord[2].key == "d0" && ord[3].key == "b0");
        CemuPads cp = cemu_pads_with_sdl(ord, sdl_first_ds);
        CHECK(cp.xinput.size() == 4 && cp.xinput[0] == 0 && cp.xinput[1] == 1 && cp.xinput[2] == -1 && cp.xinput[3] == -1 && cp.sdl[2] == "0_030000d" && cp.sdl[3] == "0_030000b");
        // only a DualSense + one Xbox pad: Xbox is the GamePad on slot 0
        Pads two = {ds(0, "d0"), xb(0)};
        CemuPads c2 = cemu_pads_with_sdl(default_player_order(two), two);
        CHECK(c2.xinput.size() == 2 && c2.xinput[0] == 0 && c2.sdl[0].empty() && c2.xinput[1] == -1 && c2.sdl[1] == "0_030000d");
        // no XInput pad at all: SDL order is kept
        Pads sd = {bd("b0"), ds(-1, "d0")};
        CHECK(default_player_order(sd)[0].key == "b0" && default_player_order({}).empty());
    }
    Pads x4 = {xb(0), xb(1), xb(2), xb(3)};
    check_cemu("1", x4, {0}, {{"XInput", "0"}});
    check_cemu("2", x4, {0, 1}, {{"XInput", "0"}, {"XInput", "1"}});
    check_cemu("3", x4, {0, 1, 2}, {{"XInput", "0"}, {"XInput", "1"}, {"XInput", "2"}});
    check_cemu("4", x4, {0, 1, 2, 3}, {{"XInput", "0"}, {"XInput", "1"}, {"XInput", "2"}, {"XInput", "3"}});
    // port order differs from the slots: player 1 = slot 3 (before: the N-th XInput pad was taken as slot N-1, so the order was ignored)
    check_cemu("4 reordered", x4, {3, 2, 1, 0}, {{"XInput", "3"}, {"XInput", "2"}, {"XInput", "1"}, {"XInput", "0"}});
    // pads on slots 1 and 3 only
    Pads gap = {xb(1), xb(3)};
    check_cemu("gaps", gap, {0, 1}, {{"XInput", "1"}, {"XInput", "3"}});
    check_cemu("gaps swapped", gap, {1, 0}, {{"XInput", "3"}, {"XInput", "1"}});
    // no SDL index at all: the XInput pads are numbered in list order
    Pads noidx = {xb(-1, "a"), xb(-1, "b")};
    check_cemu("no index", noidx, {0, 1}, {{"XInput", "0"}, {"XInput", "1"}});
    // two identical DualSense: Cemu's uuid is "<index among same GUID>_<guid>"
    Pads twin = {ds(0, "030000d"), ds(1, "030000d#dup2")};
    check_cemu("2 identical sdl", twin, {0, 1}, {{"SDLController", "0_030000d"}, {"SDLController", "1_030000d"}});
    check_cemu("2 identical sdl swapped", twin, {1, 0}, {{"SDLController", "1_030000d"}, {"SDLController", "0_030000d"}});
    // mixed XInput + SDL + XInput + SDL
    Pads mix = {xb(0), ds(-1, "d0"), xb(1), bd("b0")};
    check_cemu("4 mixed", mix, {1, 0, 3, 2}, {{"SDLController", "0_030000d"}, {"XInput", "0"}, {"SDLController", "0_030000b"}, {"XInput", "1"}});
    // the GamePad (player 1) and the Pro Controllers map the same physical buttons: Wii U A must be on the pad's A (XInput 12) for both
    {
        const hubctl::Profile prof = hubctl::default_profile("wiiu");
        auto find_btn = [](const std::vector<std::pair<int, int>>& v, int m) { for (auto& e : v) if (e.first == m) return e.second; return -1; };
        auto gp = xml_entries(cemu_gamepad_profile_xml(prof, 0)), pro = xml_entries(cemu_pro_profile_xml(prof, 1, 1));
        CHECK(find_btn(gp, 1) == 13 && find_btn(pro, 1) == 13);            // Wii U A = the Xbox B button (east), same on both
        CHECK(find_btn(gp, 11) == find_btn(pro, 12));                      // d-pad up
        CHECK(find_btn(gp, 15) == find_btn(pro, 16));                      // left stick click
        CHECK(find_btn(gp, 24) == find_btn(pro, 25));                      // right stick right
        auto sp = xml_entries(cemu_pro_profile_xml_sdl(prof, "0_g", 2));
        CHECK(find_btn(sp, 1) == 1 && find_btn(sp, 12) == 11);             // SDL: Wii U A = SDL B (button 1), d-pad up is SDL button 11
        auto dsu = xml_entries(cemu_pro_profile_xml_dsu(prof, 0, 1));
        CHECK(dsu.size() == 24 && find_btn(dsu, 12) == 4);                 // DSU d-pad up bit
    }
}

/* ------------------------------------------------------------------ Azahar + PPSSPP */

static void test_single_pad_emulators() {
    using namespace hubemu;
    // Azahar follows player 1's pad: guid + the pad's rank among the same GUID
    Pads twin = {ds(0, "030000d"), ds(1, "030000d#dup2"), xb(0)};
    AzPad az;
    CHECK(azahar_pad_select(by_port(twin, {0, 1, 2}), twin, &az) && az.guid == "030000d" && az.port == 0);
    CHECK(azahar_pad_select(by_port(twin, {1, 0, 2}), twin, &az) && az.guid == "030000d" && az.port == 1);
    CHECK(azahar_pad_select(by_port(twin, {2, 0, 1}), twin, &az) && az.guid == "03000000x" && az.port == 0);
    CHECK(!azahar_pad_select({}, twin, &az) && az.guid.empty());
    const hubctl::Profile p3 = hubctl::default_profile("3ds");
    std::string q = azahar_config_patch("", p3, az);
    CHECK(q.find("maptype:all") != std::string::npos);   // no pad chosen = any pad
    AzPad pick;
    azahar_pad_select(by_port(twin, {1, 0, 2}), twin, &pick);
    q = azahar_config_patch("", p3, pick);
    for (const char* b : {"a", "b", "x", "y", "up", "down", "left", "right", "l", "r", "start", "select", "zl", "zr"})
        CHECK(ini_get(q, "Controls", std::string("button_") + b).find("guid:030000d,port:1") != std::string::npos);
    CHECK(ini_get(q, "Controls", "circle_pad").find("guid:030000d,port:1") != std::string::npos && ini_get(q, "Controls", "c_stick").find("port:1") != std::string::npos);

    // PPSSPP: one pad, addressed by the XInput slot (device 20 + slot)
    const hubctl::Profile psp = hubctl::default_profile("psp");
    for (int slot : {0, 1, 2, 3}) {
        std::string c = ppsspp_controls_ini(psp, slot);
        CHECK(c.find("Cross = " + std::to_string(20 + slot) + "-") != std::string::npos);
        CHECK(c.find("Cross = " + std::to_string(20 + (slot + 1) % 4) + "-") == std::string::npos);
    }
    // the slot comes from the XInput pads of the port order, not their rank
    Pads gap = {xb(1), xb(3)};
    CHECK((cemu_pads_with(by_port(gap, {1, 0})).xinput == std::vector<int>{3, 1}));
    CHECK((cemu_pads_with(by_port(gap, {0, 1})).xinput == std::vector<int>{1, 3}));
    Pads mixed = {ds(-1, "d0"), xb(2)};
    CHECK((cemu_pads_with(by_port(mixed, {0, 1})).xinput == std::vector<int>{2}));   // PPSSPP: first XInput pad
    CHECK(cemu_pads_with(by_port(Pads{ds(-1, "d0")}, {0})).xinput.empty());           // no XInput pad: caller falls back to slot 0
}

int main() {
    std::printf("ps2\n"); test_ps2();
    std::printf("gc\n"); test_gc();
    std::printf("cemu\n"); test_cemu();
    std::printf("azahar+ppsspp\n"); test_single_pad_emulators();
    std::printf("hub_ext_pads_test: %d check(s) failed\n", g_fail);
    return g_fail ? 1 : 0;
}
