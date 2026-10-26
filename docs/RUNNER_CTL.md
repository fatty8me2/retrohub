# Runner control channel, save-state slots and screenshots

Shared contract between RetroHub.exe and the built-in runners (`retrorun`, `n64run`). Header: `src/common/runner_ctl.h`. Tests: `runner_ctl_test`, `qa/ctl_headless.ps1`.

## Launch arguments (hub -> runner)
| Argument | Meaning |
|---|---|
| `--control <run_dir>/control.json` | enables the channel; the ack is the sibling `control_ack.json` |
| `--game-key <key>` | written into state sidecars |
| `--shot-dir <dir>` | screenshots folder = `<vault>/<system>/<game key>/shots` |
| `--state-dir <dir>` | states folder (retrorun always had it; n64run takes it too) |

`<run_dir>` is `<data>/run/<sanitized key>`; the hub creates `control.json` = `{"seq":0}` at launch and deletes any old ack.

## Commands (hub -> runner)
`control.json`: `{"seq":N,"cmd":"save_state|load_state|screenshot|swap_disc|quit","slot":0-9}`.
- The runner polls the file's mtime/size about 4 times a second and acts once per `seq`. A `seq` that is not larger than the last one is ignored; whatever is in the file at start-up counts as already handled.
- `slot` is 0-9 for `save_state`; `load_state` also takes -1 (the `auto` resume point).
- `save_state` / `load_state` are refused with `ok:false` in online sessions (rollback / lockstep peers must stay in step). `screenshot` and `quit` work online.
- `swap_disc` works in retrorun (multi-disc PS1) only. `quit` is graceful: saves flush exactly as on a normal exit.
- `send_command()` in runner_ctl.h writes the next `seq` atomically.

## Ack (runner -> hub)
`control_ack.json`: `{"seq":N,"ok":true|false,"msg":"..."}`, written once the command finished (a state save acks after the file and thumbnail exist). On success `msg` of a screenshot is the file path.

## Files
- States: `<state_dir>/slot<N>.state` and `auto.state` (written on exit).
- Thumbnail `<state_dir>/slot<N>.png`, at most 320 px wide, written on every save (n64run in a real window keeps the core's full size).
- Sidecar `<state_dir>/slot<N>.json`: `{"game_key","time","playtime","disc"}`.
- Screenshots: `<vault>/<system>/<game key>/shots/<yyyyMMdd-HHmmss>.png`. F12 takes one in both runners (retrorun by itself, n64run through the core's hotkey, filed into the vault within a second).
- Cloud: files under `shots/` are the "shot" class and are only synced when Settings > Cloud saves > "Back up screenshots" is on (default off). They then share the large-item quota with save states.

## Quick menu (hub)
View + Y while a game runs opens the menu (`src/hub/quickmenu.cpp`): Resume, Save state (slot picker with thumbnails), Load state, Take a screenshot, Cheats, Swap disc (PS1), Invite a friend (when in a room), Quit to RetroHub. For other emulators (Dolphin, Azahar, Cemu, PCSX2) only Resume, Cheats, Invite and Quit appear. The details modal shows a screenshots strip (open, delete, open folder).
