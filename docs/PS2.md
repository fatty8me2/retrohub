# PlayStation 2 (PCSX2)

Code: `src/hub/pcsx2.{h,cpp}` (BIOS, PCSX2.ini, pads, args), `src/hub/emulators.cpp` (`Kind::Pcsx2`: download, card session),
`src/common/ps2.{h,cpp}` (disc identification), `src/common/ps2_db.cpp` (generated serial -> title table, `scripts/gen_ps2_db.js`).
Test: `hub_ps2_test` (offline; `--download <dir>` also downloads and installs PCSX2 into a scratch dir and reads the config back).

- **Emulator.** PCSX2 v2.8.2 stable, `pcsx2-v2.8.2-windows-x64-Qt.7z` (25,670,075 bytes, SHA-256 `7dfc829c...00b4`, GitHub's asset digest),
  checked before unpacking (tar.exe, entry names vetted like Dolphin's 7z). Installed in `<data>\emulators\pcsx2\` with `portable.ini`, so a
  user's own PCSX2 is never read or changed. Launch: `pcsx2-qt.exe -batch -nogui -fullscreen -- "<game>"` (`-nofullscreen` when the game's fullscreen setting is off).
- **Library.** `.iso`, raw `.bin` + `.cue`, `.cso` are read (ISO9660 `SYSTEM.CNF`: `BOOT2` = PS2, `BOOT` = PS1 and skipped). `.chd` (CD and DVD) is read with libchdr (pinned, BSD-3, `CMakeLists.txt`; its miniz is renamed to avoid clashing with ours) through the shared `src/common/disc_id.*` reader (also for PS1: `Cnf::Ps1`), so the serial comes from the disc, not the file name. A `.chd` libchdr cannot read falls back to a known serial in its name. `.zip`/`.7z` holding one `.iso`/`.chd`/`.cso` are listed and unpacked on
  Play into `rom-cache\ps2\<key>\` (same flow as GameCube). Key `ps2:<serial>` (or `ps2:file-<name>`). `Desktop\Everything\GamesPS2` is a default Find-my-games root.
- **BIOS.** Not shipped. `scph*.bin` / `ps2-*.bin` (1-8 MB) is searched in the folder chosen in Settings, `Desktop\Everything\BIOS`, the game folders, their `BIOS`
  subfolders and parents. Without one, Play shows a message and nothing is downloaded or started.
- **Config.** `inis\PCSX2.ini`: defaults once (wizard done, no update check, no exit confirm, SDL input), pads when the profile or pads change, and per launch
  BIOS, memory card, renderer (Auto/Vulkan/DX12/DX11), upscale (default 2x), widescreen patches.
- **Saves.** Memory card 1 is `memcards\RetroHub\<key>\Mcd001.ps2`, synced with `<vault>\ps2\<key>\battery\memcards\RetroHub\<key>` (newest wins).
- **Controls.** Profile system `ps2`: Xbox A/B/X/Y -> Cross/Circle/Square/Triangle, triggers -> L2/R2 (analog = pressure), sticks, rumble. Players follow the
  RetroHub port order; 3-4 pads switch on the multitap. No pads = PCSX2's keyboard defaults on Pad1. Back + Start held closes the game gracefully (`pad_exit_watch`).
- **Online.** Offline play always uses PCSX2. Online rooms (2 players) run retrorun + the LRPS2 libretro core, downloaded on demand and hash-pinned; the room blocks READY until the core and a BIOS are there, and
  every player must have the same BIOS file and disc image (checked before the game starts). The host's PCSX2 memory card is shared for the session and never written back. See `docs/PS2_ONLINE.md`.
