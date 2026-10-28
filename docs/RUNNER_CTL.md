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
| `shader` | `text`: `off`, `crt`, `lcd`, `dmg`, `sharp`, `next` (cycle), `bezel:on`, `bezel:off`, `bezel:toggle` | switches the screen filter / console frame live (OSD shows `Filter: CRT`); unknown name = `ok:false` | ack `ok:false`, msg `unsupported` (retrorun only) |
| `rewind` | `value` 1/0 | retrorun: 1 rewinds continuously until 0 (or the history runs out); `ok:false` "Rewind is not available for this game" when rewind is off (PS1/DS default, `--rewind off`). n64run: `unsupported` | same |
| `overlay`, `clip` | `value` / `text` | ack `ok:false`, msg `unsupported` | same |

- **`input`** (`value` 0 = off, 1 = on; allowed online): while off, the runner feeds NEUTRAL input for its local player(s) to the core (retrorun: `local_pad_cache_`/sticks zeroed in `poll_all_local`; n64run: the lockstep `read_local` hook returns 0). Frames and netplay packets still flow, so rollback/lockstep timing is unchanged. The hub sends `input off` when the View+Y menu opens, re-sends it every 10 s while open, and `input on` on close. Failsafes (`runctl::InputGate`): the runner re-enables input after 30 s without a refresh (hub died) and on a fresh View+Y press.
- **Offline only:** `speed`, `ff`, `rewind`, `shader`, `overlay`, `clip` are refused online (`ok:false`, `Not available in online sessions`), like save states. `volume`, `mute`, `osd_msg`, `screenshot` and `quit` work online (volume and mute are local: they never touch the emulation). `runctl::offline_only()` is the single rule both runners use.
- `speed` shows `Speed 2x` / `Speed 0.5x` / `Speed max` on the OSD whenever it changes. The Tab key still fast-forwards while held (retrorun).
- The hub's View+Y menu sends `speed` / `volume` / `mute` with `send_command_ex()` (fire and forget, no ack wait). `runctl::parse_speed_arg("2x")`, `speed_label()` and `kSpeeds` are the shared helpers; the menu's pure logic is `src/hub/qm_logic.h`.

## Launch volume and the quick boost
- `--volume N` (0-100) on `retrorun` and `n64run` sets the starting volume (hub: the global Settings > Video & Games > Game volume, overridden per system or per game by the `volume` option in `src/common/game_settings.h`). Dolphin gets `-C Dolphin.DSP.Volume=N`. The default 100 adds no argument.
- **Quick boost:** hold View (Back) + RT. While held, the runner runs at the last fast speed chosen (default 2x); on release it returns to 1x. Offline only (retrorun: `poll_all_local`, n64run: `LocalInput::boost_held`). `hubctl::boost_combo_down()` (`src/common/exit_combo.h`) is false whenever Start (exit) or Y (RetroHub's menu) is also held, so it never overlaps those combos.

## On-screen display (`src/common/osd.h`)
`osd::push(text, kind, seconds)` queues a toast (newest four shown, bottom-left, fades); `osd::set_corner(text, kind)` is a persistent top-right slot (`""` clears) for overlays. `osd::render()` fills an ARGB layer with a built-in 5x7 font; each runner blends it in its own renderer.
- **retrorun:** an SDL texture drawn over the frame after the controller tips. Every `set_status()` message (state saved / loaded, slot changed, screenshot, disc, ...) goes to the OSD as well as the window title (the title still works).
- **n64run:** the plugin owns the GL window, so the OSD text is mirrored into the window caption (toast text appended for its lifetime). A real GL overlay exists behind `N64RUN_OSD_GL=1` (fixed-function quad drawn in the VI callback). It is experimental: it needs a compatibility context and the callback must run before the plugin's buffer swap, which is unverified, so it is off by default.
- **QA:** `retrorun --headless --osd-msg "text" --screenshot out.png` with `RETRORUN_OSD_SHOT=1` composites the OSD into the screenshot (frame upscaled to ~480 px high).

## Quick menu (hub)
View + Y while a game runs opens the menu (`src/hub/quickmenu.cpp`): Resume, Speed, Volume + Mute, Save state (slot picker with thumbnails), Load state, Take a screenshot, Cheats, Swap disc (PS1), Invite a friend (when in a room), Quit to RetroHub.
- **Speed** (built-in runners): 0.25x, 0.5x, 1x, 1.5x, 2x, 3x, 5x, 8x, Max. Left/right on the pad changes it live and the OSD shows `Speed 2x`. Offline only; online the row is disabled with "Not available online". It resets to 1x every session.
- **Volume** (0-100, steps of 5) and **Mute**: allowed online. Built-in runners get `volume` / `mute`; for the external emulators the hub scales the emulator's own Windows audio session (like the Volume Mixer; Dolphin additionally starts at its DSP volume, and the slider then scales that). The session is re-applied every 2 s until the emulator opens its audio.
- **Screenshot** (Dolphin, Azahar, Cemu, PCSX2): captures the emulator's window with Windows Graphics Capture (PrintWindow as a fallback) into the same `<vault>/<system>/<game key>/shots/` folder the gallery reads (`src/hub/ext_media.cpp`). Cheats, Invite and Quit are unchanged for them. The details modal shows a screenshots strip (click opens the full-size viewer, which has Copy, Set as cover, Delete and Open in viewer; plus Open folder).
