#pragma once
/* Hookshot (github.com/NotRexed/Hookshot, GPL-3.0-or-later): a native Windows build of The Legend of Zelda: Ocarina of
 * Time (Nintendo Switch 2 remake), made ON the player's PC from their own game file by static recompilation. Nothing
 * of the game is downloaded or shared; the build never leaves this PC (never offer it to friends or packs).
 *
 * Experimental and opt-in: Lichii plays that game by default. RetroHub runs Hookshot instead only when it is built AND the
 * player chose it ("switch.runner" = hookshot), with the save in the
 * SAME vault folder Lichii uses (<vault>/switch/<key>/battery/save/<TID>), so the two stay interchangeable and every
 * PC stays in step. Source audited 2026-11-07 at kSourceCommit: no network code, no telemetry, no registry or system
 * changes (docs/HOOKSHOT.md). The build pipeline is hookshot_build.h. */
#include <cstdint>
#include <map>
#include <string>

#include "emulators.h"

namespace hub { struct GameEntry; }

namespace hookshot {

constexpr uint64_t kTitleId = 0x0400486024DEC000ull;   // program id: the only game Hookshot runs
constexpr const char* kGameBuild = "e0f3ccfa";            // the game build it supports (update v131072); others fail the check
constexpr const char* kSourceCommit = "8ef84611049f4fc31835a0b12793cf074ed49c3c";   // Hookshot's audited source (v2.1.0); per flavor: info(f).source_commit
constexpr const char* kSourceVersion = "v2.1.0";
/* The previously audited source (2026-11-07). A build made from it keeps working (ready() accepts it) and the card offers a rebuild: never forced. */
constexpr const char* kPreviousAuditedCommit = "137c423589f178bb4274e339b17b4dfd466435e9";
/* host/nn_applet.c: the fixed user id {0x1, 0x5a3c9e1f0b7d2468}; host/nn_fs.c names the save folder high word first.
 * The game's own save files sit in it unwrapped: the same bytes Lichii keeps under nand/user/save/0../<uuid>/<TID>. */
constexpr const char* kSaveRel = "save/5a3c9e1f0b7d24680000000000000001";   // relative to user_dir()

/* ---- flavors: the native builds RetroHub can make for this one game. Hookshot is the original; Longshot (github.com/madebyjordan/Longshot,
 * GPL-3.0-or-later) is a fork of Hookshot v2.0.5 that only merges changes its author has played. Both are built the same way from the player's
 * own game file (hookshot_build.h), live in their own folder, and share the save through the vault. docs/HOOKSHOT.md, "Longshot". ---- */
enum class Flavor { Hookshot = 0, Longshot = 1 };
constexpr int kFlavorCount = 2;
struct FlavorInfo {
    Flavor flavor;
    const char* id;                 // the "switch.runner" value AND the folder name under <data_dir>\emulators: "hookshot" | "longshot"
    const char* name;               // for the player: "Hookshot" | "Longshot"
    const char* exe_rel;            // the game program inside game_dir(f) (what Play starts)
    const char* user_rel;           // the folder that holds save\ (kSaveRel is relative to it), inside game_dir(f)
    const char* source_commit;      // the audited source RetroHub builds
    const char* source_version;     // "v2.1.0" | "v1.0.0"
    const char* previous_commit;    // an older audited source whose builds keep working ("" = none)
    const char* source_zip;         // in data\hookshot beside RetroHub.exe
    const char* source_zip_sha256;
    int journal_kind;               // hubjournal::Entry::emu_kind: 100 Hookshot, 101 Longshot (not hubemu::Kind values)
    const char* manifest_suffix;    // the save session's "last agreed" manifest: "-hookshot" | "-longshot"
};
const FlavorInfo& info(Flavor f);
constexpr Flavor kFlavors[kFlavorCount] = {Flavor::Hookshot, Flavor::Longshot};
/* "hookshot" / "longshot" (exact, lower case) -> the flavor. Anything else: false. */
bool flavor_from_id(const std::string& id, Flavor* out);

/* The one game these builds run: System::Switch with title id kTitleId (key "switch:0400486024dec000"). */
bool supports(const hub::GameEntry& e);

/* <data_dir>\emulators\<id>: the built game (the exe, hookshot_vk.dll, SDL2.dll, romfs\, user\, caches). */
std::string game_dir(Flavor f = Flavor::Hookshot);
std::string exe_path(Flavor f = Flavor::Hookshot);   // <game_dir>\<exe_rel>
std::string user_dir(Flavor f = Flavor::Hookshot);   // <game_dir>\<user_rel>

/* What RetroHub writes after a successful build: <dir>\retrohub_hookshot.json (the same file name for every flavor; the commit says which)
 *   {"commit":"<source_commit>","game_build":"e0f3ccfa","built":<unix>,"source":"<game file>","size":<bytes>} */
struct Stamp {
    std::string commit, game_build, source;
    int64_t built = 0;
    uint64_t size = 0;
};
bool read_stamp(const std::string& dir, Stamp* out);
bool write_stamp(const std::string& dir, const Stamp& s);

/* Ready to play from `dir` (default game_dir(f)): the flavor's files exist and the stamp names its source_commit or previous_commit.
 * why (optional): what is missing, in plain words. */
bool ready(std::string* why = nullptr, const std::string& dir = std::string(), Flavor f = Flavor::Hookshot);
/* A build that is ready() but made from the flavor's previous audited source: the card offers the newer one as a rebuild. */
bool is_older_audited(const Stamp& s, Flavor f = Flavor::Hookshot);

/* Setting "switch.runner" (game_settings.cpp, Visual, shown only for games where supports() is true): "lichii" (default) = the emulator,
 * "fritchii", "hookshot", "longshot" (an old v1.37.0 "auto" is dropped on load and means Lichii). Never read by online / sync code: native builds are offline only. */
constexpr const char* kRunnerSetting = "switch.runner";
using Values = std::map<std::string, std::string>;   // == hub::gs::Values

/* What Play does for this game with these settings. THE one decision: launch.cpp, the card and the settings page all call it.
 *   Emulator:        the runner is not a native build (or the game is not supported): Lichii / Fritchii decide (nx_runner.h).
 *   Native:          the player chose flavor `flavor` and it is ready(): Play starts it.
 *   NativeNotBuilt:  the player chose flavor `flavor` but it is not ready() (`why` says what is missing). Play must NOT quietly start an
 *                    emulator instead: it says so and offers to build it or to play with the emulator this once (hookshot_run_ui.cpp). */
enum class Choice { Emulator, Native, NativeNotBuilt };
struct Decision {
    Choice choice = Choice::Emulator;
    Flavor flavor = Flavor::Hookshot;
    std::string why;
};
Decision decide(const hub::GameEntry& e, const Values& v);
/* decide() says Native with Flavor::Hookshot (kept for the older tests). */
bool use_for(const hub::GameEntry& e, const Values& v);

/* Is a Media Foundation AV1 video decoder installed (Microsoft's free "AV1 Video Extension")? Without one Hookshot would open a top-most
 * message box over the game, so RetroHub asks first. Cached; force_recheck re-asks Windows (after the player installed it, and on the next Play). */
bool av1_decoder_present(bool force_recheck = false);
constexpr const char* kAv1StoreUri = "ms-windows-store://pdp/?ProductId=9MVZQVXJBQ9V";

/* Saves: <user_dir()>/kSaveRel <-> <vault battery>/save/0400486024DEC000 (the folder the Lichii branch of hubemu::begin_session uses, so the
 * two stay interchangeable). Same vault->emulator newest-wins step as begin_session, with its own "last agreed" manifest (suffix -hookshot).
 * Finish with hubemu::end_session(). Inactive (nothing copied) when e is not supported. */
hubemu::SaveSession begin_save_session(const hub::GameEntry& e, const std::string& vault_battery_dir, Flavor f = Flavor::Hookshot);

}   // namespace hookshot
