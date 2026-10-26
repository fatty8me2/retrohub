# PlayStation 2 online: research note

Status: **PS2 is offline-only in RetroHub.** Rooms never list PS2 games, the library shows a disabled "Offline only" button
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

## Spike results (w3-ps2online, desk research; no prototype ran)
Verdict: **no-go for local-multiplayer PS2 games (BT3, Digimon Rumble Arena 2) with today's tooling.** Nothing was launched: no PCSX2 is
installed in this checkout (deps has no PS2 files), and a live test needs a 25 MB download plus unpacking 1.3 GB games, which was out of the
time box. Findings below are from docs and project pages, not measurements; verify before relying on them.

### (a) DEV9 LAN tunnel - feasible tech, wrong games
- PCSX2 DEV9 has Sockets (no driver), TAP and pcap modes. A UDP forwarder in the `netplay_3ds_run` style could carry L3 traffic, but
  Sockets mode is a NAT'd client, not a LAN segment, so peers cannot see each other; a real shared LAN needs TAP (admin driver) plus a user-mode L2 switch.
- BT3 and Digimon RA2 are local-only (no System Link), so (a) cannot help them. Useful only for System Link titles; per-game recipe.
- Latency: game-defined (they were built for LAN). Effort: medium per title, high for the L2 bridge.

### (b) PINE lockstep - blocked
- PINE (default port 28011) exposes memory read/write, savestate slots, and status (running/paused). It has no pad injection and no frame step.
- Pad input would have to be injected by writing the SIO/pad buffers in EE RAM (per game, fragile) or by a PCSX2 fork. Determinism
  is unproven: PCSX2 states are not guaranteed bit-identical across machines (JIT/VU rounding, GS timing, async IOP/DMA).
- Effort: fork-level. Not recommended.

### (c) LRPS2 / libretro under retrorun - the only plausible path, unproven
- LRPS2 (libretro/ps2) is GPLv3, Windows x86_64 builds exist, savestate support is flagged "basic". Not in `deps/cores`. A newer
  PCSX2-derived core, PCEE2 (WizzardSK/pcee2-libretro), claims deterministic states; unreviewed here.
- Rollback needs a save/load per frame. PS2 states are roughly 30-60 MB (32 MB EE RAM + 4 MB VRAM + IOP/SPU), so per-frame rollback
  is unrealistic; **input-delay lockstep** (no savestates, N frames of delay, desync checksum on EE RAM) is the realistic model, as with
  retrorun's existing lockstep. Both peers run ~50/60 Hz at full speed (PS2 emulation is CPU heavy), and any nondeterminism desyncs.
- Existing retrorun netplay/hash code would likely be reusable. Effort: large (core integration, determinism hunting per game).

### Recommended next step (if wanted)
A 1-day spike: download LRPS2 or PCEE2 DLL into a scratch dir, run BT3 headless in retrorun twice with the same scripted input, hash EE RAM
every 60 frames over 5 minutes. If hashes match across two processes (and ideally two machines), build lockstep on retrorun; if not, stay offline.
Until then PS2 stays "Offline only" (no code change made).
