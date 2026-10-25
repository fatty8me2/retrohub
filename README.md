# RetroHub

## ⬇️ [Download RetroHub for Windows (RetroHub-Setup.exe)](https://github.com/fatty8me2/retrohub/releases/latest/download/RetroHub-Setup.exe)

Run the setup, and RetroHub walks you through your controller and games. No command line needed. Windows may show a SmartScreen warning: click **More info**, then **Run anyway** ([why](#windows-smartscreen-warning)). All versions and patch notes: [Releases](https://github.com/fatty8me2/retrohub/releases).

RetroHub is a Windows launcher for retro games. It puts your N64, GameCube, Wii U, Nintendo 3DS, Game Boy Advance, Game Boy, Game Boy Color, Nintendo DS, PlayStation and PlayStation 2 games in one library, and it lets you play them online with friends.

- **One library.** Browse and launch every game from a single window, with cover art and per-game details.
- **Online play with friends.** Add friends by friend code, see who is online, send invites, and chat in a room. Rooms use a short room code. The room page shows READY, then LAUNCH for the host once everyone is ready, and a Rejoin button if the connection drops.
- **Netplay.** N64 games run through a lockstep input netplay built into the runner. Native ports can use their own rollback netplay.
- **Save vault.** Save files are kept in one folder. Point it at OneDrive, Dropbox, a USB stick or a NAS and your saves follow you between PCs.
- **Controllers.** Gamepads are configured once and used across all systems.
- **Auto-update.** RetroHub checks GitHub releases and can download new versions.

## Systems

| System | How it runs |
|---|---|
| Nintendo 64 | mupen64plus 2.6.0 inside `n64run.exe`, with GLideN64 video |
| GameCube | Dolphin, downloaded on demand (`scripts/fetch-dolphin.ps1`) |
| Nintendo 3DS | Azahar, downloaded on demand into RetroHub's data folder (zipped games are unpacked on request) |
| Wii U | Cemu: an existing install is used, otherwise downloaded on demand (`.wua`, `.wud/.wux`, loadiine `.rpx`) |
| Game Boy Advance | mGBA (libretro core) |
| Game Boy / Game Boy Color | SameBoy by default, Gambatte as a fallback (libretro cores) |
| Nintendo DS | melonDS DS (libretro core, built-in BIOS, two screens + mouse touch) |
| PlayStation | Beetle PSX (libretro core, rollback online, multitap; needs your BIOS) |
| PlayStation 2 | PCSX2, downloaded on demand (offline; needs your BIOS) |
| Native ports | Run directly, for example Smash Classic (BattleShip) |

See `docs/CORES.md`, `docs/DOLPHIN.md`, `docs/EMULATORS.md` (3DS and Wii U) and `docs/RETRORUN.md` for details.

## Bring your own games

RetroHub does not include any games, ROMs, BIOS files or copyrighted material. You supply your own game files from discs or cartridges you own. Point RetroHub at the folder that holds them, and it identifies each game by its checksum.

## Build

RetroHub is C++17, x64 only, and builds with the CMake bundled with Visual Studio 2026.

1. Install Visual Studio 2026 with the "Desktop development with C++" workload.
2. Open a Developer PowerShell for VS 2026 in the repository folder.
3. Fetch the emulator and core runtime files (only needed once, or after an update):

   ```
   powershell -ExecutionPolicy Bypass -File scripts\fetch-emu.ps1
   powershell -ExecutionPolicy Bypass -File scripts\fetch-cores.ps1
   ```

4. Configure and build:

   ```
   cmake -S . -B build
   cmake --build build --config Release
   ```

The first configure downloads SDL2, Dear ImGui, the mupen64plus headers and miniz. This takes a few minutes. Downloaded and staged files go into `deps\`, which is not tracked by git.

## Releases

Every release is published here as `RetroHub-Setup.exe` (and the portable `BattleShip-windows.zip`), and the same files go to the Smash Classic repository for its built-in updater. The packaging script lives in the Smash Classic repository. It stamps the release version into `VERSION` and builds the package that is published on GitHub. The lobby and friends server is a Cloudflare Worker, and its code also lives in the Smash Classic repository.

## Install (Windows)

Download **[RetroHub-Setup.exe](https://github.com/fatty8me2/retrohub/releases/latest/download/RetroHub-Setup.exe)** from the latest release and run it. It installs per-user to `%LOCALAPPDATA%\Programs\RetroHub` (no admin needed), adds a Start-menu shortcut and an optional Desktop shortcut. Uninstall from Apps & features; your saves and settings are kept unless you choose to delete them. The portable alternative is `BattleShip-windows.zip` from the same release: extract it anywhere and run `RetroHub.exe`. The in-app updater works for both. Building the setup needs [Inno Setup 6](https://jrsoftware.org/isinfo.php) (`winget install --id JRSoftware.InnoSetup -e`); `package-windows.ps1` in the Smash Classic repo builds it.

## Windows SmartScreen warning

RetroHub is not code-signed yet, so Windows SmartScreen may say it "protected your PC" the first time you run the setup or RetroHub. To continue:

1. Click **More info**.
2. Click **Run anyway**.

This warning appears for any unsigned program downloaded from the internet. Only download RetroHub from the official GitHub releases.

## Project layout

```
src/hub/       RetroHub.exe, the ImGui launcher
src/n64run/    n64run.exe, the mupen64plus frontend with netplay input
src/retrorun/  retrorun.exe, the libretro frontend used for the other cores
src/net/       hubnet: UDP, WebSocket, STUN, lobby and room client
src/saves/     save vault and sync
src/common/    game catalog, ROM identification, paths, logging
server/        lobby and friends server (Cloudflare Worker)
scripts/       dependency fetch scripts
docs/          architecture and subsystem notes
qa/            QA scripts (headless)
```

Start with `docs/ARCHITECTURE.md`.

## License

RetroHub is free software, licensed under the GNU General Public License, version 3 or (at your option) any later version. See [LICENSE](LICENSE). Bundled and downloaded third-party components keep their own licenses, listed in [THIRD_PARTY.md](THIRD_PARTY.md).
