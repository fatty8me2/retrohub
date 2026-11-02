# Convenience features (Plan 4, Wave 2 C)

All pure logic is unit tested by `hub_convenience_test` (Discord frames and opt-in gate, diagnostics exclusion, NAT verdict with loopback STUN, glyph mapping, palette ranking, setup steps).

## Discord Rich Presence (opt-in)
- Settings > Online, off by default; also an optional line in the setup wizard's Online step.
- `discord_logic.h` builds frames (`u32 opcode | u32 length | JSON`, little endian) and the SET_ACTIVITY payload; `discord_ipc.cpp` talks to `\\.\pipe\discord-ipc-0..9` on one background thread and fails silently when Discord is not running.
- Shows game, console, elapsed time and party size. Art: a public https cover URL if one is ever supplied, else the console icon asset `console_<system>` (fallback `retrohub`).
- The Join button is a second toggle (off by default). It adds a `buttons` entry with the existing RetroHub invite link, so it exposes the room code to Discord friends.
- **Owner action:** create a Discord application, set `hubdiscord::kClientId` in `src/hub/discord_logic.h` to its Application ID, and upload art assets named `retrohub`, `console_n64`, `console_gba`, `console_gb`, `console_gbc`, `console_gc`, `console_3ds`, `console_wiiu`, `console_nds`, `console_ps2`. While the constant is the placeholder nothing is ever sent.

## Copy diagnostics (Settings > Help)
Zips `userdata\logs` (includes hang reports), `versions.txt`, `emulators.txt` and `summary.txt` to the Desktop and opens the folder. `identity*.json` and anything under `_identity` is excluded at every depth, in both the collector and the zip writer.

## Connection test (Settings > Online)
`nettest.cpp` sends STUN binding requests from one UDP port to `stun.cloudflare.com:3478` and `stun.l.google.com:19302`. Same mapped address:port = direct should work; different ports = strict NAT, relay likely; no answer = UDP blocked. Then pings the lobby with a HEAD request (user initiated). 

## Firewall probe (NC-M6)
`n64run --net-probe` and `retrorun --net-probe` (`src/common/net_probe.h`) bind a UDP port, listen briefly and exit 0, with no window. The setup wizard's Online step (and a Settings > Online button) runs both while RetroHub is in front so the Windows Firewall question never appears over a first online game. `net_probe_done` is stored in hub.json. Screenshot/headless runs never start the probe.

## Controller glyphs and battery
`glyph_logic.h` maps Xbox hint names to PlayStation (shapes, L1/R1, Share/Options) and Switch (L/R, ZL/ZR, -/+) from `SDL_GameControllerGetType`; `ui::pad_kind` is set by `glyph_ui.cpp` from the pad that last had input. `key_hint` applies it everywhere (footer, wizard, palette). Battery (`SDL_JoystickCurrentPowerLevel`) is a small badge by the top bar while the hub is focused and the pad is in use, or nearly flat; never while a game is active.

## Find anything
- Type on Home / All Games to search (F, I, S stay shortcuts; `/` always starts a search).
- Command palette: Ctrl+K, L3, or View+Y. Searches games (opens details, never launches), friends (opens chat), pages/settings and actions.

## Help and first run
Settings > Help has the FAQ and Copy diagnostics. Home shows a "Finish setup" card while steps are open (no games, no username, firewall probe not run, tour not finished); "Not now" hides it for the session.

## Screenshots
`qa\conv_shots.ps1` (headless, muted). Aids: `HUB_SHOT_PALETTE=<query>`, `HUB_SHOT_SETTINGS_TAB=<0-4>`, `HUB_SHOT_FAKE_PADTYPE=xbox|ps|switch`, `HUB_SHOT_FAKE_BATTERY=empty|low|medium|full`, `HUB_SHOT_WIZARD=online`.
