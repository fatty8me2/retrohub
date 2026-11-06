#pragma once
/* Nintendo Switch / Switch 2 via Lichii (github.com/zortos293/Lichii, an Eden fork; Kind::Lichii in emulators.h, pinned to build 88).
 * RetroHub's own portable download only: a `user` folder beside lichii.exe makes it portable (Lichii common/fs/fs_paths.h PORTABLE_DIR),
 * so a player's own Eden/yuzu/Ryujinx install is NEVER written to. UI-free; see docs/EMULATORS.md (Switch).
 *
 * The player supplies keys and firmware from a console they own. RetroHub never downloads them; it can only COPY the player's own files
 * (from a folder they choose, or from an Eden / yuzu / Ryujinx / Citron / Suyu / Sudachi install already on this PC) into
 * <install>\user\keys and <install>\user\nand\system\Contents\registered. Sources are only read, never changed.
 *
 * Layout under <install>\user (Lichii source, build 88):
 *   keys\prod.keys, keys\title.keys
 *   nand\system\Contents\registered\*.nca                         firmware
 *   nand\system\save\8000000000000010\su\avators\profiles.dat     user profiles (ProfileManager)
 *   nand\user\save\0000000000000000\<user uuid 32 hex>\<title id 16 hex upper>\   game saves
 *   config\qt-config.ini                                          SimpleIni; every value written with a "key\default=false" twin
 *   config\custom\<TITLE ID 16 hex upper>.ini                     per-game overrides
 * Launch: lichii.exe [-f] -u 0 -g "<game>". Default hotkeys: F5 stop, F11 fullscreen, Esc leave fullscreen, F4 pause. No save states. */

#include <cstdint>
#include <string>
#include <vector>

#include "controls.h"
#include "emulators.h"
#include "game_settings.h"

namespace hubemu {

std::string lichii_user_dir(const std::string& install_dir);   // <install>\user

/* ---- keys + firmware ---- */
struct NxSetup {
    bool prod_keys = false;      // user\keys\prod.keys present (non-empty)
    bool title_keys = false;     // user\keys\title.keys present (optional)
    bool firmware = false;       // user\nand\system\Contents\registered holds .nca files
    std::string firmware_version;   // "" unknown
};
NxSetup lichii_setup_status(const std::string& install_dir);
/* What the game needs before it can start: loose Switch 2 / homebrew dumps need no keys (Lichii main_window.cpp comment);
 * NSP / NSZ / XCI / XCZ need prod.keys. Firmware is recommended for every game; prod.keys + firmware is the full setup. */
bool lichii_needs_keys(const std::string& game_path);

/* Existing installs on this PC whose keys / firmware can be copied: %APPDATA%\{eden,yuzu,citron,suyu,sudachi}\keys + nand\system\Contents\registered,
 * %APPDATA%\Ryujinx\system\prod.keys (+ bis\system\Contents\registered firmware), and a portable `user` folder beside any of their exes found in
 * the usual places. Each source lists what it has. `appdata` = %APPDATA% (passed in for tests). */
struct NxSource { std::string label, keys_dir, firmware_dir; };   // dirs "" when that part is missing
std::vector<NxSource> lichii_find_sources(const std::string& appdata, const std::vector<std::string>& extra_roots);
/* Copy prod.keys / title.keys from a folder (or a single .keys file the player picked). Returns "" or an error. Never deletes the source. */
std::string lichii_import_keys(const std::string& install_dir, const std::string& from_path);
/* Copy firmware .nca files from a folder (recursively; *.nca and *.cnmt.nca), or from a firmware .zip (miniz; entry names checked for ..).
 * Replaces what is in registered\ (as Lichii's own Install Firmware does). Returns "" or an error. */
std::string lichii_import_firmware(const std::string& install_dir, const std::string& from_path);

/* ---- profile: one fixed RetroHub user so save paths are identical on every PC ---- */
/* Writes profiles.dat with one user "Player" and the uuid below when the file is missing or holds no user; leaves an existing user list alone
 * and returns the uuid of user 0. Format (ProfileManager ProfileDataRaw): 0x10 bytes padding, then 8 x UserRaw {uuid[16], uuid2[16],
 * u64 timestamp, char username[0x20], 0x80 extra} = 0x650 bytes. */
extern const char* const kLichiiUserUuid;   // 32 hex, the RetroHub default user
std::string lichii_ensure_profile(const std::string& install_dir);
/* nand\user\save\0000000000000000\<uuid>\<TITLE ID> relative to <install>\user, forward slashes. */
std::string lichii_save_rel(const std::string& uuid, uint64_t title_id);

/* ---- config ---- */
/* qt-config.ini, pure text transforms (SimpleIni sections, "key\default=false" + "key=value" pairs).
 * Defaults (once): [UI] firstStart=false, check_for_updates=false, confirmStop=2, showConsole=false, pauseWhenInBackground=false,
 * select_user_on_boot=false, enable_discord_presence=false, singleWindowMode=true; [Renderer] fullscreen_mode=0 (borderless). */
std::string lichii_ini_defaults(const std::string& ini);
/* Every launch: [Renderer] resolution_setup (switch.res), frame_gen / frame_gen_type=1 (FSR3) / frame_gen_multiplier (switch.framegen),
 * use_vsync (switch.vsync: fifo 2, mailbox 1, off 0), use_asynchronous_shaders (switch.async_shaders: on / off; "lichii" leaves it alone),
 * gpu_accuracy (switch.gpu_accuracy: low 0 (default), high 1, "lichii" = untouched);
 * [UI] fullscreen; [System] current_user=0. */
std::string lichii_ini_launch(const std::string& ini, const hub::gs::Values& v, bool fullscreen);
/* [Controls] player 1 (player_0_*) from the RetroHub "switch" profile on one SDL pad (guid + port among same-guid pads); player_0_connected=true,
 * player_0_type=0 (Pro Controller), players 2-8 disconnected. Value format (Lichii sdl_driver.cpp):
 *   button  engine:sdl,port:<n>,guid:<guid>,button:<sdl button>
 *   d-pad   engine:sdl,port:<n>,guid:<guid>,hat:0,direction:up      (when the pad's SDL mapping uses a hat; else button)
 *   trigger engine:sdl,port:<n>,guid:<guid>,axis:<n>,threshold:0.5,invert:+
 *   stick   engine:sdl,port:<n>,guid:<guid>,axis_x:<n>,axis_y:<n>,offset_x:0,offset_y:0,invert_x:+,invert_y:+,deadzone:<0-1>,range:1
 * prepare_lichii skips the controls when the guid is empty (no pad connected), so a mapping made in Lichii itself is kept. */
/* The pad's raw SDL joystick bindings (SDL_GameControllerGetBindForButton / ForAxis). Lichii's SDL engine (yuzu lineage, sdl_driver.cpp
 * BuildParamPackageForBinding) addresses RAW joystick buttons / hats / axes, not SDL GameController numbers, so these are looked up for the
 * chosen pad before every launch (page_controllers.cpp lichii_pad_for). Without them (no pad connected) the GameController numbers are written,
 * which is right for XInput pads under SDL's default XInput mapping only. */
struct SdlBinds {
    struct B { char type = 0; int idx = -1; int hat = 0; };   // type 'b' button, 'a' axis, 'h' hat (hat = SDL_HAT_* mask); 0 = not bound on this pad
    bool valid = false;
    B button[21];   // by SDL_GameControllerButton
    B axis[6];      // by SDL_GameControllerAxis
};
std::string lichii_controls_ini(const std::string& ini, const hubctl::Profile& profile, const std::string& sdl_guid, int port, const SdlBinds* binds = nullptr);

/* Pad for player 1 from the RetroHub port order (same choice as Azahar's azahar_pad_select): the guid and the rank among same-guid pads in SDL order.
 * Returns false (guid "", port 0) when no pad is known. */
bool lichii_pad_select(const std::vector<PadDesc>& by_port, const std::vector<PadDesc>& by_sdl, std::string* guid, int* port);

/* Creates user\ (portable), writes the defaults once, the per-launch values, controls and the profile. Returns "" or an error. */
std::string prepare_lichii(const std::string& install_dir, const hubctl::Profile& profile, const std::string& sdl_guid, int port,
                           const hub::gs::Values& v, bool fullscreen, const SdlBinds* binds = nullptr);
/* [-f] -u 0 -g "<game>" */
std::string lichii_args(const std::string& game_path, bool fullscreen);

}  // namespace hubemu
