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
