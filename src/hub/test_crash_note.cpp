// Console test: "the emulator crashed" note logic (crash_note.h). Offline; uses a temp dir for the dump lookup.
#include "crash_note.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace hubcrash;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    // crashes
    const uint32_t crashes[] = {0xC0000005u, 0xC0000409u, 0xC00000FDu, 0xC000001Du, 0xC0000374u, 0x80000003u, 0xC0000094u, 0xE06D7363u};
    for (uint32_t c : crashes) CHECK(classify(c) == Verdict::Crash && is_crash(c));
    // normal exits and user ends
    CHECK(classify(1) == Verdict::Normal);
    CHECK(classify(2) == Verdict::Normal);
    CHECK(classify(4) == Verdict::Normal);
    CHECK(classify(0xFFFFFFFFu) == Verdict::Normal);
    CHECK(classify(0xC000013Au) == Verdict::Normal);   // Ctrl+C / console closed
    CHECK(classify(0x40010004u) == Verdict::Normal);
    // the code RetroHub's own hang watch ends an emulator with is never a crash
    CHECK(kHubEndExitCode == 0);
    CHECK(classify(kHubEndExitCode) == Verdict::HubEnded);
    CHECK(!is_crash(kHubEndExitCode));
    CHECK(hex_code(0xC0000005u) == "0xC0000005");
    CHECK(hex_code(0x80000003u) == "0x80000003");
    CHECK(hex_code(0) == "0x00000000");

    // dump lookup
    const fs::path dir = fs::temp_directory_path() / "retrohub_crashnote_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    CHECK(find_dump(dir.u8string(), "lichii.exe", 1234).empty());   // folder missing
    fs::create_directories(dir, ec);
    CHECK(find_dump(dir.u8string(), "lichii.exe", 1234).empty());   // no file yet
    { std::ofstream(dir / "lichii.exe.1234.dmp") << "x"; }
    { std::ofstream(dir / "lichii.exe.99.dmp") << "x"; }
    { std::ofstream(dir / "Other.exe.555.dmp") << "x"; }
    const std::string hit = find_dump(dir.u8string(), "lichii.exe", 1234);
    CHECK(!hit.empty() && fs::path(hit).filename() == "lichii.exe.1234.dmp");
    CHECK(find_dump(dir.u8string(), "lichii.exe", 1235).empty());   // other pid
    CHECK(find_dump(dir.u8string(), "dolphin.exe", 1234).empty());  // other exe
    CHECK(!find_dump(dir.u8string(), "other.exe", 555).empty());    // case-insensitive exe name
    CHECK(find_dump("", "lichii.exe", 1234).empty());
    CHECK(find_dump(dir.u8string(), "lichii.exe", 0).empty());
    CHECK(folder_of(hit) == dir.u8string());

    // texts
    Crash c;
    c.emulator = "Fritchii"; c.version = "5.7"; c.exe_name = "lichii.exe"; c.game = "Zelda"; c.exit_code = 0xC0000005u; c.pid = 1234; c.time_unix = 1700000000; c.dump_path = hit;
    CHECK(headline(c) == "Fritchii stopped unexpectedly while running Zelda.");
    const std::string d = details_text(c);
    CHECK(d.find("Fritchii 5.7") != std::string::npos && d.find("0xC0000005") != std::string::npos && d.find("lichii.exe.1234.dmp") != std::string::npos);
    c.dump_path.clear(); c.game.clear();
    CHECK(headline(c) == "Fritchii stopped unexpectedly.");
    CHECK(details_text(c).find("none found") != std::string::npos);

    fs::remove_all(dir, ec);
    std::printf(fails ? "hub_crashnote_test: %d FAILED\n" : "hub_crashnote_test: all passed\n", fails);
    return fails ? 1 : 0;
}
