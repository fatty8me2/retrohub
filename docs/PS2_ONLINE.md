# PlayStation 2 online: research note

Status: **PS2 online works in rooms** (2-4 players, input-delay lockstep through retrorun + the LRPS2 libretro core). Offline PS2 still uses PCSX2 (`docs/PS2.md`).
The sections "How it works in the hub" below describe what ships; "Why PCSX2 cannot do it" and "Options" are the original research; "Spike results" are the measurements it rests on.

## How it works in the hub
- **Core.** `pcsx2_libretro.dll` (LRPS2 / ParaLLEl PS2 v2.0.0-8f1d15f, GPL-3.0, libretro buildbot nightly of 2026-11-02) is NOT shipped. `hubemu::Kind::Lrps2` (src/hub/emulators.cpp)
  downloads it on demand into `<data>\emulators\lrps2\` from a **release asset we host**: `https://github.com/fatty8me2/retrohub/releases/download/core-lrps2-8f1d15f/pcsx2_libretro.dll.zip`
  (the buildbot zip, unmodified), pinned by size (3,432,192 bytes) and SHA-256 `ff97bd2690e7a53433e1deb3570a72f9a06a78253f1db3796c8df6742aca7fe7` (same downloader as PCSX2 / Dolphin;
  attribution in THIRD_PARTY.md and the licenses text). There is NO fallback URL: lockstep needs every peer on the byte-identical core, so a differing build must be refused, not accepted. The
  DLL inside is 10,251,776 bytes. Corresponding GPL-3.0 source: https://github.com/libretro/ps2 at commit 8f1d15f (the core prints `2.0.0-8f1d15f`).
  **Why it is hosted:** the first version pinned the buildbot's moving `nightly/.../latest/` URL (38d3a83, 3,760,634 bytes). The nightly moved on and users got "download has the wrong size". The old
  38d3a83 zip was byte-identical to the pin (checked) but has no multitap option, so the pin moved to 8f1d15f together with the 4-player work rather than re-hosting 38d3a83.
  **Refreshing the core:** download a nightly, re-run the QA below (`qa/ps2_online.sh` 2- and 4-player, direct and relay, determinism spike), upload the zip under a NEW tag `core-lrps2-<id>`,
  change the pin in `emulators.cpp` + `test_ps2_online.cpp`, update THIRD_PARTY.md. Old tags stay up for existing installs. A `pcsx2_libretro.dll` in the cores folder is also accepted (dev).
- **Engine.** Online PS2 rooms run `retrorun --system ps2` (software renderer, no GPU needed; pinned options: `pcsx2_renderer`, `mtvu`, `sw_renderer_threads=0`, fastboot,
  analog mode on, no deadzone/scale/invert, rumble off, shared cards). Lockstep only: PS2 states are not faithful, so there is no rollback. The input delay uses the N64-style
  negotiation (`n64run::negotiate_delay`, auto from the measured RTT, then adaptive during play). The desync check is a hash of EE RAM (32 MB) every 600 frames.
- **Setup fingerprint (before frame 0).** retrorun exchanges `{core build, BIOS file hash, disc fingerprint + serial, pcsx2_* options}` (`kPktCfg2`). A mismatch logs which part differs
  and exits with 9 (BIOS), 10 (disc) or 11 (core build / options); the hub shows one clear sentence per case (`hubps2::setup_mismatch_text`). The disc fingerprint is the file size plus 64
  x 64 KiB blocks spread over the image (a full 4 GB hash would delay the start by many seconds) plus the serial the hub passes with `--setup-tag`.
- **Input.** Per port the lockstep sample is 8 bytes: 16 RetroPad buttons, L2/R2 pressure (0..255, bits 16-31 of the button word), and both analog sticks quantised to 8 bits
  (`retrorun::pack_sticks`, the PS1 format). A DualShock 2 reports 0..255 per axis, so nothing is lost. Online PS2 uses the DualShock layout of the "ps1" controls profile (per-game PS2
  bindings made for PCSX2 are not applied online). Verified: with sticks and pressure fuzzed the output differs from a no-stick run and two identical runs match bit for bit.
- **Players.** 2-4. 8f1d15f adds the core option `pcsx2_multitap` ("disabled", "port 1", "port 2", "ports 1 and 2"; read at load). For rooms of 3+ players retrorun pins
  `pcsx2_multitap=port 1` (a multitap in console port 1; frontend ports 0-3 are its four slots, slot k = lobby slot k); 1-2 player rooms pin it `disabled`, so they behave exactly as before.
  The option is part of the setup fingerprint (a peer with a different value fails with exit 11). `online_max_for` no longer caps PS2: the catalog's per-game `online_max`/`max_players`
  (e.g. Crash Nitro Kart, TimeSplitters 2 = 4) applies, non-catalog PS2 games default to 4 ("Untested online"). The hub refuses rooms above 4. Only games that support the multitap use the extra
  pads. Alternatives considered and rejected: patching/building LRPS2 ourselves (a second binary to maintain, not needed now that upstream ships the option); standalone PCSX2 (no netplay hooks,
  not deterministic across PCs). Console port 2 then has no frontend pad (a 4-player multitap game uses one tap).
- **Pads must be plugged in (v1.31.7).** LRPS2 reads `input_state` for every port but only plugs a DualShock 2 into frontend port 0; ports 1-7 default to "None" until the frontend calls
  `retro_set_controller_port_device(port, RETRO_DEVICE_JOYPAD)` (libretro/ps2 `pcsx2/PAD/PAD.cpp`). Through v1.31.6 retrorun never did, so player 2 was a pad the game could not see (Gauntlet: Dark Legacy
  character select) while the QA's "pad reads" counter still counted reads. retrorun now plugs ports 0-1 (console ports 1-2; 1-2 players, multitap off) or 0-3 (3-4 players, multitap in console port 1).
  Proof: `qa/ps2_p2_input.sh` (EE RAM with pads on one port vs all ports must differ; with `RETRORUN_PS2_NOPLUG=1` they are identical). `RETRORUN_FUZZ_PORTS=<mask>` fuzzes only some ports.
- **Catalog.** Tekken 5, Burnout 3, Crash Nitro Kart, Crash Tag Team Racing and TimeSplitters 2 are flagged `online`; every other PS2 game is offered with the "Untested online" tag
  (`online_needs_catalog_flag`). The room picker lists them, the library shows "Play Online".
- **Memory card.** See docs/SAVES.md. Both PCSX2 (`Mcd001.ps2`) and LRPS2 (`pcsx2\memcards\Mcd001.ps2`) use the standard 8 MB card file with ECC, 8,650,752 bytes (LRPS2 creates exactly
  that size), so the host's vault card is used as is: the hub copies it into the throwaway session dir (`netplay\ps2\sys`), retrorun deflates it and sends it to every guest through a short
  sibling room BEFORE the core boots (the core opens the card while loading), and guests overwrite their throwaway copy. Slot 2 starts blank. Nobody's vault is written; progress made online is
  discarded. (The format equality is by size/layout; a card made by PCSX2 was not loaded by LRPS2 in these tests.) Without the host's card (or with "use host's save" off) everyone starts blank.
- **Performance.** LRPS2's software renderer runs ~90-210 fps on a menu/attract load on the test PC (two instances sharing one CPU: ~90 each). Lockstep waits for the slowest PC, so a PC that
  cannot hold 60 fps slows the pair down evenly and never desyncs; each peer logs `done: N frames in S s (X fps)` and the adaptive delay absorbs jitter. Advice: 4+ physical cores.
- **RTC / known limits.** The PS2 clock follows the host time; games that read the RTC during play (rare) may differ. Rumble is off. USB cameras, HDD, DEV9 are off.

### QA scripts
`[PLAYERS=2..4] [CORE=dll] [CORE1=dll] qa/ps2_online.sh <tag> <iso> <frames> [direct|relay] [port]` (bash, headless, muted, local wrangler only; scratch copies of the core, BIOS and discs). It seeds a different memory card per
peer and checks that the guest ends with the host's card. `PS2_BIOS1=<file>` / `PS2_TAG1=<text>` make the guest differ (BIOS / disc mismatch must exit 9 / 10 on both PCs); `CORE1=<other build>` makes the guest's core differ (must exit 11 on both PCs). Each peer logs `ps2 pad reads with buttons down: port0..port3`, the proof that every pad reaches the core.
Unit tests: `hub_ps2_online_test` (readiness reasons, retrorun args, exit texts, pinned download), `hub_online_caps_test`, `hubcommon_test`, `hubcontrols` (pressure).

### Results (8f1d15f, Digimon Rumble Arena 2, local wrangler, headless, muted)
- 2 players, direct, 1800 frames: both exit 0, EE RAM sync checks equal, output and input hashes identical, 113 fps each.
- 4 players, direct and relay (`HUBNET_FORCE_RELAY=1`), 3600 frames each: all four exit 0, every EE RAM check (every 600 frames) equal on all peers, output hash and input hash identical on all four, ~112 fps.
  Pad reads with buttons down on every peer: 1796/1796/1796/1796 for ports 0-3. Offline control with the same core: multitap on = ports 2 and 3 read 1200/1200; `pcsx2_multitap=disabled` = 0/0.
- Mixed cores (38d3a83 guest, 8f1d15f host): both exit 11 with "slot N runs a different emulator core build" before frame 0.
- Not verified: real human 3-4 player gameplay in a multitap game, a 4-PC run over the internet, other discs (a 4-player-heavy title would be the next check).

## Why PCSX2 cannot do it today
- PCSX2 has no netplay and no rollback. Its save states are not deterministic across machines, and the EE/VU/GS timing is not
  built for lockstep (the same reason Dolphin needed its own NetPlay code).
- Online games that used the PS2 network adapter are supported only as a LAN/DNAS shim: PCSX2 emulates the network adapter
  (DEV9, TAP or sockets mode) so games can reach the real internet or a private network.

## Options, best first
1. **System Link / LAN over a virtual network.** A handful of games (Burnout 3, Halo-style LAN titles, Gran Turismo 4 over
   PS2 link, Ratchet and Clank: Up Your Arsenal on revived servers) support the PS2 "System Link". PCSX2's DEV9 can run in
   *Sockets* mode (no admin driver) or *TAP*; both players need the same virtual LAN. RetroHub already tunnels UDP between room
   members for 3DS/Dolphin (`netplay_3ds_run`), so a virtual L2/L3 tunnel is plausible: the host gets 192.168.x.1, guests .2-.n,
   DEV9 in TAP mode bridged to a TAP adapter that the tunnel feeds. Cost: needs the TAP-Windows driver (admin install) or a user-mode
   switch; each game has its own way to start a LAN session, so it is a per-game recipe, not a generic "Play Online" button.
2. **Revived public servers.** Several PS2 games have community servers (e.g. Online-Content / Project Cafe style revivals).
   RetroHub only has to set DEV9 up (sockets mode plus the server's DNS). No new netcode, but it is not "play with the friends in my
   room" and each revival has its own account steps.
3. **Parsec-style remote play.** The host runs PCSX2 and streams video to a guest who sends pad input. Works for any game, adds
   latency to the guest only. Needs a streaming stack (encode, WebRTC) - large new work, out of scope for the launcher.
4. **Custom lockstep/rollback in PCSX2.** A fork with deterministic input injection (like the N64 lockstep) would be the "real"
   answer, but PCSX2's JIT/VU determinism and its size make this a long project; not planned.

## Recommendation
Keep PS2 offline-only (done). If online is wanted later, start with option 1 for one title (a per-game "LAN room" recipe on top
of the existing room tunnel), after checking which games the player actually owns. Before any of it, PCSX2 must be launched
with DEV9 configured in `inis\PCSX2.ini` (`[DEV9/Eth] EthEnable`, `EthApi`, `EthDevice`), which `prepare_pcsx2` can write.

## Spike results (w3-ps2online, measured)
Verdict: **PS2 input-delay lockstep over the room relay works.** Two loopback peers stayed bit-identical for 5 minutes on both
Digimon Rumble Arena 2 and Budokai Tenkaichi 3. Rollback does not work (savestate restore is not faithful and is too slow).
(Spike: it was behind `RETROHUB_PS2_NETPLAY=1`; that gate is gone, the hub now ships the online path.)

### Cores
- **LRPS2 / "ParaLLEl PS2" v2.0.0-38d3a83**, libretro buildbot nightly, GPLv3, Windows x86_64:
  `https://buildbot.libretro.com/nightly/windows/x86_64/latest/pcsx2_libretro.dll.zip` (3,760,634 bytes, zip SHA-256
  `730112622345f0ca12e7650c6d4052f443603aa630e9b6ab8a68874a4d66bae6`; DLL 11,074,048 bytes, SHA-256 starts `c3d278c0dd27ab06`).
- **PCEE2** (WizzardSK/pcee2-libretro): no releases, source only (needs a full PCSX2 build); not tested.
- LRPS2 asks for a HW context, retrorun refuses, and the core falls back to its **software renderer** (`pcsx2_renderer=Software (SW)`),
  so no GPU is needed. BIOS: any `scph*.bin` placed in `<system-dir>/pcsx2/bios/` (used SCPH-70012, `pcsx2_bios=` option).
  Options used: `pcsx2_renderer=Software (SW)`, `pcsx2_mtvu=disabled`, `pcsx2_sw_renderer_threads=0`, fastboot on. Output 640x448 @ 59.94 Hz.

### Measurements (retrorun headless, hidden, muted, loopback, Windows 11)
| Test | Result |
|---|---|
| Cross-process determinism, 2 runs x 18000 frames (5 min), pseudo-random pad input, hash every 60 frames | **Digimon RA2: 300/300 video+audio hashes identical. BT3: 300/300 identical.** |
| Raw `retro_serialize` bytes between identical runs | differ (host pointers/ASLR in 14 bytes, plus 2 tiny counters), so a state hash cannot be the desync check. EE RAM (32 MB, `RETRO_MEMORY_SYSTEM_RAM`) is used instead. |
| Savestate size | 46.5 MB (BT3 selftest, BIOS 1.60) to 50.7 MB (Digimon, BIOS 2.00) |
| Savestate speed | serialize 12-19 ms avg (max 43-106 ms), unserialize 40-55 ms avg (max 96-183 ms), vs a 16.7 ms frame |
| Synctest (save, load, resim) | video fine, but EE RAM diverges after a load from ~frame 95 (98 of 598 re-simulated frames mismatched); `--selftest` also reports the restore is not byte-faithful. **Rollback: no-go.** Replays of 16 frames in the Digimon selftest matched, so it is intermittent, not safe. |
| Emulation speed (software renderer, menus and attract) | ~210 fps single, 87 fps each with two instances on one PC |
| **Lockstep, 2 peers, local wrangler relay, 18000 frames** | **exit 0 on both, no DESYNC, 29/29 EE RAM sync checks equal, final output and input hashes identical on both peers.** Digimon 47.9 fps (two instances sharing one PC, below 60), BT3 59.0 fps. Auto input delay 2-6 frames at 20 ms RTT, 1.3-2.1 s total stalls in 5 min. |

### Caveats
- Input in the test is a digital RetroPad bitmask (`--fuzz-input`, mostly menus/attract). Analog sticks and pressure are not carried; fighting
  games work on the d-pad. Real gameplay with a human is untested; games with RTC, rumble or heavy GS effects may still differ.
- Each peer needs an identical BIOS, disc image and core options; the prototype does not yet check this (PS1 has `ps1_config_check`).
- Two PCSX2 instances on one PC are CPU bound (software renderer). Real play is one instance per PC; a ~4-core CPU is advised (not measured on separate machines).
- `GET_VARIABLE` options and nvm/mec files: copied identically into each peer's system dir.

### Prototype (off by default)
`src/retrorun/main.cpp`: the sync hash is EE RAM (the spike gate `RETROHUB_PS2_NETPLAY=1` was removed).
`RETRORUN_HASHLOG=N` logs state/output/video hashes every N frames. Scripts: `qa/ps2_run_spike.sh`, `qa/ps2_determinism_spike.sh`,
`qa/ps2_netplay_spike.sh` (run from the repo root with scratch copies of core, ISO, BIOS; needs a LOCAL wrangler on port 8793).

### Next steps if wanted
1. Human playtest of BT3 / Digimon over a real internet link; measure felt input delay (delay 3 frames = 50 ms plus RTT).
2. (Done) 4-player multitap: LRPS2 8f1d15f `pcsx2_multitap`.
3. Consider an EE RAM + GS VRAM hash for faster desync pinpointing.

(Done in the productized branch: hash-pinned core download, launch path, setup fingerprint, analog sticks + pressure, host memory card sharing, performance notes.)

