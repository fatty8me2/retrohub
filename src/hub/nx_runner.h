#pragma once
/* Which program plays a Switch game, and what Fritchii needs on top of Lichii. UI-free; tested by test_nx_runner.
 *
 * Fritchii (emulators.h Kind::Fritchii) is Frizti's Ocarina-of-Time fork of Lichii. It runs through ALL of Lichii's code (install, upgrade, config, keys, firmware,
 * profiles, controls, hang watch) with its own install folder <data>\emulators\fritchii and its OWN `user\` folder. Nothing is linked into Lichii's folder
 * (no junctions, no symlinks): everything shared is COPIED, and every copy that could replace data is backed up first.
 *
 * Setting "switch.runner" (game_settings.cpp): "lichii" (default) | "fritchii" | "hookshot". Hookshot is decided first (hookshot::use_for); this header decides
 * between the two emulators. */
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "emulators.h"
#include "game_settings.h"

namespace hubemu {

constexpr const char* kRunnerKey = "switch.runner";
enum class NxRunner { Lichii, Fritchii, Hookshot };
/* The stored value ("fritchii" / "hookshot"; anything else, including a missing value and the old "auto", = Lichii). */
NxRunner nx_runner_value(const hub::gs::Values& v);
/* Play this Switch game with Fritchii: System::Switch and the runner is exactly "fritchii". (Hookshot is checked before this by launch.cpp.) */
bool fritchii_selected(const hub::GameEntry& e, const hub::gs::Values& v);

/* Lichii or Fritchii for a Switch game. Fritchii is offline only (like Hookshot): an online or room launch always gets Lichii, whatever the setting says. */
Kind nx_emulator_for(const hub::GameEntry& e, const hub::gs::Values& v, bool online_or_room);

/* ---- config / environment ---- */
/* "Section.key" names RetroHub must NOT write into Fritchii's qt-config.ini. Fritchii resets its own tuned Ocarina keys at every start, so RetroHub's values would not
 * stick. Empty until the compatibility audit names the exact list; lichii_ini_launch / prepare_lichii take it as skip_keys. Lichii never skips anything. */
const std::vector<std::string>& nx_skip_keys(Kind k);
/* user\configrizti.ini (Fritchii's record of what it last saved): the text with `revision=999`, NO `global.` lines, every `game.` line (and any other line) kept.
 * Fritchii's MigrateFriztiSettings applies min(qt-config revision, this revision) and skips labels without a `global.` value, so after this it changes nothing.
 * An empty / missing file gives a file with just the revision. Pure. */
std::string frizti_ini_pin(const std::string& existing);
/* Before EVERY Fritchii launch, after prepare_lichii: pins the settings migration in qt-config.ini (fritchii_ini_pin) and rewrites user\configrizti.ini
 * (frizti_ini_pin; created when missing: a missing file counts as revision 0 and triggers a full apply). Returns "" or an error. */
std::string prepare_fritchii(const std::string& install_dir);
/* Fritchii's updater is neutralised twice: check_for_updates=false in qt-config.ini (lichii_ini_defaults) and this variable, so even Help > Check for Updates
 * cannot replace RetroHub's vetted build. RetroHub is the only updater. */
constexpr const char* kFritchiiUpdateUrl = "http://127.0.0.1:9/x";
/* Environment added to the child: Lichii = lichii_env (unchanged). Fritchii = EDEN_UPDATE_URL, and EDEN_NX2_PRELOAD_ALL_PIPELINES only for "Shaders at start = All" (the
 * merged cache holds ~29k pipelines = ~13 GB of driver memory: "auto" does NOT preload there; "recent" never does). Never EDEN_NX2_DEBUG_SERVER / EDEN_NX2_CRASH_DUMP, and
 * RetroHub never creates or copies an nx2_switches.txt (Fritchii accepts any variable name from it): everything goes through the process environment. */
std::vector<std::pair<std::string, std::string>> nx_env(Kind k, const hub::gs::Values& v, uint64_t physical_ram);

/* ---- keys + firmware: copied from the Lichii install when the target has none ---- */
/* Copies keys\prod.keys (+ title.keys) and the firmware .nca files from `from_dir`'s user\ into `to_dir`'s user\ for whichever of them the target lacks.
 * The source is only read. Returns "" or the first error. `did` (optional) says whether anything was copied. Copies nothing else. */
std::string nx_seed_setup(const std::string& from_dir, const std::string& to_dir, bool* did = nullptr);
/* True when nx_seed_setup would copy something (cheap: file checks only). */
bool nx_seed_setup_needed(const std::string& from_dir, const std::string& to_dir);

/* ---- shader cache seed (Lichii -> Fritchii) ---- */
/* <from>\user\cache\shader\<tid>\vulkan.fork-v53.bin (and -v54.bin) COPIED to <to>\user\cache\shader\<tid>\ when the destination is missing or older (by
 * modification time). Fritchii v4.1 merges them into its own v52 cache. A copy, never a hardlink; the file can be 1.4 GB, so the run reports progress
 * and can be cancelled. The copy goes to "<name>.retrohub-part" and is renamed into place, with the source's modification time. */
struct ShaderSeedItem { std::string src, dst; uint64_t size = 0; };
std::vector<ShaderSeedItem> shader_seed_plan(const std::string& from_dir, const std::string& to_dir, uint64_t title_id);
/* "" = done (or nothing to do); otherwise a plain reason. "skipped: ..." (free space on the destination drive under 2x the copy + 256 MiB) and "cancelled" are not
 * failures of the launch. progress(done, total) is in bytes over the whole plan. */
std::string shader_seed_run(const std::vector<ShaderSeedItem>& plan, const std::function<void(uint64_t done, uint64_t total)>& progress, const std::atomic<bool>* cancel = nullptr);

/* ---- saves: the newest save wins across Lichii, Fritchii and the vault ---- */
struct NxSaveSync {
    Kind launching = Kind::Fritchii;   // the emulator about to start (Lichii or Fritchii)
    uint64_t title_id = 0;
    std::string lichii_dir, fritchii_dir;   // install dirs; "" or without a user\ folder = not there
    std::string battery;                    // vault battery dir of the game: <vault>\switch\<key>\battery
    std::string game_label;                 // folder name under backup_root (the game key, made file-safe, is used when empty)
    std::string backup_root;                // default_save_backup_root() when empty
};
struct NxSaveSyncReport {
    bool ok = true;
    bool ran = false;                      // false: the other emulator is not installed, nothing was compared
    int copied = 0;                        // files written (to the vault or to the launching emulator)
    int backups = 0;                       // trees saved under backup_root
    std::vector<std::string> backup_dirs;  // the folders written under backup_root
    std::string error, summary;
};
/* %LOCALAPPDATA%\RetroHub\save-backups (RetroHub never deletes anything under it). */
std::string default_save_backup_root();
/* Before launching `launching`: make ITS save folder for the game hold the newest save, whichever install or the vault has it.
 *   1. other install -> vault (newest wins, with the other install's own "last agreed" manifest),
 *   2. vault -> launching install (newest wins, with its manifest).
 * Before a tree is overwritten, or left behind holding changes made since it last synced, it is copied to
 * <backup_root>\<game>\<yyyymmdd-hhmmss>-before-<emulator>-launch\<vault|lichii|fritchii>-before-sync... Nothing is ever deleted; a missing folder on any side is fine; if a
 * backup cannot be written the launch ends with ok=false and NOTHING has been written: every snapshot is planned and taken BEFORE the first write, each in its own folder
 * (vault-before-sync, lichii-before-sync, fritchii-before-sync) under a stamp folder that did not exist before, and a snapshot never overwrites a file already in the backup
 * folder. The _conflicts copies of sync_tree_newest are never pruned either. A no-op when the other emulator is not installed (a Lichii-only PC
 * behaves exactly as before). The session's own begin_session / end_session still run afterwards, with the manifest suffix nx_manifest_suffix(kind). */
NxSaveSyncReport nx_sync_saves_before_launch(const NxSaveSync& in);
/* "" for Lichii, "-fritchii" for Fritchii (a manifest per emulator: a shared one would let a stale copy win). */
const char* nx_manifest_suffix(Kind k);

}  // namespace hubemu
