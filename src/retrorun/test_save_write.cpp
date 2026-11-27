/* Console test: retrorun's save writes (save_write.h). In game one attempt that never waits; at exit flushed and retried while another program
 * briefly holds the old save, so the session's last battery save is not lost to a sync client or virus scanner. Temp folders only. */
#include "plat/plat.h"
#if RH_WIN
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "save_write.h"

namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::string get(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

/* Opens `p` the way a reader without FILE_SHARE_DELETE does (std::ifstream, many sync / backup tools): a rename over it fails meanwhile. */
#if RH_WIN
static HANDLE hold(const fs::path& p) {
    return CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}
#endif

int main() {
#if RH_WIN
    wchar_t tmpw[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpw);
    const fs::path root = fs::path(tmpw) / (L"rr_savewrite_é世_" + std::to_wstring(GetCurrentProcessId()));   // non-ASCII folder
#else
    const fs::path root = fs::temp_directory_path() / fs::u8path(u8"rr_savewrite_é世_" + std::to_string(plat::current_pid()));   // non-ASCII folder
#endif
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    const fs::path srm = root / fs::u8path(u8"Pokémon Red.srm");
    const std::string path = srm.u8string();

    // plain writes, both flavours, replace an existing file
    CHECK(retrorun::write_atomic(path, "first", 5));
    CHECK(get(srm) == "first");
    CHECK(retrorun::write_atomic(path, "second!", 7, true));
    CHECK(get(srm) == "second!");
    CHECK(retrorun::write_atomic(path, "", 0));
    CHECK(fs::exists(srm) && fs::file_size(srm) == 0);
    CHECK(retrorun::write_atomic(path, "third", 5));

#if RH_WIN   // sharing violations only exist on Windows; POSIX rename replaces a file that is open
    // in game: a held save makes the write fail at once (retried 30 s later by the caller), the old file is untouched, no temp is left behind
    {
        HANDLE h = hold(srm);
        CHECK(h != INVALID_HANDLE_VALUE);
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!retrorun::write_atomic(path, "ingame", 6));
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200));   // never waits on the emulation thread
        CloseHandle(h);
        CHECK(get(srm) == "third");
        CHECK(!fs::exists(root / fs::u8path(u8"Pokémon Red.srm.tmp")));
    }

    // at exit: the same short hold (a sync client reading the save) is waited out and the final save lands
    {
        HANDLE h = hold(srm);
        CHECK(h != INVALID_HANDLE_VALUE);
        std::thread release([h] { std::this_thread::sleep_for(std::chrono::milliseconds(250)); CloseHandle(h); });
        CHECK(retrorun::write_atomic(path, "final save", 10, true));
        release.join();
        CHECK(get(srm) == "final save");
        CHECK(!fs::exists(root / fs::u8path(u8"Pokémon Red.srm.hubtmp")));
    }

    // at exit, held for longer than the budget: fails cleanly, the old save is untouched and no temp is left
    {
        HANDLE h = hold(srm);
        CHECK(h != INVALID_HANDLE_VALUE);
        CHECK(!retrorun::write_atomic(path, "too late", 8, true));
        CloseHandle(h);
        CHECK(get(srm) == "final save");
        CHECK(!fs::exists(root / fs::u8path(u8"Pokémon Red.srm.hubtmp")));
    }

#endif

    // a missing folder fails (no crash, nothing created)
    CHECK(!retrorun::write_atomic((root / "nope" / "x.srm").u8string(), "x", 1));
    CHECK(!retrorun::write_atomic((root / "nope" / "x.srm").u8string(), "x", 1, true));
    CHECK(!fs::exists(root / "nope"));

    fs::remove_all(root, ec);
    if (fails) { std::printf("%d failures\n", fails); return 1; }
    std::printf("retrorun_savewrite_test: ok\n");
    return 0;
}
