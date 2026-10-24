# RetroHub: session rules

RetroHub is a Windows launcher for retro games (N64, GameCube, Wii U, 3DS, GBA, GB, GBC). It has online play, friends and invites, and a save vault that syncs across PCs. Start with `docs/ARCHITECTURE.md`, and see the other docs in `docs/` (FRIENDS, SAVES, RETRORUN, DOLPHIN, EMULATORS, CORES).

## Agent roles
- **Opus: orchestrator.** Plans, writes the contracts/interfaces, splits work into parallel lanes, integrates, and releases.
- **Sonnet: coders.** Each implements a feature in files it owns, against those contracts.
- **Haiku: small tasks, second set of eyes, and testing.** Lookups, scripts, code review of Sonnet's work, and QA.

## Parallel agents
- Every agent works in its own git worktree under `..\wt\<task>` on branch `agent/<task>`, created with `git worktree add ..\wt\<task> -b agent/<task>` from the repo root.
- Agents build and test only inside their worktree, commit there, and never merge, push, deploy or release.
- **Opus merges** the `agent/<task>` branches into `main` and handles pushes and releases.
- Do not edit the main checkout while agents are running.

## Tests
- Register new console tests in the top-level `CMakeLists.txt` with `add_test`. Network tests get `LABELS online`.
- Offline run (no network, no visible windows): `ctest --test-dir build -C Release -LE online --output-on-failure`.
- Online run (needs the live server): `ctest --test-dir build -C Release -L online`. The server is read at run time from `RETROHUB_TEST_SERVER` (env var), falling back to `hubnet::default_server()` (the production Cloudflare Worker). It is not baked into the ctest command lines, so set the env var when you run ctest.
- Run online tests against a local worker, never production: `set RETROHUB_TEST_SERVER=ws://localhost:8761` (start `npx wrangler dev --port 8761` from `..\BattleShip\server\cloudflare`), then `ctest --test-dir build -C Release -L online`. Confirm in the wrangler log that requests arrive.

## Standard QA on every change (required)
Before any change is released, run **Haiku QA agents** in parallel, scoped to what changed. Typical lanes:
- **Online:** use headless `retrorun --headless --net ...` peers against the live server and the online console tests (`ctest -L online`: `hubnet_test`, `hubfriends_test`). Also try relay mode (`HUBNET_FORCE_RELAY=1`) and bad networks.
- **Saves across machines:** simulate two PCs sharing a sync folder, and check that the vault layout, the game keys and the runner save-file names are identical on every PC.
- **UI:** use the headless screenshot mode (`HUB_SCREENSHOT=<file.bmp>`, `HUB_SHOT_*` env vars; see `src/hub/shot.cpp` and `main.cpp`) and inspect the PNGs.

QA agents must:
- test **headless only** and never open visible windows (the user is often gaming or watching something while work happens);
- keep test launches muted;
- put rerunnable scripts in `qa/`;
- make only small, focused fixes and report anything larger.

Fold their findings in before packaging and release.

## Build and release
- Build with the VS 2026 bundled CMake: `cmake -S . -B build`, then `cmake --build build --config Release`. The first configure downloads SDL, which takes a few minutes.
- **MinGW-w64 gcc is required for `sameboy_rb_libretro.dll`** (GB/GBC rollback core; SameBoy uses GNU C extensions MSVC cannot compile). Install MSYS2 and the `mingw-w64-x86_64-gcc` package so `C:\msys64\mingw64\bin\gcc.exe` exists, or pass `-DSAMEBOY_RB_GCC=<path to gcc.exe>`. Without it the target is skipped and GB/GBC netplay stays on lockstep. `package-windows.ps1` fails if the DLL is missing. The DLL is statically linked (only KERNEL32/msvcrt imports) and ships in the release `cores\`.
- Releases ship inside the Smash Classic package. Run `..\BattleShip\scripts\package-windows.ps1`, which copies the release version from the BattleShip `VERSION` into the RetroHub `VERSION`. Then publish the SAME assets (`dist\RetroHub-Setup.exe`, `dist\BattleShip-windows.zip`, `dist\SHA256SUMS.txt`) to BOTH repos:
  - `gh release create vX.Y.Z <assets> -R fatty8me2/smash-classic ...` (installed copies' auto-updater reads this one — never skip it)
  - `gh release create vX.Y.Z <assets> -R fatty8me2/retrohub --latest ...` (the public download page: `https://github.com/fatty8me2/retrohub/releases/latest/download/RetroHub-Setup.exe`)
- The lobby/friends server is the Cloudflare Worker in `..\BattleShip\server\cloudflare`. Deploy it with `npx wrangler deploy`.
