// Console test for Plan 4 Wave 2 C (convenience): Discord frames + opt-in gate, diagnostics exclusion, NAT verdict (+ loopback STUN),
// controller glyph mapping, palette ranking, setup steps. Loopback / scratch dirs only.
#include "diag_logic.h"
#include "discord_logic.h"
#include "glyph_logic.h"
#include "help_logic.h"
#include "nettest.h"
#include "palette_logic.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include "miniz.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
namespace fs = std::filesystem;

static void test_discord() {
    using namespace hubdiscord;
    std::string f = encode_frame(Frame, "{\"a\":1}");
    CHECK(f.size() == 15 && f[0] == 1 && f[1] == 0 && f[4] == 7 && f.substr(8) == "{\"a\":1}");
    uint32_t op = 9; std::string j;
    CHECK(decode_frame(f.substr(0, 10), &op, &j) == 0);
    CHECK(decode_frame(f, &op, &j) == 15 && op == Frame && j == "{\"a\":1}");
    CHECK(decode_frame(std::string("\x09\0\0\0\0\0\0\0", 8), &op, &j) == -1);
    CHECK(handshake_json("123") == "{\"v\":1,\"client_id\":\"123\"}");
    CHECK(!client_id_valid(kClientId) && client_id_valid("123456789012345678") && !client_id_valid("12a456789012345678"));

    const std::string id = "123456789012345678";
    Presence p; p.playing = true; p.title = "Super \"Smash\" Bros."; p.console = "Nintendo 64"; p.console_key = "n64"; p.start_unix = 1700000000;
    p.in_room = true; p.room_code = "ABC-1"; p.invite_url = "https://example.test/j/XYZ"; p.party_size = 2; p.party_max = 4;
    Opts off; Opts on; on.enabled = true; Opts join; join.enabled = join.join = true;
    CHECK(activity_json(off, p, 7, "n", id).empty());                       // off by default: nothing is built, so nothing is sent
    CHECK(activity_json(on, p, 7, "n", kClientId).empty());                 // placeholder id: nothing
    Presence idle;
    CHECK(activity_json(on, idle, 7, "n", id).empty());                     // nothing to show
    std::string a = activity_json(on, p, 7, "n", id);
    CHECK(a.find("\\\"Smash\\\"") != std::string::npos && a.find("\"size\":[2,4]") != std::string::npos && a.find("console_n64") != std::string::npos);
    CHECK(a.find("start\":1700000000") != std::string::npos);
    CHECK(a.find("XYZ") == std::string::npos && a.find("ABC") == std::string::npos && a.find("buttons") == std::string::npos);   // Join is its own opt-in
    Opts joinOnly; joinOnly.join = true;                                    // join toggle without the master switch
    CHECK(activity_json(joinOnly, p, 7, "n", id).empty() && !may_join_button(joinOnly, p));
    std::string b = activity_json(join, p, 7, "n", id);
    CHECK(b.find("buttons") != std::string::npos && b.find("https://example.test/j/XYZ") != std::string::npos);
    Presence nolink = p; nolink.invite_url = "http://insecure";
    CHECK(!may_join_button(join, nolink));
    p.image_url = "https://img.test/cover.png";
    CHECK(activity_json(on, p, 7, "n", id).find("https://img.test/cover.png") != std::string::npos);
}

static void test_diag() {
    using namespace hubdiag;
    CHECK(excluded("identity.json") && excluded("identity_abc.json") && excluded("Identity-2.JSON"));
    CHECK(excluded("userdata/_identity/key.bin") && excluded("a\\_identity\\x") && excluded("logs/identity_backup.json"));
    CHECK(!excluded("logs/hub.log") && !excluded("logs/identity.txt") && !excluded("logs/my_identity.json"));
    fs::path root = fs::temp_directory_path() / "rh_diag_test";
    fs::remove_all(root);
    fs::create_directories(root / "logs" / "_identity");
    fs::create_directories(root / "logs" / "sub");
    auto put = [&](const char* rel, const char* body) { std::ofstream(root / rel, std::ios::binary) << body; };
    put("logs/hub.log", "hello"); put("logs/identity.json", "SECRET"); put("logs/identity_x.json", "SECRET");
    put("logs/_identity/k.txt", "SECRET"); put("logs/sub/hang-1.txt", "hang");
    auto files = collect_dir(root.u8string() + "/logs", "logs/");
    CHECK(files.size() == 2);
    files.push_back({"logs/identity.json", (root / "logs" / "identity.json").u8string()});   // refused by write_zip as well
    std::string err;
    fs::path zip = root / "out.zip";
    CHECK(write_zip(zip.u8string(), files, {{"versions.txt", "RetroHub 1"}}, &err));
    mz_zip_archive z;
    std::memset(&z, 0, sizeof z);
    CHECK(mz_zip_reader_init_file(&z, zip.string().c_str(), 0));
    int n = (int)mz_zip_reader_get_num_files(&z); bool secret = false, hub = false, ver = false;
    for (int i = 0; i < n; ++i) {
        char nm[260]; mz_zip_reader_get_filename(&z, i, nm, sizeof nm);
        std::string s = nm; secret |= s.find("identity") != std::string::npos; hub |= s == "logs/hub.log"; ver |= s == "versions.txt";
    }
    mz_zip_reader_end(&z);
    CHECK(n == 3 && hub && ver && !secret);
    fs::remove_all(root);
}

static void fake_stun(SOCKET s, std::atomic<bool>* stop) {
    while (!*stop) {
        uint8_t buf[64]; sockaddr_in from; int fl = sizeof from;
        int n = recvfrom(s, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n < 20) continue;
        uint8_t r[32] = {0x01, 0x01, 0, 12, 0x21, 0x12, 0xA4, 0x42};
        std::memcpy(r + 8, buf + 8, 12);
        r[20] = 0; r[21] = 0x20; r[22] = 0; r[23] = 8; r[24] = 0; r[25] = 1;
        int port = ntohs(from.sin_port);
        r[26] = (uint8_t)((port >> 8) ^ 0x21); r[27] = (uint8_t)((port & 255) ^ 0x12);
        const uint8_t* ip = (const uint8_t*)&from.sin_addr;
        const uint8_t x[4] = {0x21, 0x12, 0xA4, 0x42};
        for (int i = 0; i < 4; ++i) r[28 + i] = ip[i] ^ x[i];
        sendto(s, (const char*)r, 32, 0, (sockaddr*)&from, fl);
    }
}

static void test_nettest() {
    using namespace hubnt;
    Mapped a{true, "1.2.3.4", 5000}, b = a, c{true, "1.2.3.4", 5001}, none;
    CHECK(verdict(a, b).v == Verdict::Direct);
    CHECK(verdict(a, c).v == Verdict::Relay);
    CHECK(verdict(none, none).v == Verdict::Blocked);
    CHECK(verdict(a, none).v == Verdict::Direct);
    CHECK(std::string(ping_text(-1)) == "Lobby unreachable" && std::string(ping_text(20)) == "Lobby ping: good");

    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET srv[2]; int ports[2]; std::atomic<bool> stop{false}; std::thread th[2];
    for (int i = 0; i < 2; ++i) {
        srv[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in sa; std::memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(srv[i], (sockaddr*)&sa, sizeof sa);
        int l = sizeof sa; getsockname(srv[i], (sockaddr*)&sa, &l); ports[i] = ntohs(sa.sin_port);
        DWORD t = 200; setsockopt(srv[i], SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
        th[i] = std::thread(fake_stun, srv[i], &stop);
    }
    std::string s1 = "127.0.0.1:" + std::to_string(ports[0]), s2 = "127.0.0.1:" + std::to_string(ports[1]);
    Report r = run_test(s1, s2, "");
    CHECK(r.a.ok && r.b.ok && r.a.ip == "127.0.0.1" && r.a.port == r.b.port && r.verdict.v == Verdict::Direct);
    Report dead = run_test("127.0.0.1:9", "127.0.0.1:9", "");
    CHECK(!dead.a.ok && dead.verdict.v == Verdict::Blocked);
    stop = true;
    for (int i = 0; i < 2; ++i) { th[i].join(); closesocket(srv[i]); }
    WSACleanup();
}

static void test_glyph() {
    using namespace hubglyph;
    CHECK(kind_from_sdl_type(1) == Kind::Xbox && kind_from_sdl_type(2) == Kind::Xbox && kind_from_sdl_type(0) == Kind::Xbox);
    CHECK(kind_from_sdl_type(4) == Kind::PlayStation && kind_from_sdl_type(7) == Kind::PlayStation && kind_from_sdl_type(3) == Kind::PlayStation);
    CHECK(kind_from_sdl_type(5) == Kind::Switch && kind_from_sdl_type(13) == Kind::Switch);
    CHECK(std::string(label(Kind::Xbox, "View")) == "View" && std::string(label(Kind::Xbox, "A")) == "A");
    CHECK(std::string(label(Kind::PlayStation, "A")) == "Cross" && std::string(label(Kind::PlayStation, "Y")) == "Triangle");
    CHECK(std::string(label(Kind::PlayStation, "LB")) == "L1" && std::string(label(Kind::PlayStation, "RT")) == "R2");
    CHECK(std::string(label(Kind::PlayStation, "Menu")) == "Options" && std::string(label(Kind::PlayStation, "Start")) == "Options");
    CHECK(std::string(label(Kind::Switch, "A")) == "A" && std::string(label(Kind::Switch, "LT")) == "ZL" && std::string(label(Kind::Switch, "Menu")) == "+");
    CHECK(std::string(label(Kind::Switch, "R3")) == "R3");
    CHECK(is_ps_shape("Circle") && !is_ps_shape("L1"));
    CHECK(battery_shown(1) && battery_low(0) && !battery_shown(4) && !battery_shown(-1) && battery_bars(2) == 2);
}

static void test_palette() {
    using namespace hubpal;
    std::vector<Item> it = {
        {Kind::Game, "Super Mario 64", "N64", "g1"}, {Kind::Game, "Mario Kart 64", "N64", "g2"}, {Kind::Game, "Metroid Fusion", "GBA", "g3"},
        {Kind::Action, "Host a room", "", "host"}, {Kind::Action, "Rescan games", "", "rescan"}, {Kind::Setting, "Settings: Online", "", "s"},
        {Kind::Friend, "Marcus", "", "f1"}, {Kind::Action, "Party mode", "", "party"}};
    auto r = rank("mario", it);
    CHECK(r.size() == 2 && it[r[0]].id == "g2");                    // prefix match (Mario Kart) beats a mid-title word match
    r = rank("host", it); CHECK(!r.empty() && it[r[0]].id == "host");
    r = rank("mkt", it); CHECK(!r.empty() && it[r[0]].id == "g2");  // fuzzy subsequence
    r = rank("zzz", it); CHECK(r.empty());
    r = rank("", it, 3); CHECK(r.size() == 3 && it[r[0]].kind == Kind::Action);   // empty query: actions first, limited
    CHECK(score("party", "Party mode") > score("party", "Tea party hat"));
    CHECK(starts_search('m') && starts_search('/') && !starts_search('f') && !starts_search('I') && !starts_search(' ') && !starts_search(200));
}

static void test_help() {
    using namespace hubhelp;
    SetupInputs s; s.have_games = s.username_claimed = s.net_probe_done = s.setup_done = true;
    CHECK(open_steps(s).empty());
    s.have_games = false; s.net_probe_done = false;
    CHECK(open_steps(s).size() == 2);
    s = SetupInputs(); CHECK(open_steps(s).size() == 3);
    s.have_games = s.username_claimed = s.net_probe_done = true; CHECK(open_steps(s).size() == 1);   // only the tour is left
    // the empty Home card already asks for game folders: the Finish card drops "Add your games"
    s = SetupInputs();
    CHECK(open_steps(s).size() == 3 && open_steps(s)[0] == "Add your games");
    s.empty_card_showing = true;
    CHECK(open_steps(s).size() == 2 && open_steps(s)[0] == "Pick a username" && open_steps(s)[1] == "Allow online play");
    s.username_claimed = s.net_probe_done = true; s.setup_done = true;
    CHECK(open_steps(s).empty());                       // nothing else open: the card hides, the games card carries on alone
    s.setup_done = false; CHECK(open_steps(s).size() == 1 && open_steps(s)[0] == "Finish the setup tour");
    // compact one-line card below about 700 px (at 100% UI size)
    CHECK(finish_card_compact(600.f, 1.f) && finish_card_compact(699.f, 1.f) && !finish_card_compact(700.f, 1.f) && !finish_card_compact(720.f, 1.f));
    CHECK(!finish_card_compact(1050.f, 1.5f) && finish_card_compact(1049.f, 1.5f));
    CHECK(join_steps({"Pick a username", "Allow online play"}) == "Pick a username  -  Allow online play" && join_steps({}).empty());
}

static void test_scan_summary() {
    using namespace hubhelp;
    const std::string home = "C:\\Users\\me";
    // no folders: nothing to report
    CHECK(scan_summary({}, {}).text.empty());
    // nothing found
    ScanSummary r = scan_summary({}, {"C:\\Users\\me\\Documents\\Games"}, home);
    CHECK(r.none && r.games == 0 && r.text == "Looked in ~\\Documents\\Games: no game files found.");
    r = scan_summary({}, {"D:\\Roms", "E:\\More"}, home);
    CHECK(r.none && r.text == "Looked in 2 folders: no game files found.");
    // one game, one folder: singular
    r = scan_summary({"D:\\Roms\\a.z64"}, {"D:\\Roms"}, home);
    CHECK(!r.none && r.text == "Found 1 game in 1 folder.");
    // many games, one folder (slash style and case do not matter, nested folders count)
    r = scan_summary({"d:/roms/a.z64", "D:\\ROMS\\sub\\b.gba", "D:\\Roms\\c.nds"}, {"D:\\Roms\\"}, home);
    CHECK(r.text == "Found 3 games in 1 folder." && r.folders_with_games == 1);
    // several folders, only some hold games
    r = scan_summary({"D:\\Roms\\a.z64", "E:\\More\\b.gba"}, {"D:\\Roms", "E:\\More", "F:\\Empty"}, home);
    CHECK(r.text == "Found 2 games in 2 folders.");
    // a folder name that only starts the same is not a parent
    r = scan_summary({"D:\\Roms2\\a.z64"}, {"D:\\Roms"}, home);
    CHECK(r.text == "Found 1 game.");   // games found, but none under a listed folder (e.g. a pack): no folder count
    // who gets a toast: only a scan the player started, never while a game runs
    {
        const ScanSummary some = scan_summary({"D:\\Roms\\a.z64"}, {"D:\\Roms"}), none = scan_summary({}, {"D:\\Roms"}), nofolders = scan_summary({}, {});
        CHECK(scan_toast_wanted(true, false, some));      // user scan with games
        CHECK(scan_toast_wanted(true, false, none));      // user scan that found nothing
        CHECK(!scan_toast_wanted(false, false, some));    // automatic scan: silent
        CHECK(!scan_toast_wanted(false, false, none));
        CHECK(!scan_toast_wanted(true, true, some));      // a game is running
        CHECK(!scan_toast_wanted(true, false, nofolders));  // nothing to report
    }
    // paths
    CHECK(short_path("C:\\Users\\me\\Documents", home) == "~\\Documents" && short_path("c:/users/ME/Games", home) == "~/Games");
    CHECK(short_path("C:\\Users\\meow\\Documents", home) == "C:\\Users\\meow\\Documents" && short_path("D:\\Roms", home) == "D:\\Roms" && short_path("C:\\Users\\me", home) == "~");
    CHECK(short_path("D:\\Roms", "") == "D:\\Roms");
    CHECK(middle_trunc("short", 10) == "short");
    {
        const std::string t = middle_trunc("D:\\Very\\Long\\Folder\\Name\\Here", 15);
        CHECK(utf8_len(t) == 15 && t.find("...") != std::string::npos && t.substr(0, 3) == "D:\\" && t.substr(t.size() - 4) == "Here");
        const std::string u = middle_trunc("C:\\Spiele\\\xC3\xA4\xC3\xB6\xC3\xBC\xC3\xA4\xC3\xB6\xC3\xBC\xC3\xA4\xC3\xB6\xC3\xBC\\ende", 12);   // umlauts: never cut inside a character
        CHECK(utf8_len(u) == 12);
        for (size_t i = 0; i < u.size(); ++i) if (((unsigned char)u[i] & 0xC0) == 0x80) CHECK(i > 0 && ((unsigned char)u[i - 1] & 0x80));
    }
    CHECK(middle_trunc("abcdefghij", 3) == "..." && middle_trunc("abcdefghij", 0).empty());
    // caption
    CHECK(scan_caption_base(12, 340) == "Scanning your games...  12 / 340" && scan_caption_base(0, 0) == "Scanning your games...");
    CHECK(scan_browse_hint(9.9).empty() && scan_browse_hint(10.0) == "You can keep browsing while this finishes.");
    CHECK(file_types_line({".z64", ".gba"}) == "RetroHub reads these file types: .z64, .gba." && file_types_line({}).empty());
}

int main() {
    test_discord(); test_diag(); test_nettest(); test_glyph(); test_palette(); test_help(); test_scan_summary();
    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("convenience tests passed\n");
    return 0;
}
