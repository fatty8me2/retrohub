#pragma once
#include "controls.h"
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
#include "emu_saves.h"

namespace hubemu {

enum class Kind { Azahar, Cemu, Dolphin, Pcsx2, Lrps2 };   // Pcsx2: PlayStation 2 (pcsx2.h); Lrps2: the libretro PS2 core for online rooms (a DLL for retrorun, not an exe)

const char* name(Kind k);                 // "Azahar" / "Cemu" / "Dolphin"
Kind kind_for(hub::System s);             // N3DS -> Azahar, WiiU -> Cemu, GC -> Dolphin, PS2 -> Pcsx2
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
std::string md5_file(const std::string& path);      // lowercase hex, "" on error (Smash ROM identity)

/* ---- config pre-writing (merged into what is already there) ---- */
/* Azahar: creates <dir>\user (portable marker) and writes qt-config.ini defaults once: no first-run prompts,
 * no update check, controller 1 (any SDL game controller, so the first XInput pad) mapped by position. */
/* Pad choice (follows the RetroHub port order): AzPad = guid + the n-th pad with that guid; empty guid = any pad. */
struct PadDesc { std::string name, guid, key; bool xinput = false; };   // key = guid[#serial]
struct AzPad { std::string guid; int port = 0; };
bool azahar_pad_select(const std::vector<PadDesc>& by_port, const std::vector<PadDesc>& by_sdl, AzPad* out);
std::string azahar_config_patch(const std::string& qt_config_ini, const hubctl::Profile& profile, const AzPad& pad);
void prepare_azahar(const std::string& install_dir, const hubctl::Profile& profile, const AzPad& pad);
void prepare_azahar(const std::string& install_dir);   // profile = the stored 3ds profile
void prepare_azahar(const std::string& install_dir, const hubctl::Profile& profile);
std::string azahar_config_patch(const std::string& qt_config_ini, const hubctl::Profile& profile);
std::string azahar_config_patch(const std::string& qt_config_ini);   // pure: the text transform
/* Cemu (RetroHub's own download only): <dir>\portable, settings.xml (skips the Getting Started dialog, no update
 * check), controllerProfiles\controller0.xml = Wii U GamePad on XInput pad 0. An existing install is never touched. */
void prepare_cemu_managed(const std::string& install_dir);   // profile = the stored wiiu profile
void prepare_cemu_managed(const std::string& install_dir, const hubctl::Profile& profile);
/* Wii U players in RetroHub port order: xinput[i] = the XInput slot of player i+1 (empty = player 1 on slot 0). Player 1 gets the
 * GamePad profile (controller0.xml), players 2-4 Pro Controllers (controller1-3.xml). */
struct CemuPads { std::vector<int> xinput; std::vector<std::string> sdl; };   // sdl[i] non-empty = player i is an SDL pad (xinput[i] = -1)
/* Like cemu_pads_with but non-XInput pads become Cemu "SDLController" players. uuid = "<index>_<SDL GUID>" (index among same-GUID pads
 * in `all_pads`, SDL order). */
CemuPads cemu_pads_with_sdl(const std::vector<PadDesc>& by_port, const std::vector<PadDesc>& all_pads);
std::string cemu_sdl_uuid(const std::string& guid, int same_guid_index);
std::string cemu_gamepad_profile_xml_sdl(const hubctl::Profile& profile, const std::string& sdl_uuid);
std::string cemu_pro_profile_xml_sdl(const hubctl::Profile& profile, const std::string& sdl_uuid, int player);
/* A user's own Cemu: data folder (portable\ / beside settings.xml / %APPDATA%\Cemu), one-time backup of controllerProfiles to
 * controllerProfiles.retrohub-backup, write RetroHub's controller0-3.xml only when `manage`, and restore the backup. */
std::string cemu_user_data_dir(const std::string& exe_dir, const std::string& appdata);
bool cemu_backup_controls(const std::string& user_data);
void prepare_cemu_own(const std::string& user_data, const hubctl::Profile& profile, const CemuPads& pads, bool manage);
bool cemu_restore_controls(const std::string& user_data);
CemuPads cemu_pads_with(const std::vector<PadDesc>& by_port);   // non-XInput pads are skipped
void prepare_cemu_managed(const std::string& install_dir, const hubctl::Profile& profile, const CemuPads& pads);
std::string cemu_gamepad_profile_xml(const hubctl::Profile& profile, int xinput_idx);
std::string cemu_pro_profile_xml(const hubctl::Profile& profile, int xinput_idx, int player);   // player 1..3 (0-based index)
std::string cemu_gamepad_profile_xml(const hubctl::Profile& profile);
std::string cemu_default_settings_xml();
std::string cemu_gamepad_profile_xml();

/* Dolphin (GameCube), RetroHub's own portable download: <dir>\portable.txt, User\Config\{Dolphin,GFX,GCPadNew}.ini.
 * `pads` = which XInput slots (0-3) hold a connected controller; adapter = a GameCube adapter (WUP-028) is plugged in.
 * Defaults (Dolphin.ini, GFX.ini, GCPadNew.ini) are written once behind a marker; every launch only updates SlotA/B and SIDevice0-3. */
struct DolphinPads {
    bool xinput[4] = {false, false, false, false};
    bool adapter = false;
    std::string dev[4];   // device when the pad on that port is not XInput/<port>/Gamepad: "SDL/0/<name>" (DualSense, Switch Pro, 8BitDo ...)
};
/* RetroHub port order -> devices: non-XInput pads become SDL devices, XInput pads are numbered among the XInput pads. */
DolphinPads dolphin_pads_with(DolphinPads base, const std::vector<PadDesc>& by_port, const std::vector<PadDesc>& by_sdl);
std::string dolphin_key_name(const std::string& key);   // profile key name -> Dolphin DInput keyboard name ("" = none)
DolphinPads detect_dolphin_pads();                        // XInputGetState x4 + USB 057E:0337
std::string dolphin_ini_defaults(const std::string& ini);                               // pure: written once
std::string dolphin_ini_ports(const std::string& ini, const DolphinPads& p);            // pure: every launch
std::string dolphin_dolphin_ini_patch(const std::string& ini, const DolphinPads& p);   // pure
std::string dolphin_gfx_ini_patch(const std::string& ini);                              // pure
std::string dolphin_pad_ini(const std::string& existing, const DolphinPads& p);         // pure: GCPad1..4
void prepare_dolphin(const std::string& install_dir, const DolphinPads& pads);   // profile = the stored gc profile
void prepare_dolphin(const std::string& install_dir, const DolphinPads& pads, const hubctl::Profile& profile);
std::string dolphin_pad_ini(const std::string& existing, const DolphinPads& p, const hubctl::Profile& profile);
/* Tidy up after older RetroHub builds (see docs/DOLPHIN.md): saves stay where they are, nothing user-made is deleted. */
void dolphin_migrate_old(const std::string& data_dir, const std::string& exe_dir, const std::string& install_dir);
std::string dolphin_args(const std::string& game_path, bool fullscreen, const std::string& gci_dir);
/* Where the emulator keeps this game's memory card folder (copied from / to the vault around a session). */
std::string dolphin_card_root(const std::string& install_dir, const std::string& game_key);

/* ---- per-game settings (game_settings.h): pure patchers + one writer called before launch ---- */
std::string dolphin_gfx_apply(const std::string& gfx_ini, const hub::gs::Values& v);        // InternalResolution, wideScreenHack
std::string azahar_config_apply(const std::string& qt_config, const hub::gs::Values& v);    // resolution_factor, layout_option
std::string cemu_settings_apply(const std::string& settings_xml, const hub::gs::Values& v); // graphic packs on/off (reversible)
void apply_game_settings(Kind k, const std::string& install_dir, const hub::gs::Values& resolved);   // Cemu: RetroHub's own portable download only

/* ---- Wii U: the user's OWN Cemu (portable\ / beside settings.xml / %APPDATA%\Cemu). Everything RetroHub changes is recorded in
 * <user data> retrohub-cemu-state.txt and put back after the session (or at the next launch after a crash). settings.xml is backed up
 * once to settings.xml.retrohub-backup. Only these are touched: <fullscreen> (made to agree with the -f flag) and, when the game's
 * "Graphic packs" setting is off, the <GraphicPack> list (saved in the state file, restored afterwards). ---- */
struct CemuOwnState {
    bool fs_touched = false;  std::string fs_orig, fs_written;   // fs_orig "" = the tag was absent
    bool gp_stripped = false; std::string gp_block;               // the user's <GraphicPack>...</GraphicPack> text
};
std::string cemu_fullscreen_apply(const std::string& settings_xml, bool fullscreen);     // sets <fullscreen> (adds it when missing)
std::string cemu_fullscreen_value(const std::string& settings_xml);                      // "true" / "false" / ""
std::string cemu_own_settings_apply(const std::string& settings_xml, const hub::gs::Values& v, CemuOwnState* st);
std::string cemu_own_settings_revert(const std::string& settings_xml, const CemuOwnState& st);
std::string cemu_own_state_text(const CemuOwnState& st);
bool cemu_own_state_parse(const std::string& text, CemuOwnState* st);
bool prepare_cemu_own_settings(const std::string& user_data, const hub::gs::Values& v);   // revert leftovers, back up once, apply, record
bool revert_cemu_own_settings(const std::string& user_data);                              // false when there was nothing to revert
/* Per-game Cemu profile (gameProfiles\<title id>.ini in the user data dir; Cemu layers it over its shipped gameProfiles\default\).
 * Only keys the game's RetroHub settings set are written (wiiu.cpu_mode); the previous file is kept as <file>.retrohub-orig
 * (or a ".retrohub-new" marker when there was none) and restored after the session. */
std::string cemu_profile_apply(const std::string& ini, const hub::gs::Values& v);
bool apply_cemu_game_profile(const std::string& user_data, uint64_t title_id, const hub::gs::Values& v);
bool revert_cemu_game_profile(const std::string& user_data, uint64_t title_id);
/* What the profile files say today: "CPU mode: Singlecore-Recompiler (Cemu default profile) ..." for the game's details page. */
std::string cemu_profile_summary(const std::string& exe_dir, const std::string& user_data, uint64_t title_id);
/* Cemu 2.6 default hotkey "toggle pad screen" = Ctrl+Tab. RetroHub sends it when View (Back) + Right bumper are held. */
std::vector<int> cemu_toggle_gamepad_keys();                // virtual-key codes, modifier first
bool cemu_toggle_combo(unsigned xinput_buttons);            // XINPUT_GAMEPAD_BACK + RIGHT_SHOULDER

/* ---- launch ---- */
std::string azahar_args(const std::string& game_path, bool fullscreen, const std::string& extra = "");   // extra: e.g. -m "nick@127.0.0.1:port"   // after the exe path
std::string cemu_args(const std::string& game_path, bool fullscreen);   // -g <game> [-f]; windowed = settings.xml fullscreen=false (see cemu_fullscreen_apply)

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
                          const std::string& vault_root, uint64_t title_id_override = 0);   // override: Wii U title id read from an unpacked .wua
/* After exit: emulator -> vault. */
void end_session(SaveSession& s);

/* PCSX2 memory card folder (relative to the install dir) for a game: synced with <vault>\ps2\<game>attery around a session. */
std::string pcsx2_card_dir_rel(const std::string& game_key);   // "memcards/RetroHub/<key>"
std::string pcsx2_safe_key(const std::string& game_key);

/* True when a process with this exe's file name is already running (a new launch would hand over to it). */
bool is_running(const std::string& exe);

/* "3ds:0004000000055d00" / "wiiu:000500001019e500" -> title id (0 if the key carries none). */
uint64_t title_id_from_key(const std::string& key);
/* Title id (base game) read from a .wua file, 0 when the file has no readable ZArchive footer or is another container. */
uint64_t wiiu_title_id_of_file(const std::string& path);
hub::FsProbe wiiu_probe();   // real file system, for hub::cemu_dirs_for / wiiu_keys_check / wiiu_installed_addons

} // namespace hubemu
