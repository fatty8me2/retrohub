#include "netplay_dolphin.h"
#include "winutil.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace gcnp;

namespace fs = std::filesystem;
static void put(const fs::path& p, const std::string& t) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << t; }
static std::string get(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
/* Dolphin 2509 Config::GetGCIFolderPath(configured, slot, region) (Core/Config/MainSettings.cpp): a trailing /USA, /JPN or /EUR is
 * dropped and the folder of the disc's region is appended. This is the folder NetPlayServer::SyncSaveData sends to the guests. */
static fs::path dolphin_gci_folder(std::string configured, const char* region_dir) {
    for (auto& c : configured) if (c == '\\') c = '/';
    while (!configured.empty() && configured.back() == '/') configured.pop_back();
    for (const char* r : {"/USA", "/JPN", "/EUR"}) {
        const size_t n = strlen(r);
        if (configured.size() > n && configured.compare(configured.size() - n, n, r) == 0) { configured.resize(configured.size() - n); break; }
    }
    return fs::path(winutil::to_wide(configured + "/" + region_dir));
}

static void test_host_card_sync() {
    const fs::path base = fs::temp_directory_path() / (L"rh_gcnp_sync_" + std::to_wstring(GetCurrentProcessId()) + L"_é");   // non-ASCII path
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path card = base / L"card é\\GCI";
    put(card / "01-G4QE-MarioSoccer.gci", std::string("STRIKERS\0save", 13));
    put(card / "01-GYQE-MarioBaseball.GCI", "MSB");            // upper-case extension counts
    put(card / "notes.txt", "x");                              // not a save
    put(card / "sub" / "deep.gci", "nested");                  // only the folder itself, like Dolphin's DoFileSearch
    const std::string card8 = winutil::to_utf8(card.wstring()), root8 = winutil::to_utf8((base / L"host-sync").wstring());
    put(base / L"host-sync" / "USA" / "stale.gci", "old session");
    std::string err;
    const std::string a = host_card_sync_args(card8, root8, &err);
    CHECK(a == " -C \"Dolphin.Core.GCIFolderAPath=" + root8 + "\"");
    for (const char* r : {"USA", "EUR", "JPN"}) {
        const fs::path d = dolphin_gci_folder(root8, r);         // what Dolphin's NetPlay server reads for a disc of that region
        CHECK(get(d / "01-G4QE-MarioSoccer.gci") == std::string("STRIKERS\0save", 13));
        CHECK(get(d / "01-GYQE-MarioBaseball.GCI") == "MSB");
        CHECK(!fs::exists(d / "notes.txt") && !fs::exists(d / "sub") && !fs::exists(d / "deep.gci"));
        CHECK(!fs::exists(d / "stale.gci"));                    // wiped every launch
    }
    CHECK(get(card / "01-G4QE-MarioSoccer.gci") == std::string("STRIKERS\0save", 13) && fs::exists(card / "notes.txt"));   // the real card is untouched
    /* the room's saves change between sessions: a deleted save must not be sent again */
    fs::remove(card / "01-GYQE-MarioBaseball.GCI", ec);
    CHECK(!host_card_sync_args(card8, root8, &err).empty());
    CHECK(!fs::exists(dolphin_gci_folder(root8, "USA") / "01-GYQE-MarioBaseball.GCI") && fs::exists(dolphin_gci_folder(root8, "USA") / "01-G4QE-MarioSoccer.gci"));
    /* no card folder yet (first online game): empty region folders, so host and guests both start blank */
    const std::string none8 = winutil::to_utf8((base / L"missing").wstring());
    CHECK(!host_card_sync_args(none8, root8, &err).empty());
    CHECK(fs::is_directory(dolphin_gci_folder(root8, "EUR")) && fs::is_empty(dolphin_gci_folder(root8, "EUR")));
    /* bad input: no argument (the launch then logs and goes on with the override alone) */
    err.clear();
    CHECK(host_card_sync_args("", root8, &err).empty() && !err.empty());
    fs::remove_all(base, ec);
}

int main() {
    Spec s;
    s.mode = "traversal"; s.host_code = "1a2B3c4D"; s.game_id = "GM4E01"; s.title = "Mario Kart: \"DD\"";
    s.sha256 = "abc"; s.size = 494961216ULL; s.players = 3;
    std::string j = spec_json(s);
    Spec t;
    CHECK(spec_parse(j, &t));
    CHECK(t.mode == s.mode && t.host_code == s.host_code && t.game_id == s.game_id && t.title == s.title);
    CHECK(t.size == s.size && t.players == 3 && t.sha256 == "abc" && t.port == 2626);

    /* invalid specs */
    CHECK(!spec_parse("{\"mode\":\"bogus\",\"game_id\":\"GM4E01\"}", &t));
    CHECK(!spec_parse("{\"mode\":\"direct\",\"game_id\":\"GM4E01\"}", &t));            // direct needs addr
    CHECK(!spec_parse("{\"mode\":\"traversal\",\"game_id\":\"G\"}", &t));
    CHECK(!spec_parse("{\"mode\":\"traversal\",\"game_id\":\"GM4E01\",\"host_code\":\"xyz\"}", &t));
    CHECK(spec_parse("{\"mode\":\"traversal\",\"game_id\":\"GM4E01\"}", &t) && t.host_code.empty());

    /* embed in / extract from a launch message */
    std::string launch = "{\"kind\":\"launch\",\"game\":\"mkdd\",\"launch\":3,\"delay\":0}";
    std::string with = add_to_launch(launch, s);
    CHECK(with.find("\"kind\":\"launch\"") != std::string::npos && with.back() == '}');
    Spec u;
    CHECK(spec_parse(extract_from_launch(with), &u) && u.title == s.title && u.host_code == "1a2B3c4D");
    CHECK(extract_from_launch(launch).empty());

    /* disc identity */
    CHECK(compare_disc(s, 494961216ULL, "abc") == Match::Same);
    CHECK(compare_disc(s, 494961217ULL, "abc") == Match::DifferentDisc);
    CHECK(compare_disc(s, 494961216ULL, "def") == Match::DifferentDisc);
    {   /* version check: ID + disc + revision only */
        unsigned char hd[0x80] = {0};
        memcpy(hd, "GM4E01", 6); hd[6] = 0; hd[7] = 1; hd[0x1C] = 0xC2; hd[0x1D] = 0x33; hd[0x1E] = 0x9F; hd[0x1F] = 0x3D;
        DiscVer v = disc_version_from_header(hd, sizeof hd);
        Spec hs; hs.game_id = "GM4E01"; hs.disc = 0; hs.rev = 1;
        CHECK(v.ok && compare_version(hs, v) == Match::Same);
        hs.rev = 0; CHECK(compare_version(hs, v) == Match::DifferentDisc && version_diff(hs, v).find("revision 1 vs 0") != std::string::npos);
        hs.rev = 1; hs.game_id = "GM4P01"; CHECK(compare_version(hs, v) == Match::DifferentDisc);
        hs.game_id = "GM4E01"; hs.disc = 1; CHECK(compare_version(hs, v) == Match::DifferentDisc);
        hs.disc = -1; hs.rev = -1; CHECK(compare_version(hs, v) == Match::Same);   // old host: unknown never blocks
        hd[0x1F] = 0; CHECK(!disc_version_from_header(hd, sizeof hd).ok);
        Spec rt; CHECK(spec_parse(spec_json(hs), &rt) && rt.rev == -1);
    }
    Spec nohash; CHECK(compare_disc(nohash, 5, "x") == Match::Unknown);

    CHECK(valid_host_code("deadBEEF") && !valid_host_code("deadbee") && !valid_host_code("deadbeeg"));
    CHECK(guest_connect_delay_s(1) == 0 && guest_connect_delay_s(2) == 4 && guest_connect_delay_s(3) == 8);
    CHECK(list_item_suffix(s) == "(GM4E01)");

    /* ini patch: guest in traversal mode keeps other keys, sets code and nickname */
    std::string ini = "[General]\nISOPath0 = old\nFoo = 1\n[NetPlay]\nNickname = Player\nHostCode = 00000000\n[Core]\nSIDevice0 = 6\n";
    IniParams p; p.host = false; p.nickname = "Ann"; p.iso_dir = "C:\\games"; p.mute = true;
    std::string o = ini_patch(ini, s, p);
    CHECK(o.find("Nickname = Ann") != std::string::npos);
    { IniParams q = p; q.nickname = "A;n#n [x]=y!"; std::string oq = ini_patch(ini, s, q); CHECK(oq.find("Nickname = Ann xy!") != std::string::npos); }   // INI-syntax characters stripped, space and ! kept
    CHECK(o.find("HostCode = 1a2B3c4D") != std::string::npos && o.find("00000000") == std::string::npos);
    CHECK(o.find("TraversalChoice = traversal") != std::string::npos);
    CHECK(o.find("ISOPath0 = C:\\games") != std::string::npos && o.find("Foo = 1") != std::string::npos);
    CHECK(o.find("SIDevice0 = 6") != std::string::npos);
    CHECK(o.find("[DSP]") != std::string::npos && o.find("Backend = No Audio Output") != std::string::npos);
    CHECK(o.find("ConfirmStop = False") != std::string::npos && ini_patch(std::string("[Interface]\nConfirmStop = True\n"), s, p).find("ConfirmStop = False") != std::string::npos);   // never Dolphin's stop dialog online
    CHECK(o.find("CPUThread = False") != std::string::npos && o.find("SyncGPU = False") != std::string::npos && o.find("OverclockEnable = False") != std::string::npos);   // default single core, other determinism pins kept
    { Spec b = s; b.game_id = "GYQE01"; std::string ob = ini_patch(o, b, p); CHECK(ob.find("CPUThread = True") != std::string::npos && ob.find("SyncGPU = False") != std::string::npos);   // allowlisted: dual core
      b.game_id = "GYQP01"; CHECK(dual_core_allowed("GYQJ01") && ini_patch("", b, p).find("CPUThread = True") != std::string::npos); CHECK(!dual_core_allowed("GM4E01") && !dual_core_allowed("")); CHECK(!dual_core_allowed("G4QE01") && !dual_core_allowed("G4QP01")); }
    CHECK(std::string(driver_script()).find("$BufFile") != std::string::npos && std::string(driver_script()).find("RangeValuePattern") != std::string::npos);   // host types the measured buffer before Start
    CHECK(pad_buffer_for_ping(-1, false) == 8 && pad_buffer_for_ping(-1, true) == 10);
    /* measured: session RTT ~170 ms needs 12 (8 -> 75 %, 10 -> 92 %, 12 -> 100 %) */
    CHECK(pad_buffer_for_ping(170, false) >= 12 && pad_buffer_for_ping(170, true) <= 14);
    CHECK(pad_buffer_for_ping(0, false) == 4 && pad_buffer_for_ping(20, true) == 6 && pad_buffer_for_ping(100, false) == 8 && pad_buffer_for_ping(5000, true) == 30);
    for (int r = 0; r < 400; r += 7) CHECK(pad_buffer_for_ping(r + 7, false) >= pad_buffer_for_ping(r, false));   // monotonic
    CHECK(next_pad_buffer(-1, 9) == 9 && next_pad_buffer(9, 12) == 12 && next_pad_buffer(12, 10) == 12 && next_pad_buffer(12, 9) == 9);
    CHECK(ini_patch("", s, p).find("BufferSize = 8") != std::string::npos);
    { IniParams q = p; q.ping_ms = 100; q.relayed = true; CHECK(ini_patch("", s, q).find("BufferSize = 8") != std::string::npos); }
    { const std::string d = driver_script(); CHECK(d.find("function Set-Buf") != std::string::npos && d.find("if ($Role -eq \"host\") { Set-Buf }") != std::string::npos); }   // applied before Start and live
    CHECK(ini_patch(o, s, p) == o);                    // idempotent

    /* host never gets a HostCode written; direct mode sets address/ports */
    Spec d; d.mode = "direct"; d.addr = "127.0.0.1"; d.port = 2700; d.game_id = "GALE01";
    IniParams h; h.host = true; h.nickname = "Bob";
    std::string oh = ini_patch("", d, h);
    CHECK(oh.find("HostCode") == std::string::npos);
    CHECK(oh.find("Address = 127.0.0.1") != std::string::npos && oh.find("ConnectPort = 2700") != std::string::npos && oh.find("HostPort = 2700") != std::string::npos);
    CHECK(oh.find("TraversalChoice = direct") != std::string::npos && oh.find("[DSP]") == std::string::npos);

    CHECK(launch_fragment(s).rfind(",\"np_gc\":{", 0) == 0);
    CHECK(oh.find("UsePanicHandlers = False") != std::string::npos && oh.find("UpdateTrack =") != std::string::npos);
    CHECK(std::string(driver_script()).find("Start NetPlay...") != std::string::npos);
    CHECK(std::string(driver_script()).find("ClassName -eq \"MainWindow\"") != std::string::npos);   // main window found by class even when the title is "dolphin-emu"
    CHECK(std::string(driver_script()).find("$k -lt 40") != std::string::npos && std::string(driver_script()).find(" < 40") == std::string::npos);   // PowerShell has no < operator (it broke the driver once)

    /* memory cards + desync-prone settings: guests never write; host writes only if it chose to; strict sync + code sync forced */
    CHECK(oh.find("WriteSaveData = False") != std::string::npos);
    { IniParams hw = h; hw.write_save = true; std::string ow = ini_patch("", d, hw); CHECK(ow.find("WriteSaveData = True") != std::string::npos); }
    { IniParams gw = p; gw.write_save = true; CHECK(ini_patch("", s, gw).find("WriteSaveData = False") != std::string::npos); }
    CHECK(o.find("SyncSaves = True") != std::string::npos && o.find("StrictSettingsSync = True") != std::string::npos && o.find("SyncCodes = True") != std::string::npos);
    CHECK(o.find("GPUDeterminismMode = auto") != std::string::npos);

    test_host_card_sync();

    if (!fails) printf("netplay_dolphin tests passed\n");
    return fails ? 1 : 0;
}
