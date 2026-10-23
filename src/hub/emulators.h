#pragma once
/* External emulators for Nintendo 3DS (Azahar) and Wii U (Cemu): locate / download-on-demand,
 * portable user dirs inside RetroHub's data dir, config pre-writing, launch arguments, and the
 * save-vault round trip. UI-free (main.cpp draws the modal and spawns the process).
 * See docs/EMULATORS.md. */

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "library.h"
#include "game_settings.h"

namespace hubemu {

enum class Kind { Azahar, Cemu, Dolphin };

const char* name(Kind k);                 // "Azahar" / "Cemu" / "Dolphin"
Kind kind_for(hub::System s);             // N3DS -> Azahar, WiiU -> Cemu, GC -> Dolphin
/* Pinned release: version, archive URL, exact size and SHA-256 (checked after the download). */
struct Release {
    const char* version;
    const wchar_t* host;
    const wchar_t* path;
    uint64_t size;
    const char* sha256;       // lowercase hex of the archive
    const wchar_t* exe;       // file that must exist after extraction
    const char* license;
};
const Release& release(Kind k);

struct Install {
    std::string exe;          // full path, "" when not installed
    std::string dir;          // folder holding the exe
    bool managed = false;     // RetroHub's own download (portable, inside its data dir)
    std::string how;          // human text for Settings: "RetroHub download", "existing install (game folder)", ...
};

/* <data dir>\emulators\ */
std::string managed_root();
/* RetroHub-managed install dir for the emulator: <root>\azahar or <root>\cemu. */
std::string managed_dir(Kind k, const std::string& root = managed_root());
/* Locate an install. Cemu: the configured path, else an existing install near the game file or in the usual
 * places (never modified), else RetroHub's own download. Azahar: RetroHub's own download only. */
Install find(Kind k, const std::string& configured_exe, const std::string& game_path,
             const std::string& root = managed_root());

/* ---- download + install (one background job at a time) ---- */
struct Installer {
    enum State { Idle, Asking, Working, Failed } state = Idle;
    Kind kind = Kind::Azahar;
    std::string game_key;                 // the game to start once installed
    std::atomic<bool> done{false}, ok{false};
    std::atomic<uint64_t> got{0}, total{0};
    std::atomic<int> phase{0};            // 0 download, 1 verify, 2 unpack, 3 install
    std::string error;
    std::mutex mu;
    std::thread th;
    std::string root;                     // managed root used by the job
    ~Installer();
    void start(Kind k, const std::string& managed_root_dir);   // begins the job (state -> Working)
    /* UI thread, each frame: when the job ended, join it and return true exactly once; ok tells the result. */
    bool poll(bool* success);
};
/* Same job, synchronously (console test): returns false and fills err on failure. */
bool install_blocking(Kind k, const std::string& root, std::string* err,
                      const std::function<void(int phase, uint64_t got, uint64_t total)>& progress);
std::string sha256_file(const std::string& path);   // lowercase hex, "" on error

/* ---- config pre-writing (merged into what is already there) ---- */
/* Azahar: creates <dir>\user (portable marker) and writes qt-config.ini defaults once: no first-run prompts,
 * no update check, controller 1 (any SDL game controller, so the first XInput pad) mapped by position. */
void prepare_azahar(const std::string& install_dir);
std::string azahar_config_patch(const std::string& qt_config_ini);   // pure: the text transform
/* Cemu (RetroHub's own download only): <dir>\portable, settings.xml (skips the Getting Started dialog, no update
 * check), controllerProfiles\controller0.xml = Wii U GamePad on XInput pad 0. An existing install is never touched. */
void prepare_cemu_managed(const std::string& install_dir);
std::string cemu_default_settings_xml();
std::string cemu_gamepad_profile_xml();

/* Dolphin (GameCube), RetroHub's own portable download: <dir>\portable.txt, User\Config\{Dolphin,GFX,GCPadNew}.ini.
 * `pads` = which XInput slots (0-3) hold a connected controller; adapter = a GameCube adapter (WUP-028) is plugged in.
 * Defaults (Dolphin.ini, GFX.ini, GCPadNew.ini) are written once behind a marker; every launch only updates SlotA/B and SIDevice0-3. */
struct DolphinPads { bool xinput[4] = {false, false, false, false}; bool adapter = false; };
DolphinPads detect_dolphin_pads();                        // XInputGetState x4 + USB 057E:0337
std::string dolphin_ini_defaults(const std::string& ini);                               // pure: written once
std::string dolphin_ini_ports(const std::string& ini, const DolphinPads& p);            // pure: every launch
std::string dolphin_dolphin_ini_patch(const std::string& ini, const DolphinPads& p);   // pure
std::string dolphin_gfx_ini_patch(const std::string& ini);                              // pure
std::string dolphin_pad_ini(const std::string& existing, const DolphinPads& p);         // pure: GCPad1..4
void prepare_dolphin(const std::string& install_dir, const DolphinPads& pads);
/* Tidy up after older RetroHub builds (see docs/DOLPHIN.md): saves stay where they are, nothing user-made is deleted. */
void dolphin_migrate_old(const std::string& data_dir, const std::string& exe_dir, const std::string& install_dir);
std::string dolphin_args(const std::string& game_path, bool fullscreen, const std::string& gci_dir);
/* Where the emulator keeps this game's memory card folder (copied from / to the vault around a session). */
std::string dolphin_card_root(const std::string& install_dir, const std::string& game_key);

/* ---- per-game settings (game_settings.h): pure patchers + one writer called before launch ---- */
std::string dolphin_gfx_apply(const std::string& gfx_ini, const hub::gs::Values& v);        // InternalResolution, wideScreenHack
std::string azahar_config_apply(const std::string& qt_config, const hub::gs::Values& v);    // resolution_factor, layout_option
std::string cemu_settings_apply(const std::string& settings_xml, const hub::gs::Values& v); // graphic packs on/off (reversible)
void apply_game_settings(Kind k, const std::string& install_dir, const hub::gs::Values& resolved);

/* ---- launch ---- */
std::string azahar_args(const std::string& game_path, bool fullscreen, const std::string& extra = "");   // extra: e.g. -m "nick@127.0.0.1:port"   // after the exe path
std::string cemu_args(const std::string& game_path, bool fullscreen);

/* ---- saves: emulator folder <-> vault ---- */
struct SaveSession {
    bool active = false;
    std::string emu_root;                 // folder the relative save paths hang off
    std::string battery;                  // vault battery dir of the game
    std::string backup_dir;               // overwritten files are kept here
    std::vector<std::string> rels;        // relative save folders (forward slashes)
    bool wiiu_unknown_title = false;      // wud/wux: find the save folder by what changed during the session
    std::string wiiu_save_parent;         // <mlc>\usr\save\00050000
    int64_t started_unix = 0;
    bool restore_only_if_missing = false; // existing Cemu install: fill an empty save folder, never overwrite
    std::string summary;                  // for a log/toast after end_session
    std::string error;                    // first failure of begin/end_session ("" = all fine)
    std::string manifest_path;            // <vault>\.sync\emu-<key>.json: what both sides agreed on last time
};
/* Before launch: vault -> emulator (newest wins; see sync_tree_newest). */
SaveSession begin_session(const hub::GameEntry& e, const Install& inst, const std::string& vault_battery_dir,
                          const std::string& vault_root);
/* After exit: emulator -> vault. */
void end_session(SaveSession& s);

/* True when a process with this exe's file name is already running (a new launch would hand over to it). */
bool is_running(const std::string& exe);

/* "3ds:0004000000055d00" / "wiiu:000500001019e500" -> title id (0 if the key carries none). */
uint64_t title_id_from_key(const std::string& key);

} // namespace hubemu
