# Nintendo 3DS (Azahar) and Wii U (Cemu)

Both systems run through an external emulator that RetroHub downloads on demand, configures once and
launches with the game path, like Dolphin for GameCube (`docs/DOLPHIN.md`). The code is in
`src/hub/emulators.{h,cpp}` (download, config, launch, save round trip), `src/common/emu_formats.{h,cpp}`
(header parsers), `src/common/emu_saves.{h,cpp}` (save-folder mapping, newest-wins tree copy) and the
identification in `src/common/library.cpp`. Test: `hub_emu_test` (offline; `--download <dir>` also downloads both
emulators into a scratch dir and never starts them).

System short names are `3ds` ("Nintendo 3DS", `Engine::Azahar`) and `wiiu` ("Wii U", `Engine::Cemu`).

## Library

| Format | Identification | Key | Serial (catalog match) |
|---|---|---|---|
| `.3ds` `.cci` (NCSD) | `NCSD` at 0x100, partition 0 NCCH, product code at NCCH+0x150 | `3ds:<title id, 16 hex>` | 4-char game code ("EKJA" from `CTR-P-EKJA`) |
| `.cxi` (NCCH) | same NCCH header | same | same |
| `.cia` | header 0x2020, TMD title id | same | 8 hex digits of the title id low word |
| `.3dsx` | `3DSX` magic only | `3ds:file-<name>` | none |
| `.zcci .zcia .zcxi .z3dsx` | Azahar's own compressed formats (`Z3DS` magic) | `3ds:file-<name>` | none |
| `.zip` holding one 3DS image | header read from the first 17 KB of the entry (nothing extracted) | as above | as above |
| `.wua` | ZArchive footer + root directory (`<titleid>_v<n>` folders); base game `00050000-...` wins over update/DLC | `wiiu:<title id, 16 hex>` | the 16-hex title id, upper case |
| `.rpx` (loadiine folder) | `<game>/meta/meta.xml` `<title_id>`; base games only; no meta.xml = not listed | `wiiu:<title id>` | same |
| `.wud` `.wux` | disc id text "WUP-P-ALZE" (wux: sector 0) | `wiiu:wup-p-<code>` | 4-char code |

Updates, DLC and system titles (`0004000E`, `0004008C`, ...) are not listed as games.

`GameEntry::issue` flags what the player must know, and the hub shows it as a card badge and in the details dialog:
- `extract`: the 3DS image is inside a `.zip`. **Azahar cannot open zip files** (it reads `.3ds .cci .cia .cxi .3dsx` and
  its own `.zcci .zcia .zcxi .z3dsx`; source: `citra_qt.cpp` file filters). Pressing Play asks first, then unpacks the one
  image into `<data>\rom-cache\3ds\<key>\` with a progress bar (needs the free disk space; the zip is untouched).
  Nothing is ever unpacked silently. Unpacking the zip yourself into a ROM folder works too.
- `encrypted`: NCCH "NoCrypto" flag (flags[7] bit 2) is not set. Azahar then needs the user's own AES keys (`aes_keys.txt`
  in its `user\sysdata`). RetroHub does not supply keys.
- `keys`: `.wud` / `.wux` discs need the disc key in Cemu's `keys.txt`. **`.wua` archives need no keys** (already decrypted);
  loadiine `.rpx` folders are decrypted too.

The user's files (read-only check, 2026-10-22): all nine 3DS zips under `Everything\Games` (X, Y, Omega Ruby, Alpha
Sapphire, Sun x2 identical copies, Moon, Ultra Sun, Ultra Moon) hold a single `.3ds` NCSD image, **all decrypted**
(NCCH flags `0000000001030004`, NoCrypto set), so Azahar needs no keys; they are zipped, so each needs the one-time unpack.
Twilight Princess HD is a `.wua` with the base game `000500001019e500_v81` and update `0005000e1019e500_v81`.

## Emulators

| | Azahar (3DS) | Cemu (Wii U) |
|---|---|---|
| Source | github.com/azahar-emu/azahar (official Citra successor, GPL-3.0) | github.com/cemu-project/Cemu (MPL-2.0) |
| Pinned release | 2126.2 (2026-10-08), `azahar-windows-msvc-2126.2.zip`, 43,941,048 bytes | v2.6, `cemu-2.6-windows-x64.zip`, 26,374,093 bytes |
| SHA-256 | `4412eac8...9db0` (GitHub's published asset digest) | `a6bcc2bc...0896` (GitHub lists none; computed from the official asset, trust on first use) |
| Install dir | `<data>\emulators\azahar\` | existing install if found, else `<data>\emulators\cemu\` |
| Portable data | `azahar\user\` (a `user` folder beside the exe is Azahar's portable mode: `common/file_util.cpp`) | `cemu\portable\` (Cemu's portable folder: `gui/wxgui/CemuApp.cpp`) |
| Launch | `azahar.exe -f "<game>"` (`-w` windowed; the path must be last) | `Cemu.exe -g "<game>" -f` |

Download: WinHTTP to `<data>\emulators\_downloads\`, size and SHA-256 checked before anything is unpacked, then RetroHub's own unzip (miniz;
every entry name is checked for `..`, drive letters and absolute paths, and any entry that cannot be written fails the install). The folder holding
the exe is moved into place and `retrohub-install.txt` is written last: only an install with that marker counts as installed, and any failure deletes
the half-installed folder. A mismatch deletes the download and shows the error. Verified by `hub_emu_test --download` into a scratch dir: Azahar
`azahar.exe` (35,499,008 bytes) and Cemu `Cemu.exe` (24,490,496 bytes) were found after extraction (including Cemu's RTL-mark file name), and
neither was started.

**Sessions.** The emulator runs inside a Windows job object; the save copy-back and the vault sync wait until every process in it has exited, so
a hand-off or relaunch cannot end the session early. A launch is refused while an `azahar.exe` / `Cemu.exe` is already running (it would hand the game to
that instance). Save-copy failures are shown as a toast and never overwrite anything.

### Finding Cemu
1. The path chosen in Settings (`cemu_exe` in `hub.json`).
2. `Cemu.exe` in the game file's folder or up to three folders above (the user's `Cemu_2.6\game\x.wua` finds `Cemu_2.6\Cemu.exe`).
3. Usual places (Desktop, Documents, Downloads, `C:\Cemu`, `C:\Games`, `C:\Emulators`, Program Files, two levels deep, names containing "cemu").
4. RetroHub's own download.

An existing install is used as it is: RetroHub never writes to its settings, controller profiles or mlc folder.

## Pre-written config (RetroHub-managed installs only)

**Azahar** (`user\config\qt-config.ini`, written once; marker `retrohub-pad.txt`, delete it to write again):
no first-run state (`firstStart`, `confirmClose`, discord presence, update check off) and the controller mapped by position
to "any SDL game controller" (`engine:sdl,api:controller,maptype:all`, so the first XInput pad without needing its GUID):
Xbox B -> 3DS A, A -> B, Y -> X, X -> Y, d-pad, LB/RB -> L/R, triggers -> ZL/ZR, Start/Back -> Start/Select, left stick -> circle pad,
right stick -> C-stick. Audio is on (Azahar's default). Fullscreen comes from the launch flag.

**Cemu** (`portable\settings.xml`, `portable\controllerProfiles\controller0.xml`, each written only if missing): `settings.xml`
existing is what suppresses the Getting Started dialog; update checks off. Controller 0 = "Wii U GamePad" on XInput pad 0
with Cemu's own default mapping ids. Audio uses Cemu's defaults.

## Saves in the vault

Vault layout is the usual `<vault>\<system>\<key>\battery\`. The emulator's per-title save folders are mirrored under it with the
same relative paths, so the vault tree can be copied back verbatim on another PC:

| System | Emulator folder | Mirrored as |
|---|---|---|
| 3DS | `user\sdmc\Nintendo 3DS\<id0>\<id1>\title\00040000\<id low>\data` (+ `extdata\00000000\<id low>` when it exists) | `battery\title\...`, `battery\extdata\...` |
| Wii U | `<mlc>\usr\save\00050000\<id low>` | `battery\usr\save\00050000\<id low>` |

- **Before launch** (vault -> emulator) and **after exit** (emulator -> vault), per file: missing = copied; same bytes = skipped (no backup);
  different = the side that changed since the last agreed state wins (`<vault>\.sync\emu-*.json` holds the agreed content hashes, so a skewed clock cannot
  make an old copy win); with no history the newer modification time wins and the older file is kept in `<vault>\_conflicts\<system>\<key>\battery\<rel>.local.<time>`
  (the same `_conflicts` area the sync uses; the newest 10 per file are kept). Nothing is deleted. The vault sync is queued while the
  game runs and starts after the copy-back (`poll_process`).
- **RetroHub-managed Azahar/Cemu:** both directions as above.
- **The user's existing Cemu:** its saves are not moved or overwritten. The Wii U data dir follows Cemu's rules (`portable` folder, legacy
  `settings.xml` beside the exe, else `%APPDATA%\Cemu`; `mlc_path` from `settings.xml` if set). Before launch the vault copy is restored
  **only if the game's save folder there has no files** (fresh PC); after the session its folder is backed up into the vault (newer wins).
  Their `%APPDATA%\Cemu\mlc01\usr\save\00050000\1019e500` for Twilight Princess HD is the folder involved on this PC.
- **`.wud`/`.wux`:** the title id is not readable without decrypting, so the save folder is learned from what changed in
  `usr\save\00050000` during a session (exactly one changed folder) and remembered in the vault for next time.
- Azahar keeps a title's save data (cartridge and digital alike) in `title\00040000\<id low>\data` as the folder `00000001\` plus
  `00000001.metadata` (`file_sys/archive_source_sd_savedata.cpp`); the whole `data` folder is mirrored. Extdata is best effort (its id is
  not always the title's unique id). `.3dsx` homebrew and anything without a title id has no mapping.

## Needs a visible check (not done headlessly)

Launching a game was not done (no visible windows). To verify on the user's PC: play one 3DS game and one Wii U game and confirm
(1) the emulator starts fullscreen with sound and the first controller, (2) the mapping feels right (Azahar `[Controls]` values and
the Cemu profile were taken from the emulators' source, not from a run), (3) after exit the vault has `battery\...` files, (4) the Azahar
config file is not rewritten into something that drops the pad (it should keep the `profiles` array it builds from our keys).

## Sources

- Azahar: `src/citra_qt/citra_qt.cpp` (command line, file filters), `src/common/file_util.cpp` (`user` folder), `src/citra_qt/configuration/config.cpp`
  (qt-config.ini keys), `src/input_common/sdl/sdl_impl.cpp` (`maptype:all`, `api:controller`), `src/common/settings.h` (button/analog names).
- Cemu: `src/gui/wxgui/CemuApp.cpp` (portable folder, first start = no `settings.xml`), `src/config/LaunchSettings.cpp` (`-g -f -m`),
  `src/config/CemuConfig.cpp`, `src/gui/wxgui/wxCemuConfig.cpp`, `src/input/InputManager.cpp` + `src/input/emulated/*.cpp` (controller profile
  format and default XInput mapping), `src/input/api/ControllerState.h` (mapping ids).
- ZArchive footer/tree layout: github.com/Exzap/ZArchive; NCSD/NCCH/CIA: 3dbrew.org.
