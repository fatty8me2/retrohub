/* Scan cancel + ROM cache exclusion (synthetic data in %TEMP%, a scratch data dir).
 *  1. A cancel raised mid-scan makes scan_library return promptly with nothing, leaves library_cache.json exactly as it was (or absent), leaves no
 *     ".part" files, and a following scan gives the full, correct result.
 *  2. A scan that unpacks zipped ROMs while rom_cache_clear / rom_cache_trim run on other threads never loses a game (clear waits for unpacks). */
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "library.h"
#include "miniz.h"

namespace fs = std::filesystem;
using namespace hub;
typedef std::vector<uint8_t> Bytes;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static Bytes noise(size_t n, uint32_t seed) {
    Bytes v(n);
    uint32_t x = seed;
    for (auto& b : v) { x = x * 1103515245u + 12345u; b = uint8_t(x >> 16); }
    return v;
}
static void put(Bytes& v, size_t off, const char* s) { std::memcpy(&v[off], s, std::strlen(s)); }
static Bytes make_gba(uint32_t seed) {
    Bytes v = noise(128 * 1024, seed);
    std::memset(&v[0xA0], 0, 12);
    put(v, 0xA0, "CANCELTEST");
    put(v, 0xAC, "ACNL");
    put(v, 0xB0, "01");
    v[0xB2] = 0x96;
    unsigned s = 0;
    for (int i = 0xA0; i <= 0xBC; ++i) s += v[i];
    v[0xBD] = uint8_t(-(int)s - 0x19);
    return v;
}
static void write_file(const fs::path& p, const Bytes& d) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write((const char*)d.data(), (std::streamsize)d.size());
}
static bool write_zip(const fs::path& out, const std::string& inner, const Bytes& d) {
    mz_zip_archive z;
    std::memset(&z, 0, sizeof z);
    fs::create_directories(out.parent_path());
    if (!mz_zip_writer_init_file(&z, out.string().c_str(), 0)) return false;
    bool ok = mz_zip_writer_add_mem(&z, inner.c_str(), d.data(), d.size(), MZ_DEFAULT_COMPRESSION) != 0;
    ok = mz_zip_writer_finalize_archive(&z) && ok;
    mz_zip_writer_end(&z);
    return ok;
}
static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static int count_part_files(const fs::path& d) {
    int n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == L".part" || it->path().extension() == L".unpacking") ++n;
    return n;
}
static double secs_since(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); }

int main() {
    fs::path root = fs::temp_directory_path() / (L"scancancel_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    library_set_data_dir((root / "data").u8string());
    const fs::path lib = root / "lib";
    const fs::path cache_json = root / "data" / "library_cache.json";
    const std::string libs = lib.u8string();

    /* base set: 20 loose + 20 zipped GBA ROMs */
    uint32_t seed = 1;
    for (int i = 0; i < 20; ++i) write_file(lib / ("loose" + std::to_string(i) + ".gba"), make_gba(seed++));
    for (int i = 0; i < 20; ++i) CHECK(write_zip(lib / ("zip" + std::to_string(i) + ".zip"), "inner" + std::to_string(i) + ".gba", make_gba(seed++)));

    std::printf("-- baseline scan\n");
    auto base = scan_library({libs}, nullptr);
    CHECK(base.size() == 40);
    CHECK(fs::exists(cache_json));
    const std::string old_cache = slurp(cache_json);
    CHECK(!old_cache.empty());

    /* 60 more zips: the next scan has real unpack work */
    for (int i = 20; i < 80; ++i) CHECK(write_zip(lib / ("zip" + std::to_string(i) + ".zip"), "inner" + std::to_string(i) + ".gba", make_gba(seed++)));

    std::printf("-- cancel mid-scan\n");
    {
        std::atomic<int> seen{0};
        std::vector<GameEntry> got;
        std::atomic<bool> finished{false};
        std::thread th([&] {
            got = scan_library({libs}, [&](int d, int) { seen = d; });
            finished = true;
        });
        auto t0 = std::chrono::steady_clock::now();
        while (seen.load() < 50 && !finished.load() && secs_since(t0) < 60) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool was_running = !finished.load();
        scan_request_cancel();
        auto t1 = std::chrono::steady_clock::now();
        while (!finished.load() && secs_since(t1) < 20) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const double wait = secs_since(t1);
        th.join();
        std::printf("   cancelled after %d files, returned in %.3f s (scan still running at cancel: %s)\n", seen.load(), wait, was_running ? "yes" : "no");
        CHECK(was_running);                 // the scan really was cut short
        CHECK(wait < 5.0);                  // prompt
        CHECK(got.empty());
        CHECK(scan_cancelled());
        CHECK(slurp(cache_json) == old_cache);   // the old, complete cache: never a half-written one
        CHECK(count_part_files(root / "data" / "rom-cache") == 0);
    }

    std::printf("-- cancel before the scan starts\n");
    {
        auto t0 = std::chrono::steady_clock::now();
        auto r = scan_library({libs}, nullptr);
        CHECK(r.empty());
        CHECK(secs_since(t0) < 1.0);
        CHECK(slurp(cache_json) == old_cache);
    }

    std::printf("-- scan after the cancel is complete and correct\n");
    scan_cancel_reset_for_tests();
    CHECK(!scan_cancelled());
    {
        auto r = scan_library({libs}, nullptr);
        CHECK(r.size() == 100);             // 20 loose + 80 zipped
        CHECK(slurp(cache_json) != old_cache);
        for (const GameEntry& g : r) {
            std::error_code e2;
            CHECK(fs::exists(fs::u8path(g.path), e2));
        }
        CHECK(count_part_files(root / "data" / "rom-cache") == 0);
        auto r2 = scan_library({libs}, nullptr);   // and the cache it wrote is a valid one: same answer
        CHECK(r2.size() == r.size());
    }

    std::printf("-- clear / trim running beside a scan never cost it a game\n");
    for (int round = 0; round < 3; ++round) {
        CHECK(rom_cache_clear());
        std::atomic<bool> scan_done{false};
        std::atomic<int> removers{0};
        std::atomic<bool> saw_busy{false};
        std::vector<GameEntry> r;
        std::thread scanner([&] { r = scan_library({libs}, nullptr); scan_done = true; });
        std::thread clearer([&] {
            while (!scan_done.load()) { ++removers; rom_cache_clear(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        });
        std::thread trimmer([&] {
            while (!scan_done.load()) { rom_cache_trim(1); if (rom_cache_busy()) saw_busy = true; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        });
        scanner.join();
        clearer.join();
        trimmer.join();
        std::printf("   round %d: %zu games, %d clears ran beside the scan\n", round, r.size(), removers.load());
        CHECK(r.size() == 100);
        CHECK(count_part_files(root / "data" / "rom-cache") == 0);
        CHECK(!rom_cache_busy());
    }

    std::printf("-- after a clear the next scan unpacks everything again\n");
    CHECK(rom_cache_clear());
    {
        auto r = scan_library({libs}, nullptr);
        CHECK(r.size() == 100);
        for (const GameEntry& g : r) {
            std::error_code e2;
            CHECK(fs::exists(fs::u8path(g.path), e2));
        }
    }


    std::printf("-- child waits, list_archive, 3DS unzip and the apply guard under a cancel\n");
    {
        auto spawn_ping = [](int count) -> HANDLE {   // a harmless long-running System32 command, started and ended by this test
            wchar_t sysdir[MAX_PATH];
            GetSystemDirectoryW(sysdir, MAX_PATH);
            std::wstring cmd = std::wstring(L"\"") + sysdir + L"\\PING.EXE\" -n " + std::to_wstring(count) + L" 127.0.0.1";
            STARTUPINFOW si{}; si.cb = sizeof si;
            PROCESS_INFORMATION pi{};
            if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return nullptr;
            CloseHandle(pi.hThread);
            return pi.hProcess;
        };
        scan_cancel_reset_for_tests();
        CHECK(scan_result_applies());
        {   // exits by itself
            HANDLE h = spawn_ping(1);
            CHECK(h && child_wait_cancellable(h, 20000) == 0);
            if (h) CloseHandle(h);
        }
        {   // timeout: ended by its handle
            HANDLE h = spawn_ping(30);
            auto t0 = std::chrono::steady_clock::now();
            CHECK(h && child_wait_cancellable(h, 300) == 1);
            CHECK(secs_since(t0) < 5.0);
            if (h) { CHECK(WaitForSingleObject(h, 0) == WAIT_OBJECT_0); CloseHandle(h); }
        }
        {   // cancel raised while waiting: ended promptly, long before its 30 s
            HANDLE h = spawn_ping(30);
            std::thread t([] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); scan_request_cancel(); });
            auto t0 = std::chrono::steady_clock::now();
            CHECK(h && child_wait_cancellable(h, 120000) == 2);
            t.join();
            CHECK(secs_since(t0) < 5.0);
            if (h) { CHECK(WaitForSingleObject(h, 0) == WAIT_OBJECT_0); CloseHandle(h); }
            CHECK(!scan_result_applies());
        }
        /* list_archive through tar under a cancel: refused, and the failure is not remembered for later */
        const fs::path arc = root / "arc" / "probe.7z";
        fs::create_directories(arc.parent_path());
        write_file(root / "arc" / "stage" / "a.txt", Bytes(100, 'a'));
        {
            wchar_t sysdir[MAX_PATH];
            GetSystemDirectoryW(sysdir, MAX_PATH);
            std::wstring cmd = std::wstring(L"\"") + sysdir + L"\\tar.exe\" -a -cf \"" + arc.wstring() + L"\" -C \"" + (root / "arc" / "stage").wstring() + L"\" a.txt";
            STARTUPINFOW si{}; si.cb = sizeof si;
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 30000);
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            }
        }
        if (fs::exists(arc)) {
            scan_request_cancel();
            const std::string while_cancelled = archive_skip_reason(arc.u8string());   // the listing is refused: "could not be read"
            scan_cancel_reset_for_tests();
            const std::string after = archive_skip_reason(arc.u8string());             // a real listing: not remembered as a failure
            CHECK(while_cancelled.find("could not be read") != std::string::npos);
            CHECK(after.find("could not be read") == std::string::npos);
        } else std::printf("   (tar cannot write .7z here: list_archive check skipped)" "\n");
        scan_cancel_reset_for_tests();
        /* a 3DS image in a zip: a cancel stops the unpack, leaves no work folder, and the next try works */
        {
            const fs::path z = root / "arc3ds" / "Game.zip";
            CHECK(write_zip(z, "Game.3ds", noise(2 * 1024 * 1024, 77)));
            GameEntry e;
            e.system = System::N3DS;
            e.key = "3ds:cancel-test";
            e.path = z.u8string();
            e.issue = "extract";
            std::string out, err;
            scan_request_cancel();
            CHECK(!extract_3ds_zip(e, nullptr, &out, &err));
            CHECK(err == "cancelled");
            CHECK(count_part_files(root / "data" / "rom-cache") == 0);
            CHECK(!fs::exists(fs::u8path(extract_3ds_dir(e)) / "Game.3ds"));
            scan_cancel_reset_for_tests();
            CHECK(extract_3ds_zip(e, nullptr, &out, &err));
            CHECK(fs::exists(fs::u8path(out)));
        }
        CHECK(scan_result_applies());
    }

    library_set_data_dir("");
    fs::remove_all(root, ec);
    if (g_fail) { std::printf("scancancel_test: %d FAILED\n", g_fail); return 1; }
    std::printf("scancancel_test: OK\n");
    return 0;
}
