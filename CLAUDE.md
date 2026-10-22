# RetroHub: session rules

RetroHub is a Windows launcher for retro games (N64, GameCube, GBA, GB, GBC). It has online play, friends and invites, and a save vault that syncs across PCs. Start with `docs/ARCHITECTURE.md`, and see the other docs in `docs/` (FRIENDS, SAVES, RETRORUN, DOLPHIN, CORES).

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
- Online run (needs the live server): `ctest --test-dir build -C Release -L online`. The server is `RETROHUB_TEST_SERVER`, which defaults to the Cloudflare Worker.

## Standard QA on every change (required)
Before any change is released, run **Haiku QA agents** in parallel, scoped to what changed. Typical lanes:
- **Online:** use headless `retrorun --headless --net ...` peers against the live server and the online console tests (`ctest -L online`: `hubnet_test`, `hubfriends_test`). Also try relay mode (`HUBNET_FORCE_RELAY=1`) and bad networks.
- **Saves across machines:** simulate two PCs sharing a sync folder, and check that the vault layout, the game keys and the runner save-file names are identical on every PC.
- **UI:** use the headless screenshot mode (`HUB_SCREENSHOT=<file.bmp>`, `HUB_SHOT_*` env vars; see `src/hub/main.cpp`) and inspect the PNGs.

QA agents must:
- test **headless only** and never open visible windows (the user is often gaming or watching something while work happens);
- keep test launches muted;
- put rerunnable scripts in `qa/`;
- make only small, focused fixes and report anything larger.

Fold their findings in before packaging and release.

## Build and release
- Build with the VS 2026 bundled CMake: `cmake -S . -B build`, then `cmake --build build --config Release`. The first configure downloads SDL, which takes a few minutes.
- Releases ship inside the Smash Classic package. Run `..\BattleShip\scripts\package-windows.ps1`, which copies the release version from the BattleShip `VERSION` into the RetroHub `VERSION`. Then run `gh release create vX.Y.Z ... -R fatty8me2/smash-classic`.
- The lobby/friends server is the Cloudflare Worker in `..\BattleShip\server\cloudflare`. Deploy it with `npx wrangler deploy`.
