// Console test (no network, no windows): Smash Classic one-time legacy save copy, post-exit rules,
// and the queued friend join. File checks use a scratch directory under the temp folder only.
#include "logic.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

namespace fs = std::filesystem;

static void write_bytes(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary);
    o << bytes;
}
static std::string read_bytes(const fs::path& p) {
    std::ifstream i(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(i), std::istreambuf_iterator<char>());
}

int main() {
    using namespace hubui;

    /* 1. Smash Classic legacy save: copied once into the vault, never overwrites, never deletes the legacy file. */
    {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        fs::path root = fs::temp_directory_path() / ("rh_session_test_" + std::to_string(stamp));
        fs::path legacy = root / "BattleShip" / "ssb64_save.bin";
        fs::path vault = root / "vault" / "n64" / "native_smash64" / "ssb64_save.bin";

        CHECK(!smash_should_migrate_save(false, false) && smash_should_migrate_save(false, true));

        // no legacy file: nothing copied
        CHECK(!smash_migrate_legacy_save(legacy.string(), vault.string()));
        CHECK(!fs::exists(vault));

        // legacy present, vault empty: copied byte-for-byte, legacy kept
        // (the vault battery folder already exists in the app; the copy itself does not create it)
        fs::create_directories(vault.parent_path());
        write_bytes(legacy, std::string("\x00\x01SAVE\xff", 7));
        CHECK(smash_migrate_legacy_save(legacy.string(), vault.string()));
        CHECK(fs::exists(vault) && read_bytes(vault) == std::string("\x00\x01SAVE\xff", 7));
        CHECK(fs::exists(legacy) && read_bytes(legacy) == std::string("\x00\x01SAVE\xff", 7));

        // vault already has a save: the vault wins, nothing overwritten
        write_bytes(legacy, "OLD-LOCAL");
        CHECK(!smash_migrate_legacy_save(legacy.string(), vault.string()));
        CHECK(read_bytes(vault) == std::string("\x00\x01SAVE\xff", 7));
        CHECK(read_bytes(legacy) == "OLD-LOCAL");

        // a conflict only when both exist and differ
        CHECK(smash_save_conflict(true, true, "a", "b") && !smash_save_conflict(true, true, "a", "a"));

        std::error_code ec;
        fs::remove_all(root, ec);
    }

    /* 2. Post-exit: play time goes to the vault only for tracked offline launches of a known system;
          auto sync runs only with a sync target and auto-sync on. */
    CHECK(exit_should_touch_game(true, true, false, "n64"));
    CHECK(!exit_should_touch_game(false, true, false, "n64"));   // no key
    CHECK(!exit_should_touch_game(true, false, false, "n64"));   // not tracked
    CHECK(!exit_should_touch_game(true, true, true, "n64"));     // online launch
    CHECK(!exit_should_touch_game(true, true, false, ""));       // unknown system
    CHECK(auto_sync_should_run(true, true));
    CHECK(!auto_sync_should_run(false, true) && !auto_sync_should_run(true, false) && !auto_sync_should_run(false, false));

    /* 3. Queued friend join: a join clicked while our game runs is queued and runs after exit. */
    CHECK(join_while_running(false) == JoinWhileRunning::Queue);
    CHECK(join_while_running(true) == JoinWhileRunning::AlreadyInRoom);
    CHECK(!pending_join_ready(true, true));    // game still running: keep waiting
    CHECK(pending_join_ready(true, false));    // game closed: run it
    CHECK(!pending_join_ready(false, false));  // nothing queued

    /* 4. Cold-start / forwarded link join: held until a player name exists, then released once. */
    {
        LinkJoinQueue q;
        std::string c;
        CHECK(!q.pending() && !q.take(true, &c));
        q.push("ABCDEF");                              // cold start, settings not loaded / no name yet
        CHECK(q.needs_name(false) && !q.take(false, &c) && q.pending());
        q.push("K7M2QX");                              // a later link replaces it
        CHECK(!q.needs_name(true) && q.take(true, &c) && c == "K7M2QX");
        CHECK(!q.pending() && !q.take(true, &c));      // released exactly once
    }

    std::printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
