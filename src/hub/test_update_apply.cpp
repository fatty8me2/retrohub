// Console test for the native update applier: pure logic, apply_files (incl. rollback) and an
// integration dry-run of `RetroHub.exe --apply-update` against scratch dirs (non-ASCII path,
// locked-file rollback, wait-for-pid). Never shows a window and never relaunches anything
// (--no-relaunch). Usage: hub_apply_test <path to RetroHub.exe>
//
// Plan 4 cases: the applier NEVER terminates a game or emulator (a fake game process under the install folder is waited for, and
// survives); a failed restore keeps the backup; startup recovery repairs from it (and refuses to downgrade); a failed update leaves a
// status note and keeps its staged files. The "fake game" is a copy of this test exe started with `--sleep <ms>`; only processes
// this test started are ever ended.
#include "update_apply.h"
#include "winutil.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fs = std::filesystem;
using namespace hubupdate::apply;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

static void put(const fs::path& p, const std::string& s) {
    std::error_code ec; fs::create_directories(p.parent_path(), ec);
    std::ofstream o(p, std::ios::binary | std::ios::trunc); o << s;
}
static std::string get(const fs::path& p) {
    std::ifstream i(p, std::ios::binary); std::stringstream ss; ss << i.rdbuf(); return ss.str();
}
static bool has(const fs::path& p) { std::error_code ec; return fs::exists(p, ec); }

/* Installed files are older than the staged ones, as in real life (a rollback or a recovery must not mistake one for the other). */
static void age_files(const fs::path& dir) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) fs::last_write_time(it->path(), fs::file_time_type::clock::now() - std::chrono::hours(48), ec);
}

/* staged: new version of everything; install: old version + user data that must survive. */
static void make_scene(const fs::path& root, fs::path* staged, fs::path* install) {
    std::error_code ec; fs::remove_all(root, ec);
    *staged = root / L"staged"; *install = root / L"RetroHub";
    put(*staged / L"RetroHub.exe", "new-hub");
    put(*staged / L"BattleShip.exe", "new-bs");
    put(*staged / L"0new.txt", "brand new");
    put(*staged / L"a.txt", "a-new");
    put(*staged / L"b.dat", "b-new");
    put(*staged / L"sub\\deep\\c.txt", "c-new");
    put(*staged / L"z.txt", "z-new");
    put(*staged / L"BattleShip.o2r", "o2r-from-zip");        // must be skipped
    put(*staged / L"userdata\\hub.json", "zip-userdata");     // must be skipped
    put(*staged / L"logs\\x.log", "zip-log");                 // must be skipped
    put(*install / L"RetroHub.exe", "old-hub");
    put(*install / L"BattleShip.exe", "old-bs");
    put(*install / L"a.txt", "a-old");
    put(*install / L"b.dat", "b-old");
    put(*install / L"z.txt", "z-old");
    put(*install / L"BattleShip.o2r", "user-o2r");
    put(*install / L"userdata\\hub.json", "user-settings");
    put(*install / L"userdata\\saves\\mario.sav", "user-save");
    put(*install / L"extra-user-file.txt", "keep me");        // never deleted
    age_files(*install);
}

static void check_untouched(const fs::path& install) {
    CHECK(get(install / L"BattleShip.o2r") == "user-o2r");
    CHECK(get(install / L"userdata\\hub.json") == "user-settings");
    CHECK(get(install / L"userdata\\saves\\mario.sav") == "user-save");
    CHECK(get(install / L"extra-user-file.txt") == "keep me");
    CHECK(!has(install / L"logs\\x.log"));
}
static void check_old(const fs::path& install) {
    CHECK(get(install / L"RetroHub.exe") == "old-hub");
    CHECK(get(install / L"BattleShip.exe") == "old-bs");
    CHECK(get(install / L"a.txt") == "a-old");
    CHECK(get(install / L"b.dat") == "b-old");
    CHECK(get(install / L"z.txt") == "z-old");
    CHECK(!has(install / L"0new.txt"));
    CHECK(!has(install / L"sub"));  // new dirs' files removed (dir may remain, files must not)
}
static void check_new(const fs::path& install) {
    CHECK(get(install / L"RetroHub.exe") == "new-hub");
    CHECK(get(install / L"BattleShip.exe") == "new-bs");
    CHECK(get(install / L"0new.txt") == "brand new");
    CHECK(get(install / L"a.txt") == "a-new");
    CHECK(get(install / L"b.dat") == "b-new");
    CHECK(get(install / L"sub\\deep\\c.txt") == "c-new");
    CHECK(get(install / L"z.txt") == "z-new");
}

/* Runs a command line hidden; returns exit code or -1. */
static int run_hidden(std::wstring cmd, DWORD* pidOut = nullptr, bool wait = true) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    if (pidOut) *pidOut = pi.dwProcessId;
    DWORD code = 0;
    if (wait) { WaitForSingleObject(pi.hProcess, 60000); GetExitCodeProcess(pi.hProcess, &code); }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return wait ? (int)code : 0;
}

/* ---- fake games: a copy of this exe that just sleeps. Only processes started here are ever ended. ---- */
struct Proc { HANDLE h = nullptr; DWORD pid = 0; };
static std::vector<Proc> g_spawned;
static std::wstring self_exe() { wchar_t b[2048]; DWORD n = GetModuleFileNameW(nullptr, b, 2048); return std::wstring(b, n); }
static Proc spawn_sleeper(const fs::path& exe, unsigned ms) {
    std::error_code ec;
    fs::create_directories(exe.parent_path(), ec);
    fs::copy_file(self_exe(), exe, fs::copy_options::overwrite_existing, ec);
    std::wstring cmd = L"\"" + exe.wstring() + L"\" --sleep " + std::to_wstring(ms);
    STARTUPINFOW si{}; si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    Proc p;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        p.h = pi.hProcess; p.pid = pi.dwProcessId;
        CloseHandle(pi.hThread);
        g_spawned.push_back(p);
    }
    return p;
}
static bool proc_alive(const Proc& p) { return p.h && WaitForSingleObject(p.h, 0) == WAIT_TIMEOUT; }
static void proc_kill(Proc& p) {
    if (!p.h) return;
    if (proc_alive(p)) TerminateProcess(p.h, 0);
    WaitForSingleObject(p.h, 5000);
}
static void kill_spawned() { for (auto& p : g_spawned) { proc_kill(p); if (p.h) { CloseHandle(p.h); p.h = nullptr; } } g_spawned.clear(); }
static double secs_since(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }

/* A recovery manifest as the applier writes it (hand-built so the test needs no private API). */
static void put_manifest(const fs::path& dir, const fs::path& install, const char* from, const char* to, DWORD pid) {
    std::string inst = winutil::to_utf8(install.wstring()), esc;
    for (char c : inst) { if (c == '\\' || c == '"') esc += '\\'; esc += c; }
    put(dir / L"manifest.json", "{\"v\":1,\"install\":\"" + esc + "\",\"from\":\"" + from + "\",\"to\":\"" + to + "\",\"pid\":" + std::to_string(pid) +
                                ",\"started\":1,\"reported\":false}");
}

static HANDLE g_lock = INVALID_HANDLE_VALUE;
static bool lock_a_then_fail_b(const std::wstring& from, const std::wstring& to) {
    if (to.size() >= 5 && to.compare(to.size() - 5, 5, L"b.dat") == 0) {
        /* a.txt is already replaced: hold it open (read-share only) so its restore fails, then fail b.dat itself */
        g_lock = CreateFileW((fs::path(to).parent_path() / L"a.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return false;
    }
    return CopyFileW(from.c_str(), to.c_str(), FALSE) != FALSE;
}


/* ---- hub-only payload (RetroHub-windows.zip): RetroHub's files only; Smash Classic's files are never touched ---- */
static const char* const kSmashFiles[] = {"BattleShip.exe", "torch.exe", "f3d.o2r", "config.yml", "gamecontrollerdb.txt", "yamls\\us\\a.yml",
                                          "assets\\custom\\fonts\\f.ttf", "licenses\\README.txt", "licenses\\torch-LICENSE.txt", "LICENSE.txt",
                                          ".tcc\\include\\x.h", "BattleShip.ico"};
static void hub_scene(const fs::path& root, fs::path* staged, fs::path* install, bool hostile) {
    std::error_code ec; fs::remove_all(root, ec);
    *staged = root / L"staged"; *install = root / L"RetroHub";
    // the hub zip: RetroHub's part only
    put(*staged / L"RetroHub.exe", "new-hub");
    put(*staged / L"n64run.exe", "new-n64");
    put(*staged / L"SDL2.dll", "new-sdl");
    put(*staged / L"data\\changelog.json", "new-log");
    put(*staged / L"cores\\mgba_libretro.dll", "new-core");
    put(*staged / L"emu\\m64p.dll", "new-emu");
    put(*staged / L"licenses\\emulator-README.txt", "new-emu-readme");
    if (hostile) for (const char* f : kSmashFiles) put(*staged / fs::path(f), std::string("EVIL-") + f);   // a bad zip carrying Smash Classic's files
    // the install: old everything + the user's data
    put(*install / L"RetroHub.exe", "old-hub");
    put(*install / L"n64run.exe", "old-n64");
    put(*install / L"SDL2.dll", "old-sdl");
    put(*install / L"data\\changelog.json", "old-log");
    put(*install / L"cores\\mgba_libretro.dll", "old-core");
    put(*install / L"licenses\\emulator-README.txt", "old-emu-readme");
    for (const char* f : kSmashFiles) put(*install / fs::path(f), std::string("smash-") + f);
    put(*install / L"BattleShip.o2r", "user-o2r");
    put(*install / L"userdata\\hub.json", "user-settings");
    put(*install / L"userdata\\saves\\mario.sav", "user-save");
    age_files(*install);
}
static void check_smash_untouched(const fs::path& install) {
    for (const char* f : kSmashFiles) CHECK(get(install / fs::path(f)) == std::string("smash-") + f);
    CHECK(get(install / L"BattleShip.o2r") == "user-o2r");
    CHECK(get(install / L"userdata\\hub.json") == "user-settings");
    CHECK(get(install / L"userdata\\saves\\mario.sav") == "user-save");
}
static void check_hub_new(const fs::path& install) {
    CHECK(get(install / L"RetroHub.exe") == "new-hub");
    CHECK(get(install / L"n64run.exe") == "new-n64");
    CHECK(get(install / L"SDL2.dll") == "new-sdl");
    CHECK(get(install / L"data\\changelog.json") == "new-log");
    CHECK(get(install / L"cores\\mgba_libretro.dll") == "new-core");
    CHECK(get(install / L"emu\\m64p.dll") == "new-emu");
    CHECK(get(install / L"licenses\\emulator-README.txt") == "new-emu-readme");
}
static void check_hub_old(const fs::path& install) {
    CHECK(get(install / L"RetroHub.exe") == "old-hub");
    CHECK(get(install / L"n64run.exe") == "old-n64");
    CHECK(get(install / L"SDL2.dll") == "old-sdl");
    CHECK(get(install / L"data\\changelog.json") == "old-log");
    CHECK(get(install / L"cores\\mgba_libretro.dll") == "old-core");
    CHECK(!has(install / L"emu\\m64p.dll"));
    CHECK(get(install / L"licenses\\emulator-README.txt") == "old-emu-readme");
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--sleep") == 0) { Sleep((DWORD)std::atoi(argv[2])); return 0; }   // fake game mode
    // ---- pure helpers ----
    CHECK(!is_excluded(L"rh-helper.exe") && !is_excluded(L"data\\helpers\\dolphin_netplay_driver.ps1") && !is_excluded(L"data\\helpers\\n3ds_join.ps1"));   // an update delivers the helper exe + scripts
    CHECK(is_excluded(L"userdata\\hub.json"));
    CHECK(is_excluded(L"UserData/saves/x.sav"));
    CHECK(is_excluded(L"BattleShip.o2r"));
    CHECK(is_excluded(L"sub\\battleship.O2R"));
    CHECK(is_excluded(L"logs\\updater.log"));
    CHECK(is_excluded(L"anything.log"));
    CHECK(is_excluded(L"updates\\update.zip"));
    CHECK(is_excluded(L"saves\\a.bin"));
    CHECK(is_excluded(L"game.sav"));
    CHECK(!is_excluded(L"RetroHub.exe"));
    CHECK(!is_excluded(L"deps\\n64run.exe"));
    CHECK(!is_excluded(L"userdata"));            // a plain file with that name is not the folder
    CHECK(!is_excluded(L"docs\\userdata.txt"));
    CHECK(is_safe_rel(L"a\\b.txt"));
    CHECK(!is_safe_rel(L"..\\evil.txt"));
    CHECK(!is_safe_rel(L"a\\..\\..\\evil.txt"));
    CHECK(!is_safe_rel(L"C:\\evil.txt"));
    CHECK(!is_safe_rel(L"\\evil.txt"));
    CHECK(!is_safe_rel(L""));
    CHECK(path_is_under(L"C:\\Games\\RetroHub", L"c:\\games\\retrohub\\deps\\x.exe"));
    CHECK(path_is_under(L"C:\\Games\\RetroHub\\", L"C:\\Games\\RetroHub\\x.exe"));
    CHECK(!path_is_under(L"C:\\Games\\RetroHub", L"C:\\Games\\RetroHub2\\x.exe"));
    CHECK(!path_is_under(L"C:\\Games\\RetroHub", L"C:\\Games\\RetroHub"));

    wchar_t tb[MAX_PATH + 1]; DWORD tn = GetTempPathW(MAX_PATH, tb);
    fs::path scratch = fs::path(std::wstring(tb, tn)) / (L"retrohub_apply_test_" + std::to_wstring(GetCurrentProcessId()));
    fs::path root = scratch / L"Jeux Vid\u00e9o \u65e5\u672c";   // non-ASCII (accent + CJK)
    fs::path staged, install;

    // ---- list_staged ----
    make_scene(root / L"list", &staged, &install);
    auto files = list_staged(staged.wstring());
    CHECK(files.size() == 7);
    bool leaked = false;
    for (auto& f : files) if (is_excluded(f)) leaked = true;
    CHECK(!leaked);

    // ---- apply_files: success, non-ASCII path ----
    make_scene(root / L"ok", &staged, &install);
    Options o; o.staged = staged.wstring(); o.install = install.wstring(); o.backup = (root / L"ok" / L"bk").wstring();
    Result r = apply_files(o);
    CHECK(r.ok); CHECK(r.copied == 7);
    check_new(install); check_untouched(install);
    CHECK(!has(root / L"ok" / L"bk"));    // backup removed on success

    // ---- apply_files: locked file => full rollback ----
    make_scene(root / L"lock", &staged, &install);
    o.staged = staged.wstring(); o.install = install.wstring(); o.backup = (root / L"lock" / L"bk").wstring();
    o.retries = 1; o.retry_ms = 0;
    HANDLE lk = CreateFileW((install / L"b.dat").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(lk != INVALID_HANDLE_VALUE);
    r = apply_files(o);
    CHECK(!r.ok); CHECK(r.rolled_back); CHECK(!r.error.empty());
    CloseHandle(lk);
    check_old(install); check_untouched(install);
    CHECK(!has(install / L"sub\\deep\\c.txt"));

    // ---- mid-copy failure: dest truncated, then copy fails => original bytes restored ----
    make_scene(root / L"mid", &staged, &install);
    o.staged = staged.wstring(); o.install = install.wstring(); o.backup = (root / L"mid" / L"bk").wstring();
    o.copy_hook = [](const std::wstring& from, const std::wstring& to) -> bool {
        if (to.size() >= 5 && to.compare(to.size() - 5, 5, L"b.dat") == 0) {
            std::ofstream t(fs::path(to), std::ios::binary | std::ios::trunc); t << "PARTIAL";
            return false;
        }
        return CopyFileW(from.c_str(), to.c_str(), FALSE) != FALSE;
    };
    r = apply_files(o);
    o.copy_hook = nullptr;
    CHECK(!r.ok); CHECK(r.rolled_back);
    CHECK(get(install / L"b.dat") == "b-old");
    check_old(install); check_untouched(install);

    // ---- is_safe_rel rejects ':' (alternate data streams) ----
    CHECK(!is_safe_rel(L"a.txt:evil")); CHECK(is_safe_rel(L"sub\a.txt"));

    // ---- missing required exe ----
    make_scene(root / L"req", &staged, &install);
    fs::remove(staged / L"BattleShip.exe");
    o.staged = staged.wstring(); o.install = install.wstring(); o.backup.clear();
    r = apply_files(o);
    CHECK(!r.ok); CHECK(get(install / L"RetroHub.exe") == "old-hub");

    // ---- hub-only payload: pure helpers ----
    CHECK(is_smash_classic_file(L"BattleShip.exe") && is_smash_classic_file(L"battleship-jp.exe") && is_smash_classic_file(L"torch.exe"));
    CHECK(is_smash_classic_file(L"F3D.O2R") && is_smash_classic_file(L"config.yml") && is_smash_classic_file(L"gamecontrollerdb.txt"));
    CHECK(is_smash_classic_file(L"yamls\\us\\a.yml") && is_smash_classic_file(L"assets/custom/fonts/f.ttf") && is_smash_classic_file(L".tcc\\x.h"));
    CHECK(is_smash_classic_file(L"licenses\\README.txt") && is_smash_classic_file(L"licenses\\libultraship-LICENSE.txt") && is_smash_classic_file(L"LICENSE.txt"));
    CHECK(!is_smash_classic_file(L"RetroHub.exe") && !is_smash_classic_file(L"n64run.exe") && !is_smash_classic_file(L"retrorun.exe"));
    CHECK(!is_smash_classic_file(L"SDL2.dll") && !is_smash_classic_file(L"msvcp140.dll") && !is_smash_classic_file(L"rh-helper.exe"));
    CHECK(!is_smash_classic_file(L"data\\changelog.json") && !is_smash_classic_file(L"cores\\mgba_libretro.dll") && !is_smash_classic_file(L"emu\\mupen64plus.dll"));
    CHECK(!is_smash_classic_file(L"licenses\\emulator-README.txt") && !is_smash_classic_file(L"licenses\\mupen64plus-LICENSES.txt"));
    CHECK(!is_smash_classic_file(L"data\\yamls.txt") && !is_smash_classic_file(L""));

    // ---- hub-only apply: RetroHub's files replaced, Smash Classic's byte-identical ----
    {
        hub_scene(root / L"hub_ok", &staged, &install, false);
        auto fl = list_staged(staged.wstring(), true);
        CHECK(fl.size() == 7);
        Options ho; ho.staged = staged.wstring(); ho.install = install.wstring(); ho.backup = (root / L"hub_ok" / L"bk").wstring(); ho.hub_only = true;
        Result hr = apply_files(ho);
        CHECK(hr.ok); CHECK(hr.copied == 7);
        check_hub_new(install); check_smash_untouched(install);
        CHECK(!has(root / L"hub_ok" / L"bk"));
        // the same zip run as a full apply is refused (no BattleShip.exe): an old-style apply never takes a partial payload
        hub_scene(root / L"hub_full", &staged, &install, false);
        Options fo; fo.staged = staged.wstring(); fo.install = install.wstring(); fo.backup.clear();
        hr = apply_files(fo);
        CHECK(!hr.ok); check_hub_old(install); check_smash_untouched(install);
        // a hostile zip that carries Smash Classic's files: ignored in hub-only mode
        hub_scene(root / L"hub_evil", &staged, &install, true);
        CHECK(list_staged(staged.wstring(), true).size() == 7);
        CHECK(list_staged(staged.wstring(), false).size() == 7 + sizeof(kSmashFiles) / sizeof(kSmashFiles[0]));   // ... while a full apply takes them all
        ho.staged = staged.wstring(); ho.install = install.wstring(); ho.backup = (root / L"hub_evil" / L"bk").wstring();
        hr = apply_files(ho);
        CHECK(hr.ok && hr.copied == 7);
        check_hub_new(install); check_smash_untouched(install);
        // hub-only still requires RetroHub.exe
        hub_scene(root / L"hub_noexe", &staged, &install, false);
        fs::remove(staged / L"RetroHub.exe");
        ho.staged = staged.wstring(); ho.install = install.wstring(); ho.backup.clear();
        hr = apply_files(ho);
        CHECK(!hr.ok); check_hub_old(install); check_smash_untouched(install);
        // a locked hub file: full rollback, Smash Classic still untouched, user data untouched
        hub_scene(root / L"hub_lock", &staged, &install, false);
        ho.staged = staged.wstring(); ho.install = install.wstring(); ho.backup = (root / L"hub_lock" / L"bk").wstring(); ho.retries = 1; ho.retry_ms = 0;
        HANDLE hl = CreateFileW((install / L"data\\changelog.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(hl != INVALID_HANDLE_VALUE);
        hr = apply_files(ho);
        CHECK(!hr.ok && hr.rolled_back);
        CloseHandle(hl);
        check_hub_old(install); check_smash_untouched(install);
        // a partial extraction (the zip was cut short: the tail files never arrived, and one is a stub): the apply stops, nothing half-applied
        hub_scene(root / L"hub_partial", &staged, &install, false);
        fs::remove(staged / L"emu\\m64p.dll"); fs::remove(staged / L"cores\\mgba_libretro.dll");
        ho.backup = (root / L"hub_partial" / L"bk").wstring();
        ho.staged = staged.wstring(); ho.install = install.wstring();
        ho.copy_hook = [](const std::wstring& from, const std::wstring& to) -> bool {
            if (to.size() >= 8 && to.compare(to.size() - 8, 8, L"SDL2.dll") == 0) {
                std::ofstream t(fs::path(to), std::ios::binary | std::ios::trunc); t << "TRUNC";   // dies mid-copy
                return false;
            }
            return CopyFileW(from.c_str(), to.c_str(), FALSE) != FALSE;
        };
        hr = apply_files(ho);
        ho.copy_hook = nullptr;
        CHECK(!hr.ok && hr.rolled_back);
        CHECK(get(install / L"SDL2.dll") == "old-sdl" && get(install / L"RetroHub.exe") == "old-hub" && get(install / L"n64run.exe") == "old-n64");
        CHECK(get(install / L"cores\\mgba_libretro.dll") == "old-core");
        check_smash_untouched(install);
        // full apply still works exactly as before (same scene as the existing cases)
        make_scene(root / L"hub_fullok", &staged, &install);
        Options fo2; fo2.staged = staged.wstring(); fo2.install = install.wstring(); fo2.backup = (root / L"hub_fullok" / L"bk").wstring();
        hr = apply_files(fo2);
        CHECK(hr.ok && hr.copied == 7); check_new(install); check_untouched(install);
    }

    // ---- plain-language reasons and the status note (shown once in the bell on the next start) ----
    CHECK(plain_reason(32) == "a file was in use by another program" && plain_reason(33) == plain_reason(32));
    CHECK(plain_reason(112).find("disk is full") != std::string::npos && plain_reason(5).find("denied") != std::string::npos);
    CHECK(plain_reason(0).find("incomplete") != std::string::npos && !plain_reason(99999).empty());
    {
        const fs::path sp = root / L"status" / L"userdata" / L"update_status.json";
        Status st; st.result = "failed"; st.version = "v1.30.0"; st.reason = plain_reason(32); st.detail = "could not replace b.dat (error 32)"; st.at = 1234;
        CHECK(write_status(sp.wstring(), st));
        Status back;
        CHECK(read_status(sp.wstring(), &back) && back.result == "failed" && back.version == "v1.30.0" && back.reason == st.reason && back.detail == st.detail && back.at == 1234);
        CHECK(status_line(back) == "Update failed: a file was in use");
        Status po; po.result = "postponed"; po.version = "v1.30.0";
        CHECK(status_line(po) == "Update postponed: a game was running");
        Status rf; rf.result = "rollback_failed"; rf.version = "v1.30.0";
        CHECK(status_line(rf) == "Update failed; some files not restored");
        Status ip; ip.result = "interrupted";
        CHECK(status_line(ip).find("interrupted") != std::string::npos);
        Status nov; nov.result = "failed"; nov.reason = "the disk is full";
        CHECK(status_line(nov) == "Update failed: the disk is full");
        for (const Status* s : {&back, &po, &rf, &ip, &nov}) CHECK(status_line(*s).size() <= 40);   // the bell cuts a headline off at about 42 characters
        CHECK(take_status(sp.wstring(), &back) && !has(sp));      // read once, then gone
        CHECK(!take_status(sp.wstring(), &back));
        put(sp, "not json"); CHECK(!read_status(sp.wstring(), &back));
        put(sp, "{\"v\":1}"); CHECK(!read_status(sp.wstring(), &back));   // no result: not a status note
        CHECK(!read_status((root / L"status" / L"none.json").wstring(), &back));
    }

    // ---- H1: the applier NEVER terminates a game or emulator running from the install folder ----
    {
        make_scene(root / L"game", &staged, &install);
        Proc game = spawn_sleeper(install / L"n64run.exe", 120000);   // a fake game under the install folder
        CHECK(game.h != nullptr);
        Sleep(400);
        bool listed = false, listed_when_excepted = false;
        for (auto& p : processes_under(install.wstring())) if (p.pid == game.pid) listed = true;
        for (auto& p : processes_under(install.wstring(), {game.pid})) if (p.pid == game.pid) listed_when_excepted = true;
        CHECK(listed && !listed_when_excepted);
        Options w; w.install = install.wstring(); w.wait_games_secs = 2; w.hub_wait_secs = 1;
        std::wstring blocker;
        auto t0 = std::chrono::steady_clock::now();
        CHECK(wait_for_old_processes(w, &blocker) == WaitOutcome::TimedOut);      // gives up instead of closing the game
        CHECK(secs_since(t0) >= 1.5);
        CHECK(blocker.find(L"n64run.exe") != std::wstring::npos);
        CHECK(proc_alive(game));                                                   // the game is still running
        proc_kill(game);
        CHECK(wait_for_old_processes(w, &blocker) == WaitOutcome::Clear);          // nothing left: the apply may go ahead
        // the old RetroHub itself is the one process that is ended, and only after its grace period (it asked for this update)
        Proc oldhub = spawn_sleeper(root / L"oldhub" / L"RetroHub.exe", 120000);
        Options w2 = w; w2.pid = oldhub.pid; w2.hub_wait_secs = 1;
        t0 = std::chrono::steady_clock::now();
        CHECK(wait_for_old_processes(w2, &blocker) == WaitOutcome::TimedOut);   // even the old hub is never ended (it may own a game)
        CHECK(secs_since(t0) >= 0.8 && proc_alive(oldhub));
        kill_spawned();
    }

    // ---- H3: a rollback whose restore fails KEEPS the backup; startup recovery puts the files back ----
    {
        make_scene(root / L"h3", &staged, &install);
        const fs::path tmp = root / L"h3" / L"tmp";
        const fs::path bkdir = tmp / L"RetroHub-update-777";
        Options h; h.staged = staged.wstring(); h.install = install.wstring(); h.backup = (bkdir / L"backup").wstring();
        h.retries = 1; h.retry_ms = 0; h.restore_retries = 1; h.restore_retry_ms = 0; h.copy_hook = lock_a_then_fail_b;
        Result hr = apply_files(h);
        if (g_lock != INVALID_HANDLE_VALUE) { CloseHandle(g_lock); g_lock = INVALID_HANDLE_VALUE; }
        CHECK(!hr.ok && !hr.rolled_back && hr.backup_kept);
        CHECK(hr.unrestored.size() == 1 && hr.unrestored[0] == L"a.txt");
        CHECK(!hr.backup_dir.empty() && has(hr.backup_dir));
        CHECK(get(bkdir / L"backup" / L"a.txt") == "a-old");                      // the only good copy is still there
        CHECK(get(install / L"a.txt") == "a-new");                                  // ... and the install still has the replacement
        CHECK(get(install / L"b.dat") == "b-old" && get(install / L"RetroHub.exe") == "old-hub");   // everything else was put back
        put_manifest(bkdir, install, "v1.0.0", "v2.0.0", 0);
        RecoverReport rep = recover_interrupted(install.wstring(), "v1.0.0", tmp.wstring());
        CHECK(rep.found == 1 && rep.repaired == 1 && rep.restored_files >= 1 && rep.partial == 0);
        CHECK(!rep.notes.empty() && rep.notes[0].find("Repaired an interrupted update") == 0);
        CHECK(get(install / L"a.txt") == "a-old");                                  // repaired
        CHECK(!has(bkdir));                                                         // and cleaned up
        CHECK(recover_interrupted(install.wstring(), "v1.0.0", tmp.wstring()).found == 0);   // nothing left to find

        // a leftover for another version is never restored (it would downgrade), only removed; a report only when the new exe is already in place
        make_scene(root / L"h3b", &staged, &install);
        const fs::path tmp2 = root / L"h3b" / L"tmp";
        put(tmp2 / L"RetroHub-update-1" / L"backup" / L"a.txt", "a-ancient");
        put_manifest(tmp2 / L"RetroHub-update-1", install, "v0.9.0", "v1.0.0", 0);
        rep = recover_interrupted(install.wstring(), "v1.2.0", tmp2.wstring());
        CHECK(rep.stale == 1 && rep.repaired == 0 && rep.notes.empty());
        CHECK(get(install / L"a.txt") == "a-old" && !has(tmp2 / L"RetroHub-update-1"));
        put(tmp2 / L"RetroHub-update-2" / L"backup" / L"a.txt", "a-ancient");
        put_manifest(tmp2 / L"RetroHub-update-2", install, "v0.9.0", "v1.0.0", 0);   // the apply stopped after the new RetroHub.exe was in place
        rep = recover_interrupted(install.wstring(), "v1.0.0", tmp2.wstring());
        CHECK(rep.stale == 1 && rep.notes.size() == 1 && rep.notes[0].find("interrupted") != std::string::npos);
        CHECK(get(install / L"a.txt") == "a-old");

        // someone else's, unreadable, or still being applied by a live applier: left exactly as found
        const fs::path tmp3 = root / L"h3c" / L"tmp";
        make_scene(root / L"h3c", &staged, &install);
        put(tmp3 / L"RetroHub-update-3" / L"backup" / L"a.txt", "a-other");
        put_manifest(tmp3 / L"RetroHub-update-3", root / L"h3c" / L"SomeOtherInstall", "v1.0.0", "v2.0.0", 0);
        put(tmp3 / L"RetroHub-update-4" / L"backup" / L"a.txt", "a-nomanifest");
        put(tmp3 / L"RetroHub-update-5" / L"manifest.json", "garbage");
        Proc applier = spawn_sleeper(root / L"h3c" / L"RetroHub-apply-9" / L"RetroHub.exe", 60000);   // image path of a live applier
        Sleep(300);
        put(tmp3 / L"RetroHub-update-6" / L"backup" / L"a.txt", "a-live");
        put_manifest(tmp3 / L"RetroHub-update-6", install, "v1.0.0", "v2.0.0", applier.pid);
        rep = recover_interrupted(install.wstring(), "v1.0.0", tmp3.wstring());
        CHECK(rep.found == 4 && rep.other == 3 && rep.active == 1 && rep.repaired == 0 && rep.notes.empty());
        CHECK(has(tmp3 / L"RetroHub-update-3") && has(tmp3 / L"RetroHub-update-4") && has(tmp3 / L"RetroHub-update-5") && has(tmp3 / L"RetroHub-update-6"));
        CHECK(get(install / L"a.txt") == "a-old");
        kill_spawned();

        // a restore that still fails keeps the backup and is reported ONCE
        make_scene(root / L"h3d", &staged, &install);
        const fs::path tmp4 = root / L"h3d" / L"tmp";
        put(tmp4 / L"RetroHub-update-8" / L"backup" / L"z.txt", "z-original");
        put_manifest(tmp4 / L"RetroHub-update-8", install, "v1.0.0", "v2.0.0", 0);
        put(install / L"z.txt", "z-damaged");
        HANDLE zl = CreateFileW((install / L"z.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(zl != INVALID_HANDLE_VALUE);
        rep = recover_interrupted(install.wstring(), "v1.0.0", tmp4.wstring());
        CHECK(rep.partial == 1 && rep.repaired == 0 && rep.notes.size() == 1 && rep.notes[0].find("Update repair incomplete") == 0);
        CHECK(has(tmp4 / L"RetroHub-update-8" / L"backup" / L"z.txt"));
        rep = recover_interrupted(install.wstring(), "v1.0.0", tmp4.wstring());
        CHECK(rep.partial == 1 && rep.notes.empty());                              // not reported again
        CloseHandle(zl);
        rep = recover_interrupted(install.wstring(), "v1.0.0", tmp4.wstring());     // the lock is gone: now it is repaired
        CHECK(rep.repaired == 1 && get(install / L"z.txt") == "z-original" && !has(tmp4 / L"RetroHub-update-8"));
    }

    // ---- integration: RetroHub.exe --apply-update (needs the exe) ----
    if (argc >= 2) {
        std::wstring exe = fs::path(argv[1]).wstring();
        CHECK(has(exe));
        auto cmdFor = [&](const fs::path& st, const fs::path& in, DWORD pid, const std::wstring& extra) {
            return L"\"" + exe + L"\" --apply-update \"" + st.wstring() + L"\" \"" + in.wstring() + L"\" " +
                   std::to_wstring(pid) + L" --no-relaunch " + extra;
        };
        // success into "...\Jeux Vidéo ...\RetroHub"
        make_scene(root / L"i_ok", &staged, &install);
        int rc = run_hidden(cmdFor(staged, install, 0, L"--version v9.9.9"));
        CHECK(rc == 0);
        check_new(install); check_untouched(install);
        CHECK(!has(staged));                                  // staged dir cleaned
        CHECK(has(install / L"userdata\\logs\\updater.log")); // log lives in userdata\logs
        // locked file => exit 1 and everything restored
        make_scene(root / L"i_lock", &staged, &install);
        lk = CreateFileW((install / L"b.dat").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(lk != INVALID_HANDLE_VALUE);
        rc = run_hidden(cmdFor(staged, install, 0, L"--retries 1 --retry-ms 0"));
        CloseHandle(lk);
        CHECK(rc == 1);
        check_old(install); check_untouched(install);
        {   // M1: the failure is not silent: a status note for the next start's bell, and the staged files are kept for a retry
            Status fst;
            CHECK(read_status((install / L"userdata\\update_status.json").wstring(), &fst) && fst.result == "failed" && fst.reason == plain_reason(32));
            CHECK(status_line(fst).find("a file was in use") != std::string::npos);
            CHECK(has(staged / L"RetroHub.exe") && has(staged / L"sub\\deep\\c.txt"));
        }
        // waits for the given pid to exit (ping runs ~2s), then applies
        make_scene(root / L"i_wait", &staged, &install);
        DWORD pid = 0;
        std::wstring ping = L"ping -n 3 127.0.0.1";
        run_hidden(L"cmd /c " + ping, &pid, false);
        auto t0 = std::chrono::steady_clock::now();
        rc = run_hidden(cmdFor(staged, install, pid, L""));
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(rc == 0); CHECK(secs >= 0.8);
        check_new(install);
        // H1: a game still running from the install folder is waited for, never closed: the applier gives up (exit 3) and changes nothing
        {
            make_scene(root / L"i_game", &staged, &install);
            const fs::path tmpr = root / L"i_game" / L"tmp";
            Proc game = spawn_sleeper(install / L"retrorun.exe", 120000);
            CHECK(game.h != nullptr);
            Sleep(400);
            auto t1 = std::chrono::steady_clock::now();
            rc = run_hidden(cmdFor(staged, install, 0, L"--version v9.9.9 --from v1.0.0 --wait-secs 3 --tmp-root \"" + tmpr.wstring() + L"\""));
            CHECK(rc == 3);
            CHECK(secs_since(t1) >= 2.5);
            CHECK(proc_alive(game));                                  // the game was never closed
            check_old(install); check_untouched(install);            // nothing was replaced
            CHECK(has(staged / L"RetroHub.exe"));                      // staged files kept: a retry needs no download
            CHECK(!has(tmpr / L"RetroHub-update-0"));                  // (no backup was ever made)
            Status pst;
            CHECK(read_status((install / L"userdata\\update_status.json").wstring(), &pst) && pst.result == "postponed" && pst.version == "v9.9.9" &&
                  pst.detail.find("retrorun.exe") != std::string::npos);
            CHECK(status_line(pst) == "Update postponed: a game was running");
            proc_kill(game);
        }
        // ... and once the game closes by itself, the same apply goes through
        {
            make_scene(root / L"i_gameend", &staged, &install);
            Proc game = spawn_sleeper(install / L"n64run.exe", 3000);
            CHECK(game.h != nullptr);
            Sleep(400);
            put(install / L"userdata\\update_status.json", "{\"v\":1,\"result\":\"failed\",\"version\":\"v0.0.1\"}");   // an older failure note
            auto t1 = std::chrono::steady_clock::now();
            rc = run_hidden(cmdFor(staged, install, 0, L"--version v9.9.9 --from v1.0.0 --wait-secs 30 --tmp-root \"" + (root / L"i_gameend" / L"tmp").wstring() + L"\""));
            CHECK(rc == 0);
            CHECK(secs_since(t1) >= 1.5);                             // it really waited for the game
            check_new(install); check_untouched(install);
            CHECK(!has(install / L"userdata\\update_status.json"));    // a success clears the old failure note
            CHECK(!has(staged));
            kill_spawned();
        }
        // hub-only through the real applier (--hub-only): RetroHub's files replaced, Smash Classic's untouched, staged cleaned up
        {
            hub_scene(root / L"i_hub", &staged, &install, true);
            rc = run_hidden(cmdFor(staged, install, 0, L"--version v9.9.9 --from v1.0.0 --hub-only --tmp-root \"" + (root / L"i_hub" / L"tmp").wstring() + L"\""));
            CHECK(rc == 0);
            check_hub_new(install); check_smash_untouched(install);
            CHECK(!has(staged));
            // without --hub-only the very same staged files (no BattleShip.exe) are refused and nothing changes
            hub_scene(root / L"i_hub2", &staged, &install, false);
            rc = run_hidden(cmdFor(staged, install, 0, L"--version v9.9.9 --from v1.0.0 --tmp-root \"" + (root / L"i_hub2" / L"tmp").wstring() + L"\""));
            CHECK(rc == 1);
            check_hub_old(install); check_smash_untouched(install);
            CHECK(has(staged));   // kept for a retry
        }
        // usage error
        CHECK(run_hidden(L"\"" + exe + L"\" --apply-update onlyone") == 2);
    } else {
        std::printf("note: no RetroHub.exe path given, integration cases skipped\n");
    }

    kill_spawned();
    std::error_code ec; fs::remove_all(scratch, ec);
    std::printf(g_fail ? "hub_apply_test: %d FAILED\n" : "hub_apply_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
