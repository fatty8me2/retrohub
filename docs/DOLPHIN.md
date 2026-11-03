# Dolphin integration reference

Integration notes for launching GameCube games through Dolphin as an external,
portable process from RetroHub.

Status legend:
- **[src]** verified in Dolphin source (dolphin-emu/dolphin, `master`, read 2026-10-19).
- **[run]** verified by running the installed build.
- **[unverified]** not confirmed; test before relying on it.

Source base URL used below: `https://github.com/dolphin-emu/dolphin/blob/master/`
(raw: `https://raw.githubusercontent.com/dolphin-emu/dolphin/master/`).

---

## 1. Installed build

| Item | Value | Status |
|---|---|---|
| Build | Dolphin 2509 (x64, stable track per the URL naming) | [unverified] stable track; see note |
| Download URL | `https://dl.dolphin-emu.org/releases/2509/dolphin-2509-x64.7z` | [run] HTTP 200, 18,402,050 bytes |
| Archive format | 7z, extracted with Windows `tar.exe` (bsdtar) | [run] |
| Install dir | `Hub\deps\dolphin\` (Dolphin.exe, DolphinTool.exe, DSPTool.exe, Sys\, Languages\, QtPlugins\, portable.txt) | [run] |
| Portable marker | empty `portable.txt` beside `Dolphin.exe` | [src] `Source/Core/UICommon/UICommon.cpp` (checks `exe_dir/portable.txt`, then uses `exe_dir/User/`) |
| Installed size | about 64 MB excluding `_downloads` | [run] |

Note on the track: the update API `https://dolphin-emu.org/update/latest/beta` currently
returns `2609a` (dated 2026-10-08, Windows x64 at
`https://dl.dolphin-emu.org/releases/2609a/dolphin-2609a-x64.7z`). The stable endpoint
`https://dolphin-emu.org/update/latest/stable` returned an HTTP 500 page during research, so the
stable identity of 2509 was taken from the task brief and the URL resolving, not from the API.
Re-check the stable endpoint before a release.

The download page `https://dolphin-emu.org/download/` is client-rendered and gave no links to
scrape. Use the update API or the `dl.dolphin-emu.org/releases/<ver>/` pattern.

Fetch script: `Hub\scripts\fetch-dolphin.ps1` (idempotent; reuses the archive and skips
extraction when `Dolphin.exe` is present).

---

## 2. Command line

Source: `Source/Core/UICommon/CommandLineParse.cpp` (`CreateParser`) and
`Source/Core/DolphinNoGUI/MainNoGUI.cpp` (headless entry, not used here).

| Flag | Meaning | Status |
|---|---|---|
| `-e <file>` / `--exec <file>` | Boot a game file (`.iso`, `.gcm`, `.rvz`, `.ciso`, etc.). Repeatable (`append`). | [src] |
| `-b` / `--batch` | Run without the main UI. Help text: "Requires --exec or --nand-title". | [src] |
| `-u <dir>` / `--user <dir>` | Use this folder as the user directory (overrides portable default). | [src] |
| `-C <System>.<Section>.<Key>=<Value>` / `--config` | Layer a config value for this run only. Repeatable. | [src] |
| `-s <file>` / `--save_state <file>` | Load this save state at boot. | [src] |
| `-n <16 hex>` / `--nand_title` | Launch a Wii NAND title (not needed for GameCube). | [src] |
| `-v <backend>` / `--video_backend` | Override video backend (config name, see section 6). | [src] |
| `-a HLE` or `-a LLE` / `--audio_emulation` | Audio emulation. | [src] |
| `-c` / `--confirm` | Set Confirm on Stop (CLI-side). | [src] |
| `-m <file>` / `--movie` | Play a `.dtm` movie. | [src] |

Config override syntax (from `CommandLineConfigLayerLoader`): the parser splits on `.` for the
system name, then the section, then reads the key up to `=`, then the value up to the next `=`.
Multiple `-C` flags can be given. An unknown system name is silently ignored.

System names accepted in `-C` (from `system_to_name` in `Source/Core/Common/Config/Config.cpp`):

| `-C` prefix | Maps to | Used for |
|---|---|---|
| `Dolphin.` | `Dolphin.ini` (Main) | Core, Display, Interface, Analytics, AutoUpdate, NetPlay |
| `Graphics.` | `GFX.ini` (GFX) | Hardware, Settings |
| `GCPad.` | `GCPadNew.ini` | GameCube pad profiles |
| `GCKeyboard.` | keyboard pad profiles | |
| `SYSCONF.` | SYSCONF | |
| `Logger.` | Logger | |

Examples (all [src] for syntax; the file names in parentheses are [unverified]):

```
Dolphin.exe -b -e "D:\Games\Melee.rvz" -C Dolphin.Display.Fullscreen=True
Dolphin.exe -e "D:\Games\Melee.rvz" -u "C:\Hub\deps\dolphin\User" -C Dolphin.Core.SlotA=8
Dolphin.exe -b -e "D:\Games\Melee.rvz" -s "C:\Vault\Melee\melee.sav" -v D3D12
Dolphin.exe -b -e "D:\Games\Melee.rvz" -C Dolphin.Core.GCIFolderAPathOverride=C:\Vault\Melee\GCI
```

Notes on `-C`: the value cannot contain `=`. Spaces in paths are fine. Quote the whole argument
only if the shell requires it; the `=` split means a bare `C:\a b` value works.

### Fullscreen

- `-C Dolphin.Display.Fullscreen=True` sets `MAIN_FULLSCREEN` (`Source/Core/Core/Config/MainSettings.cpp`, `{System::Main, "Display", "Fullscreen"}`). [src]
- In batch mode `MainWindow` skips `show()` and the render window is used instead (`RenderToMain` is forced off in batch; `CommandLineParse.cpp`). [src]
- Verified: the render window opens fullscreen in batch mode (Melee, scratch install, real launch path).
- Borderless alternative: `-C Graphics.Settings.BorderlessFullscreen=True` [src, key from `VideoCommon/VideoConfig.cpp`].

### Making Escape / Stop exit in batch mode

- Escape is the default Stop hotkey: `set_key_expression(HK_STOP, "Escape")` in `Source/Core/Core/HotkeyManager.cpp`. [src]
- In batch mode, `MainWindow` on stop calls `ApplicationCloseRequest()` and then `QGuiApplication::exit(0)` (`Source/Core/DolphinQt/MainWindow.cpp`, the `if (m_exit_requested || IsBatchModeEnabled())` block). [src]
- So Escape in batch mode should end the process with exit code 0. The launcher can detect exit via the process handle.
- Whether `ConfirmStop` shows a prompt before exit in batch mode is [unverified]. Set `[Interface] ConfirmStop = False` to avoid the risk (key verified, section 4).
- The launcher can also stop the emulator by closing the process; that skips any save flush, so prefer Escape first.

---

## 3. User directory layout

Portable mode: the user dir is `Hub\deps\dolphin\User\` (created on first run; it does not exist yet). [src]
Source for the portable path: `UICommon.cpp` (`exe_path + "User/"`).

```
deps\dolphin\
  Dolphin.exe, Sys\, ...            install
  portable.txt                      portable marker
  User\
    Config\
      Dolphin.ini                   main settings
      GFX.ini                       graphics settings  [confirmed: section 11]
      GCPadNew.ini                  GameCube pad profiles  [src: InputConfig "GCPadNew"]
    GC\
      USA\                          region folders: USA, EUR, JAP
        MemoryCardA.USA.raw         raw memory card (SlotA = 1)
        Card A\                     GCI folder mode (SlotA = 8, default)  [src]
        MemoryCardB.USA.raw
        Card B\
        USA\ (IPL dir)              GC IPL, not needed (SkipIPL is on by default)
    StateSaves\                     save states  [src: D_STATESAVES_IDX; folder name unverified]
    Pipes\                          named-pipe controller inputs (see section 7)  [src]
```

Memory card naming [src, `Source/Core/Core/Config/MainSettings.cpp` ~line 800-807]:
- raw card: `User\GC\<REGION>\MemoryCardA.<REGION>.raw` (B for slot B).
- GCI folder: `User\GC\<REGION>\Card A\` (B for slot B), built by `Config::GetGCIFolderPath`.
- Region folder comes from the game's region (fallback: `MAIN_FALLBACK_REGION`).

Memory card mode keys, section `[Core]` of `Dolphin.ini` [src, `MainSettings.cpp`]:

| Key | Default | Meaning |
|---|---|---|
| `SlotA` | `8` (MemoryCardFolder) | Slot A device. `8` = GCI folder mode, `1` = raw `.raw` card. Value `255` (0xFF) = none. |
| `SlotB` | `255` (none) | Slot B device. |
| `MemcardAPath` | `""` | Explicit raw card file path for slot A (overrides region default). |
| `GCIFolderAPath` | `""` | Base GCI folder for slot A (empty = `User\GC\<REGION>\Card A`). |
| `GCIFolderAPathOverride` | `""` | Absolute GCI folder for slot A that overrides everything else. |
| `MemoryCardSize` | `-1` | Card size (default). |

The enum values come from `enum class EXIDeviceType` in `Source/Core/Core/HW/EXI/EXI_Device.h`:
Dummy=0, MemoryCard=1, ..., Gecko=7, MemoryCardFolder=8, None=0xFF. [src]
The `.ini` storage of this enum as a plain integer is [unverified] until one run is inspected.
Slot B is the same with `GCIFolderBPath` and `GCIFolderBPathOverride`.

**Recommendation for save syncing:** keep `SlotA = 8` (already the default) and set
`GCIFolderAPathOverride` per launch with `-C Dolphin.Core.GCIFolderAPathOverride=<vault>\<game>\GCI`.
Each game then reads and writes its own GCI folder inside the vault, with no shared
`MemoryCardA.USA.raw` file. The override is a per-run layer, so the vault path can change per game
without editing `Dolphin.ini`.

---

## 4. Config keys to pre-set

All keys are in `Dolphin.ini` under the listed section, default values from
`Source/Core/Core/Config/MainSettings.cpp` [src].

| Purpose | Section.Key | Value to write | Default in source |
|---|---|---|---|
| Skip the first-run analytics/consent prompt | `[Analytics] PermissionAsked` | `True` | `false`. The prompt sets this to true on answer (`DolphinQt/QtUtils/AnalyticsPrompt.cpp`). Also `[Analytics] Enabled = False`. |
| Disable auto-update checks | `[AutoUpdate] UpdateTrack` | empty (`UpdateTrack =`) | Non-empty. The GUI's "disable" option stores `""` (`DolphinQt/Settings/GeneralPane.cpp`, `AUTO_UPDATE_DISABLE_STRING`). [src] |
| Turn off panic-handler dialogs | `[Interface] UsePanicHandlers` | `False` | `true` |
| No Confirm on Stop | `[Interface] ConfirmStop` | `False` | `true` |
| Hide OSD messages (optional) | `[Interface] OnScreenDisplayMessages` | `False` | `true` |

Startup behavior: [Interface] keys and Analytics keys are plain `.ini` values; write them
before the first launch so no dialog appears. Verify the analytics prompt does not appear after
the first run [unverified].

### Controller and keyboard defaults

Pad profiles live in `User\Config\GCPadNew.ini`. [src: `Source/Core/DolphinQt/...`, `InputConfig("GCPadNew", ..., "GCPad", "Pad")` in `Source/Core/Core/HW/GCPad.cpp`]

Device strings are of the form `<Backend>/<index>/<name>`:
- XInput gamepad: `XInput/0/Gamepad`. [src: backend in `Source/Core/InputCommon/ControllerInterface/XInput/XInput.cpp`; the device string `XInput/<n>/Gamepad` is confirmed by the live run (sections 10 and 11).]
- Keyboard: `DInput/0/Keyboard Mouse` (live-verified, section 11).

Example `GCPadNew.ini` fragment (section and button names [unverified]; they follow the
`GCPad1` naming and the XInput button names in `XInput.cpp`, e.g. "Button A"):

```
[GCPad1]
Device = XInput/0/Gamepad
Buttons/A = `Button A`
Buttons/B = `Button B`
Buttons/Start = `Button START`
Main Stick/Up = `Left Y+`
```

Pre-write this file for a known controller, or let the user map once via the GUI and copy the
file into the vault.

---

## 5. Save states and GCI saves pointed at an external folder

- **GCI saves:** `-C Dolphin.Core.GCIFolderAPathOverride=<folder>` redirects slot A GCI saves
  to the folder [src: `EXI_DeviceMemoryCard.cpp`, `GetGCIFolderPath` returns the override first].
  Use one folder per game so each game's GCI files sync on their own.
- **Raw card:** `-C Dolphin.Core.MemcardAPath=<file>` points slot A at one `.raw` file [src, key `MemcardAPath`]. This is one shared file, so per-game separation needs a per-game path, which is what the GCI override gives you.
- **Save states:** loading is explicit with `-s <file>` at boot [src]. The default save state folder is `User\StateSaves\` [src: `D_STATESAVES_IDX`; name unverified]. There is no source-verified config key that moves the save state folder. Options:
  1. Launcher passes `-s` for the state the user picks from the vault, and copies a newly written state back into the vault after exit. [unverified: the file name Dolphin writes for a state slot.]
  2. Replace `User\StateSaves` with a directory junction to the vault folder. This is a filesystem-level workaround and [unverified] with Dolphin.
- Save-state slot naming and whether Dolphin writes the file on exit (not only on hotkey) are [unverified].

---

## 6. Video backend names

`-v` / `-C Dolphin.Core.GFXBackend=<name>` takes the backend config name [src: `Dolphin.Core.GFXBackend`, `MAIN_GFX_BACKEND`, `Source/Core/Core/Config/MainSettings.cpp`; default from `VideoBackendBase::GetDefaultBackendConfigName`].
The exact name strings for each backend (D3D12, Vulkan, OpenGL, etc.) were not confirmed in this research [unverified]. Read them from `Dolphin.ini` after selecting a backend once in the GUI.

---

## 7. Controller input from the launcher: named pipes

Dolphin reads named-pipe controller inputs from `User\Pipes\`: every file in that directory is
opened read-only and added as a device [src: `Source/Core/InputCommon/ControllerInterface/Pipes/Pipes.cpp`,
`ScanDirectoryTree(File::GetUserPath(D_PIPES_IDX))`]. Each pipe becomes a device named after the file.
The line protocol and the device string for the pad mapping are [unverified] and should be taken
from `Pipes.cpp` before use. This is an alternative to driving keyboard or XInput from the
launcher, and it gives the launcher deterministic input.

---

## 8. Netplay feasibility

Source: `Source/Core/Core/NetPlayClient.cpp`, `Source/Core/Core/Config/NetplaySettings.cpp`,
`Source/Core/UICommon/CommandLineParse.cpp`.

- **No netplay flags on the command line.** The option list in `CreateParser` has no netplay
  options. [src] Netplay is started from the GUI (DolphinQt netplay dialog). [src]
- **No scripting interface found** in the CLI or the DolphinTool/DSPTool options. [src, to the extent of the files read] There is no documented IPC or RPC for netplay.
- **Batch mode (`-b`) hides the UI**, so the netplay dialog cannot be opened in batch mode. Netplay needs a normal (non-batch) launch and a user action in the GUI.
- Config keys for netplay exist in `Dolphin.ini` `[NetPlay]` [src]:
  - `TraversalServer` default `stun.dolphin-emu.org`; `TraversalPort` 6262; `TraversalPortAlt` 6226.
  - `TraversalChoice` default `direct`.
  - `IndexServer` default `https://lobby.dolphin-emu.org`; `UseIndex` false.
  - `HostCode` default `00000000`; `HostPort`/`ListenPort`/`ConnectPort` default 2626.
- Hosting via the traversal server ("host code") and direct connect both go through the GUI [src, `NetPlayClient`].

Realistic options:
1. **Launch Dolphin normally (not batch), let the user start netplay in the GUI.** Lowest effort, works now. The launcher only passes the game and config.
2. **Launcher drives the GUI** (UI automation to open the netplay dialog and enter a host code). Fragile across Dolphin versions and not recommended.
3. **Launcher-owned lobby with input relay** over the Hub's own server, feeding each player's pad through the pipe mechanism (section 7). This is the only path with no GUI dependency, but it is a large project and the netplay sync (rollback, desync) would be entirely ours. [relay netplay with 2-4 players verified, section 10; the full rollback design is still ours]

Recommendation: option 1 for the first release. Keep batch mode for single-player and use a
normal launch when netplay is needed.

---

## 9. Launch template (summary)

```
"Hub\deps\dolphin\Dolphin.exe"
  -e "<game file>"
  -u "Hub\deps\dolphin\User"                      (optional; portable default already matches)
  -b                                              (no UI; exits on Escape/Stop)
  -s "<state file>"                               (optional)
  -C Dolphin.Display.Fullscreen=True
  -C Dolphin.Core.SlotA=8
  -C Dolphin.Core.GCIFolderAPathOverride=<vault>\<game>\GCI
```

Pre-write `Dolphin.ini` [Analytics], [AutoUpdate], [Interface] keys (section 4) before first run.

---

## 10. Verified results and open items

Verified so far:
- Melee boots fullscreen through the real launch path in a scratch install; the memory card is detected.
- Double Dash netplay ran 100 s through the relay forwarder with no desync (hidden desktop).
- Pads 1-4 map from XInput.
- Batch-mode fullscreen (section 2).

Live run 2026-10-24 (scratch install, hidden desktop, muted, `agent/gc-live`):
- **Smoke (`hub_emu_live --fullscreen --pads 4 --esc`)**: Melee and Double Dash boot fullscreen through the real launch path (first window after 2-3 s); `GCPadNew.ini` has `GCPad1-4 = XInput/0-3/Gamepad`, `SIDevice0-3 = 6`; the card folder `User\GC\RetroHub\<key>\GCI` and the vault `saves\gc\<key>attery` are created. Posted Escape key messages do NOT stop Dolphin (it reads Escape through DInput, not window messages); WM_CLOSE ends it gracefully (job empty in 1.5 s, `end_session` clean). No save file was written: answering "Create Game Data?" needs real input, which cannot be injected into a hidden desktop (no virtual XInput driver, Pipes backend is not on Windows).
- **Online (`qa\gc_room_online.ps1`, RETROHUB_AUTOTEST, local wrangler)**: Double Dash 2 players direct (`HUBNET_LOOPBACK=1`) and relay (`HUBNET_FORCE_RELAY=1`): both Dolphins in-game, alive 116-130 s, no desync line. Disc mismatch (1 byte flipped copy): `warn:different-disc` toast, session still runs. Mario Party 6 with 3 and 4 players: NetPlay player table `At0..At3 -> GC1..GC4` (room-slot order), alive 100+ s, no desync. Config checks: `WriteSaveData = False` on guests and on the host unless "Keep this session's progress"; the guests' real card folder and the host's sentinel files were untouched in both modes (no real save was created, see above).
- Fixed on the way: the driver now finds the main window by class (title is `dolphin-emu` on a fresh user dir), status lines are written with retries (a lost `stage:netplay-open` stalled the room), no 60 s host-code wait in direct mode, the autotest bypasses the by-name `is_running` check so several Dolphins can run on one PC, and the QA script only uses valid room codes.

Still open:
1. Stable identity of 2509 (the stable API returned HTTP 500 during research).
2. Whether ConfirmStop prompts in batch mode. `[Interface] ConfirmStop = False` remains the safe setting.
3. `Dolphin.ini` storage format of `SlotA` (integer assumed from the enum order).
4. `GFX.ini` file name and the `GCPadNew.ini` section/key format.
5. Keyboard device string format in the GUI-generated profile, and non-XInput pads (pending).
6. Save-state folder name and file naming; whether Dolphin writes states on exit.
7. Backend config names for `-v` / `GFXBackend`.
8. Pipe line protocol and device string.
9. Analytics prompt is fully suppressed by `PermissionAsked = True` on a fresh install.

## Sources

- Dolphin source (master): `https://github.com/dolphin-emu/dolphin`
  - `Source/Core/UICommon/CommandLineParse.cpp`, `Source/Core/DolphinNoGUI/MainNoGUI.cpp`
  - `Source/Core/Core/Config/MainSettings.cpp`, `Source/Core/Common/Config/Config.cpp`
  - `Source/Core/Core/HW/EXI/EXI_Device.h`, `EXI_DeviceMemoryCard.cpp`
  - `Source/Core/Core/HotkeyManager.cpp`, `Source/Core/DolphinQt/MainWindow.cpp`
  - `Source/Core/DolphinQt/Settings/GeneralPane.cpp`, `Source/Core/DolphinQt/QtUtils/AnalyticsPrompt.cpp`
  - `Source/Core/Core/NetPlayClient.cpp`, `Source/Core/Core/Config/NetplaySettings.cpp`
  - `Source/Core/InputCommon/ControllerInterface/Pipes/Pipes.cpp`, `.../XInput/XInput.cpp`
- Update API: `https://dolphin-emu.org/update/latest/beta` (returned 2609a; stable endpoint returned 500)
- Build download: `https://dl.dolphin-emu.org/releases/2509/dolphin-2509-x64.7z`
- Download page (client-rendered): `https://dolphin-emu.org/download/`

## 11. As integrated in RetroHub (hubemu::Kind::Dolphin)

- Install: pinned 2509 7z, size + SHA-256 (`3a910cd0...e355`) checked, entry list vetted before `tar -xf` into a staging folder, `portable.txt` + `retrohub-install.txt` marker last, any failure removes the folder. Lives in `<data>\emulators\dolphin`; same job-object session wait and "already running" refusal as Azahar/Cemu.
- Launch (live-verified): `-b -C Dolphin.Display.Fullscreen=True -C Dolphin.Core.SlotA=8 -C Dolphin.Core.GCIFolderAPathOverride=<card> -e <game>`; the render window covers the monitor with no caption. `SkipNKitWarning=True` is needed for .nkit.iso, else a dialog blocks boot.
- Pads: `GCPadNew.ini` regenerated each launch (XInput/0..3, Xbox A/X/B/Y -> GC A/B/X/Y, Z = RB, analog L/R triggers, C-stick = right stick); port 1 also accepts Dolphin's default keyboard keys (`DInput/0/Keyboard Mouse:*`). `SIDevice0..3` follow connected XInput pads; a WUP-028 adapter with a WinUSB/libusbK driver switches all ports to the adapter (12).
- Saves: card folder `User\GC\RetroHub\<key>\GCI` is synced with `<vault>\gc\<key>\battery\GCI` (manifest/conflict rules of emu_saves).
- Graphics (GFX.ini, written once): 2x internal resolution, no widescreen hack, async ubershaders, Direct3D 11.
- Defaults (Dolphin.ini, GFX.ini, GCPadNew.ini) are written once behind `User\Config
etrohub-defaults.txt`; later edits are kept. Every launch only updates `SlotA/SlotB` and `SIDevice0-3` (which ports have a pad).
- Migration from older builds (`hubemu::dolphin_migrate_old`): the old download in `<exe>\dolphin` is removed when untouched; `<data>\dolphin-user` is renamed to `dolphin-user.migrated` (save states copied to the new User folder). Memory cards were already kept in the vault (`battery\GCI`), which the new sync still reads, so saves stay put.
- GC keys: disc ID (`gc:<id>-<disc>`) from the header for iso/gcm/rvz/wia/ciso/gcz and archive entries; only an unreadable .gcz falls back to `gc:file-<name>`.

## Cheats (GameCube)
- The Cheats button works for GameCube games like for the other systems (panel, search, custom codes, first-time save backup). Formats: `gecko` (8+8 hex lines) and `ar_gc` (Action Replay: decrypted 8+8 or encrypted `XXXX-XXXX-XXXXX`, Dolphin decrypts it itself). Validation lives in `src/common/cheats.*`.
- Engine (`src/common/dolphin_cheats.*`, called from `cheats_apply_dolphin` in `cheats_ui.cpp`): right before Dolphin starts, the enabled cheats are written to `User\GameSettings\<GAMEID>.ini` (`[Gecko]`/`[Gecko_Enabled]`, `[ActionReplay]`/`[ActionReplay_Enabled]`) and `[Core] EnableCheats = True` is set in `Dolphin.ini`. RetroHub's entries are named `RetroHub: <desc>` (not starting with `[`: Dolphin's Gecko loader treats `[` as the start of the creator name); every launch removes all of them first, so turning a cheat off cleans the file. Your own codes and Dolphin's bundled ones are untouched. Dolphin cannot apply changes mid-game: the panel says changes apply the next time the game starts.
- Online (Dolphin NetPlay): `SyncCodes` is on, so the host's list is sent to every peer by Dolphin. RetroHub also carries the host's list in the launch message and each guest writes the same codes into its own ini; the version check blocks hosts whose peers run another RetroHub build.
- Database: Dolphin 2509's bundled `Sys/GameSettings` codes (GPL-2.0-or-later; libretro-database has no GameCube lists) are in `data/cheats_db.bin` (`scripts/build_cheats_db.py --gc-only --dolphin <GameSettings dir>`), matched by the 6-char disc ID. Lists named `<ID>rN` are per disc revision (addresses differ), so they are offered only when the disc's version byte (header 0x07) matches. `data/cheats_featured.json` can key curated entries by `GALE01r2`.
- Verified: `qa\gc_cheats_play.ps1` runs RetroHub's real Play path on a hidden desktop (muted), then reads game RAM through Dolphin's GDB stub (`qa\gdb_read_mem.py`): Melee r2 "Unlock All Characters and Stages" gives `FFFFFFFF` at 0x8045BF28/2C, the control run gives 0.

## Game menu Speed row (offline only)
RetroHub writes `[Core] EmulationSpeed = 1.0` every launch and binds Dolphin's Increase / Decrease Emulation Speed hotkeys in `Hotkeys.ini` (`[Hotkeys]`, `Device = DInput/0/Keyboard Mouse` if unset) to `@(Ctrl+Shift+F13)` and `@(Ctrl+Shift+F14)`. When the Game menu closes, `dolphin_send_speed_keys` (launch.cpp) sends them as scancodes with SendInput while Dolphin is foreground (Quit sends nothing).
Verified in dolphin-emu/dolphin tag 2509 [src]: hotkey names `Core/HotkeyManager.cpp` 49-51; `@(a+b)` combo syntax, same file 445-447; key names F13-F15 and combined `Ctrl`/`Shift` in `InputCommon/ControllerInterface/DInput/NamedKeys.h` 18 and `DInputKeyboardMouse.cpp` 139-140; step 0.1 with snap to 1.0 within 0.95..1.05, decrease ignored at <= 0, no upper cap in `DolphinQt/HotkeyScheduler.cpp` 519-535; presses are edge-triggered (`HotkeyManager.cpp` IsPressed 237-255).
- 0.25x runs as 0.3x (nearest reachable). There is no Max: "Disable Emulation Speed Limit" (default TAB) is hold-to-disable, not a toggle.
- DirectInput opens the keyboard `DISCL_BACKGROUND | DISCL_NONEXCLUSIVE` (`DInputKeyboardMouse.cpp` 82) and reads `GetDeviceState`, i.e. system keyboard state, so SendInput scancodes reach it (virtual-key-only events and PostMessage WM_KEYDOWN do not, so PostMessage is not a usable fallback). Hotkeys only fire while Dolphin is focused (`Core::UpdateInputGate(MAIN_FOCUSED_HOTKEYS)`, default true), hence the foreground guard.
- Not run against a live Dolphin; hidden-desktop SendInput does not reach it.

## Online speed (NetPlay pad buffer and the forwarder)
How Dolphin 2509 NetPlay paces a room (verified in source: `Core/NetPlayClient.cpp` `GetNetPads`, `NetPlayServer.cpp`, `DolphinQt/NetPlay/NetPlayDialog.cpp`):
- Every SI pad poll pops one entry per in-game pad; when the other player's entry has not arrived, the CPU thread **blocks** in `GetNetPads` (no rollback). With too small a buffer the whole room runs slow and stutters on every poll.
- The **host's** buffer is the room's (sent as `PadBuffer`); guests ignore their own `[NetPlay] BufferSize` (non host-input-authority). The host dialog starts from `[NetPlay] BufferSize` (default 5); changes during the game apply at once ("Buffer size changed to N" in the chat).
- Dolphin's guide: about 1 buffer per 15 ms of latency.

What RetroHub does:
- The GameCube forwarder (`hubnp3ds::Runner`, tag "gc") runs on its own thread with a 1 ms wake (it used to be pumped by the hub's ~60 Hz UI loop: +45 ms round trip and bursty delivery). `RETROHUB_GCFW_NOTHREAD=1` restores the old pump for comparisons.
- Host: `gc_tick` turns the session's worst ping into `gcnp::pad_buffer_for_ping` (= ceil(rtt/15)+1, at least 4, at least 6 relayed; unknown ping: 8, relayed 10) with `next_pad_buffer` hysteresis (raise at once, lower by 3+), and writes it to `gc_host_buffer.txt`; the UIA driver types it into the NetPlay window's Buffer box before Start and again whenever it changes. The driver logs `buffer:N` (typed) and `padbuffer:N` (Dolphin's chat: the room's real value).

Measured with `qa\gc_online_speed.ps1` / `hub_gc_speed` (two Dolphins on a hidden desktop, muted, scripted DSU pad, speed = the game's VI-rate counter and Dolphin's `vblank_times.txt`), 90 s each, Mario Superstar Baseball / Super Mario Strikers:

| row | path | MSB | Strikers |
|---|---|---|---|
| A | offline | 100 % | 100 % |
| B / C | Dolphin direct 127.0.0.1, RetroHub / stock config | 100 % | |
| F40 before | forwarder, sim RTT ~115 ms, UI pump, buffer 5 | 66 % | 66 % |
| F40 after | threaded, auto buffer 7 | 100 % | 100 % |
| F80 before | sim RTT ~200 ms (owner's real Dolphin ping: 177 ms), UI pump, buffer 5 | 41 % | 41 % |
| F80 after | threaded, auto buffer 13-14 | 100 % | 100 % |
| buffer sweep | threaded, RTT ~170 ms: 8 / 10 / 12 / 16 | 75 / 92 / 100 / 100 % | |

Notes: two instances on one PC cannot test Dolphin's traversal server (hairpin NAT: the guest never joins). Real Internet relay latency is not reproduced by the local wrangler.

## NetPlay memory card sync (the Super Mario Strikers desync, fixed in 1.31.8)
Symptom: Super Mario Strikers (G4QE01) logged "Possible desync detected: <host> might have desynced at frame 420" in every harness row
(direct, relay, buffer 8 or 13) and at frame 480 in the owner's two-PC session. It is a real desync, not a detector artefact.

Cause (verified in Dolphin 2509 source and logs):
- RetroHub points the card at the vault folder with `-C Dolphin.Core.GCIFolderAPathOverride=<card>`. The host's own Dolphin plays on
  that folder (`EXI_DeviceMemoryCard.cpp` `GetGCIFolderPath`: the override wins).
- Dolphin's NetPlay save sync does NOT read the override: `NetPlayServer.cpp` `SyncSaveData` sends
  `Config::GetGCIFolderPath(slot, region)` = `GCIFolderAPath` (empty: `User\GC\USA\Card A`) + the disc's region folder. That folder does
  not exist, so the host log says `Sending empty marker for GCI memcard .../User/GC/USA/Card A`, the guest says
  `Received GCI memcard data for slot A: .../NetPlayTemp/Card A, 0 files`, and the chat still says "All players' saves synchronized".
- Strikers reads the card on boot ("Checking/Loading from the Memory Card", ~7 s = Dolphin's frame 420 check). Host: save found; guest:
  blank card, it creates a new save. Different code paths, different timebase, then different menus: in the before run below the host
  was in a match while the guest's game sat in the Spoils/trophy screen with the same inputs. Every game that reads a save
  diverges the same way (Mario Superstar Baseball too, at frame 1140 in the hash run; the old 90 s speed rows did not reach it).
- Dolphin's detector (`NetPlayServer.cpp` `TimeBase`): every 60th PE-finish each client sends the emulated timebase; the first
  mismatch is reported once and checking stops (`m_desync_detected`). So one line, at the first divergence.

Fix: `gcnp::host_card_sync_args` (netplay_dolphin.cpp), called by the host in `gc_begin` (launch_online.cpp): wipe
`<data>\netplay\gc\host-sync`, copy the card folder's `.gci` files into `host-sync\USA`, `\EUR`, `\JPN` and add
`-C "Dolphin.Core.GCIFolderAPath=<data>\netplay\gc\host-sync"` (this run only, never written to Dolphin.ini). The host still plays and
saves on the vault card (the override); only the copy is sent. Guests are unchanged (Dolphin points them at NetPlayTemp).

How it was proven (two Dolphins on one PC, hidden desktop, muted, owner's disc and a copy of the owner's save):
- `qa\gc_state_hash.py` (started by `hub_gc_speed --hash-addr`): Dolphin's GDB stub + a write watchpoint on the game's VI-rate counter
  stops both instances at the same instruction of the same frame; the script hashes all 24 MiB of MEM1 there. `qa\gc_desync_check.ps1`
  runs the before/after pair and prints the detector frame and MATCH/DIFF per sample frame. The GDB stub costs ~75 % speed (it polls
  its socket from a CoreTiming event every 100k cycles), so `--emu-clock` runs the pad script on emulated time.
- Arming/disarming the watchpoint at different wall times does not perturb emulation: in the fixed runs the hashes match and
  Dolphin's detector stays silent.

| run (np-direct, single core, buffer 8) | card | Dolphin detector | MEM1 host vs guest |
|---|---|---|---|
| Strikers, before (`--card gci`) | override only, guest gets 0 files | frame 420 (every run: 3 here + all Strikers rows of the speed matrix) | 7200: DIFF, 3.9 M of 6.3 M words |
| Strikers, after (`--card hub`) | + host_card_sync_args, guest gets 1 file | none | 300 / 3600 / 7200: MATCH |
| Strikers, after, full match at 100 % (no GDB) | hub | none over 540 s | same screens: final score DK 0 - Daisy 5, identical match stats |
| Strikers, after, full match with hashes (2400 s at ~23 %: a whole match, DK 0 - Daisy 3, and part of a second) | hub | none | 18 samples, frames 300-34200: all MATCH |
| Strikers, after, guest boots 1.3 s later (1.2 s one-way forwarder latency), 5700 frames | hub | none | 300-3900 MATCH; 4200-5700 the same 9 words differ (below) |
| Strikers, after, guest boots later, match with hashes (first try: up to 1:05 left, then the guest's hasher timed out and left the guest halted; fixed) | hub | none | 300 MATCH; 3600-19800 (10 samples): the same 9 words |
| Strikers, after, guest boots later, full match with hashes (3300 s at ~22 %: "Daisy wins 3-0", then most of a second match) | hub | none | 300 MATCH; 3600-45000 (23 samples): the same 9 words, nothing else |
| Mario Superstar Baseball, before | override only | frame 1140 | 600 MATCH, 1800 / 3600 DIFF |
| Mario Superstar Baseball, after (dual core: its allowlist) | hub | none | 600 / 1800 / 3600 MATCH |
| Mario Superstar Baseball, after, speed (F80, sim RTT ~170 ms, buffer 13) | hub | none | 100 % host and guest |
| Mario Party 6, before | override only | frame 2940 | 600 MATCH, 1800 / 3600 DIFF |
| Mario Party 6, after | hub | none | 600 / 1800 / 3600 MATCH |

Rerun the A/B with `qa\gc_desync_check.ps1 -Build build\bin\Release -Dolphin <dolphin dir> -Disc <disc> -Game G4QE01 -Gci <saves>
-Work <dir with G4QE01.vi.txt> -Secs 150 -Every 600 -Start 600` (~10 min): B `detector: frame 420, 600=MATCH 1200=DIFF 1800=DIFF`,
A `detector: none, 600=MATCH 1200=MATCH 1800=MATCH`.

Ruled out on the way: the two Dolphin.ini files differ only in the analytics ID and the nickname; RTC is the host's (`GetInitialNetPlayRTC`,
sent to every client); SRAM is the host's; G4QE01's GameINI has no patches (AR codes only, off); pad buffer (8 vs 13) and forwarder
(direct vs relay) did not move the frame. A blank raw card (`--card raw`) did not desync either, consistent with "the guest misses
the host's save" rather than a card-format issue.

Two PCs: Dolphin builds a GCI folder card's header (format time, serial) from each PC's local clock at boot (`EXI.cpp`
`GetLocalTimeSinceJan1970`, not the NetPlay RTC), so host and guest headers differ on two PCs even with the fix (on one PC both
usually boot in the same second). In the skew rows the guest boots 1.3 s later. Once Strikers mounts the card exactly 9 words differ
and they stay the same 9: 0x804BDC00-13 (the header copy in the CARD work area: serial, format time 0x327B9F83 vs 0x327B9F84),
0x804BDDFC (its checksum), 0x80373E20 and 0x804BD6B0 (the card serial number the game keeps). Dolphin's timebase check stays
silent and nothing else diverges, so it is harmless for Strikers. A game that mixes the card serial into game state could still
diverge; Dolphin has no setting for it (a raw card would carry the host's header, but RetroHub's vault is GCI folders).

End to end through RetroHub's real room flow (`qa\gc_room_online.ps1 -Game gc:g4qe01-0 -SeedGci <save> -CardKey gc_g4qe01-0`,
local wrangler): `host-sync\USA|EUR|JPN` held the host's save, the guest's `NetPlayTemp\Card A` received it (41024 bytes), and
no desync line appeared.
Also: the harness's DSU pad maps its `a` to GC B and `b` to GC A (DSU face layout), which is why older patterns never left the title.
