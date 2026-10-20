# RetroHub: session rules

RetroHub is a Windows launcher for retro games (N64, GameCube, GBA, GB, GBC). It has online play, friends and invites, and a save vault that syncs across PCs. Start with `docs/ARCHITECTURE.md`, and see the other docs in `docs/` (FRIENDS, SAVES, RETRORUN, DOLPHIN, CORES).

## Standard QA on every change (required)
Before any change is released, run **Haiku QA agents** in parallel, scoped to what changed. Typical lanes:
- **Online:** use headless `retrorun --headless --net ...` peers against the live server and the console tests (`n64run_lockstep_test`, `hubnet_test`, `hubfriends_test`). Also try relay mode (`HUBNET_FORCE_RELAY=1`) and bad networks.
- **Saves across machines:** simulate two PCs sharing a sync folder, and check that the vault layout, the game keys and the runner save-file names are identical on every PC.
- **UI:** use the headless screenshot mode (`HUB_SCREENSHOT=<file.bmp>`, `HUB_SHOT_*` env vars; see `src/hub/main.cpp`) and inspect the PNGs.

QA agents must:
- test **headless only** and never open visible windows (the user is often gaming or watching something while work happens);
- keep test launches muted;
- put rerunnable scripts in `qa/`;
- make only small, focused fixes and report anything larger.

Fold their findings in before packaging and release.

## Build and release
- Build with the VS 2026 bundled CMake: `cmake --build build --config Release`.
- Releases ship inside the Smash Classic package. Run `..\BattleShip\scripts\package-windows.ps1`, which stamps the release version into `VERSION`. Then run `gh release create vX.Y.Z ... -R fatty8me2/smash-classic`.
- The lobby/friends server is the Cloudflare Worker in `..\BattleShip\server\cloudflare`. Deploy it with `npx wrangler deploy`.
