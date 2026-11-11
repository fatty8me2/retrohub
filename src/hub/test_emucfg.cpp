// Console test for the emulator config writers (no emulator, temp dirs only):
//  1. every writer commits durably: bytes identical to the old truncate-and-write helper, unchanged files are not rewritten, a failed
//     write leaves the old file untouched and no temp file behind
//  2. managed Cemu "graphic packs off" parks the player's block outside settings.xml and restores it byte for byte
//  4. clamp_analog table, PCSX2 deadzone = larger of the two sticks, Lichii range clamp unchanged
//  5. PPSSPP controls.ini is merged, not replaced
#include <windows.h>
#include "cfg_write.h"
#include "controls.h"
#include "emulators.h"
#include "lichii.h"
#include "logic.h"
#include "pcsx2.h"
#include "ppsspp.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void put(const fs::path& p, const std::string& t) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(t.data(), (std::streamsize)t.size());
}
static std::string u8(const fs::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }
/* the helper every writer used before this change */
static bool old_write(const fs::path& p, const std::string& t) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(t.data(), (std::streamsize)t.size());
    return (bool)f;
}
static bool has_tmp(const fs::path& dir) {
    for (auto& e : fs::directory_iterator(dir)) if (e.path().extension() == ".hubtmp") return true;
    return false;
}

static void test_durable(const fs::path& root) {
    const std::string samples[] = {"", "[A]\nk = v\n", "[A]\r\nk = v\r\n", std::string("\xEF\xBB\xBF[A]\r\nk=v\r\n"), std::string("a\0b\n", 4),
                                   "caf\xC3\xA9 = \xE2\x82\xAC\n", std::string(300000, 'x')};
    int n = 0;
    for (const auto& s : samples) {
        std::string name = "f" + std::to_string(n++) + ".ini";
        fs::path a = root / "old" / name, b = root / fs::u8path("d\xC3\xA9j\xC3\xA0 vu") / "sub" / name;   // non-ASCII + missing folders
        CHECK(old_write(a, s));
        CHECK(hubcfg::write_utf8(u8(b), s));
        CHECK(slurp(a) == slurp(b) && slurp(b) == s);
        CHECK(!has_tmp(b.parent_path()));
        put(b, "previous");
        CHECK(hubcfg::write_utf8(u8(b), s) && slurp(b) == s);   // overwrite
    }
    // unchanged content is not rewritten (write time stays)
    fs::path p = root / "same.ini";
    CHECK(hubcfg::write_file(p, "[X]\na = 1\n"));
    FILETIME before = {}, after = {};
    auto wt = [&](FILETIME* ft) {
        HANDLE h = CreateFileW(p.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) { GetFileTime(h, nullptr, nullptr, ft); CloseHandle(h); }
    };
    wt(&before);
    Sleep(50);
    CHECK(hubcfg::write_file(p, "[X]\na = 1\n"));
    wt(&after);
    CHECK(before.dwLowDateTime == after.dwLowDateTime && before.dwHighDateTime == after.dwHighDateTime);
    CHECK(hubcfg::write_file(p, "[X]\na = 2\n") && slurp(p) == "[X]\na = 2\n");
    // a failed write (destination held open without sharing delete) leaves the old file and no temp file
    fs::path q = root / "held.ini";
    put(q, "KEEP");
    HANDLE h = CreateFileW(q.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(!hubcfg::write_file(q, "NEW"));
    CloseHandle(h);
    CHECK(slurp(q) == "KEEP" && !has_tmp(root));
    // controls.json / profile files go through the same path
    hubctl::Profile pr = hubctl::default_profile("gc"), back;
    CHECK(hubctl::write_profile_file(u8(root / "prof" / "p.json"), pr));
    CHECK(hubctl::read_profile_file(u8(root / "prof" / "p.json"), &back) && back.buttons == pr.buttons);
    CHECK(!has_tmp(root / "prof"));
}

static const char* kBlock = "<GraphicPack>\n\t\t<Entry filename=\"graphicPacks/a/rules.txt\"/>\n\t\t<Entry filename=\"graphicPacks/b/rules.txt\"/>\n\t</GraphicPack>";

static std::string cemu_xml(const std::string& gp) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<content>\n\t<fullscreen>false</fullscreen>\n\t<unknown_hub_thing>x</unknown_hub_thing>\n\t" + gp + "\n</content>\n";
}
/* what Cemu writes on exit: its own form, elements it does not know are gone */
static std::string cemu_rewrite(const std::string& in, bool keep_placeholder) {
    std::string t = in;
    size_t a = t.find("\t<unknown_hub_thing>");
    size_t b = t.find('\n', a);
    t.erase(a, b - a + 1);
    if (!keep_placeholder) {
        size_t c = t.find("\t<GraphicPack/>");
        if (c != std::string::npos) t.erase(c, t.find('\n', c) - c + 1);
    }
    return t;
}
static std::string replace_all(std::string s, const std::string& a, const std::string& b) {
    for (size_t p = 0; (p = s.find(a, p)) != std::string::npos; p += b.size()) s.replace(p, a.size(), b);
    return s;
}

static void test_cemu_gp(const fs::path& root) {
    hub::gs::Values off, on;
    off["wiiu.graphic_packs"] = "false";
    on["wiiu.graphic_packs"] = "true";
    const std::string orig = cemu_xml(kBlock);
    const fs::path dir = root / "cemu", sx = dir / "settings.xml", sf = dir / "retrohub-cemu-gp-state.txt";
    for (int keep = 1; keep >= 0; --keep) {
        fs::remove_all(dir);
        put(sx, orig);
        CHECK(hubemu::apply_cemu_managed_settings(u8(dir), off));
        std::string o1 = slurp(sx);
        CHECK(o1.find("<GraphicPack>") == std::string::npos && o1.find("<GraphicPack/>") != std::string::npos);
        CHECK(fs::exists(sf));
        CHECK(hubemu::apply_cemu_managed_settings(u8(dir), off));   // idempotent: the parked original survives a second off
        CHECK(slurp(sx) == o1 && fs::exists(sf));
        put(sx, cemu_rewrite(o1, keep != 0));   // Cemu exits and rewrites settings.xml
        CHECK(hubemu::apply_cemu_managed_settings(u8(dir), on));
        std::string r = slurp(sx);
        CHECK(r.find(kBlock) != std::string::npos);   // the original block, byte for byte
        CHECK(r.find("<GraphicPack>") == r.rfind("<GraphicPack>") && r.find("<GraphicPack/>") == std::string::npos);
        CHECK(!fs::exists(sf));
        if (keep) CHECK(r == orig.substr(0, orig.find("\t<unknown_hub_thing>")) + orig.substr(orig.find("\t<GraphicPack>")));
        CHECK(hubemu::apply_cemu_managed_settings(u8(dir), on));   // on again: nothing changes
        CHECK(slurp(sx) == r);
    }
    // state file exists and settings.xml already has the block: no duplicate, state dropped
    fs::remove_all(dir);
    put(sx, orig);
    CHECK(hubemu::apply_cemu_managed_settings(u8(dir), off));
    put(sx, orig);
    CHECK(hubemu::apply_cemu_managed_settings(u8(dir), on));
    std::string r = slurp(sx);
    CHECK(r.find("<GraphicPack>") == r.rfind("<GraphicPack>") && r.find(kBlock) != std::string::npos && !fs::exists(sf));
    // packs on with nothing parked: the file is as before
    fs::remove_all(dir);
    put(sx, orig);
    CHECK(hubemu::apply_cemu_managed_settings(u8(dir), on) && slurp(sx) == orig);
    // legacy file from an older build (list renamed inside settings.xml) comes back on
    put(sx, replace_all(replace_all(orig, "<GraphicPack>", "<GraphicPack_off_by_retrohub>"), "</GraphicPack>", "</GraphicPack_off_by_retrohub>"));
    CHECK(hubemu::apply_cemu_managed_settings(u8(dir), on) && slurp(sx) == orig);
    // empty block (nothing enabled): nothing is parked
    fs::remove_all(dir);
    put(sx, cemu_xml("<GraphicPack/>"));
    CHECK(hubemu::apply_cemu_managed_settings(u8(dir), off) && !fs::exists(sf) && slurp(sx) == cemu_xml("<GraphicPack/>"));
    // pure function: not a Cemu file, restore cannot be placed -> block stays parked
    hubemu::CemuOwnState st;
    st.gp_stripped = true;
    st.gp_block = kBlock;
    CHECK(hubemu::cemu_managed_gp_apply("garbage", true, &st) == "garbage" && st.gp_stripped);
}

static void test_clamp() {
    using hubctl::AnalogTarget;
    struct Row { AnalogTarget t; double dz, sens, edz, esens; };
    const Row rows[] = {
        {AnalogTarget::N64Run, 0.08, 1, 0.08, 1}, {AnalogTarget::N64Run, -1, 0.01, 0, 0.1}, {AnalogTarget::N64Run, 2, 9, 0.95, 4},
        {AnalogTarget::Lichii, 0.15, 1, 0.15, 1}, {AnalogTarget::Lichii, 0, 0.1, 0, 0.25}, {AnalogTarget::Lichii, 0.99, 4, 0.95, 1.5},
        {AnalogTarget::Dolphin, 0.3, 3, 0.3, 1}, {AnalogTarget::Cemu, 0.25, 2, 0.25, 1}, {AnalogTarget::Azahar, 5, 2, 0.95, 1},
        {AnalogTarget::Pcsx2, 0.2, 0.5, 0.2, 1}, {AnalogTarget::Pcsx2, -0.5, 1, 0, 1},
    };
    for (const auto& r : rows) {
        auto o = hubctl::clamp_analog(r.t, r.dz, r.sens);
        CHECK(o.deadzone == r.edz && o.sensitivity == r.esens);
    }
    // NaN / inf behave exactly like the std::min(hi, std::max(lo, x)) the call sites used: NaN gives the lower bound
    const double nan = std::nan(""), inf = HUGE_VAL;
    auto o = hubctl::clamp_analog(AnalogTarget::Lichii, nan, nan);
    CHECK(o.deadzone == 0.0 && o.sensitivity == 0.25);
    o = hubctl::clamp_analog(AnalogTarget::Lichii, inf, inf);
    CHECK(o.deadzone == 0.95 && o.sensitivity == 1.5);
    o = hubctl::clamp_analog(AnalogTarget::Lichii, -inf, -inf);
    CHECK(o.deadzone == 0.0 && o.sensitivity == 0.25);
    o = hubctl::clamp_analog(AnalogTarget::N64Run, nan, nan);
    CHECK(o.deadzone == 0.0 && o.sensitivity == 0.1);
    o = hubctl::clamp_analog(AnalogTarget::Dolphin, nan, inf);
    CHECK(o.deadzone == 0.0 && o.sensitivity == 1.0);
}

static void test_deadzone() {
    using namespace hubemu;
    hubctl::Profile prof = hubctl::default_profile("ps2");
    Ps2Pads p1;
    p1.sdl[0] = 0;
    p1.count = 1;
    // default profile: 0 / 0 = PCSX2's own default, nothing written (unchanged)
    CHECK(hubui::ini_get(pcsx2_ini_pads("", p1, prof), "Pad1", "Deadzone", "none") == "none");
    prof.analogs["left_stick"].deadzone = 0.2;
    CHECK(hubui::ini_get(pcsx2_ini_pads("", p1, prof), "Pad1", "Deadzone", "") == "0.2");
    prof.analogs["right_stick"].deadzone = 0.35;   // the larger one wins
    CHECK(hubui::ini_get(pcsx2_ini_pads("", p1, prof), "Pad1", "Deadzone", "") == "0.35");
    prof.analogs["left_stick"].deadzone = 0.5;
    CHECK(hubui::ini_get(pcsx2_ini_pads("", p1, prof), "Pad1", "Deadzone", "") == "0.5");
    prof.analogs["left_stick"].deadzone = 0;
    prof.analogs["right_stick"].deadzone = 0.1;   // the right stick slider alone now has an effect
    CHECK(hubui::ini_get(pcsx2_ini_pads("", p1, prof), "Pad1", "Deadzone", "") == "0.1");
    // Lichii: range clamped to 0.25..1.5, deadzone text unchanged
    hubctl::Profile sw = hubctl::default_profile("switch");
    sw.analogs["left_stick"].deadzone = 0.3;
    sw.analogs["left_stick"].sensitivity = 4;
    std::string c = lichii_controls_ini("", sw, "00000000000000000000000000000000", 0, nullptr);
    CHECK(c.find("deadzone:0.3,range:1.5") != std::string::npos);
}

static void test_ppsspp(const fs::path& root) {
    using namespace hubemu;
    hubctl::Profile prof = hubctl::default_profile("psp");
    const std::string gen = ppsspp_controls_ini(prof, 0);
    CHECK(ppsspp_controls_merge("", prof, 0) == gen);
    CHECK(ppsspp_controls_merge("  \r\n", prof, 0) == gen);
    CHECK(ppsspp_controls_merge(gen, prof, 0) == gen);   // a file RetroHub wrote earlier: same bytes
    std::string user = "# my binds\n[ControlMapping]\nCross = 10-190\nVol Up = 1-24\nCustom Thing = 1-99\n[Other]\nkeep = me\n";
    std::string m = ppsspp_controls_merge(user, prof, 0);
    CHECK(m.find("# my binds\n") == 0 && hubui::ini_get(m, "ControlMapping", "Vol Up", "") == "1-24" && hubui::ini_get(m, "ControlMapping", "Custom Thing", "") == "1-99");
    CHECK(hubui::ini_get(m, "Other", "keep", "") == "me");
    CHECK(hubui::ini_get(m, "ControlMapping", "Cross", "") == hubui::ini_get(gen, "ControlMapping", "Cross", "x"));
    CHECK(hubui::ini_get(m, "ControlMapping", "An.Up", "") == hubui::ini_get(gen, "ControlMapping", "An.Up", "x"));
    CHECK(ppsspp_controls_merge(m, prof, 0) == m);   // idempotent
    std::string m2 = ppsspp_controls_merge(m, prof, 2);   // another slot: only managed keys change
    CHECK(hubui::ini_get(m2, "ControlMapping", "Cross", "") == hubui::ini_get(ppsspp_controls_ini(prof, 2), "ControlMapping", "Cross", "y"));
    CHECK(hubui::ini_get(m2, "ControlMapping", "Custom Thing", "") == "1-99");
    // prepare_ppsspp end to end
    fs::path inst = root / "psp";
    hub::gs::Values v;
    CHECK(prepare_ppsspp(u8(inst), prof, 0, v).empty());
    fs::path ci = inst / "memstick" / "PSP" / "SYSTEM" / "controls.ini";
    CHECK(slurp(ci) == gen);
    put(ci, user);
    CHECK(prepare_ppsspp(u8(inst), prof, 0, v).empty());
    CHECK(slurp(ci) == m && slurp(ci).find("Custom Thing = 1-99") != std::string::npos);
    CHECK(!has_tmp(ci.parent_path()));
}

static int count_files(const fs::path& dir, const std::string& prefix) {
    int n = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) if (e.path().filename().string().compare(0, prefix.size(), prefix) == 0) ++n;
    return n;
}
static std::string first_file(const fs::path& dir, const std::string& prefix) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) if (e.path().filename().string().compare(0, prefix.size(), prefix) == 0) return slurp(e.path());
    return "";
}

static void test_readonly(const fs::path& root) {
    fs::path p = root / "ro.ini";
    put(p, "KEEP\n");
    SetFileAttributesW(p.wstring().c_str(), FILE_ATTRIBUTE_READONLY);
    CHECK(!old_write(p, "NEW\n"));            // what the old helper did
    CHECK(!hubcfg::write_file(p, "NEW\n"));   // same result now, and the file is untouched
    CHECK(!hubcfg::write_file(p, "KEEP\n"));  // even identical bytes: same failure as before
    CHECK(slurp(p) == "KEEP\n" && (GetFileAttributesW(p.wstring().c_str()) & FILE_ATTRIBUTE_READONLY));
    SetFileAttributesW(p.wstring().c_str(), FILE_ATTRIBUTE_NORMAL);
    CHECK(hubcfg::write_file(p, "NEW\n") && slurp(p) == "NEW\n");
}

static void test_cemu_gp_more(const fs::path& root) {
    hub::gs::Values off, on;
    off["wiiu.graphic_packs"] = "false";
    on["wiiu.graphic_packs"] = "true";
    const fs::path dir = root / "cemu2", sx = dir / "settings.xml", sf = dir / "retrohub-cemu-gp-state.txt";
    const std::string orig = cemu_xml(kBlock);
    const std::string newer = "<GraphicPack>\n\t\t<Entry filename=\"graphicPacks/NEW/rules.txt\"/>\n\t</GraphicPack>";
    // (a1) off, the player enables other packs inside Cemu, off again: the newer list is parked, the older one kept in a dated file
    fs::remove_all(dir);
    put(sx, orig);
    CHECK(hubemu::apply_cemu_managed_settings(u8(dir), off));
    put(sx, cemu_xml(newer));
    CHECK(hubemu::apply_cemu_managed_settings(u8(dir), off));
    CHECK(slurp(sx).find("<GraphicPack>") == std::string::npos);
    CHECK(count_files(dir, "retrohub-cemu-gp-parked-") == 1 && first_file(dir, "retrohub-cemu-gp-parked-").find("graphicPacks/a/rules.txt") != std::string::npos);
    CHECK(hubemu::apply_cemu_managed_settings(u8(dir), on));
    CHECK(slurp(sx).find(newer) != std::string::npos && slurp(sx).find("graphicPacks/a/") == std::string::npos);   // most recent choice wins
    // (a2) on while a list is in settings.xml and one is parked: the list stays, the parked block goes to a dated file, not deleted
    fs::remove_all(dir);
    put(sx, orig);
    CHECK(hubemu::apply_cemu_managed_settings(u8(dir), off));
    put(sx, cemu_xml(newer));
    CHECK(hubemu::apply_cemu_managed_settings(u8(dir), on));
    CHECK(slurp(sx).find(newer) != std::string::npos && !fs::exists(sf));
    CHECK(count_files(dir, "retrohub-cemu-gp-parked-") == 1 && first_file(dir, "retrohub-cemu-gp-parked-").find(kBlock) != std::string::npos);
    // (b) damaged state files are renamed aside, never deleted, and mean "nothing parked"
    const std::string good = [&] { hubemu::CemuOwnState s; s.fs_touched = true; s.gp_stripped = true; s.gp_block = kBlock; return hubemu::cemu_own_state_text(s); }();
    hubemu::CemuOwnState chk;
    CHECK(hubemu::cemu_gp_state_parse(good, &chk) && chk.gp_block == kBlock);
    const std::string bad[] = {good.substr(0, good.size() / 2), good.substr(0, good.size() - 1), "garbage\n", "", "gp_stripped=1\ngp_block=nope\n"};
    for (const auto& b : bad) {
        hubemu::CemuOwnState x;
        CHECK(!hubemu::cemu_gp_state_parse(b, &x));
        fs::remove_all(dir);
        put(sx, cemu_xml(newer));
        put(sf, b);
        CHECK(hubemu::apply_cemu_managed_settings(u8(dir), on));
        CHECK(slurp(sx) == cemu_xml(newer));
        CHECK(!fs::exists(sf) && count_files(dir, "retrohub-cemu-gp-state.txt.bad") == 1 && first_file(dir, "retrohub-cemu-gp-state.txt.bad") == b);
    }
    // (c) <GraphicPack /> with whitespace is the same placeholder
    fs::remove_all(dir);
    put(sx, orig);
    CHECK(hubemu::apply_cemu_managed_settings(u8(dir), off));
    std::string sp = slurp(sx);
    sp.replace(sp.find("<GraphicPack/>"), 14, "<GraphicPack />");
    put(sx, sp);
    CHECK(hubemu::apply_cemu_managed_settings(u8(dir), on));
    CHECK(slurp(sx).find(kBlock) != std::string::npos && slurp(sx).find("<GraphicPack />") == std::string::npos && !fs::exists(sf));
}

static void test_ppsspp_more() {
    using namespace hubemu;
    hubctl::Profile prof = hubctl::default_profile("psp");
    const std::string gen = ppsspp_controls_ini(prof, 0);
    std::string user = "# my binds\n[ControlMapping]\nCross = 10-190\nCustom Thing = 1-99\n";
    std::string crlf;
    for (char c : user) { if (c == '\n') crlf += '\r'; crlf += c; }
    std::string m = ppsspp_controls_merge(crlf, prof, 0);
    CHECK(m.find("\r\n") != std::string::npos);
    for (size_t i = 0; i < m.size(); ++i) if (m[i] == '\n') { CHECK(i > 0 && m[i - 1] == '\r'); }
    CHECK(m.find("Custom Thing = 1-99\r\n") != std::string::npos && ppsspp_controls_merge(m, prof, 0) == m);
    CHECK(ppsspp_controls_merge(user, prof, 0).find('\r') == std::string::npos);   // LF stays LF
    // BOM: kept, and no second [ControlMapping] section
    std::string bomf = "\xEF\xBB\xBF" + user;
    std::string mb = ppsspp_controls_merge(bomf, prof, 0);
    CHECK(mb.compare(0, 3, "\xEF\xBB\xBF") == 0 && mb.compare(3, 3, "\xEF\xBB\xBF") != 0);
    size_t first = mb.find("[ControlMapping]");
    CHECK(first != std::string::npos && mb.find("[ControlMapping]", first + 1) == std::string::npos);
    CHECK(ppsspp_controls_merge(mb, prof, 0) == mb);
    std::string bomcr = "\xEF\xBB\xBF" + crlf;
    std::string mc = ppsspp_controls_merge(bomcr, prof, 0);
    CHECK(mc.compare(0, 3, "\xEF\xBB\xBF") == 0 && mc.find("[ControlMapping]", mc.find("[ControlMapping]") + 1) == std::string::npos && mc.find("\n\n") == std::string::npos);
    // collisions: the player's own line stays, the report names it
    CHECK(ppsspp_controls_collisions(gen, prof, 0).empty());   // a file RetroHub wrote: no collisions
    CHECK(ppsspp_controls_collisions("", prof, 0).empty());
    std::string col = "[ControlMapping]\nPause = 1-132\nTurbo = 1-134,1-60\nSave State = 1-132\nMy Vol = 1-37\nCross = 1-132\n";
    auto w = ppsspp_controls_collisions(col, prof, 0);
    bool f2 = false, f4 = false, an = false, save_ok = true, cross = false;
    for (const auto& s : w) {
        if (s.find("'Pause'") != std::string::npos && s.find("1-132") != std::string::npos) f2 = true;
        if (s.find("'Turbo'") != std::string::npos && s.find("1-134") != std::string::npos) f4 = true;
        if (s.find("'My Vol'") != std::string::npos && s.find("An.Up") != std::string::npos) an = true;
        if (s.find("'Save State'") != std::string::npos) save_ok = false;   // its own function: not a collision
        if (s.find("'Cross'") != std::string::npos) cross = true;           // managed key: overwritten, not reported
    }
    CHECK(f2 && f4 && an && save_ok && !cross);
    std::string kept = ppsspp_controls_merge(col, prof, 0);
    CHECK(hubui::ini_get(kept, "ControlMapping", "Pause", "") == "1-132" && hubui::ini_get(kept, "ControlMapping", "Turbo", "") == "1-134,1-60");   // player's lines untouched
    CHECK(hubui::ini_get(kept, "ControlMapping", "Cross", "") == hubui::ini_get(gen, "ControlMapping", "Cross", "x"));                       // hub mapping stays authoritative
}

int main() {
    fs::path root = fs::temp_directory_path() / ("hub_emucfg_test_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(root);
    fs::create_directories(root);
    test_durable(root / "durable");
    test_cemu_gp(root / "cemugp");
    test_readonly(root / "ro");
    test_cemu_gp_more(root / "cemugp2");
    test_clamp();
    test_deadzone();
    test_ppsspp(root / "ppsspp");
    test_ppsspp_more();
    std::error_code ec;
    fs::remove_all(root, ec);
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("hub_emucfg_test OK\n");
    return 0;
}
