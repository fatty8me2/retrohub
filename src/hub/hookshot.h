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
constexpr const char* kSourceCommit = "137c423589f178bb4274e339b17b4dfd466435e9";   // the audited source RetroHub builds
/* host/nn_applet.c: the fixed user id {0x1, 0x5a3c9e1f0b7d2468}; host/nn_fs.c names the save folder high word first.
 * The game's own save files sit in it unwrapped: the same bytes Lichii keeps under nand/user/save/0../<uuid>/<TID>. */
constexpr const char* kSaveRel = "save/5a3c9e1f0b7d24680000000000000001";   // relative to user_dir()

/* The one game Hookshot runs: System::Switch with title id kTitleId (key "switch:0400486024dec000"). */
bool supports(const hub::GameEntry& e);

/* <data_dir>\emulators\hookshot: the built game (hookshot.exe, hookshot_vk.dll, SDL2.dll, romfs\, user\, caches). */
std::string game_dir();
std::string exe_path();   // <game_dir>\hookshot.exe
std::string user_dir();   // <game_dir>\user   (host/hos_main.c: <exe dir>\user)

/* What RetroHub writes after a successful build: <dir>\retrohub_hookshot.json
 *   {"commit":"<kSourceCommit>","game_build":"e0f3ccfa","built":<unix>,"source":"<game file>","size":<bytes>} */
struct Stamp {
    std::string commit, game_build, source;
    int64_t built = 0;
    uint64_t size = 0;
};
bool read_stamp(const std::string& dir, Stamp* out);
bool write_stamp(const std::string& dir, const Stamp& s);

/* Ready to play from `dir` (default game_dir()): hookshot.exe, hookshot_vk.dll, SDL2.dll and romfs\ exist and the stamp
 * names kSourceCommit. why (optional): what is missing, in plain words. */
bool ready(std::string* why = nullptr, const std::string& dir = std::string());

/* Setting "switch.runner" (game_settings.cpp, Visual, shown only for games where supports() is true): "lichii" (default) = the emulator,
 * "hookshot" = Hookshot once it is built (an old v1.37.0 "auto" is dropped on load and means Lichii). Never read by online / sync code: Hookshot is offline only. */
constexpr const char* kRunnerSetting = "switch.runner";
using Values = std::map<std::string, std::string>;   // == hub::gs::Values
/* Play this game with Hookshot: supports(e), ready(), and the setting is exactly "hookshot". */
bool use_for(const hub::GameEntry& e, const Values& v);

/* Is a Media Foundation AV1 video decoder installed (Microsoft's free "AV1 Video Extension")? Without one Hookshot would open a top-most
 * message box over the game, so RetroHub asks first. Cached; force_recheck re-asks Windows (after the player installed it, and on the next Play). */
bool av1_decoder_present(bool force_recheck = false);
constexpr const char* kAv1StoreUri = "ms-windows-store://pdp/?ProductId=9MVZQVXJBQ9V";

/* Saves: <user_dir()>/kSaveRel <-> <vault battery>/save/0400486024DEC000 (the folder the Lichii branch of hubemu::begin_session uses, so the
 * two stay interchangeable). Same vault->emulator newest-wins step as begin_session, with its own "last agreed" manifest (suffix -hookshot).
 * Finish with hubemu::end_session(). Inactive (nothing copied) when e is not supported. */
hubemu::SaveSession begin_save_session(const hub::GameEntry& e, const std::string& vault_battery_dir);

}   // namespace hookshot
