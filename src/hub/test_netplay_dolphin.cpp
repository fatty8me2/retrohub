#include "netplay_dolphin.h"

#include <cstdio>
#include <string>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace gcnp;

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
    CHECK(o.find("HostCode = 1a2B3c4D") != std::string::npos && o.find("00000000") == std::string::npos);
    CHECK(o.find("TraversalChoice = traversal") != std::string::npos);
    CHECK(o.find("ISOPath0 = C:\\games") != std::string::npos && o.find("Foo = 1") != std::string::npos);
    CHECK(o.find("SIDevice0 = 6") != std::string::npos);
    CHECK(o.find("[DSP]") != std::string::npos && o.find("Backend = No Audio Output") != std::string::npos);
    CHECK(o.find("ConfirmStop = False") != std::string::npos && ini_patch(std::string("[Interface]\nConfirmStop = True\n"), s, p).find("ConfirmStop = False") != std::string::npos);   // never Dolphin's stop dialog online
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

    if (!fails) printf("netplay_dolphin tests passed\n");
    return fails ? 1 : 0;
}
