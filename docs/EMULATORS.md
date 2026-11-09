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

A local library check (read-only, 2026-10-22): all nine 3DS zips under the game library folder (X, Y, Omega Ruby, Alpha
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
existing is what suppresses the Getting Started dialog; update checks off. Controller 0 = "Wii U GamePad" for port-order player 1 and controller1-3 = Pro Controllers for players 2-4 (Pro button ids differ from the GamePad's after Minus: Home sits at 11). An XInput pad is addressed by its real XInput slot (its SDL player index), any other pad as an `SDLController` (`<index>_<guid>`); with no stored port order every connected pad is used, in SDL order. Test: `hub_ext_pads_test`.

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

## Verified headlessly (hidden desktop, muted, loopback) and what still needs eyes

Run through RetroHub's real Play path (`RETROHUB_AUTOTEST=play`, `qa\ext_play.ps1`); the leave combo is simulated by `RETROHUB_AUTOTEST_COMBO_AFTER=<s>`
(it feeds the real `ExitCombo` timer in `pad_exit_watch`, so `request_game_close` -> WM_CLOSE runs as for a held View+Start).
- **3DS (Azahar 2126.2, Pokemon X):** boots; `qt-config.ini` keeps `button_*`/`circle_pad` (`engine:sdl,api:controller,...`) after Azahar exited and
  rewrote it, `confirmClose=false`; the combo closes Azahar gracefully; `title\00040000\00055d00\data` lands in the vault `battery\`.
- **Wii U (scratch Cemu 2.6 with a `portable` folder, Twilight Princess HD .wua):** boots; `controller0.xml` written and `controllerProfiles.retrohub-backup`
  created; combo closes Cemu; the save folder `usr\save\00050000\1019e500` is in the vault. Restore (`cemu_restore_controls`) is covered by `hub_emu_test`.
  A hand-written SDL profile survives a Cemu run (`qa\cemu_sdl_readback.ps1`; that only proves Cemu keeps the file, not that it maps pads).
- **Host leaves mid-game** (`qa\ds3ds_n3ds_room.ps1 -KillHostAfter`, `qa\gc_room_online.ps1 -KillHostAfter`; local worker): the 3DS guest logs "host left",
  Azahar closes gracefully, `end_session` syncs, "The host left - game ended" notice. The GC guest's Dolphin ignores WM_CLOSE while NetPlay has lost the host, so
  RetroHub now asks first and terminates after 20 s (it used to kill at once); the guest card is disposable. Kanto Together guest: code review only
  (`A.run.room` -> `pad_exit_fn` -> `Session::stop` WM_CLOSE on mGBA); not run, its package is a network download.
- **Still needs a person:** real pad feel, sound, fullscreen, and a game writing its own save with a physical pad.

## Sources

- Azahar: `src/citra_qt/citra_qt.cpp` (command line, file filters), `src/common/file_util.cpp` (`user` folder), `src/citra_qt/configuration/config.cpp`
  (qt-config.ini keys), `src/input_common/sdl/sdl_impl.cpp` (`maptype:all`, `api:controller`), `src/common/settings.h` (button/analog names).
- Cemu: `src/gui/wxgui/CemuApp.cpp` (portable folder, first start = no `settings.xml`), `src/config/LaunchSettings.cpp` (`-g -f -m`),
  `src/config/CemuConfig.cpp`, `src/gui/wxgui/wxCemuConfig.cpp`, `src/input/InputManager.cpp` + `src/input/emulated/*.cpp` (controller profile
  format and default XInput mapping), `src/input/api/ControllerState.h` (mapping ids).
- ZArchive footer/tree layout: github.com/Exzap/ZArchive; NCSD/NCCH/CIA: 3dbrew.org.

## Per-game settings (`src/common/game_settings.h`)
Stored in the vault at `<vault>\_settings\game_settings.json`. Layers: option default, system defaults, per-game override (`Store::resolve`). Each option is Visual (local only), Sync (host's value goes in the launch JSON as `"gs":{...}` via `sync_fragment`; guests use `extract_sync` + `online_effective`) or Locked (forced to default online: GLideN64 framebuffer-emulation options). Runner wiring: `n64run --gfx Key=i|b|s:value` (GLideN64 config, locked keys ignored online), `retrorun --filter/--integer-scale/--opt`, and `hubemu::apply_game_settings` (Dolphin GFX.ini, Azahar qt-config.ini, Cemu graphic packs).

## Wii U offline details (user-style Cemu, `%APPDATA%\Cemu`)

- **Own install, reversible.** Before each launch RetroHub changes only these things in the user's Cemu data folder (portable\, beside
  `settings.xml`, or `%APPDATA%\Cemu`) and puts them back when the session ends (or at the next launch after a crash):
  `<fullscreen>` (made to agree with `-f`: `-f` is passed only when the game's Fullscreen setting is on, otherwise the tag is set to false),
  and the `<GraphicPack>` list when the game's "Graphic packs" setting is off (the list is parked in `retrohub-cemu-state.txt`). `settings.xml` is
  copied once to `settings.xml.retrohub-backup`; nothing is written if that copy fails or Cemu has not run yet (no `settings.xml`). Changes the
  user makes inside the session (toggled fullscreen, newly enabled packs) win. RetroHub's own portable download follows the same settings without the revert.
- **Per-game profile.** The "CPU mode" game setting writes `[CPU] cpuMode` into `gameProfiles\<title id>.ini` (the user layer Cemu applies over its
  shipped `gameProfiles\default\`), keeping the previous file as `.retrohub-orig` (or a `.retrohub-new` marker) and restoring it afterwards. The
  details page shows what the shipped/user profile says (CPU mode, accurate shader multiplication, stream-out cache).
- **Names.** `.wua` files are read as ZArchive (zstd blocks): `<title>/meta/meta.xml` gives the title id, English long name and product code. A loadiine folder
  uses its `meta.xml` the same way. `.wud`/`.wux` are encrypted: title from the file name, and the key check below applies.
- **Archives.** A `.7z`/`.zip` holding one `.wua` (a readme next to it is ignored) is listed by its entry name, key `wiiu:file-<name>`, and unpacked on first Play
  (with the usual progress dialog) to `rom-cache\wiiu\<key>\disc.wua`. The real title id is read from the unpacked file, so the save mapping is the same as for a plain `.wua`.
- **Updates and DLC** (details page): installed ones from `mlc01\usr\title\0005000e` / `0005000c` (version from `code\app.xml`) and ones bundled inside a `.wua`.
- **Keys.** For `.wud`/`.wux` the details page and the launch notice say whether `keys.txt` exists (`<user data>\keys.txt` or beside Cemu.exe) and whether it has
  32-digit key lines; they never print key material.
- **Saves.** Synced folders per game: `usr/save/00050000/<id low>` and `usr/save/0005000e/<id low>`; every account (`user/<persistentId>`, `user/common`) is inside them. Vault
  layout: `<vault>\wiiu\<game key>\battery\usr\save\...`. Unknown-title discs pick up changed folders under both ids. The shader cache is not vaulted.
- **GamePad screen.** Cemu 2.6's default hotkey "toggle pad screen" is Ctrl+Tab (its first-run assistant lists it). RetroHub sends it when View (Back) + right bumper are held,
  and from Game menu > "Swap GamePad / TV screen". The keys are injected into the focused Cemu window, so the hidden-desktop QA only logs it.
- **Cover art.** libretro-thumbnails "Nintendo - Wii U": the repository's file list (one GitHub API request, cached) is matched by title with the article moved
  ("Legend of Zelda, The - ..."), USA release first. Fallback: GameTDB front cover by the product code (`art.gametdb.com/wiiu/cover/<US|EN|JA>/<id>01.jpg`), one image per game, fetched on demand and cached.
  GameTDB states no explicit usage terms for covers; credit: covers by GameTDB. Info rows come from the existing Wikidata path (platform Wii U, Q56942).
- QA: `qa\wiiu_offline_play.ps1` (user-style scratch Cemu, scratch APPDATA, hidden desktop, PrintWindow capture via `qa\wiiu_capture.ps1`), `hub_wiiu_test`.

# PlayStation 1 (Beetle PSX via retrorun)

System short name `ps1` ("PlayStation", `System::PS1`). It runs through `retrorun.exe --system ps1` with `beetle_psx_libretro.dll` (docs/CORES.md). Code: `src/common/ps1.{h,cpp}` (probing, catalog, BIOS), `src/hub/ps1_ui.cpp` (launch, BIOS picker, settings), `src/beetle_psx/` (core build, serial database generator). Tests: `ps1_test` (synthetic discs in a scratch dir), plus `qa/make_ps1_testdisc.py` for a headless boot disc.

## Library

| Format | Identification | Key |
|---|---|---|
| `.cue` + `.bin` | the first FILE of the cue is read (MODE1/2048, MODE1/2352, MODE2/2352 all detected); ISO 9660 root -> `SYSTEM.CNF` -> `BOOT = cdrom:\SLUS_006.94;1` | `ps1:slus-00694` |
| `.chd` | read with the shared `hub::disc` reader (libchdr, `src/common/disc_id.*`); same SYSTEM.CNF walk | same |
| `.iso` `.img` `.bin` | PVD at sector 16 (2048 or 2352 layout); GameCube `.iso` is tried first | same |
| `.pbp` | `DISC_ID` in the embedded PARAM.SFO | same |
| `.m3u` | the first disc is probed; the other discs listed in the m3u are hidden. Launched as the m3u (disc swap in the core) | disc 1's key |
| `.zip` `.7z` | by Redump file name (title -> serial); unpacked on Play like GameCube (asks first) into `rom-cache\ps1\<key>\` | same |

- **PS1 vs PS2** is decided by the disc: `BOOT =` is PS1, `BOOT2 =` is PS2 (PS1 is probed first, a miss falls to the PS2 reader). Homebrew with `BOOT = cdrom:\PSX.EXE` is a PS1 game with key `ps1:file-<name>`.
- **Serial -> title** from a bundled Redump catalog (11,260 rows): `database/psx_games.inc` of the pinned Beetle tree, turned into `ps1_db_data.cpp` at CMake configure time (`src/beetle_psx/gen_ps1_db.cmake`). Titles are shown without region tags ("Crash Bash"); a single disc keeps "(Disc N)". Cover art uses the Redump name (`Sony - PlayStation` thumbnails).
- Files a `.cue` (its bins) or `.m3u` (its discs) refers to are not listed as games of their own. A damaged/unreadable header falls back to a file name that exactly matches a Redump title.
- Folders: any folder named `GamesPS1`, `Games*`, `PS1`, `PSX` or `PlayStation` below Desktop/Documents/Downloads is suggested ("Find my games" also finds `Desktop\Everything\GamesPS1` through its parent). Add games copies a cue with its bins (and an m3u with its discs) into `PlayStation\<name>\`.

## BIOS

PS1 needs a BIOS dump; RetroHub does not ship one. Searched (first hit wins per region): the file chosen in Settings, `<data>\system\ps1`, each game folder and its `BIOS` / `System` subfolders, a `BIOS` folder next to a game folder (up to two levels up, so `Desktop\Everything\BIOS` beside `Desktop\Everything\GamesPS1` works), `Documents\RetroHub Games\BIOS`, `<exe>\system`, `<exe>\bios`, `%APPDATA%\RetroArch\system`. Files must be 512 KB (or the 1 MB SCPH-5903) named `scph*.bin`. Known MD5s: scph5500 `8dd7d529...`, scph5501 `490f666e...`, scph5502 `32736f17...` (from the libretro `.info`), scph1001 `924e392e...`, scph7001 `1e68c231...`, scph7003 `8abc1b54...`. An unrecognised checksum is still used (with a toast).

The best BIOS per region is copied to `<data>\system\ps1\scph5501.bin` / `scph5502.bin` / `scph5500.bin` (the names the core looks for; a region without its own BIOS borrows another: any retail BIOS runs any region's discs) and passed as `--system-dir`. With none found, Play shows "PS1 needs a BIOS file (scph5501.bin)" with **Choose BIOS file...**, **Use built-in OpenBIOS** and Cancel; Settings > Playing shows the BIOS in use and has the same picker.

## Controls

Controllers page, tab **PlayStation** (profile system `ps1`). Same libretro pad ids as the `retro` profile, PlayStation names: Xbox A = Cross, B = Circle, X = Square, Y = Triangle, LB/RB = L1/R1, LT/RT = L2/R2, Back = Select, Start, L3/R3 = stick clicks, D-pad = D-pad; the two sticks are **analog** (DualShock, deadzone 0.08), they do not press the D-pad. Keyboard: arrows, Z/X/A/S, Q/W, Enter, Right Shift (D-pad and buttons only). retrorun sets every port to DualShock (`0x205`), so rumble works and games can switch analog mode. Four players: **Multitap** setting (Automatic = on with 3+ controllers).

## Saves, video, settings

- Memory card 1 (128 KB) is the core's save RAM, saved as `<stem>.srm` in the game's vault battery folder (`saves\ps1\<key>`), so it syncs with the vault like any other save. Per game, not shared; an `.m3u` set shares one card. No `--resume`: states are 16 MB and a stale auto state must not overwrite a synced card.
- Fullscreen (global default / per game) and the common scaling options work; PS1 uses smooth, non-integer scaling at the core's aspect ratio. Per-game settings: Internal resolution (1x-8x, software), Widescreen hack, Multitap.
- **Online** (2-4 players; catalog `online = true` for the games with 2+ DualShock players): the host's room launch starts `retrorun --system ps1` on every PC with `--rollback --rb-pred 6` (lockstep when the core is not the RetroHub build). Inputs are buttons + both analog sticks per port; 3-4 players turn the emulated multitap on automatically (`hub::ps1::ports_need_multitap`, `beetle_psx_enable_multitap_port1`, port i = i-th member's pad). **Every PC needs the same BIOS** (the same scph file, or all on OpenBIOS): retrorun exchanges a setup fingerprint (core build, console options, BIOS files) before frame 0 and every PC refuses to start on a mismatch (exit 7). The host's memory card is the session save (`--share-host-save`, docs/SAVES.md); online play never writes a vault. Resolution, widescreen and PGXP are pinned for the room (docs/CORES.md, PS1 determinism).
- Memory card 2 (`ps1.memcard2`, `<game>.1.mcr` in the vault battery folder) and disc swap (the `ps1_ui` disc-swap channel into `retrorun --disc-ctl`) are shipped. Card 2 is offline only: the setting is locked off for online rooms. Online disc swaps need rollback netplay and travel as input events. Not done here: card management UI, online for Buzz! quiz controllers.

# PlayStation Portable (PPSSPP)

PSP games run through PPSSPP, RetroHub's own portable download (`Kind::Ppsspp`), like PCSX2 for PS2. Code: `src/common/psp.{h,cpp}` (identification),
`src/hub/ppsspp.{h,cpp}` (config, arguments, state keys), `src/hub/emulators.cpp` (pin, install, save session). Test: `hub_psp_test`
(synthetic images only; `--download <dir>` also downloads and unpacks the pinned PPSSPP, never starts it). System short name `psp`, display "PSP",
key `psp:<disc id lower case>` (for example `psp:ulus10041`), or `psp:file-<name>` when the image carries no disc id (homebrew).

## Emulator pin

| | |
|---|---|
| Source | ppsspp.org (official), GPL-2.0+ |
| Pinned release | 1.20.4, `https://www.ppsspp.org/files/1_20_4/ppsspp_win.zip`, 27,658,759 bytes |
| SHA-256 | `a60f04ebdb0b5f1655422bd7f88349a46999b17ad5115d6ddb290c3934bd5163` (ppsspp.org publishes no digest: computed from the official file on 2026-11-01, trust on first use, like Cemu) |
| Install dir | `<data>\emulators\ppsspp\` (`PPSSPPWindows64.exe` at its root); a user's own PPSSPP is never used or touched |
| Portable data | no `installed.txt` beside the exe = PPSSPP keeps everything in `<dir>\memstick` (`PSP\SYSTEM\ppsspp.ini`, `controls.ini`, `PSP\SAVEDATA`, `PSP\PPSSPP_STATE`) |
| Launch | `PPSSPPWindows64.exe --fullscreen "<game>"` (`--windowed` when Fullscreen is off) |

Download, size and SHA-256 check, unpack and the `retrohub-install.txt` marker are the same Installer as the other emulators. No BIOS is needed.

## Library

PARAM.SFO supplies the disc id (`DISC_ID`), title (`TITLE`) and `CATEGORY`. The PSP claim runs BEFORE the PS1 / PS2 / GC claims on `.pbp .iso .cso .chd`.

| Format | Identification |
|---|---|
| `.iso` | ISO9660: `PSP_GAME\PARAM.SFO` (a PS1/PS2/GC/data disc has none and falls through to its own claim) |
| `.cso` | CISO v1 read block by block, then as an ISO (CSO v2 / ZSO is not read) |
| `.chd` | libchdr (DVD or CD units), then as an ISO |
| `.pbp` | PARAM.SFO from the PBP header. **A PS1 "PSone Classic" eboot stays PlayStation 1**: category `ME`, or a DATA.PSAR starting `PSISOIMG` / `PSTITLEIMG` |
| `.zip .7z .rar` | the one ISO / CSO / PBP member is streamed (first 8 MB) and read; listed with `issue = extract`, unpacked on Play into `rom-cache\psp\<key>` after asking, like the other discs. A CHD inside an archive is not read (left to the PS1/PS2 claims) |

Covers: libretro-thumbnails `Sony_-_PlayStation_Portable` (redump DAT matched by the disc id). Game info: Wikidata platform Q170325.
Homebrew PBP/ISO without a `DISC_ID` is listed under `psp:file-<name>`; it has no save mapping (its SAVEDATA folder cannot be told from other games').

## Saves

`<install>\memstick\PSP\SAVEDATA\<ID>*` folders (the folder name starts with the 9-character disc id: `ULUS10041DATA00`, `ULUS10041_SYSTEM`) are synced
with `<vault>\psp\<key>\battery\SAVEDATA\<folder>` around each session by `begin_session` / `end_session` (`sync_tree_newest`, newest wins, conflicts kept in
`_conflicts`). Folders the game creates during a session are picked up at the end. Save states (`PPSSPP_STATE`) stay local.

## Controls and in-game

- Profile system `psp` (Xbox A/B/X/Y = Cross/Circle/Square/Triangle, LB/RB = L/R, Back/Start = Select/Start, D-pad, left stick = analog stick). `controls.ini` is rewritten on every launch from the
  profile for the first XInput pad (device `20 + slot`), keeps PPSSPP's own keyboard keys, and leaves every hotkey (pause, fast-forward, rewind, screenshot ...) EMPTY, because PPSSPP erases its defaults for
  every name once the section exists. Save State / Load State stay unmapped on purpose so PPSSPP keeps its built-in F2 / F4 accelerators. Non-XInput pads are not mapped (PPSSPP reads them through DirectInput).
- Back + Start held closes the game (`pad_exit_watch`, a normal window close). Screenshot and volume use the generic external-emulator paths (window capture, per-app audio session).
- Quick save / load state: `ppsspp_send_state(save)` (launch.cpp) raises the game and sends F2 / F4 with `run_key_chord`, only while the game is foreground. Speed is not supported (the quick menu says so).
- Settings: fullscreen (default on) and internal resolution (`psp.res`, ppsspp.ini `InternalResolution`).

## Online

None. PSP is offline only (`system_has_online_launch` is false for PSP, so the library shows "Offline only").

# Nintendo Switch / Switch 2 (Lichii)

## Library

Identification is header-only (`src/common/nx.h`, `nx.cpp`): no keys, no decryption. Key `switch:<title id, 16 lowercase hex>` (no id: `switch:file-<name>`); serial = the title id in upper case. Update and DLC packages (title id low 12 bits not zero) are not listed.

| Format | Identification |
|---|---|
| `.nsp .nsz` | PFS0 container; title id from the `<rights id>.tik` entry name (first 16 hex), else the `[0100XXXXXXXXXXXX]` tag in the file name |
| `.xci .xcz` | `HEAD` magic at 0x100 (or 0x1100 with a key area); title id only from the file-name tag |
| `.nro` | `NRO0` magic; title from the ASET/NACP when present, else the file name |
| loose dump | `exefs\main` + `exefs\main.npdm` (how Switch 2 games are dumped). The game file is `exefs\main`; title = the dump's top folder name; title id = the NPDM ACI0 program id. Only a file called `main` inside a folder called `exefs` is considered, so scans stay fast |
| `.nup` | Lichii's own "NX Package" (build 88 `src/core/file_sys/nup_container.cpp`): unencrypted ExeFS/RomFS trees for a base game, its updates and add-ons in one file. `NUP\x01` superblock, then the TOC's 0x40-byte package records (title id, kind 0 base / 1 update / 2 add-on, ExeFS dir), then each package's meta TLVs (1 NPDM, 5 title, 9 localized titles). Only those records and the chosen package's meta are read, never the file tree. The game is picked like Lichii's loader: a base package with an ExeFS, else the newest update with one (dumps holding only a standalone update are playable). Title id = that package's id with the low 12 bits cleared; title = the meta title (else an English localized one, else the file name); Switch 2 = the meta NPDM's ACID layout, else a `04xx` title id. A package with only add-ons is not listed |

Switch 2 (`GameEntry::nx2`, cached with the scan) = the NPDM ACID magic sits at +0x300 (Switch 2 signature area) instead of +0x200. Files inside `.zip / .7z / .rar` are not read (Lichii cannot open archives); they stay unlisted with a skip reason.

### Nintendo Switch / Switch 2 (Lichii)

| | |
|---|---|
| Source | github.com/zortos293/Lichii (an Eden / yuzu fork) |
| Pinned release | Lichii build 125 (nx2 @ cbb512e, a pre-release published 2026-11-09 UTC): `github.com/zortos293/Lichii/releases/download/build-125/lichii-windows-x86_64.zip`, 87352192 bytes, SHA-256 `c55eaaedbdcd87eebcab5510a3d19c49545a4718b9355d95150150d5aeb67bb6`. It includes Lichii PR #515 (mesh shaders that output points or lines: build 95 logged "Mesh output topology ... is not implemented" for them) and PR #550 (see "Shaders at start" below). Its Vulkan shader cache is v54, which still reads a v53 cache, so nothing is rebuilt. The zip no longer has .pdb files but adds `lichii-updater.exe`, Lichii's own updater, which RetroHub deletes: RetroHub is the only updater, and `[UI] check_for_updates=false` is also written before every launch (`lichii_ini_launch`). v1.37.1 to v1.40.0 pinned build 95 (release tag `build-94`); v1.37.0 and earlier pinned build-88 |
| Upgrading an install | A managed install whose `retrohub-install.txt` sha256 differs from the pin is "outdated" (`Install::outdated`). It keeps working, and the next Play (dialog "Update", or "Not now" to play with the old build) or Settings > Emulators > "Update Lichii" upgrades it in place, with the same job and progress dialog as the first install ("Updating Lichii..."). The download's size and SHA-256 are checked and the zip is unpacked into `lichii\_extract` first. Nothing is replaced while `lichii.exe` runs from that folder: the update is refused (a note, tried again later) and never forced. The swap parks the old program files in `lichii\_old`, moves the new ones in, drops the debug symbols / VC++ installer / Lichii's own `lichii-updater.exe`, and writes the new marker last. `lichii\user\` (keys, firmware, saves, config, shader caches, RetroHub's own files) is never moved, replaced or deleted, even if an archive shipped a `user` folder. Any failure moves the old build back: it stays installed and playable, a toast says so, no modal. A crash mid-swap is healed by `recover_interrupted` (at start and before the next install job) from the journal `retrohub-upgrade.txt`: a finished swap just loses its scratch folders, otherwise the parked old build is restored. The leftover cleanup of a failed first install also keeps `user\`. Code: `install_core`, `swap_in_new_build`, `recover_interrupted` in `emulators.cpp`; tests in `test_emulators.cpp` |
| Install dir | `<data>\emulators\lichii\` (`lichii.exe` at its root); a player's own Eden / yuzu / Ryujinx is never written to |
| Portable data | a `user` folder beside `lichii.exe` is Lichii's portable mode; everything below lives in `<install>\user` |
| Launch | `lichii.exe [-f] -u 0 -g "<game>"` (`-f` only when Fullscreen is on) |

**Keys and firmware.** RetroHub never downloads them. The player copies their own `prod.keys` (must contain a `header_key = ...` line, else the import is
refused with a message) and optionally `title.keys` into `user\keys`, and their firmware `.nca` files into `user\nand\system\Contents\registered` (Lichii's
"Install Firmware" replaces that folder too). Sources are only read: a folder, a firmware `.zip` (own miniz unzip, entry names checked for `..`, drives and
absolute paths), or an Eden / yuzu / Citron / Suyu / Sudachi / Ryujinx install found in `%APPDATA%` (Ryujinx stores each NCA as a folder `x.nca` holding a
file `00`; that file is copied as `x.nca`). An empty result is rejected and leaves the installed firmware alone. NSP / NSZ / XCI / XCZ need `prod.keys`;
a loose `exefs\main` dump, `.nup` and `.nro` do not. Firmware is recommended for every game.

**Config written** (`user\config\qt-config.ini`, SimpleIni: every value has its `key\default=false` twin; the file is merged, never replaced, so the
player's other keys stay):
- Once (marker `user\config\retrohub-defaults.txt`; delete it to write them again): `[UI]` firstStart=false, check_for_updates=false, confirmStop=2,
  showConsole=false, pauseWhenInBackground=false, select_user_on_boot=false, enable_discord_presence=false, singleWindowMode=true; `[Renderer]` fullscreen_mode=0.
- Every launch: `[Renderer]` resolution_setup (`switch.res`), frame_gen / frame_gen_type=1 (FSR3) / frame_gen_multiplier (`switch.framegen`), use_vsync
  (`switch.vsync`: fifo 2, mailbox 1, off 0), use_asynchronous_shaders (`switch.async_shaders`: on (default) / off / `lichii` = untouched),
  gpu_accuracy (`switch.gpu_accuracy`: low 0 (default) / high 1 / `lichii` = untouched; Lichii's own default is High. Measured on Ocarina of Time
  (Switch 2, i9-14900K + RTX 3090 Ti, Kokiri Forest): High ~30 fps, Low ~40 fps; CPU accuracy Unsafe, CPU clock Boost, fence Immediate, reactive
  flushing off, DMA Unsafe and docked mode changed nothing on top of Low; the limit is then Lichii's GPU / GPUBack threads), frame_pacing_mode
  (`switch.pacing`: `auto` 0 / `30` 1 / `60` 2 / `smooth60` = 2 plus frame_gen 2x, overriding `switch.framegen` / `lichii` = untouched; the
  swapchain presents at that steady rate, so a game running between 30 and 60 fps no longer alternates 3 and 4 refreshes per frame on a
  60 / 120 Hz screen, which reads as micro-stutter. The target counts presented frames including generated ones: measured on Ocarina of
  Time, Target_30 + frame generation ran the game at 15 fps with long stalls, Target_60 + frame generation at a steady 30 real fps
  (frame-time spread about 3 ms) shown as 60; uncapped it ran 60 in light areas and about 40 in Kokiri Forest); `[UI]` fullscreen; `[System]` current_user=0; `[Controls]` player 1 from the RetroHub `switch` profile on
  one SDL pad (SDL GameController names become SDL button / axis indices; triggers `axis:4|5,threshold:0.5`), connected as a Pro Controller, players 2-8
  disconnected. Values are quoted as Qt writes them; the screenshot button is left alone.
  Lichii runs SDL3 (RAWINPUT off, HIDAPI Xbox off) and clears the name-CRC bytes of every joystick guid, so RetroHub does too. An Xbox / XInput pad is
  written as Lichii sees it: guid `03000000<vid><pid>0000<ver>` + `78 <subtype>` (read with XInputGetCapabilitiesEx) and XInput numbering (buttons A B X Y LB RB Back Start
  L3 R3 Guide = 0..10, d-pad hat 0, axes LX LY LT RX RY RT = 0..5), NOT the RAWINPUT guid / numbers RetroHub's own SDL2 reports; `enable_raw_input=false` is forced then.
  Any other pad keeps SDL2's raw numbers (crc cleared). With no pad connected an old Microsoft RAWINPUT mapping is converted to the XInput guid.
- `profiles.dat` gets one fixed user ("Player", a hardcoded uuid) only when the file is missing or empty; an existing user list is never touched.

**Saves.** `<install>\user\nand\user\save\0000000000000000\<uuid>\<TITLE ID>` (and `nand\user\save\account\<uuid>\<TITLE ID>` when it already exists, which
Lichii prefers) is synced by `begin_session` / `end_session` with `<vault>\switch\<key>\battery\save\<TITLE ID>` (`save-account\<TITLE ID>` for the second).
The vault side never contains the user uuid (`SaveSession::vault_rels`), so any PC restores into its own profile folder. Games without a title id
(`switch:file-...`) are not synced. Lichii has no save states.

**Shaders at start (`switch.preload_shaders`, default `recent` since 1.40.4; `auto` before).** Lichii builds only the cached shader pipelines that recent sessions used when a game starts (at most 3000 when it has no used-list); the rest are built in the background or when first needed, which shows as stutter. `all` makes RetroHub start Lichii with the environment variable `EDEN_NX2_PRELOAD_ALL_PIPELINES=1` (added to RetroHub's own environment for that child only, `lichii_env` in `lichii.cpp`, passed by `spawn` in `launch.cpp`), so Lichii builds EVERY cached pipeline at start: a longer start and about 0.45 MB of driver memory per pipeline (a full 21k cache is about 9.5 GB), for fewer hitches. `auto` does the same on PCs with at least 60 GiB of physical RAM (a PC sold as 64 GB reports about 63.7 GiB) and nothing below that; `recent` never sets it. Until 1.40.3 the `auto` line was 30 GiB: with a 43,127-pipeline cache that is about 19 GB of driver memory, and after a Lichii update every pipeline is compiled again before the game draws, so a 32 GB PC (RTX 3090 Ti) waited 27 minutes for the first frame on build 125's first start. Not written to Lichii's ini. Build 95 unloaded pipelines unused for 9000 frames, so a preloaded shader could be dropped before it was ever used; build 125 includes Lichii PR #550 ("don't evict disk-cache pipelines that were never used"), so it no longer drops those. The headless launch trace prints `preload all shaders yes/no`.

**Faster GPU reads (`switch.fast_reads`, default `on`, global and per game).** In some places Ocarina of Time (Switch 2) fell from about 60 to about 17 fps on Lichii build 125: the guest CPU reads GPU-written memory 45-105 times a second and Lichii makes every read wait for the whole GPU (upstream Lichii issues #944 and #416). When this is not `off`, RetroHub starts Lichii (and Fritchii, same program family; the variable is harmless if a build ignores it) with `EDEN_NX2_NO_REACTIVE=1`, which build 125 uses (`RasterizerMarkRegionCached` in `src/core/memory.cpp`) to drop its read traps on Switch 2 programs. Trade-off: without the traps a CPU read of GPU-written memory can see stale data, so a rare small glitch is possible; turn it off if something looks wrong. Help text: "Lichii skips waiting for the GPU when a game reads back graphics memory. Much faster in parts of Ocarina of Time. Rarely a small glitch: turn it off if something looks wrong." One helper, `hubemu::lichii_fast_reads` (`lichii.cpp`), is used by `lichii_env` and `nx_env`, so every Lichii-family launch (offline, room and online) gets the same variable via `spawn` in `launch.cpp`; Hookshot (native) is unaffected. RetroHub never touches Lichii's own `use_reactive_flushing` setting (turning it off would arm a second trap path). A missing value counts as on. The headless launch trace prints `fast GPU reads yes/no`.

**Damaged-file repair (`switch.repair_damaged`, default `on`, global and per game; `src/common/nup_repair.*`).** Some shared copies of Ocarina of Time (Switch 2, `0400486024DEC000`, v1.1.1) packed as a `.nup` have a few `.zs` members whose first bytes are zeroed, so the game cannot load them and whole rooms are invisible (but solid): the Water Temple, the fairy fountain caves and a Ganon's Castle light bake (Lichii issues #420, #453, #487). Before a `.nup` game starts in Lichii or Fritchii, RetroHub reads the file (never writes it), finds each such member (a `.zs` stored zstd-compressed whose first 4 decompressed bytes are not the zstd magic) and picks a stand-in from the same directory of the merged RomFS by the rule of Lichii PR #267 (same last `_` token, longest common name prefix, then the closest size, and it must start with the zstd magic). It writes the stand-in's bytes as a LayeredFS mod in the emulator's portable user folder: `<emulator folder>\user\load\<TITLE ID>\RetroHub damaged-texture fix\romfs\<path>`, plus a manifest `retrohub-repair.txt` beside it. Lichii serves those files instead of the damaged members, so the rooms show up. After the first start a launch only checks the manifest and the written files (a few milliseconds); a changed game file or a missing or altered written file triggers a rescan (a few seconds). RetroHub only ever deletes files its manifest lists, so other mods and the player's own files are never touched. Off removes what RetroHub wrote. The game file is never changed, nothing is shown while the game starts, and the game page's About tab says so when a copy needed repair. Only a fresh dump of your own copy restores the files exactly. This does from outside what Lichii PR #267 (still open) does inside the emulator. Help text: "RetroHub fills in the files of a damaged copy with a matching intact file from the same copy, for example the Water Temple walls in some Ocarina of Time copies. Off: the game gets its files as they are."

### Fritchii (Ocarina of Time's own build of Lichii)

Fritchii is Frizti's Ocarina-of-Time fork of Lichii. RetroHub downloads it itself (one pinned build, `fritchii_pin` in `src/hub/emulators.cpp`: version, path, size, SHA-256)
into `<data>\emulators\fritchii`, with its own `user\` folder, and runs it through the same code as Lichii (keys, firmware, controls, saves, hang watch).

- **Version.** The pin is **v5.5** (`retrohub-fritchii-v5.5` on `fatty8me2/Fritchii`): RetroHub's own GitHub Actions build of Frizti's tagged source plus build-only commits
  (MSVC warning flags; Windows only; no `-DLICHII_BUILD_TAG`), so the binary is named and versioned like Frizti's release, "Fritchii v5.5". Builds before v5.5 passed
  `-DLICHII_BUILD_TAG=build-N`, which names the binary "Lichii build-N" (`CMakeModules/GenerateSCMRev.cmake`) and turns off every `FritchiiUi::Enabled()` path,
  the Preparing shaders screen included: v5's preparation then ran on idle threads beside the game, which cost about 15 fps in Ocarina of Time until it finished.
  A pin whose size and SHA-256 are zero (`release_is_placeholder()`) downloads nothing; Ocarina of Time then keeps playing with Lichii unless Fritchii is already installed.
- **Upgrading (from v4.1 or v5).** Like Lichii's in-place upgrade: `install_core` replaces the program files and keeps `user\` (keys, saves, settings, shader cache).
  The first start after a version change shows the full Preparing shaders screen once (Fritchii's "prepared" stamp includes its build version).
- **What's new.** Fritchii opens a one-time What's new window (after the game stops) while `[UI] fritchii_whats_new_seen` differs from its build version; RetroHub writes
  the pinned build's version (`kFritchiiWhatsNewSeen` in `lichii.h`, "v5.5") on every launch. Change it with every pin.
- **Default for Ocarina of Time.** Ocarina of Time (Switch 2, `0400486024DEC000`) plays with Fritchii by default. The rule is one pure function, `hubemu::nx_emulator_for` (`nx_runner.cpp`):
  no `switch.runner` choice for the game or for Switch as a whole (`Store::source` is 0) means Fritchii for that title only; any explicit choice (even `lichii`) wins. Other Switch games stay on
  Lichii. Online and room launches always use Lichii. If Fritchii is not installed, Play shows the usual download prompt. Hookshot is still opt-in and is decided first.
- **Shader preparation.** On its first start Fritchii prepares all shaders once: the "Preparing shaders" screen holds the game paused and prepares at full speed (about 15 minutes on
  an 8-core CPU, once per version); "Play now" lets the game run and the rest continue on idle threads (slower, and the game runs worse until it is done). RetroHub pins
  `[Renderer] dyna_state=2` (EDS2) and `vertex_input_dynamic_state=true` in `fritchii_ini_pin`: the recipe is recorded with them, other values make the preparation skip
  almost every pipeline, and Fritchii's own one-time repair of them (tuned-settings revision 5) is held off by RetroHub's revision pin. RetroHub does nothing to skip or shorten it: it never sets
  `EDEN_NX2_PRELOAD_ALL_PIPELINES` for Fritchii (not even for "Shaders at start = All": v5 builds without keeping pipelines, preloading would double the work and memory), never `FRITCHII_CLASSIC_UI` or `EDEN_NX2_PRECOMPILE=0`, keeps `EDEN_UPDATE_URL=http://127.0.0.1:9/x` and `EDEN_NX2_NO_REACTIVE=1`, and copies Lichii's v54 and v53 caches in as before (v5 merges them into its own cache and never writes Lichii's files). It runs on its own at `-g` launches when a recipe is found (`recipes*.nx2recipe` beside the exe, so the install keeps that folder).
- **NVIDIA.** Since v5.5 Fritchii sets `__GL_SHADER_DISK_CACHE_SIZE` (10 GiB) and `__GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1` in its own process environment (only if not set
  already; nothing is written to the driver or the registry), so the prepared shaders stay in the driver's cache; RetroHub never sets a smaller value. On older drivers the
  NVIDIA Control Panel's Shader Cache Size (Manage 3D settings) can still be set to Unlimited.
- **Frame generation.** A build named Fritchii never runs frame generation (`Settings::FrameGenActive`), so RetroHub's frame-generation and "smooth60" pacing settings have no
  frame generation under Fritchii ("smooth60" still sets Target_60 pacing).

### Hookshot (native Ocarina of Time)

Ocarina of Time (Switch 2, title id `0400486024DEC000`) is played with Fritchii by default (Lichii if it is not obtainable); the player can opt in to a native Windows build, Hookshot, instead (`src/hub/hookshot.*`; the build
itself is `docs/HOOKSHOT.md`). Hookshot is made on the player's PC from their own game file and never leaves it.

- **When it is used.** Play starts Hookshot when `hookshot::use_for` is true: the game is that one title, a build exists (`hookshot.exe`, `hookshot_vk.dll`,
  `SDL2.dll`, `romfs\` and a build stamp naming the pinned source commit, under `<data>\emulators\hookshot`) and `switch.runner` is exactly `hookshot`. The check
  comes before any Lichii step, so a built Hookshot plays even when Lichii is not installed or has no keys. Otherwise Lichii plays as before.
- **Setting.** `switch.runner` (Visual): `lichii` = Lichii (default), `hookshot` = Hookshot when it is built (otherwise Lichii plays). The v1.37.0 value `auto` is
  no longer a choice: settings load / merge / resolve drop it, so it means Lichii everywhere (launch, Settings, game page, synced settings). The card on the game page has a
  one-click "Play with Hookshot / Lichii instead" button (`gs_set_game_value`, the same store write as the Settings row). It appears under the game's Settings only for this
  game, never in the per-system defaults, and online / sync code ignores it. "Play with Lichii this time" (AV1 prompt) skips Hookshot for one launch.
- **Saves.** Hookshot keeps the game's own save files in `<build>\user\save\5a3c9e1f0b7d24680000000000000001`. They are synced with the SAME vault folder
  Lichii uses, `<vault>\switch\<key>\battery\save\0400486024DEC000`, newest wins, with the usual `_conflicts` backups, so Lichii and Hookshot are
  interchangeable and every PC stays in step. Hookshot has its own "last agreed" manifest (`.sync\emu-switch-<game>-hookshot.json`); Lichii's is never touched.
  The build's `user\` folder is kept by a rebuild.
- **AV1 check.** Hookshot's movies need Microsoft's free AV1 Video Extension; without it Hookshot opens a top-most message box over the game. RetroHub
  asks Media Foundation (`MFTEnumEx`, AV1 video decoders) before every launch and, when missing, shows its own prompt: open the Store page, play with Lichii
  this time, or cancel. Nothing is started until the decoder is there.
- **Offline only.** Online, room and netplay launches always use Lichii. Hookshot has no runner control channel, so the quick menu offers only the generic
  items (named "Hookshot"); the Lichii exit-hang watch does not apply to it. Hookshot takes `HOS_FULLSCREEN=1` from the Fullscreen setting.
