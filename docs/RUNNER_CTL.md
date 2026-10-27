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

## Extra commands: audio, speed, OSD (hub -> runner)
Same file and `seq` rules as above. Extra fields in `control.json`: `"value"` (int), `"speed"` (number or `"max"`), `"text"` (string, <= 120 bytes). `runctl::send_command_ex()` writes them.

| `cmd` | Fields | retrorun | n64run |
|---|---|---|---|
| `volume` | `value` 0-100 | scales the mixer (squared curve) | core `M64CORE_AUDIO_VOLUME` |
| `mute` | `value` 1 = mute, 0 = unmute | yes | core `M64CORE_AUDIO_MUTE` |
| `speed` | `speed` 0.25, 0.5, 1, 1.5, 2, 3, 5, 8 or `"max"` (0 = uncapped) | timer pacing at `fps*speed`; audio is resampled up to 2x (pitch follows) and dropped above 2x; video presents every `ceil(speed)` frames; `max` = the Tab fast-forward | core `M64CORE_SPEED_FACTOR` (percent); `max` turns the limiter off |
| `ff` | `value` 1 = on, 0 = off | on = the last fast speed chosen (default 2x), off = 1x | same |
| `osd_msg` | `text` | shows a toast | shows a toast |
| `rewind`, `shader`, `overlay`, `clip` | `value` / `text` | ack `ok:false`, msg `unsupported` | same |

- **Offline only:** `volume`, `mute`, `speed`, `ff`, `rewind`, `shader`, `overlay`, `clip` are refused online (`ok:false`, `Not available in online sessions`), like save states. `osd_msg`, `screenshot` and `quit` work online.
- `speed` shows `Speed 2x` / `Speed 0.5x` / `Speed max` on the OSD whenever it changes. The Tab key still fast-forwards while held (retrorun).
- The hub's View+Y speed slider is a later lane; it only needs `send_command_ex(ctl, {Cmd::Speed, speed=...})`. `runctl::parse_speed_arg("2x")`, `speed_label()` and `kSpeeds` are the shared helpers.

## On-screen display (`src/common/osd.h`)
`osd::push(text, kind, seconds)` queues a toast (newest four shown, bottom-left, fades); `osd::set_corner(text, kind)` is a persistent top-right slot (`""` clears) for overlays. `osd::render()` fills an ARGB layer with a built-in 5x7 font; each runner blends it in its own renderer.
- **retrorun:** an SDL texture drawn over the frame after the controller tips. Every `set_status()` message (state saved / loaded, slot changed, screenshot, disc, ...) goes to the OSD as well as the window title (the title still works).
- **n64run:** the plugin owns the GL window, so the OSD text is mirrored into the window caption (toast text appended for its lifetime). A real GL overlay exists behind `N64RUN_OSD_GL=1` (fixed-function quad drawn in the VI callback). It is experimental: it needs a compatibility context and the callback must run before the plugin's buffer swap, which is unverified, so it is off by default.
- **QA:** `retrorun --headless --osd-msg "text" --screenshot out.png` with `RETRORUN_OSD_SHOT=1` composites the OSD into the screenshot (frame upscaled to ~480 px high).

## Quick menu (hub)
View + Y while a game runs opens the menu (`src/hub/quickmenu.cpp`): Resume, Save state (slot picker with thumbnails), Load state, Take a screenshot, Cheats, Swap disc (PS1), Invite a friend (when in a room), Quit to RetroHub. For other emulators (Dolphin, Azahar, Cemu, PCSX2) only Resume, Cheats, Invite and Quit appear. The details modal shows a screenshots strip (open, delete, open folder).
