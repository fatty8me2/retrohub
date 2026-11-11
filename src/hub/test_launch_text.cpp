// Console test: online launch guard (busy/debounce), host's view of a guest's np_fail, player-facing launch wording, UTF-8 file helpers.
#include "launch_text.h"
#include "launch_files.h"

#include <cstdio>
#include <filesystem>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static bool contains(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

static void busy() {
    using namespace hublaunch;
    LaunchStates idle;
    CHECK(!async_launch_in_progress(idle));
    CHECK(!launch_busy(idle, 100000, 0));                  // never pressed
    CHECK(launch_busy(idle, 100000, 98000));               // 2 s after a press
    CHECK(launch_busy(idle, 100000, 100000 - kDebounceMs + 1));
    CHECK(!launch_busy(idle, 100000, 100000 - kDebounceMs));   // exactly 5 s: free again
    CHECK(!launch_busy(idle, 100000, 200000));             // clock before the press (bad data): not stuck
    // each async state alone blocks, with no press recorded
    for (int i = 0; i < 8; ++i) {
        LaunchStates s;
        bool* f[] = {&s.np3_pending, &s.np3_active, &s.gc_active, &s.gc_guest_wait, &s.gc_forwarder, &s.rp_pending, &s.rp_stream, &s.rp_probe};
        *f[i] = true;
        CHECK(async_launch_in_progress(s));
        CHECK(launch_busy(s, 1000000, 0));
    }
    CHECK(starting_text("Mario Kart 64") == "Starting Mario Kart 64...");
    CHECK(starting_text("") == "Starting the game...");
}

static void host_view() {
    using namespace hublaunch;
    CHECK(host_view_of_guest_fail("Mia", "RetroHub can't find the game file for Zelda. Open it in your Library to point RetroHub at it again.") ==
          "Mia can't start: the game file is missing on Mia's PC.");
    CHECK(host_view_of_guest_fail("Mia", "The host started a game you don't have.") == "Mia doesn't have this game.");
    CHECK(contains(host_view_of_guest_fail("Mia", "The host started a game, but a game is already running on this PC."), "already running on Mia's PC"));
    CHECK(contains(host_view_of_guest_fail("Mia", core_missing_wire("mgba")), "Mia's PC is missing a file"));
    CHECK(contains(host_view_of_guest_fail("Mia", core_missing_wire_nds()), "missing a file"));
    CHECK(contains(host_view_of_guest_fail("Mia", start_failed_wire()), "didn't open on Mia's PC"));
    CHECK(contains(host_view_of_guest_fail("Mia", ps1_core_missing_wire()), "PlayStation online files"));
    CHECK(contains(host_view_of_guest_fail("Mia", ps1_bios_missing_wire()), "no PlayStation BIOS"));
    CHECK(contains(host_view_of_guest_fail("Mia", "A GameCube session is still open on this PC (Dolphin) - close it, then the host can start again."), "still open on Mia's PC"));
    CHECK(contains(host_view_of_guest_fail("Mia", "The host's GameCube launch is invalid - ask them to update RetroHub."), "can't read this launch"));
    // nothing guest-directed or technical survives in a mapped line
    for (const std::string& raw : {std::string("RetroHub can't find the game file for X. Open it in your Library."), std::string("The host started a game you don't have."),
                                   core_missing_wire("mgba"), std::string(start_failed_wire()), std::string(ps1_core_missing_wire()), std::string(ps1_bios_missing_wire())}) {
        const std::string t = host_view_of_guest_fail("Mia", raw);
        CHECK(!contains(t, "your Library") && !contains(t, "cores folder") && !contains(t, ".dll") && !contains(t, "you don't"));
    }
    // unknown text: today's wording, unchanged; empty name is handled
    CHECK(host_view_of_guest_fail("Mia", "their copy is newer") == "Mia can't start: their copy is newer");
    CHECK(host_view_of_guest_fail("", "x") == "A guest can't start: x");
}

static void wording() {
    using namespace hublaunch;
    CHECK(core_missing_text("Game Boy") == "RetroHub is missing a file it needs for Game Boy. Reinstall RetroHub or check for updates.");
    CHECK(start_failed_text("Zelda") == "Zelda wouldn't start. Check the game opens from your Library, then try again.");
    CHECK(online_unavailable_text("PSP") == "Online play isn't available for PSP yet.");
    CHECK(!contains(connection_check_failed_text(), "streamrun"));
    CHECK(!contains(remote_play_failed_text(), "streamrun"));
    CHECK(!contains(ps1_files_missing_text(), ".dll"));
    // what goes on the wire is the older wording for the texts that changed, and unchanged otherwise
    CHECK(wire_for(ps1_files_missing_text()) == ps1_core_missing_wire());
    CHECK(wire_for(remote_play_missing_text()) == remote_play_missing_wire());
    CHECK(wire_for(std::string("PlayStation online needs a BIOS file (scph5501.bin) on every PC, or OpenBIOS on every PC. Set it up in Settings > Playing. ") + ps1_bios_own_text()) == ps1_bios_missing_wire());
    CHECK(wire_for("Unpacking X first - try again when it is done.") == "Unpacking X first - try again when it is done.");
    CHECK(core_missing_wire("mgba") == "Emulator core 'mgba' not found (cores folder).");
    CHECK(remote_play_failed_wire(5) == "Could not start remote play (streamrun error 5).");
    CHECK(online_unavailable_wire() == std::string("Online play is not available for this system yet."));
    // the older wording still maps to something the host can read
    CHECK(contains(host_view_of_guest_fail("Mia", online_unavailable_wire()), "isn't available"));
}

static void files() {
    namespace fs = std::filesystem;
    using namespace hublaunch;
    const fs::path base = fs::temp_directory_path() / fs::u8path(u8"rh_launch_test_café_日本");
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    CHECK(fs::exists(base));
    const std::string dir = base.u8string();
    const std::string probe = dir + "\\rplay_probe.json", pid = dir + "\\rplay_cemu.pid";
    CHECK(read_file_utf8(probe).empty());                 // missing file: empty, no crash
    CHECK(write_file_utf8(pid, "12345"));
    CHECK(fs::exists(fs::u8path(pid)));
    CHECK(read_file_utf8(pid) == "12345");
    CHECK(file_size_utf8(pid) == 5);
    remove_file_utf8(pid);
    CHECK(!fs::exists(fs::u8path(pid)));
    CHECK(file_size_utf8(pid) == 0);
    remove_file_utf8(pid);                                // removing a missing file is harmless
    fs::remove_all(base, ec);
}

int main() {
    busy();
    host_view();
    wording();
    files();
    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("ok\n");
    return 0;
}
