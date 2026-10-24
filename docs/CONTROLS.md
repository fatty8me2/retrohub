# Controller profiles

Model and storage: `src/common/controls.{h,cpp}` (pure). SDL half (pad list, capture, gamecontrollerdb): `src/common/controls_sdl.{h,cpp}`.

- A **profile** maps a system's logical controls to SDL GameController sources (`a`, `leftx-@9000`, `x|b|y`; see the header). Systems: `n64`, `retro` (GBA/GB/GBC/DS), `gc`, `wiiu`, `3ds`.
- **Store** = `userdata/controls.json` (and a copy in the vault dir; the newer `stamp` wins): user profiles, system default, per-game overrides, pad port order (`guid[#serial]`). The built-in defaults are not stored; they reproduce the old hardcoded mappings (`hubcontrols_test`, `hub_emu_test`).
- **n64run / retrorun** take `--controls FILE` (or `RETROHUB_CONTROLS`), a file written by `hubctl::write_profile_file(path, hubctl::resolve(store, system, game_id))`. Missing/bad file = built-in mapping.
- **Dolphin / Azahar / Cemu** configs are generated from the stored system profile in `prepare_*`. A marker next to the config holds `profile-hash:`; the config is rewritten only when the profile changed (no hash = written by an older RetroHub = default profile), so edits in the emulator's own dialogs survive otherwise.
- `gamecontrollerdb.txt` (zlib) ships next to the exes in `controls/`; `hubctl::load_bundled_db()` loads it.

## Keyboard pad
The keyboard is a pad of its own: `Profile::keys` maps control names (n64 also `stick_up/down/left/right`) to key expressions such as `x|space` (names in `controls.h`; they are SDL scancodes). The defaults are the old fixed bindings. It drives Player 1 in n64run and retrorun (merged with pad 1) and Dolphin's GCPad1 (`DInput/0/Keyboard Mouse`). Cemu and Azahar take one source per control, so wiiu / 3ds have no keyboard remap (Azahar keeps its own keyboard defaults only when no pad is bound). The Controllers page has a Controller / Keyboard switch; the keyboard capture listens to SDL's keyboard state.

## Which pad each emulator uses
- **Dolphin** follows the RetroHub port order. XInput pads stay `XInput/<n>/Gamepad` (numbered among XInput pads); every other pad (DualSense, Switch Pro, 8BitDo) is written as `SDL/<n>/<SDL name>` using Dolphin's SDL backend names (`Button S/E/W/N`, `Shoulder L/R`, `Pad N/S/W/E`, `Left X-`, `Trigger L` ...; `Y-` is up). With only XInput pads in order the file is byte-identical to before. The marker hash includes the device assignment, so changing the port order rewrites GCPadNew.ini once; otherwise edits made in Dolphin stay.
- **Azahar** binds player 1's pad (first in the port order) as `guid:<guid>,port:<n-th pad with that guid>`; with no stored port order it still uses "any pad" (`maptype:all`).

- **Cemu** (RetroHub's own managed download only; an existing Cemu install such as a hand-set-up `Cemu_2.6` folder is never written to) uses the XInput API. Player 1 = Wii U GamePad (`portable\controllerProfiles\controller0.xml`) on the first XInput pad of the port order; players 2-4 = Wii U Pro Controllers (`controller1-3.xml`) on the next XInput pads. Pads that are not XInput are skipped (Cemu's SDL API uses its own ids: map those in Cemu's input dialog). Pro Controller button ids reuse the GamePad table (first 24 ids), unverified against a running Cemu. Changing the port order rewrites the files; stale Pro files RetroHub wrote are removed.
- Every launch path (offline `launch_external`, online 3DS/GC through the same prepare calls) regenerates from the Controllers page profile (per-game override first) whenever the profile hash or pad choice changed.

## Default Xbox Series X mapping (works with no setup)
Position-based where a console's face buttons differ from the Xbox layout (Nintendo A is the east button).

| Xbox | N64 | GameCube | Wii U (GamePad/Pro) | 3DS | GB/GBA/DS (retro) |
|---|---|---|---|---|---|
| A (south) | A | A | B | B | B |
| B (east) | B | X | A | A | A |
| X (west) | B | B | Y | Y | Y |
| Y (north) | B | Y | X | X | X |
| LB / RB | L / R | - / Z (RB) | L / R | L / R | L / R |
| LT / RT | Z / R | L / R (analog) | ZL / ZR | ZL / ZR | L2 / R2 |
| Menu / View | Start / - | Start / - | + / - | Start / Select | Start / Select |
| Left stick | stick | main stick | left stick | Circle Pad | D-pad (digital) |
| Right stick | C buttons | C-stick | right stick | C-Stick | - |
| D-pad | D-pad | D-pad | D-pad | D-pad | D-pad |
| Deadzone | 0.08 | Dolphin default | 0.25 | 0.15 | - |

Dolphin also writes `Rumble/Motor` for all four ports and `SIDevice0-3` (pad present per port). Any change on the Controllers page (remap, deadzone, port order, per-game override) changes the profile hash, so the next launch rewrites the emulator config.

## Stick sensitivity
Applied by n64run to the N64 stick (deadzone + sensitivity). retrorun's cores are digital only (D-pad, buttons, triggers; no analog device is passed), so nothing there can use a sensitivity; the left stick acts as a D-pad through the `@threshold` of its terms. Dolphin, Cemu and Azahar configs only take a deadzone, so the page shows just the deadzone slider for those systems.

## N64 Rumble Pak
Per-profile (so per game) option `rumble`, offline only. n64run reports a Rumble Pak (`PLUGIN_RUMBLE_PAK`) on every present port instead of the Controller Pak, decodes the core's pak write to 0xC000 in `ControllerCommand`, and rumbles the matching SDL pad (`SDL_GameControllerRumble`). Online games ignore it, so the pak seen by every player (and the shared host `.mpk` save) is unchanged.

## Leave-game combo (all emulators)
Hold **Back/View + Start/Menu** on one pad for **1 second** to leave a game and return to RetroHub. Logic: `src/common/exit_combo.h` (`ExitCombo`, fires once per hold; unit tested in `hubcontrols_test`).
- **n64run, retrorun** (N64, GB/GBC/GBA/DS): checked every input poll on every connected pad; sets the normal quit flag (same path as Esc / closing the window), so saves flush in `finish()` and netplay leaves cleanly.
- **BattleShip** (Smash): checked in its main loop when started with `--from-hub`; same path as closing the window. RetroHub's watcher also covers it.
- **Dolphin, Azahar, Cemu, Kanto Together mGBA**: RetroHub watches the pads itself while the game runs (`pad_exit_watch` in `launch.cpp`: XInput for Xbox pads, SDL for the rest, hot-plug included) and sends `WM_CLOSE` to the emulator's windows (all processes of its job object), so the emulator saves and exits normally; Kanto uses its session stop. If an emulator ignores the close for 20 s RetroHub leaves it alone (it never kills it). When the emulator is gone RetroHub restores and focuses its own window.
- The combo is fixed (not remappable); Back and Start still reach the game while held.

## Built-in runners: what follows the profile
- Port order (`port_order`), per-game override and system default are resolved by `hubctl::resolve` and passed with `--controls` (`controls_arg`). Pads hot-plug in n64run (rescan on joystick count change) and retrorun (device added/removed events).
- **n64run**: C-buttons, Z, stick (deadzone, sensitivity) all from the profile; Rumble Pak per profile. **retrorun**: digital buttons from the profile; libretro rumble requests (cores that use them) drive the pad of that port.
- **DS touch**: the right stick moves a stylus cursor (drawn as a cross); the profile's `r3` control (default: click right stick) touches. Moving the mouse switches back to the mouse stylus.
- **Kanto Together mGBA**: `[gba.input.SDLC]` is rewritten from the `retro` profile whenever its hash changes (`kanto_pad`), so remaps carry over (first button term per control).
- **Smash / BattleShip**: RetroHub writes the resolved `n64` profile to `controls_run_smash.json` and sets `SSB64_CONTROLS=<file>`; BattleShip (`port/hub_controls*`) maps it onto libultraship when started with `--from-hub`. The profile is **session-only**: BattleShip snapshots its own controller config first and restores it when the game exits, so nothing the hub applies persists in BattleShip's config. Applied for the session: the buttons, the stick/C-button mapping, rumble, the pad **port order**, and the stick and trigger **axis thresholds** (`@threshold`). If the N64 profile is not customized (same mapping as the built-in default, even when an untouched `n64-custom` copy exists from opening the Controllers page), RetroHub sends the name `default-n64` (`hubctl::canonical_name`) and BattleShip keeps its natural Smash layout (A attack, B special, X/Y jump, RB shield) instead of the generic N64 mapping.

## Pad-only setup and paging
- The first-run wizard (Settings > Run setup again) works with a pad alone: A/B/D-pad, the name is typed on the on-screen keyboard (A on the field; B deletes, Start = Done, View = Cancel; the pad keeps navigating afterwards and focus returns to the field). "Customize..." opens the Controllers page; B (or LB/RB) returns to the same wizard step.
- LT / RT page up / down on every page, including Controllers, Settings and Saves (the focus follows onto what is on screen). `qa\pad_nav.ps1` walks all of this headless.
