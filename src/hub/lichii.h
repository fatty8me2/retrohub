#pragma once
/* Nintendo Switch / Switch 2 via Lichii (github.com/zortos293/Lichii, an Eden fork; Kind::Lichii in emulators.h, pinned to build 130, release tag build-130).
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
#include <utility>
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
 * gpu_accuracy (switch.gpu_accuracy: low 0 (default), high 1, "lichii" = untouched),
 * frame_pacing_mode (switch.pacing: auto 0, 30 1, 60 2, smooth60 = 2 + frame generation 2x overriding switch.framegen (the target counts
 * presented frames, generated ones included: 30 real + 30 generated), "lichii" = untouched);
 * [UI] fullscreen; [System] current_user=0. */
/* skip_keys: "Section.key" names the caller does not want written (Fritchii resets its own tuned graphics keys at every start: nx_runner.h kFritchiiSkipKeys). nullptr = write all (Lichii). */
std::string lichii_ini_launch(const std::string& ini, const hub::gs::Values& v, bool fullscreen, const std::vector<std::string>* skip_keys = nullptr);
/* Fritchii only (nx_runner.h): [Renderer] frizti_settings_revision=999 (+ the \default=false twin), oot_fps_asked=true, dyna_state=2 (EDS2) and
 * vertex_input_dynamic_state=true (what Fritchii's shader recipe is recorded with), [UI] fritchii_whats_new_seen. Fritchii reapplies 15 "tuned" graphics keys at start
 * from its own settings revision; a revision above its own (3 in v4.1) means "already applied", so RetroHub's values stay what Lichii would get. Pure. */
constexpr int kFriztiPinRevision = 999;
/* Fritchii shows a one-time "What's new" window (when the game stops or fails to boot) while [UI] fritchii_whats_new_seen differs from its build
 * version (Common::g_build_version, exact string compare). Since v5.5 RetroHub's CI build is named like Frizti's own ("Fritchii", version "v" +
 * dist/frizti_version.txt), so the pinned build reports "v5.5"; writing it keeps the window shut. (v5's CI build reported "build-3": named "Lichii",
 * every FritchiiUi::Enabled() path was off, the Preparing shaders screen included.) Change it with every Fritchii pin. */
constexpr const char* kFritchiiWhatsNewSeen = "v5.5";
std::string fritchii_ini_pin(const std::string& ini);
/* [Controls] player 1 (player_0_*) from the RetroHub "switch" profile on one SDL pad (guid + port among same-guid pads); player_0_connected=true,
 * player_0_type=0 (Pro Controller), players 2-8 disconnected. Value format (Lichii sdl_driver.cpp):
 *   button  engine:sdl,port:<n>,guid:<guid>,button:<sdl button>
 *   d-pad   engine:sdl,port:<n>,guid:<guid>,hat:0,direction:up      (when the pad's SDL mapping uses a hat; else button)
 *   trigger engine:sdl,port:<n>,guid:<guid>,axis:<n>,threshold:0.5,invert:+
 *   stick   engine:sdl,port:<n>,guid:<guid>,axis_x:<n>,axis_y:<n>,offset_x:0,offset_y:0,invert_x:+,invert_y:+,deadzone:<0-1>,range:1
 * prepare_lichii skips the controls when the guid is empty (no pad connected), so a mapping made in Lichii itself is kept - except a Microsoft pad's
 * old non-XInput mapping, which is converted to the XInput guid so the pad works when it is switched on later. */
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

/* ---- which GUID / button numbers Lichii's own SDL3 will see (checked against the SDL 3.4.14 sources, the SDL Lichii build 88 links) ----
 * Lichii's GetGUID zeroes the name-CRC bytes (2-3) of SDL's joystick GUID, so every guid written to the ini has them zeroed (lichii_controls_ini
 * does this itself). Lichii runs SDL3 with RAWINPUT off and the HIDAPI Xbox driver off, so an Xbox / XInput pad is read by SDL's XInput driver:
 *   guid = 03 00 | 00 00 | vendor LE | 00 00 | product LE | 00 00 | version LE | 'x' (0x78) | XInput subtype (1 = gamepad), from XInputGetCapabilitiesEx;
 *   11 buttons A B X Y LB RB Back Start L3 R3 Guide = 0..10, hat 0 = d-pad, axes 0 LX, 1 LY, 2 LT, 3 RX, 4 RY, 5 RT.
 * RetroHub's own SDL2 reads the same pad through RAWINPUT (driver 'r', other button / axis numbers), so those must not be written for Lichii. */
std::string lichii_zero_guid_crc(const std::string& guid);   // 32 hex digits -> lower case, chars 4-7 zeroed; anything else unchanged
std::string lichii_xinput_guid(unsigned vendor, unsigned product, unsigned version, unsigned subtype);
bool lichii_guid_is_xinput(const std::string& guid);          // 32 hex digits with driver byte 'x'
/* A Microsoft-vendor (045e) guid that is not XInput yet (SDL2 RAWINPUT 'r', WGI 'w'...) -> the same pad's XInput guid (subtype 1); "" otherwise. */
std::string lichii_guid_to_xinput(const std::string& guid);
SdlBinds lichii_xinput_binds();                               // the numbers above, per SDL_GameControllerButton / Axis
/* HIDAPI pads from Sony (054c) and Nintendo (057e): SDL guid driver byte 14 = 'h'. RetroHub's SDL2 (2.30.9) reports the D-pad of these as raw BUTTONS 11-14, but
 * SDL3 (3.2.14 sources read for ps4 / ps5 / switch; Lichii's / Fritchii's bundled SDL3.dll could not be version-checked offline) exposes it as HAT 0
 * (SDL_SendJoystickHat; 1 hat). Buttons 0-10 and the 6 axes (triggers are axes 4 / 5 in both versions) are unchanged. A D-pad bind that is neither unbound nor SDL2's
 * default 11-14 is kept. SDL3 has no raw button 15+ on these pads: touchpad (Sony) / capture (Nintendo) move to 11, other misc / paddle binds are dropped.
 * Xbox (XInput), other HIDAPI vendors and generic (DirectInput / RAWINPUT) pads are returned unchanged. */
bool lichii_guid_is_hidapi(const std::string& guid);          // 32 hex digits with driver byte 'h'
SdlBinds lichii_sdl3_binds(const std::string& guid, const SdlBinds& sdl2_binds);
/* Player 1's button A guid + port as currently written in the ini; false when there is none. */
bool lichii_ini_player0(const std::string& ini, std::string* guid, int* port);
struct XinputId {
    bool present = false;   // an XInput pad sits in that slot (not the "unknown" subtype)
    unsigned vendor = 0, product = 0, version = 0, subtype = 0;
    std::string guid() const { return lichii_xinput_guid(vendor, product, version, subtype); }
};
XinputId lichii_xinput_id(int slot);   // slot 0-3, read from xinput1_4 exactly like SDL3 does (generic 045e:028e when CapsEx is missing)
/* Lichii port of the pad in `slot` = its rank among the present slots with the same guid (SDL3 lists XInput pads in slot order). false = nothing in that slot. */
bool lichii_xinput_pad(const XinputId ids[4], int slot, std::string* guid, int* port);

/* Pad for player 1 from the RetroHub port order (same choice as Azahar's azahar_pad_select): the guid and the rank among same-guid pads in SDL order.
 * Returns false (guid "", port 0) when no pad is known. */
bool lichii_pad_select(const std::vector<PadDesc>& by_port, const std::vector<PadDesc>& by_sdl, std::string* guid, int* port);

/* Creates user\ (portable), writes the defaults once, the per-launch values, controls and the profile. Returns "" or an error. */
std::string prepare_lichii(const std::string& install_dir, const hubctl::Profile& profile, const std::string& sdl_guid, int port,
                           const hub::gs::Values& v, bool fullscreen, const SdlBinds* binds = nullptr, const std::vector<std::string>* skip_keys = nullptr);
/* Environment variables added to Lichii's own (the child inherits RetroHub's environment plus these; launch.cpp spawn()).
 * switch.preload_shaders: "all" -> EDEN_NX2_PRELOAD_ALL_PIPELINES=1 (Lichii builds every cached pipeline at start, ~0.45 MB driver memory each, unchanged in build 130; the 9000-frame eviction is skipped then);
 * "auto" -> the same when physical_ram_bytes >= 60 GiB (a PC sold as 64 GB reports ~63.7 GiB); "recent" (the default since 1.40.4; also missing / unknown)
 * -> nothing. Pure.
 * switch.fast_reads: anything but "off" -> EDEN_NX2_NO_REACTIVE=1 (lichii_fast_reads). */
std::vector<std::pair<std::string, std::string>> lichii_env(const hub::gs::Values& v, uint64_t physical_ram_bytes);
/* switch.fast_reads ("Faster GPU reads"): anything but "off" (missing counts as on) -> EDEN_NX2_NO_REACTIVE=1 (Lichii build 125+ drops its read traps on Switch 2
 * programs, still read in build 130; stale reads are possible but rare). One helper for every Lichii-family launch: lichii_env (Lichii) and nx_env (Fritchii) both call it. */
bool lichii_fast_reads(const hub::gs::Values& v);
uint64_t physical_ram_bytes();   // GlobalMemoryStatusEx ullTotalPhys; 0 when unknown
/* [-f] -u 0 -g "<game>" */
std::string lichii_args(const std::string& game_path, bool fullscreen);

}  // namespace hubemu
