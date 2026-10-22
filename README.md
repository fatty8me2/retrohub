# RetroHub

RetroHub is a Windows launcher for retro games. It puts your N64, GameCube, Game Boy Advance, Game Boy, Game Boy Color and Nintendo DS games in one library, and it lets you play them online with friends.

- **One library.** Browse and launch every game from a single window, with cover art and per-game details.
- **Online play with friends.** Add friends by friend code, see who is online, send invites, and chat in a room. Rooms use a short room code.
- **Netplay.** N64 games run through a lockstep input netplay built into the runner. Native ports can use their own rollback netplay.
- **Save vault.** Save files are kept in one folder. Point it at OneDrive, Dropbox, a USB stick or a NAS and your saves follow you between PCs.
- **Controllers.** Gamepads are configured once and used across all systems.
- **Auto-update.** RetroHub checks GitHub releases and can download new versions.

## Systems

| System | How it runs |
|---|---|
| Nintendo 64 | mupen64plus 2.6.0 inside `n64run.exe`, with GLideN64 video |
| GameCube | Dolphin, downloaded on demand (`scripts/fetch-dolphin.ps1`) |
| Game Boy Advance | mGBA (libretro core) |
| Game Boy / Game Boy Color | SameBoy by default, Gambatte as a fallback (libretro cores) |
| Nintendo DS | melonDS DS (libretro core, built-in BIOS, two screens + mouse touch) |
| Native ports | Run directly, for example Smash Classic (BattleShip) |

See `docs/CORES.md`, `docs/DOLPHIN.md` and `docs/RETRORUN.md` for details.

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

RetroHub releases ship inside the Smash Classic package, not as a separate download. The packaging script lives in the Smash Classic repository. It stamps the release version into `VERSION` and builds the package that is published on GitHub. The lobby and friends server is a Cloudflare Worker, and its code also lives in the Smash Classic repository.

## Windows SmartScreen warning

RetroHub is not code-signed yet, so Windows SmartScreen may say it "protected your PC" the first time you run it. To continue:

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
