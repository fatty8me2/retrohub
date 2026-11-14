// Emulator process / save-session lane: XML entity decoding of Cemu's mlc_path, environment save/restore around a spawn,
// suspended-spawn helper, and the pending-session journal. Temp folders only; no emulator is ever started.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "emu_saves.h"
#include "emulators.h"
#include "env_scope.h"
#include "lichii.h"
#include "proc_spawn.h"
#include "session_journal.h"

namespace fs = std::filesystem;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                   \
        } else {                                                        \
            ++g_pass;                                                   \
        }                                                               \
    } while (0)

static void test_mlc_entities() {
    using hub::cemu_settings_mlc_path;
    CHECK(cemu_settings_mlc_path("<content><mlc_path>D:\\Wii U &amp; Games\\mlc</mlc_path></content>") == "D:\\Wii U & Games\\mlc");
    CHECK(cemu_settings_mlc_path("<mlc_path>  C:\\a&lt;b&gt;&quot;c&quot;&apos;d&apos;\r\n</mlc_path>") == "C:\\a<b>\"c\"'d'");
    CHECK(cemu_settings_mlc_path("<mlc_path>C:\\x&amp;lt;y</mlc_path>") == "C:\\x&lt;y");   // one pass only: no double decoding
    CHECK(cemu_settings_mlc_path("<mlc_path>C:\\a &bogus; b &amp</mlc_path>") == "C:\\a &bogus; b &amp");   // unknown entities and a lone & stay as written
    CHECK(cemu_settings_mlc_path("<mlc_path>D:\\plain\\mlc</mlc_path>") == "D:\\plain\\mlc");
    CHECK(cemu_settings_mlc_path("<mlc_path></mlc_path>").empty() && cemu_settings_mlc_path("<mlc_path/>").empty());
    CHECK(hub::xml_decode_entities("a&amp;&amp;b") == "a&&b");
}

static std::wstring getenv_w(const wchar_t* n, bool* present) {
    std::vector<wchar_t> b(1024);
    SetLastError(0);
    DWORD len = GetEnvironmentVariableW(n, b.data(), (DWORD)b.size());
    *present = len != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    return std::wstring(b.data(), len < b.size() ? len : 0);
}

static void test_env_scope() {
    const wchar_t* kSet = L"RHJ_TEST_PLAYER_SET";
    const wchar_t* kEmpty = L"RHJ_TEST_PLAYER_EMPTY";
    const wchar_t* kAbsent = L"RHJ_TEST_NOT_SET";
    const wchar_t* kLong = L"RHJ_TEST_LONG";
    const std::wstring longv(700, L'v');
    SetEnvironmentVariableW(kSet, L"player-value");
    SetEnvironmentVariableW(kEmpty, L"");
    SetEnvironmentVariableW(kAbsent, nullptr);
    SetEnvironmentVariableW(kLong, longv.c_str());
    bool p = false;
    auto saved = hubenv::apply({{kSet, L"hub"}, {kEmpty, L"x"}, {kAbsent, L"first"}, {kAbsent, L"second"}, {kLong, L"short"}});
    CHECK(getenv_w(kSet, &p) == L"hub" && p);   // what the child inherits
    CHECK(getenv_w(kAbsent, &p) == L"second" && p);
    hubenv::restore(saved);
    CHECK(getenv_w(kSet, &p) == L"player-value" && p);   // the player's own value is back, not removed
    CHECK(getenv_w(kEmpty, &p).empty() && p);             // set-but-empty stays set
    getenv_w(kAbsent, &p);
    CHECK(!p);                                             // never set: gone again
    std::vector<wchar_t> big(2048);
    DWORD n = GetEnvironmentVariableW(kLong, big.data(), (DWORD)big.size());
    CHECK(n == longv.size() && std::wstring(big.data(), n) == longv);   // a value longer than the first buffer survives
    CHECK(hubenv::apply({}).empty());
    SetEnvironmentVariableW(kSet, nullptr);
    SetEnvironmentVariableW(kEmpty, nullptr);
    SetEnvironmentVariableW(kLong, nullptr);
}

static void test_spawn_suspended_in_job() {
    wchar_t sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    const std::wstring cmd_exe = std::wstring(sysdir) + L"\\cmd.exe";
    hubproc::SpawnResult r;
    bool in_cb = false, suspended_in_cb = false, in_job_in_cb = false;
    unsigned long long created_cb = 0;
    bool ok = hubproc::spawn(cmd_exe, L"\"" + cmd_exe + L"\" /c exit 3", sysdir, CREATE_NO_WINDOW, true, [&](const hubproc::SpawnResult& s) {
        in_cb = true;
        suspended_in_cb = WaitForSingleObject(s.process, 0) == WAIT_TIMEOUT && s.pid != 0;   // not a single instruction has run yet
        BOOL in = FALSE;
        in_job_in_cb = s.job && IsProcessInJob(s.process, s.job, &in) && in;                  // already in the job, so nothing it starts can escape it
        created_cb = s.created;
    }, &r);
    CHECK(ok && r.process && r.job && r.pid);
    CHECK(in_cb && suspended_in_cb && in_job_in_cb && created_cb != 0 && created_cb == r.created);
    if (ok) {
        CHECK(WaitForSingleObject(r.process, 15000) == WAIT_OBJECT_0);
        DWORD code = 0;
        GetExitCodeProcess(r.process, &code);
        CHECK(code == 3);
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION ji{};
        CHECK(QueryInformationJobObject(r.job, JobObjectBasicAccountingInformation, &ji, sizeof ji, nullptr) && ji.TotalProcesses >= 1);
        unsigned long long c2 = 0;
        CHECK(hubproc::creation_time(r.process, &c2) && c2 == r.created);
        CloseHandle(r.process);
        CloseHandle(r.job);
    }
    hubproc::SpawnResult r2;   // no job asked for: none made
    ok = hubproc::spawn(cmd_exe, L"\"" + cmd_exe + L"\" /c exit 0", sysdir, CREATE_NO_WINDOW, false, nullptr, &r2);
    CHECK(ok && !r2.job && r2.process);
    if (ok) { CHECK(WaitForSingleObject(r2.process, 15000) == WAIT_OBJECT_0); CloseHandle(r2.process); }
    hubproc::SpawnResult r3;   // a missing program: false, the Windows error is kept, no handles leak out
    ok = hubproc::spawn(std::wstring(sysdir) + L"\\no-such-program-rhj.exe", L"x", sysdir, CREATE_NO_WINDOW, true, nullptr, &r3);
    CHECK(!ok && r3.error != 0 && !r3.process && !r3.job);
}

static std::string U8(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
static std::string rd(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void wr(const fs::path& p, const std::string& t, int age_seconds = 0) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    { std::ofstream f(p, std::ios::binary | std::ios::trunc); f.write(t.data(), (std::streamsize)t.size()); }
    fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::seconds(age_seconds), ec);
}
static int count_files(const fs::path& root, const std::string& ext = "") {
    int n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && (ext.empty() || it->path().extension() == ext)) ++n;
    return n;
}

/* a process id that is gone: a child that ran and ended */
static void dead_process(unsigned long* pid, unsigned long long* created) {
    wchar_t sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    const std::wstring cmd = std::wstring(sysdir) + L"\\cmd.exe";
    hubproc::SpawnResult r;
    hubproc::spawn(cmd, L"\"" + cmd + L"\" /c exit 0", sysdir, CREATE_NO_WINDOW, false, nullptr, &r);
    WaitForSingleObject(r.process, 15000);
    *pid = r.pid;
    *created = r.created;
    CloseHandle(r.process);
}

struct Rig {
    fs::path d, vault, inst, battery, sd, jdir;
    hubemu::Install in;
    hub::GameEntry e;
    Rig(const char* name) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        d = fs::path(tmp) / (std::string("rhj_test_") + name + "_" + std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(d, ec);
        vault = d / "vault";
        inst = d / "azahar";
        battery = vault / "3ds" / "3ds-0004000000055d00" / "battery";
        jdir = d / "data" / "pending_sessions";
        fs::create_directories(inst);
        in.dir = U8(inst);
        in.exe = U8(inst / "azahar.exe");
        in.managed = true;
        e.system = hub::System::N3DS;
        e.key = "3ds:0004000000055d00";
        const std::string z(32, '0');
        sd = inst / "user" / "sdmc" / "Nintendo 3DS" / z / z;
    }
    ~Rig() { std::error_code ec; fs::remove_all(d, ec); }
    fs::path save() const { return sd / "title" / "00040000" / "00055d00" / "data" / "00000001.sav"; }
    fs::path vsave() const { return battery / "title" / "00040000" / "00055d00" / "data" / "00000001.sav"; }
    hubjournal::Entry entry(const hubemu::SaveSession& s) const {
        hubjournal::Entry j;
        j.emu_kind = 7;
        j.system_short = "3ds";
        j.game_key = e.key;
        j.title = "Test Game";
        j.sess = s;
        j.emu_exe = in.exe;
        j.inst_dir = in.dir;
        j.inst_managed = true;
        return j;
    }
    hubjournal::Config cfg() const {
        hubjournal::Config c;
        const std::string bat = U8(battery);
        c.battery_dir = [bat](const std::string&, const std::string&) { return bat; };
        const std::string dir = in.dir;
        c.install = [dir](const hubjournal::Entry&, std::string* d, bool* m) { *d = dir; *m = true; return true; };
        return c;
    }
    std::string jdir_s() const { return U8(jdir) + "\\"; }
};

static void test_roundtrip_and_corrupt() {
    Rig r("rt");
    hubjournal::Entry j = r.entry(hubemu::begin_session(r.e, r.in, U8(r.battery), ""));
    j.id = "abc-1";
    j.title = "T\tab\nnew \\ line \xC3\xA9";
    j.pid = 4242; j.created = 0x01dbaa11223344ull; j.attempts = 2; j.game_path = "D:/roms/g/tab	game.wua";
    j.nx_install_dir = "C:\\x"; j.nx_title_id = 0x0100abcd12340000ull; j.cemu_user = "D:\\c"; j.cemu_tid = 5; j.revert_profile = true; j.revert_own_settings = true;
    hubjournal::Entry k;
    CHECK(hubjournal::parse(hubjournal::serialize(j), &k));
    CHECK(k.title == j.title && k.pid == 4242 && k.created == j.created && k.attempts == 2 && k.nx_title_id == j.nx_title_id && k.cemu_tid == 5);
    CHECK(k.game_path == j.game_path);
    CHECK(k.sess.rels == j.sess.rels && k.sess.battery == j.sess.battery && k.sess.active && k.revert_profile && k.revert_own_settings && k.emu_kind == 7);
    for (int kind : {100, 101}) {   // Hookshot / Longshot native builds (hookshot::info(f).journal_kind)
        hubjournal::Entry jn = j, kn;
        jn.emu_kind = kind;
        CHECK(hubjournal::parse(hubjournal::serialize(jn), &kn) && kn.emu_kind == kind && kn.sess.rels == j.sess.rels);
    }
    const std::string good = hubjournal::serialize(j);
    hubjournal::Entry x;
    CHECK(!hubjournal::parse("", &x) && !hubjournal::parse("garbage\n", &x));
    CHECK(!hubjournal::parse(good.substr(0, good.size() / 2), &x));   // cut off
    CHECK(!hubjournal::parse(good.substr(0, good.size() - 1), &x));   // no final newline
    std::string evil = good;
    evil.replace(evil.find("rel\t"), 4, "rel\t..\\..\\");
    CHECK(!hubjournal::parse(evil, &x));                                // leaves the save folder
    // on disk: unreadable / corrupt files are renamed aside and never trusted
    fs::create_directories(r.jdir);
    wr(r.jdir / "bad1.session", "not a journal");
    wr(r.jdir / "bad2.session", good.substr(0, 40));
    wr(r.jdir / "other.session", good);   // file name does not match its id
    hubjournal::RecoverReport rep = hubjournal::recover(r.jdir_s(), r.cfg(), nullptr);
    CHECK(rep.corrupt == 3 && rep.replayed == 0);
    CHECK(!fs::exists(r.jdir / "bad1.session") && fs::exists(r.jdir / "bad1.session.corrupt") && count_files(r.jdir, ".session") == 0);
    CHECK(hubjournal::recover(r.jdir_s(), r.cfg(), nullptr).corrupt == 0);
}

static void test_recover_after_hub_exit() {
    Rig r("rec");
    wr(r.vsave(), "OLD", 600);   // the vault holds an older save
    hubemu::SaveSession s = hubemu::begin_session(r.e, r.in, U8(r.battery), "");   // restores it into the emulator
    CHECK(s.active && rd(r.save()) == "OLD");
    hubjournal::Entry j = r.entry(s);
    dead_process(&j.pid, &j.created);
    CHECK(hubjournal::begin(r.jdir_s(), j) && !j.id.empty() && fs::exists(r.jdir / (j.id + ".session")));
    hubjournal::forget_live();   // the hub is closed while the game runs ... and the game ends; nothing ran the after-exit work
    wr(r.save(), "NEW-PROGRESS", 0);
    int after_calls = 0;
    hubjournal::RecoverReport rep = hubjournal::recover(r.jdir_s(), r.cfg(), [&](const hubjournal::Entry& e) { ++after_calls; CHECK(e.game_key == r.e.key && e.system_short == "3ds"); });
    CHECK(rep.replayed == 1 && rep.failed == 0 && rep.corrupt == 0 && after_calls == 1);
    CHECK(rd(r.vsave()) == "NEW-PROGRESS");
    CHECK(count_files(r.vault / "_conflicts") >= 1);   // the older vault file was kept first
    CHECK(count_files(r.jdir, ".session") == 0);
    // double recovery: exactly once
    hubjournal::RecoverReport rep2 = hubjournal::recover(r.jdir_s(), r.cfg(), [&](const hubjournal::Entry&) { ++after_calls; });
    CHECK(rep2.replayed == 0 && after_calls == 1 && rd(r.vsave()) == "NEW-PROGRESS");
    // the normal path removes it too
    hubemu::SaveSession s2 = hubemu::begin_session(r.e, r.in, U8(r.battery), "");
    hubjournal::Entry j2 = r.entry(s2);
    CHECK(hubjournal::begin(r.jdir_s(), j2) && count_files(r.jdir, ".session") == 1);
    wr(r.save(), "NEWER-STILL", 0);
    hubjournal::finish(s2, j2, r.jdir_s());
    CHECK(rd(r.vsave()) == "NEWER-STILL" && count_files(r.jdir, ".session") == 0 && s2.error.empty());
}

static void test_recover_while_running() {
    Rig r("run");
    hubemu::SaveSession s = hubemu::begin_session(r.e, r.in, U8(r.battery), "");
    hubjournal::Entry j = r.entry(s);
    j.pid = GetCurrentProcessId();   // a process that certainly runs
    hubproc::creation_time(GetCurrentProcess(), &j.created);
    CHECK(hubjournal::process_running(j.pid, j.created) && !hubjournal::process_running(0, 0));
    {   // no creation time recorded: the image path decides; with nothing to compare only an old journal counts as a recycled pid
        wchar_t mew[2048] = {};
        GetModuleFileNameW(nullptr, mew, 2048);
        const std::string me = U8(fs::path(mew));   // UTF-8, also from a folder with non-ANSI letters
        const long long now = (long long)time(nullptr);
        CHECK(!hubjournal::process_running(j.pid, 0, "C:\\bad\xEB\xFF" ".exe", now));   // invalid UTF-8 never throws
        CHECK(hubjournal::process_running(j.pid, 0, me, now));                          // same image: running
        CHECK(!hubjournal::process_running(j.pid, 0, "C:\nowhere\other.exe", now));   // another image: recycled pid
        CHECK(hubjournal::process_running(j.pid, 0, "", now));                          // nothing to compare, fresh journal: stay careful
        CHECK(!hubjournal::process_running(j.pid, 0, "", now - 25 * 3600));             // nothing to compare, over a day old
        CHECK(hubjournal::process_running(j.pid, 0, "", 0));                            // no start time either: running
    }
    CHECK(hubjournal::begin(r.jdir_s(), j));
    hubjournal::forget_live();
    wr(r.save(), "WRITTEN-WHILE-RUNNING", 0);
    hubjournal::RecoverReport rep = hubjournal::recover(r.jdir_s(), r.cfg(), nullptr);
    CHECK(rep.still_running == 1 && rep.replayed == 0);
    CHECK(!fs::exists(r.vsave()) && count_files(r.jdir, ".session") == 1);   // nothing copied, the journal waits
    // the same pid with another creation time is a recycled pid: not the emulator
    CHECK(!hubjournal::process_running(j.pid, j.created + 10000000ull));
    hubjournal::Entry j3 = j;
    j3.created += 10000000ull;
    CHECK(hubjournal::update(r.jdir_s(), j3));
    rep = hubjournal::recover(r.jdir_s(), r.cfg(), nullptr);
    CHECK(rep.replayed == 1 && rd(r.vsave()) == "WRITTEN-WHILE-RUNNING" && count_files(r.jdir, ".session") == 0);
    // a session of this hub that is still live defers the replay of the same emulator
    hubemu::SaveSession s4 = hubemu::begin_session(r.e, r.in, U8(r.battery), "");
    hubjournal::Entry j4 = r.entry(s4);
    dead_process(&j4.pid, &j4.created);
    CHECK(hubjournal::begin(r.jdir_s(), j4));   // registered live in this process
    wr(r.save(), "LIVE-SESSION-DATA", 0);
    rep = hubjournal::recover(r.jdir_s(), r.cfg(), nullptr);
    CHECK(rep.deferred == 1 && rep.replayed == 0 && rd(r.vsave()) == "WRITTEN-WHILE-RUNNING");
    hubjournal::forget_live();
}

static void test_cemu_reverts_and_spawn_failure() {
    Rig r("cemu");
    fs::path ud = r.inst / "portable";
    const std::string orig = hubemu::cemu_default_settings_xml();
    wr(ud / "settings.xml", orig);
    hub::gs::Values v;
    v["fullscreen"] = "false";
    v["wiiu.graphic_packs"] = "true";
    const uint64_t tid = 0x0005000010145c00ull;
    CHECK(hubemu::prepare_cemu_own_settings(U8(ud), v));
    hub::gs::Values none;
    hubemu::apply_cemu_game_profile(U8(ud), tid, none);
    CHECK(fs::exists(ud / "retrohub-cemu-state.txt"));
    CHECK(count_files(ud / "gameProfiles") == 2);
    hubemu::SaveSession inactive;   // no save mapping, only temporary settings to give back
    hubjournal::Entry j = r.entry(inactive);
    j.emu_kind = 1; j.cemu_user = U8(ud); j.cemu_tid = tid; j.revert_profile = true; j.revert_own_settings = true;
    // spawn failure: the overrides are put back and the journal is gone
    CHECK(hubjournal::begin(r.jdir_s(), j) && count_files(r.jdir, ".session") == 1);
    hubjournal::abort_unspawned(j, r.jdir_s(), true);
    CHECK(rd(ud / "settings.xml") == orig && !fs::exists(ud / "retrohub-cemu-state.txt") && count_files(ud / "gameProfiles") == 0 && count_files(r.jdir, ".session") == 0);
    // busy case (another game owns them): journal dropped, files left alone
    hubemu::prepare_cemu_own_settings(U8(ud), v);
    j.id.clear();
    CHECK(hubjournal::begin(r.jdir_s(), j));
    hubjournal::abort_unspawned(j, r.jdir_s(), false);
    CHECK(fs::exists(ud / "retrohub-cemu-state.txt") && count_files(r.jdir, ".session") == 0);
    // hub closed during play: the next start gives the player's Cemu settings back
    hubemu::apply_cemu_game_profile(U8(ud), tid, none);
    dead_process(&j.pid, &j.created);
    j.id.clear();
    CHECK(hubjournal::begin(r.jdir_s(), j));
    hubjournal::forget_live();
    hubjournal::RecoverReport rep = hubjournal::recover(r.jdir_s(), r.cfg(), nullptr);
    CHECK(rep.replayed == 1 && rd(ud / "settings.xml") == orig && count_files(ud / "gameProfiles") == 0 && !fs::exists(ud / "retrohub-cemu-state.txt"));
    rep = hubjournal::recover(r.jdir_s(), r.cfg(), nullptr);   // a second time: nothing
    CHECK(rep.replayed == 0 && rd(ud / "settings.xml") == orig);
}

static void test_switch_account_folder_at_exit() {
    Rig r("nx");
    const char* tid = "0100abcd12340000";
    hub::GameEntry e;
    e.system = hub::System::Switch;
    e.key = std::string("switch:") + tid;
    fs::path bat = r.vault / "switch" / ("switch_" + std::string(tid)) / "battery";
    fs::path pc = r.d / "lichii";
    hubemu::Install in;
    in.dir = U8(pc);
    in.exe = U8(pc / "lichii.exe");
    in.managed = true;
    hubemu::SaveSession s = hubemu::begin_session(e, in, U8(bat), "");
    CHECK(s.active && s.rels.size() == 1);   // no account folder existed at launch
    const std::string uuid = hubemu::lichii_ensure_profile(in.dir);
    fs::path acct = pc / "user" / "nand" / "user" / "save" / "account" / uuid / "0100ABCD12340000";
    wr(acct / "first.sav", "FIRST-SESSION");   // the emulator created it during play
    wr(pc / "user" / hubemu::lichii_save_rel(uuid, 0x0100abcd12340000ull) / "0" / "main.sav", "MAIN");
    hubjournal::Entry j = r.entry(s);
    j.emu_kind = 6; j.nx_install_dir = in.dir; j.nx_title_id = 0x0100abcd12340000ull;
    hubemu::SaveSession plain = s;   // without the exit-time re-evaluation the account folder is missed
    hubemu::end_session(plain);
    CHECK(!fs::exists(bat / "save-account"));
    CHECK(hubjournal::begin(r.jdir_s(), j));
    hubjournal::finish(s, j, r.jdir_s());
    CHECK(s.error.empty() && rd(bat / "save-account" / "0100ABCD12340000" / "first.sav") == "FIRST-SESSION");
    CHECK(rd(bat / "save" / "0100ABCD12340000" / "0" / "main.sav") == "MAIN" && s.rels.size() == 2 && s.vault_rels.size() == 2);
    // no account folder at all: a plain no-op
    Rig r2("nx2");
    fs::path pc2 = r2.d / "lichii";
    hubemu::Install in2 = in;
    in2.dir = U8(pc2);
    in2.exe = U8(pc2 / "lichii.exe");
    fs::path bat2 = r2.vault / "switch" / ("switch_" + std::string(tid)) / "battery";
    hubemu::SaveSession s2 = hubemu::begin_session(e, in2, U8(bat2), "");
    hubjournal::Entry j2 = r2.entry(s2);
    j2.nx_install_dir = in2.dir; j2.nx_title_id = 0x0100abcd12340000ull;
    hubjournal::finish(s2, j2, r2.jdir_s());
    CHECK(s2.error.empty() && !fs::exists(bat2 / "save-account") && s2.rels.size() == 1);
    // merge helper: already-listed folders are not added twice
    hubemu::NxSaveMap m;
    m.rels = s.rels; m.vault_rels = s.vault_rels;
    CHECK(!hubjournal::merge_nx_map(s, m));
}

static void test_tampered_and_stale() {
    Rig r("stale");
    wr(r.vsave(), "VAULT-OLD", 600);
    hubemu::SaveSession s = hubemu::begin_session(r.e, r.in, U8(r.battery), "");
    wr(r.save(), "EMU-NEW", 0);
    hubjournal::Entry j = r.entry(s);
    dead_process(&j.pid, &j.created);
    const fs::path evil = r.d / "evil";
    wr(evil / "x.sav", "EVIL");
    // 1. an absolute emulator folder written into the journal: refused, nothing copied, the journal is kept
    {
        hubjournal::Entry t = j;
        t.sess.emu_root = U8(evil);
        t.sess.rels = {"."};
        t.sess.rels = {"x"};
        CHECK(hubjournal::begin(r.jdir_s(), t));
        hubjournal::forget_live();
        hubjournal::RecoverReport rep = hubjournal::recover(r.jdir_s(), r.cfg(), nullptr);
        CHECK(rep.replayed == 0 && rep.failed == 1 && !rep.warnings.empty());
        CHECK(rd(r.vsave()) == "VAULT-OLD" && !fs::exists(r.battery / "x"));
        CHECK(fs::exists(r.jdir / (t.id + ".session")));
        // three tries, then it is set aside as *.stale, never lost and never trusted
        hubjournal::recover(r.jdir_s(), r.cfg(), nullptr);
        rep = hubjournal::recover(r.jdir_s(), r.cfg(), nullptr);
        CHECK(count_files(r.jdir, ".session") == 0 && fs::exists(r.jdir / (t.id + ".session.stale")));
        CHECK(rep.warnings.size() >= 2);
    }
    // 2. a battery folder outside the real vault
    {
        hubjournal::Entry t = j;
        t.id.clear();
        t.sess.battery = U8(evil / "battery");
        CHECK(hubjournal::begin(r.jdir_s(), t));
        hubjournal::forget_live();
        hubjournal::RecoverReport rep = hubjournal::recover(r.jdir_s(), r.cfg(), nullptr);
        CHECK(rep.replayed == 0 && rep.failed == 1 && !fs::exists(evil / "battery") && rd(r.vsave()) == "VAULT-OLD");
        fs::remove(r.jdir / (t.id + ".session"));
    }
    // 3. ".." in a folder name: not even parsed
    {
        std::string text = hubjournal::serialize(j);
        text.replace(text.find("rel\t"), 4, "rel\t..\\..\\evil");
        hubjournal::Entry t;
        CHECK(!hubjournal::parse(text, &t));
    }
    // 4. the vault was moved since the launch: kept (not replayed, not dropped), then set aside after 3 tries
    {
        hubjournal::Entry t = j;
        t.id.clear();
        CHECK(hubjournal::begin(r.jdir_s(), t));
        hubjournal::forget_live();
        hubjournal::Config moved = r.cfg();
        moved.battery_dir = [&](const std::string&, const std::string&) { return U8(r.d / "other-vault" / "3ds" / "3ds-0004000000055d00" / "battery"); };
        hubjournal::RecoverReport rep = hubjournal::recover(r.jdir_s(), moved, nullptr);
        CHECK(rep.replayed == 0 && rep.failed == 1 && fs::exists(r.jdir / (t.id + ".session")) && rd(r.vsave()) == "VAULT-OLD");
        // the emulator was reinstalled somewhere else
        hubjournal::Config reinst = r.cfg();
        reinst.install = [&](const hubjournal::Entry&, std::string* dir, bool* managed) { *dir = U8(r.d / "azahar2"); *managed = true; return true; };
        rep = hubjournal::recover(r.jdir_s(), reinst, nullptr);
        CHECK(rep.replayed == 0 && rep.failed == 1 && rd(r.vsave()) == "VAULT-OLD");
        // not installed at all
        hubjournal::Config gone = r.cfg();
        gone.install = [](const hubjournal::Entry&, std::string*, bool*) { return false; };
        rep = hubjournal::recover(r.jdir_s(), gone, nullptr);
        CHECK(rep.replayed == 0 && rep.failed == 1 && fs::exists(r.jdir / (t.id + ".session.stale")));
        // the right setup finds nothing left to do (the journal was set aside, the emulator's save is still there for the next launch)
        CHECK(rd(r.save()) == "EMU-NEW");
    }
    // 5. a matching setup does replay (control)
    {
        hubjournal::Entry t = j;
        t.id.clear();
        CHECK(hubjournal::begin(r.jdir_s(), t));
        hubjournal::forget_live();
        hubjournal::RecoverReport rep = hubjournal::recover(r.jdir_s(), r.cfg(), nullptr);
        CHECK(rep.replayed == 1 && rd(r.vsave()) == "EMU-NEW");
    }
}

static void test_launch_replay_lock() {
    Rig r("lock");
    hubemu::SaveSession s = hubemu::begin_session(r.e, r.in, U8(r.battery), "");
    hubjournal::Entry j = r.entry(s);
    dead_process(&j.pid, &j.created);
    CHECK(hubjournal::begin(r.jdir_s(), j));
    hubjournal::forget_live();
    wr(r.save(), "FROM-EMULATOR", 0);
    // A. a replay is running: a launch cannot take the lock until it is done
    std::atomic<bool> in_replay{false}, release{false};
    hubjournal::set_replay_hook([&] { in_replay = true; while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5)); });
    hubjournal::RecoverReport rep;
    std::thread th([&] { rep = hubjournal::recover(r.jdir_s(), r.cfg(), nullptr); });
    for (int i = 0; i < 400 && !in_replay; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(in_replay);
    {
        std::unique_lock<std::mutex> lk(hubjournal::session_lock(), std::try_to_lock);
        CHECK(!lk.owns_lock());   // the UI thread would say "try again" instead of freezing
    }
    release = true;
    th.join();
    hubjournal::set_replay_hook(nullptr);
    CHECK(rep.replayed == 1 && rd(r.vsave()) == "FROM-EMULATOR");
    {
        std::unique_lock<std::mutex> lk(hubjournal::session_lock(), std::try_to_lock);
        CHECK(lk.owns_lock());
    }
    // B. a launch is in progress (lock held, its session registered): a replay that arrives meanwhile waits, then re-checks inside the lock
    //    and finds the emulator live again: it must NOT copy anything
    hubemu::SaveSession s2 = hubemu::begin_session(r.e, r.in, U8(r.battery), "");
    hubjournal::Entry old = r.entry(s2);
    dead_process(&old.pid, &old.created);
    old.sess.active = true;
    CHECK(hubjournal::begin(r.jdir_s(), old));
    hubjournal::forget_live();
    wr(r.save(), "STALE-LEFTOVER", 0);
    int replays_seen = 0;
    hubjournal::set_replay_hook([&] { ++replays_seen; });
    std::unique_lock<std::mutex> launch(hubjournal::session_lock());   // the launch starts: holds the lock from before begin_session
    std::atomic<bool> done{false};
    hubjournal::RecoverReport rep2;
    std::thread th2([&] { rep2 = hubjournal::recover(r.jdir_s(), r.cfg(), nullptr); done = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!done && replays_seen == 0);   // blocked behind the launch
    hubjournal::Entry fresh = r.entry(s2);
    fresh.pid = GetCurrentProcessId();
    hubproc::creation_time(GetCurrentProcess(), &fresh.created);
    CHECK(hubjournal::begin(r.jdir_s(), fresh));   // journal written, spawn returned
    launch.unlock();
    th2.join();
    CHECK(rep2.replayed == 0 && replays_seen == 0 && rep2.deferred + rep2.still_running >= 1);
    CHECK(rd(r.vsave()) == "FROM-EMULATOR");   // not touched
    hubjournal::set_replay_hook(nullptr);
    hubjournal::forget_live();
}

static void test_image_running_and_stop() {
    Rig r("img");
    wchar_t sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    const fs::path src = fs::path(sysdir) / "cmd.exe";
    const fs::path managed = r.d / "managed-emu";
    fs::create_directories(managed);
    const fs::path copy = managed / "cmd.exe";
    std::error_code ec;
    fs::copy_file(src, copy, fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    hubproc::SpawnResult sp;
    bool ok = hubproc::spawn(copy.wstring(), L"\"" + copy.wstring() + L"\" /c ping -n 20 127.0.0.1 >nul", sysdir, CREATE_NO_WINDOW, false, nullptr, &sp);
    CHECK(ok);
    if (ok) {
        CHECK(hubjournal::image_running(U8(copy), ""));                                // the exact image
        CHECK(hubjournal::image_running("", U8(managed)));                              // anything running from the managed folder
        CHECK(!hubjournal::image_running(U8(r.d / "elsewhere" / "cmd.exe"), ""));        // same exe NAME in another folder: unrelated
        CHECK(!hubjournal::image_running(U8(r.d / "elsewhere" / "cmd.exe"), U8(r.d / "elsewhere")));
        TerminateProcess(sp.process, 0);   // by handle: our own child
        WaitForSingleObject(sp.process, 5000);
        CloseHandle(sp.process);
        CHECK(!hubjournal::image_running(U8(copy), U8(managed)));
    }
    // a replay whose emulator image runs again waits; an unrelated same-name program elsewhere does not block it
    wr(r.vsave(), "OLD", 600);
    hubemu::SaveSession s = hubemu::begin_session(r.e, r.in, U8(r.battery), "");
    wr(r.save(), "NEW", 0);
    hubjournal::Entry j = r.entry(s);
    dead_process(&j.pid, &j.created);
    j.emu_exe = U8(r.d / "elsewhere" / "azahar.exe");   // not running anywhere
    CHECK(hubjournal::begin(r.jdir_s(), j));
    hubjournal::forget_live();
    CHECK(hubjournal::recover(r.jdir_s(), r.cfg(), nullptr).replayed == 1);
    // stop flag: a cancelled replay keeps the journal and counts no attempt
    wr(r.save(), "NEWER", 0);
    hubjournal::Entry j2 = r.entry(hubemu::begin_session(r.e, r.in, U8(r.battery), ""));
    dead_process(&j2.pid, &j2.created);
    CHECK(hubjournal::begin(r.jdir_s(), j2));
    hubjournal::forget_live();
    std::atomic<bool> stop{true};
    hubjournal::RecoverReport rep = hubjournal::recover(r.jdir_s(), r.cfg(), nullptr, &stop);
    CHECK(rep.replayed == 0 && rep.failed == 0 && count_files(r.jdir, ".session") == 1);
    hubjournal::stop_recovery();   // never started: harmless, twice
    hubjournal::stop_recovery();
    rep = hubjournal::recover(r.jdir_s(), r.cfg(), nullptr);
    CHECK(rep.replayed == 1 && rd(r.vsave()) == "NEWER");
}

static void test_spawn_observer_throws() {
    wchar_t sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    const std::wstring cmd_exe = std::wstring(sysdir) + L"\\cmd.exe";
    hubproc::SpawnResult r;
    bool ok = hubproc::spawn(cmd_exe, L"\"" + cmd_exe + L"\" /c exit 5", sysdir, CREATE_NO_WINDOW, true, [](const hubproc::SpawnResult&) { throw std::runtime_error("observer failed"); }, &r);
    CHECK(ok && r.process);   // the game still starts: never left suspended
    if (ok) {
        CHECK(WaitForSingleObject(r.process, 15000) == WAIT_OBJECT_0);
        DWORD code = 0;
        GetExitCodeProcess(r.process, &code);
        CHECK(code == 5);
        CloseHandle(r.process);
        if (r.job) CloseHandle(r.job);
    }
}

int main() {
    test_tampered_and_stale();
    test_launch_replay_lock();
    test_image_running_and_stop();
    test_spawn_observer_throws();
    test_roundtrip_and_corrupt();
    test_recover_after_hub_exit();
    test_recover_while_running();
    test_cemu_reverts_and_spawn_failure();
    test_switch_account_folder_at_exit();
    test_env_scope();
    test_spawn_suspended_in_job();
    test_mlc_entities();
    std::printf("session journal / emu process tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
