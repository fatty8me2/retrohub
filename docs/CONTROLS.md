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

## Stick sensitivity
Applied by n64run to the N64 stick (deadzone + sensitivity). retrorun's cores are digital only (D-pad, buttons, triggers; no analog device is passed), so nothing there can use a sensitivity; the left stick acts as a D-pad through the `@threshold` of its terms. Dolphin, Cemu and Azahar configs only take a deadzone, so the page shows just the deadzone slider for those systems.

## N64 Rumble Pak
Per-profile (so per game) option `rumble`, offline only. n64run reports a Rumble Pak (`PLUGIN_RUMBLE_PAK`) on every present port instead of the Controller Pak, decodes the core's pak write to 0xC000 in `ControllerCommand`, and rumbles the matching SDL pad (`SDL_GameControllerRumble`). Online games ignore it, so the pak seen by every player (and the shared host `.mpk` save) is unchanged.
