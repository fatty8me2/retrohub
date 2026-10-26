# PlayStation 2 online: research note

Status: **PS2 is offline-only in RetroHub** (an experimental lockstep prototype exists, see Spike results). Rooms never list PS2 games, the library shows a disabled "Offline only" button
("PS2 online isn't supported yet"), and `launch_online` refuses a PS2 launch with the same message.

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
Not shipped: behind `RETROHUB_PS2_NETPLAY=1` in retrorun only, hub still shows PS2 as offline-only.

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
`src/retrorun/main.cpp`: `RETROHUB_PS2_NETPLAY=1` is required for `--net` or `--synctest` with a PS2 core (else exit 7). The sync hash is EE RAM.
`RETRORUN_HASHLOG=N` logs state/output/video hashes every N frames. Scripts: `qa/ps2_run_spike.sh`, `qa/ps2_determinism_spike.sh`,
`qa/ps2_netplay_spike.sh` (run from the repo root with scratch copies of core, ISO, BIOS; needs a LOCAL wrangler on port 8793).

### Next steps if wanted
1. Ship the core (hash-pinned download) and a `launch_online` PS2 path that passes the options above plus a config-hash exchange.
2. Human playtest of BT3 / Digimon over a real internet link; measure felt input delay (delay 3 frames = 50 ms plus RTT).
3. Analog stick transport (extend `val_bytes` like PS1's wide pads) and 4-player multitap.
4. Consider an EE RAM + GS VRAM hash for faster desync pinpointing.

