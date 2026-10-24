/* Kanto Together: package detection, install, SHA-1 / ROM pick, launcher output parsing, commands, saves. Scratch dirs only. */
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "kanto.h"
#include "miniz.h"
#include "winutil.h"

namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static void put(const fs::path& p, const std::string& t) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << t;
}

static void make_pkg_dir(const fs::path& root) {
    for (auto& r : kanto::required_files()) put(root / r, "x:" + r);
}

int main() {
    fs::path tmp = fs::temp_directory_path() / ("kt_test_" + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    /* sha1 */
    CHECK(kanto::sha1_hex("abc", 3) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(kanto::sha1_hex("", 0) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    put(tmp / "f.bin", "abc");
    CHECK(kanto::sha1_file((tmp / "f.bin").string()) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(kanto::sha1_file((tmp / "none.bin").string()).empty());

    /* package detection: folder + zip, junk ignored */
    fs::path lib = tmp / "Games" / "GBA";
    make_pkg_dir(lib / "KantoTogether-ROM");
    put(lib / "other" / "readme.txt", "hi");
    fs::path zip = lib / "KantoTogether-ROM-Friend.zip";
    {
        mz_zip_archive za;
        memset(&za, 0, sizeof za);
        CHECK(mz_zip_writer_init_file(&za, zip.string().c_str(), 0));
        for (auto& r : kanto::required_files()) { std::string n = "KantoTogether-ROM/" + r, d = "data:" + r; mz_zip_writer_add_mem(&za, n.c_str(), d.data(), d.size(), MZ_BEST_SPEED); }
        std::string evil = "x";
        mz_zip_writer_add_mem(&za, "KantoTogether-ROM/../evil.txt", evil.data(), 1, MZ_BEST_SPEED);
        mz_zip_writer_add_mem(&za, "KantoTogether-ROM/saves/old.sav", evil.data(), 1, MZ_BEST_SPEED);
        mz_zip_writer_add_mem(&za, "KantoTogether-ROM/LICENSES.txt", "lic", 3, MZ_BEST_SPEED);
        mz_zip_writer_finalize_archive(&za);
        mz_zip_writer_end(&za);
    }
    { mz_zip_archive za; memset(&za, 0, sizeof za); fs::path bad = lib / "kanto-broken.zip"; mz_zip_writer_init_file(&za, bad.string().c_str(), 0); mz_zip_writer_add_mem(&za, "a.txt", "a", 1, 1); mz_zip_writer_finalize_archive(&za); mz_zip_writer_end(&za); }
    put(lib / "Pokemon - Emerald.zip", "not a package");
    auto pk = kanto::find_packages({lib.string(), lib.string()});
    CHECK(pk.size() == 2);   // the folder and the good zip (not the broken one, not duplicates)
    bool saw_zip = false, saw_dir = false;
    for (auto& p : pk) { if (p.is_zip) { saw_zip = true; CHECK(p.prefix == "KantoTogether-ROM/"); CHECK(p.size > 0); } else saw_dir = true; }
    CHECK(saw_zip && saw_dir);
    CHECK(kanto::find_packages({(tmp / "nope").string()}).empty());

    /* rel paths */
    CHECK(kanto::install_rel_path("KantoTogether-ROM/app/coop.lua", "KantoTogether-ROM/") == "app/coop.lua");
    CHECK(kanto::install_rel_path("KantoTogether-ROM/../evil.txt", "KantoTogether-ROM/").empty());
    CHECK(kanto::install_rel_path("KantoTogether-ROM/saves/a.sav", "KantoTogether-ROM/").empty());
    CHECK(kanto::install_rel_path("KantoTogether-ROM/rom/x.gba", "KantoTogether-ROM/").empty());
    CHECK(kanto::install_rel_path("KantoTogether-ROM/C:/x", "KantoTogether-ROM/").empty());
    CHECK(kanto::install_rel_path("other/app/x", "KantoTogether-ROM/").empty());
    CHECK(kanto::install_rel_path("KantoTogether-ROM/a\\b.txt", "KantoTogether-ROM/") == "a/b.txt");

    /* install from the zip (never touches the zip) */
    std::string zip_before = kanto::sha1_file(zip.string());
    kanto::Package zp;
    for (auto& p : pk) if (p.is_zip) zp = p;
    std::string dir = (tmp / "data" / "apps" / "KantoTogether").string() + "\\";
    std::string err;
    uint64_t last = 0;
    CHECK(!kanto::is_installed(dir));
    CHECK(kanto::install(zp, dir, [&](uint64_t d, uint64_t t) { last = d; return d <= t; }, &err));
    CHECK(kanto::is_installed(dir) && kanto::install_current(dir, zp) && last > 0);
    CHECK(!fs::exists(tmp / "data" / "apps" / "evil.txt") && !fs::exists(tmp / "data" / "evil.txt"));
    CHECK(fs::exists(fs::path(dir) / "LICENSES.txt") && !fs::exists(fs::path(dir) / "saves"));
    CHECK(kanto::sha1_file(zip.string()) == zip_before);
    kanto::Package newer = zp; newer.size += 1;
    CHECK(!kanto::install_current(dir, newer));
    CHECK(kanto::install_dir("C:\\d") == "C:\\d\\apps\\KantoTogether\\");
    /* cancel */
    std::string dir2 = (tmp / "data2").string() + "\\";
    CHECK(!kanto::install(zp, dir2, [](uint64_t, uint64_t) { return false; }, &err));
    /* install from a folder */
    kanto::Package dp;
    for (auto& p : pk) if (!p.is_zip) dp = p;
    put(fs::path(dp.path) / "rom" / "KantoTogether.gba", "romdata");   // the friend's built rom must not be copied
    std::string dir3 = (tmp / "data3").string() + "\\";
    CHECK(kanto::install(dp, dir3, nullptr, &err));
    CHECK(kanto::is_installed(dir3) && !fs::exists(fs::path(dir3) / "rom"));

    /* manifest + rom pick */
    put(tmp / "patch.json", "{\"baseSha1\":\"41CB23D8DCCC8EBD7C649CD8FBB58EEACE6E2FDC\",\"targetSha1\":\"b41b91db461d6e97f1453b047e072ddde0184270\",\"baseSize\":16777216}");
    kanto::PatchManifest pm;
    CHECK(kanto::read_manifest((tmp / "patch.json").string(), &pm) && pm.base_sha1 == kanto::kBaseSha1 && pm.base_size == 16777216);
    CHECK(!kanto::read_manifest((tmp / "nope.json").string(), &pm));
    auto fake = [](const std::string& p) { return p == "good.gba" ? std::string(kanto::kBaseSha1) : p == "rev1.gba" ? std::string("deadbeef") : std::string(); };
    kanto::RomPick r = kanto::pick_rom({{"leaf.gba", "BPGE"}, {"rev1.gba", "BPRE"}, {"good.gba", "BPRE"}}, kanto::kBaseSha1, fake);
    CHECK(r.state == kanto::RomState::Ok && r.path == "good.gba" && kanto::rom_message(r).empty());
    r = kanto::pick_rom({{"rev1.gba", "BPRE"}}, kanto::kBaseSha1, fake);
    CHECK(r.state == kanto::RomState::WrongRevision && kanto::rom_message(r).find("version 1.0") != std::string::npos);
    r = kanto::pick_rom({{"leaf.gba", "BPGE"}}, kanto::kBaseSha1, fake);
    CHECK(r.state == kanto::RomState::NoFireRed && kanto::rom_message(r).find("ROM folder") != std::string::npos);
    CHECK(kanto::pick_rom({}, kanto::kBaseSha1, fake).state == kanto::RomState::NoFireRed);

    /* zipped / serial-less FireRed: matched by hash (or catalog id), size-gated */
    {
        kanto::RomCandidate zipped; zipped.path = "good.gba"; zipped.size = 16777216;   // no serial, no catalog id
        r = kanto::pick_rom({zipped}, kanto::kBaseSha1, fake);
        CHECK(r.state == kanto::RomState::Ok && r.path == "good.gba");
        kanto::RomCandidate cat; cat.path = "good.gba"; cat.catalog_id = "pokefirered";
        CHECK(kanto::pick_rom({cat}, kanto::kBaseSha1, fake).state == kanto::RomState::Ok);
        kanto::RomCandidate small; small.path = "good.gba"; small.size = 1024;
        CHECK(kanto::pick_rom({small}, kanto::kBaseSha1, fake).state == kanto::RomState::NoFireRed);   // not even hashed
        kanto::RomCandidate other; other.path = "rev1.gba"; other.size = 16777216;   // unidentified 16 MiB, wrong hash: ignored, not "wrong revision"
        CHECK(kanto::pick_rom({other}, kanto::kBaseSha1, fake).state == kanto::RomState::NoFireRed);
        kanto::RomCandidate rev; rev.path = "rev1.gba"; rev.catalog_id = "pokefirered";
        CHECK(kanto::pick_rom({rev}, kanto::kBaseSha1, fake).state == kanto::RomState::WrongRevision);
        kanto::RomCandidate emerald; emerald.path = "good.gba"; emerald.serial = "BPEE";
        CHECK(kanto::pick_rom({emerald}, kanto::kBaseSha1, fake).state == kanto::RomState::NoFireRed);
    }
    /* failure reasons: fixed sentences only */
    CHECK(kanto::valid_fail_reason("no_rom") && kanto::valid_fail_reason("wrong_rom") && !kanto::valid_fail_reason("C:/Users/x") && !kanto::valid_fail_reason(""));
    CHECK(kanto::fail_sentence("no_rom").find("FireRed") != std::string::npos);
    CHECK(kanto::fail_sentence("C:/secret/path") == kanto::fail_sentence("") && kanto::fail_sentence("").find("secret") == std::string::npos);
    CHECK(kanto::reason_for(kanto::RomState::WrongRevision) == "wrong_rom" && kanto::reason_for(kanto::RomState::NoFireRed) == "no_rom");

    /* codes + commands + command lines */
    CHECK(kanto::normalize_code("3f9a-0c7d 21") == "3F9A0C7D21");
    CHECK(kanto::valid_code("3F9A0C7D21") && !kanto::valid_code("3F9A0C7D2") && !kanto::valid_code("3f9a0c7d21") && !kanto::valid_code("ZZZZZZZZZZ"));
    CHECK(kanto::cmd_trade(1) == "trade 1\n" && kanto::cmd_trade(6) == "trade 6\n" && kanto::cmd_trade(0).empty() && kanto::cmd_trade(7).empty());
    CHECK(kanto::cmd_confirm() == "confirm\n" && kanto::cmd_cancel() == "cancel\n" && kanto::cmd_quit() == "quit\n");
    CHECK(kanto::redact_log_text("relay 10.1.2.3:80 ok") == "relay x.x.x.x:80 ok");
    std::printf("[%s]\n", kanto::redact_log_text("relay 10.1.2.3:80 ok").c_str());
    { std::string ld = (tmp / "logs").string(); fs::create_directories(tmp / "logs");
      for (int i = 0; i < 12; ++i) { std::ofstream((tmp / "logs").string() + "/kanto-2020-01-" + std::to_string(10 + i) + ".log").put('x'); }
      kanto::write_log(ld, "hello");
      int n = 0; for (auto& e : fs::directory_iterator(ld)) { (void)e; ++n; }
      CHECK(n == 10); }
    { kanto::State g; g.host = false; kanto::feed_line(g, "[bridge] lua: world: snapshot applied (join): 3 flag bytes, 1 vars, 0 dex bytes changed"); CHECK(g.synced_story); }
    { kanto::State g; g.host = false; CHECK(kanto::guest_auto_rejoin_ok(g));
      kanto::feed_line(g, "[bridge] >>> Your partner is connected. <<<"); CHECK(g.ever_connected);
      kanto::feed_line(g, "peer disconnected"); CHECK(!kanto::guest_auto_rejoin_ok(g)); }
    CHECK(kanto::cmd_code("3f9a-0c7d21") == "code 3F9A0C7D21\n" && kanto::cmd_code("nope").empty());
    kanto::LaunchOpts lo;
    lo.host = false; lo.code = "3F9A0C7D21"; lo.dir = "C:\\d\\apps\\KantoTogether\\"; lo.rom = "C:\\d\\rom.gba"; lo.saves = "C:\\s p"; lo.sound = true;
    std::string cl = kanto::launcher_cmdline(lo);
    CHECK(cl.find("join 3F9A0C7D21") != std::string::npos && cl.find("--unmute") != std::string::npos && cl.find("\"C:\\s p\"") != std::string::npos);
    lo.host = true; lo.sound = false;
    cl = kanto::launcher_cmdline(lo);
    CHECK(cl.find(" host ") != std::string::npos && cl.find("--unmute") == std::string::npos && cl.find("join") == std::string::npos);
    CHECK(kanto::patch_cmdline("C:\\d", "C:\\fr.gba").find("--rom \"C:\\fr.gba\"") != std::string::npos);

    /* output parsing: host */
    kanto::State s;
    s.host = true;
    CHECK(kanto::feed_line(s, "    ROOM CODE:  3F9A0C7D21") && s.code == "3F9A0C7D21" && s.code_gen == 1 && s.phase == kanto::Phase::WaitingForPartner);
    CHECK(!kanto::feed_line(s, "[launch] Starting the game window. Keep this black window open while you play. (Type  quit  here to stop.)"));
    CHECK(kanto::feed_line(s, ">>> Your partner is connected. <<<") && s.phase == kanto::Phase::Connected);
    CHECK(kanto::feed_line(s, "Your partner is not connected right now (the game keeps running; they can come back by themselves).") && s.phase == kanto::Phase::PartnerAway);
    CHECK(kanto::feed_line(s, "[bridge] relay connection lost; reconnecting in 1.2s (attempt 1)") && s.phase == kanto::Phase::Reconnecting);
    CHECK(kanto::feed_line(s, "Connection is back.") && s.phase == kanto::Phase::WaitingForPartner);
    CHECK(kanto::feed_line(s, "    NEW ROOM CODE (the old one stopped working):  AAAAAAAAAA") && s.code == "AAAAAAAAAA" && s.code_gen == 2);
    CHECK(!kanto::feed_line(s, "    ROOM CODE:  AAAAAAAAAA") || s.code_gen == 2);   // same code again: no new generation
    /* verbose prefix */
    kanto::State v; v.host = true;
    CHECK(kanto::feed_line(v, "[bridge] ROOM CODE: 0123456789  (give this to your partner)") && v.code == "0123456789");
    /* trading */
    CHECK(kanto::feed_line(s, "[bridge] lua: you offer slot 2: CHARMANDER lvl 5") && s.trade.phase == kanto::TradePhase::Offered && s.trade.my_slot == 2 && s.trade.mine == "CHARMANDER lvl 5");
    CHECK(kanto::feed_line(s, "[bridge] lua: peer offers: PIDGEY lvl 3  -> use `trade <slot>` then `confirm`") && s.trade.theirs == "PIDGEY lvl 3");
    CHECK(kanto::feed_line(s, "[bridge] lua: confirmed; waiting for the peer") && s.trade.i_confirmed);
    CHECK(kanto::feed_line(s, "[bridge] lua: peer confirmed the trade") && s.trade.peer_confirmed);
    CHECK(kanto::feed_line(s, "[bridge] lua: TRADE DONE: slot 2 now holds PIDGEY lvl 3") && s.trade.phase == kanto::TradePhase::Done && s.trades_done == 1 && s.trade.mine.empty());
    CHECK(kanto::feed_line(s, "[bridge] lua: trade cancelled: peer aborted") && s.trade.phase == kanto::TradePhase::Cancelled && s.trade.note.find("peer aborted") != std::string::npos);
    CHECK(kanto::feed_line(s, "[bridge] lua: usage: trade <party slot 1-6>") && s.trade.note.find("usage") != std::string::npos);
    CHECK(!kanto::feed_line(s, "[bridge] lua: coop.lua v1 loaded (mode=pose role=host bridge=127.0.0.1:8788)"));
    CHECK(!kanto::feed_line(s, ""));
    /* hostile trade text: out-of-range / huge slot numbers are ignored, never throw */
    {
        kanto::State h;
        CHECK(!kanto::feed_line(h, "[bridge] lua: you offer slot 99999999999999999999: X lvl 1") && h.trade.phase == kanto::TradePhase::None);
        CHECK(!kanto::feed_line(h, "[bridge] lua: you offer slot 7: X lvl 1") && !kanto::feed_line(h, "[bridge] lua: you offer slot 0: X lvl 1"));
        CHECK(kanto::feed_line(h, "[bridge] lua: you offer slot 6: X lvl 1") && h.trade.my_slot == 6);
    }
    /* guest errors + end */
    kanto::State g;
    CHECK(kanto::feed_line(g, "[bridge] Could not join: no such room") && g.phase == kanto::Phase::Failed && g.last_error == "no such room");
    kanto::State e;
    CHECK(kanto::feed_line(e, "[launch] mGBA exited") && e.phase == kanto::Phase::Ended && e.mgba_exited);
    kanto::State f2;
    CHECK(kanto::feed_line(f2, "[launch] Could not start: listen EADDRINUSE") && f2.phase == kanto::Phase::Failed);

    /* saves: newer wins, backup kept, only .sav */
    fs::path a = tmp / "sa", b = tmp / "sb";
    put(a / "KantoTogether.sav", "old"); put(a / "note.txt", "no"); put(a / "KantoTogether.ss0", "state");
    CHECK(kanto::sync_saves(a.string(), b.string()) == 1 && fs::exists(b / "KantoTogether.sav") && !fs::exists(b / "note.txt") && !fs::exists(b / "KantoTogether.ss0"));
    CHECK(kanto::sync_saves(a.string(), b.string()) == 0);
    put(a / "KantoTogether.sav", "newer-content");
    fs::last_write_time(a / "KantoTogether.sav", fs::last_write_time(b / "KantoTogether.sav") + std::chrono::seconds(10));
    CHECK(kanto::sync_saves(a.string(), b.string()) == 1);
    CHECK(fs::exists(b / ".kt-backup" / "KantoTogether.sav.1"));
    std::string back; winutil::read_file((b / "KantoTogether.sav").string(), &back);
    CHECK(back == "newer-content");
    fs::last_write_time(a / "KantoTogether.sav", fs::last_write_time(b / "KantoTogether.sav") - std::chrono::seconds(60));
    put(a / "x.txt", "");
    CHECK(kanto::sync_saves(a.string(), b.string()) == 0);   // dst newer: kept

    CHECK(!kanto::howto_text().empty() && kanto::howto_text().find("Trade panel") != std::string::npos);

    fs::remove_all(tmp);
    std::printf(fails ? "kanto_test: %d FAILED\n" : "kanto_test: all passed\n", fails);
    return fails ? 1 : 0;
}
