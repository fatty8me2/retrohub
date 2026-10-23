# Controller profiles

Model and storage: `src/common/controls.{h,cpp}` (pure). SDL half (pad list, capture, gamecontrollerdb): `src/common/controls_sdl.{h,cpp}`.

- A **profile** maps a system's logical controls to SDL GameController sources (`a`, `leftx-@9000`, `x|b|y`; see the header). Systems: `n64`, `retro` (GBA/GB/GBC/DS), `gc`, `wiiu`, `3ds`.
- **Store** = `userdata/controls.json` (and a copy in the vault dir; the newer `stamp` wins): user profiles, system default, per-game overrides, pad port order (`guid[#serial]`). The built-in defaults are not stored; they reproduce the old hardcoded mappings (`hubcontrols_test`, `hub_emu_test`).
- **n64run / retrorun** take `--controls FILE` (or `RETROHUB_CONTROLS`), a file written by `hubctl::write_profile_file(path, hubctl::resolve(store, system, game_id))`. Missing/bad file = built-in mapping. Keyboard bindings are still fixed.
- **Dolphin / Azahar / Cemu** configs are generated from the stored system profile in `prepare_*`. A marker next to the config holds `profile-hash:`; the config is rewritten only when the profile changed (no hash = written by an older RetroHub = default profile), so edits in the emulator's own dialogs survive otherwise.
- `gamecontrollerdb.txt` (zlib) ships next to the exes in `controls/`; `hubctl::load_bundled_db()` loads it.
