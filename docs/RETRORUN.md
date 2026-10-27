# retrorun: libretro frontend for GBA / GB / GBC / Nintendo DS

`retrorun.exe` is a small, dependency-light libretro frontend (SDL2 only) that the hub launches for Game Boy
family games. It loads any software-rendered `*_libretro.dll` core; tested with **mGBA** (GBA) and **Gambatte** (GB/GBC).
Source: `src/retrorun/` (`core.cpp` = libretro host, `main.cpp` = frontend, `image.cpp` = PNG/BMP writer,
`third_party/` = vendored `libretro.h` (MIT) and `stb_image_write.h` (public domain)).

## Command line

```
retrorun --core <x_libretro.dll> --rom <file> --save-dir <battery dir> --state-dir <states dir>
         [--system gba|gb|gbc|nds] [--system-dir DIR] [--scale 3] [--fullscreen] [--resume]
         [--opt key=value]... [--ini FILE] [--log FILE]
         [--headless [--frames N] [--screenshot out.png|out.bmp]]
         [--net SERVER --room CODE-N --slot K --players N --ports P0,P1,P2,P3 --delay D]
         [--selftest] [--fuzz-input] [--sync-hash auto|state|output] [--pace vsync|audio|timer] [--list-options]
```

- `--net/--room/--slot/--players/--ports/--delay` mean the same as for `n64run` (ports map RetroPad ports 0-3 to owning slots,
  `-1` = absent). Default delay is `auto` (the RTT-negotiated delay and the adaptive catch-up described in `docs/ARCHITECTURE.md`). With `--rollback`, `auto` starts at 1 frame and adapts; `--no-rb-delay-adapt` pins it.
- Offline, `--players N` (without `--ports`) makes the first N ports local; controller *i* drives the *i*-th local port, the keyboard drives the first.
- `--headless` opens no window and no audio device and runs `--frames` frames (default 600) as fast as possible.
  `--screenshot` writes the last frame (PNG, or BMP if the name ends in `.bmp`).
- `--selftest` (headless): runs `--frames` frames, then checks `serialize -> unserialize -> next-frame state hash` identical, in memory and
  through a state file. `--list-options` prints the core's options and defaults. `--fuzz-input` feeds deterministic pseudo-random
  pad input (never Start/Select), used to exercise netplay.
- Exit code: 0 ok, 1 load error, 3 net setup failed, 4 connection lost, 5 desync detected.
- Environment `N64RUN_MUTE=1`, `RETRORUN_MUTE=1` or `SSB64_MUTE=1` mutes (no audio device is opened).

## Controls

| Action | Keyboard | Gamepad (SDL GameController) |
|---|---|---|
| D-pad | arrows | D-pad / left stick |
| B / A / Y / X | Z / X / A / S | A / B / X / Y (bottom/right/left/top face) |
| L / R | Q / W | shoulders (triggers = L2/R2) |
| Start / Select | Enter / Right Shift | Start / Back |
| Save / load state | F5 / F7 | |
| Slot - / + | F6 / F8 (slots 0-9) | |
| Fast forward (hold, offline only) | Tab | |
| Fullscreen | F11 or Alt+Enter | |
| DS screen layout (cycle) | F9 | |
| DS touch screen (stylus) | left mouse button | |
| Quit | Esc | |

## Core features and limits

Environment calls handled: system/save/content/core-assets directory, libretro path, username, pixel formats
(0RGB1555, RGB565, XRGB8888), `GET_VARIABLE` / `SET_VARIABLES` / `GET_VARIABLE_UPDATE`, core options v1, v1 intl, v2 and v2 intl (reports version 2),
option display callbacks (ignored), log interface, input descriptors/controller info/memory maps (ignored), `GET_CAN_DUPE`,
performance level, `SET_SUPPORT_NO_GAME`, `SET_GEOMETRY`, `SET_SYSTEM_AV_INFO`, input bitmasks (`RETRO_DEVICE_ID_JOYPAD_MASK`), language,
audio/video enable, fast-forwarding state, messages. Anything else returns false.

**Software rendering only.** `SET_HW_RENDER` is refused, so OpenGL/Vulkan cores (and anything needing it) will not run yet; supporting
GL HW cores (shared context, FBO readback or direct present) is a future step. Cores for later systems that are software-rendered work as-is.

**Core options.** Defaults come from the core. Overrides, later wins: `<save-dir>\..\retrorun.ini`, `<save-dir>\retrorun.ini`, `--ini FILE`, `--opt key=value`.
Ini format: `key = value`, `#`/`;` comments, optional `[global]`, `[gba]`, `[gb]`, `[gbc]` sections (section must match `--system`). In netplay all peers
must use identical options; the hub should pass `--opt` explicitly rather than rely on per-PC ini files.

**Directories.** Core save dir = `--save-dir`; core system dir = `--system-dir` (default `<exe dir>\system`, put BIOS files there); content dir = ROM folder.

## Nintendo DS (`--system nds`, melonDS DS core)

- **Core:** `melondsds_libretro.dll`, software renderer. The hub passes no BIOS; the core uses FreeBIOS and generated firmware (see `docs/CORES.md`). `--system nds` makes retrorun apply the defaults in `src/retrorun/dsview.h`
  (software render, DS mode even for DSi-enhanced carts such as Black/White, no SD card or Wi-Fi, silent microphone, 1 s save-RAM flush, PC-clock RTC, OS cursor as the stylus); ini / `--opt` override them.
- **Screens:** the core draws both screens into one frame. Default layout is a vertical stack (`top-bottom`, 256x384: top screen over the touch screen, as DS games are designed); **F9** cycles `top-bottom`, `left-right` (wide windows), `largescreen-top`, `hybrid-top` at run time
  (`Core::set_option`, picked up on the core's next option poll) and remembers the choice in `userdata
etrorun_layout.txt` (`nds=<layout>`). At startup retrorun checks every option key/value it relies on against the loaded core and logs `WARNING: DS core option ...` if one is missing (`qa
etrorun_nds.ps1` asserts there is none). DS frames are scaled fractionally with linear filtering (other systems keep integer nearest scaling).
- **Touch:** the left mouse button over the frame is the stylus (`RETRO_DEVICE_POINTER`, coordinates normalised to the drawn frame, high-DPI aware); a press only touches if it began over the frame (dragging in does not); the OS cursor stays visible, also in fullscreen.
  Headless test: `--stylus U,V` holds the stylus down at that fraction of the frame (with `--opt melonds_show_cursor=always` the core draws the cursor there).
- **Pad:** same RetroPad mapping as GBA; X, Y, L and R are keyboard S, A, Q, W and the controller X, Y and shoulder buttons (see Controls).
- **Saves:** battery RAM goes to `<save-dir>\<rom basename>.srm` like every system (the file is the raw DS save, same bytes as a `.sav`; rename to import/export). The hub points `--save-dir` at the vault battery folder, so it syncs. The core itself gets a scratch save dir (`userdata\core-save\nds`), so nothing but the `.srm` lands in the vault. If a synced `.srm` has a different size than the core's save RAM, retrorun logs a warning and keeps the original as `<name>.srm.bak` (once) before its first write. Save states are not written or resumed for DS.
- **Online:** not offered (the hub only enables online for N64/GBA/GB/GBC). The DS core uses a JIT and 19 MB states; lockstep netplay has not been validated.
- ROMs inside `.zip` are extracted by the hub library to `userdata
om-cache\` (DS images reach 512 MB), so retrorun always gets a plain `.nds`.

## Video

SDL window + streaming texture (`ARGB8888`, nearest filtering), default 3x, integer scale where it fits, aspect from the core's geometry (`aspect_ratio`),
black bars, resizable, vsync when the display refresh is within 5% of the core's fps.

## Audio and timing (design choice)

Audio is a lock-free ring (16384 stereo frames) feeding an SDL device opened at the core's sample rate (SDL converts to the hardware format).
Frame pacing mode is chosen once at start:

1. **vsync + dynamic rate control** - display refresh within +/-5% of core fps (60 Hz vs 59.73 fps): `SDL_RenderPresent` paces the loop and the producer resamples
   (linear) by `1 +/- 0.5%` according to ring fill vs a 70 ms target, so the 0.45% clock mismatch never underruns or overflows the buffer.
2. **audio-paced** - otherwise (e.g. 144 Hz monitor): the loop waits until the ring drops below target before running the next frame; the audio clock is master, video is dupe/skipped by the display.
3. **timer** - when muted/no audio device: QueryPerformanceCounter deadlines at core fps.

Fast forward (Tab, offline) discards audio, skips pacing and presents every 8th frame.

## Saves

- Battery: `<save-dir>\<rom basename>.srm` via `retro_get_memory_data(SAVE_RAM)`. Loaded at start (copied up to the smaller of file and core sizes),
  written on exit and every 30 s **only if changed**, always temp-file + rename (`MoveFileEx` replace + write-through) so a crash never tears the file.
  Cores that write their own saves (via the save dir) also land in `--save-dir`.
- States: `<state-dir>\slotN.state` (F5/F7), plus `auto.state` written on exit and loaded at start only with `--resume` (ignored online).
  Loading states is disabled in netplay.

## Netplay (lockstep)

Reuses `src/n64run/lockstep.cpp` + `lockstep_core` (compiled into retrorun, not modified): one "sample" is one `retro_run` frame. Each frame the local pads of owned ports
are polled, scheduled for frame + D, exchanged through `hubnet::Session`, and the frame blocks until every present port's input is known (redundant packets + 50 ms
retransmit while blocked, 10 s no-progress timeout = "Connection lost"). A "ready" barrier makes all peers start the same frame. Every 600 frames a 64-bit hash is
compared among peers; a mismatch logs `DESYNC ...` and the exit code is 5. Fast forward and state loading are disabled online. If the Worker's daily cost guard is at 50% or more of its cap, new relay sessions are refused with `server-busy`; direct UDP still works (see `docs/ARCHITECTURE.md`).

**Desync hash.** By default the hash is of the `retro_serialize` buffer (`--sync-hash state`). Gambatte's serialized state differs between runs even though its
video+audio output is bit-identical (about 10-20 bytes of the 27-51 KB state; a core quirk), which would report false desyncs, so for Gambatte `auto` uses a rolling hash
of all video frames and audio samples instead (`--sync-hash output`). Each `final:` log line prints `state-hash`, `output-hash` and `input-hash` (all peers must agree on the last two).

**Rollback:** see the Rollback section below; this paragraph is the original plan: using `retro_serialize`/`retro_unserialize` (mGBA states are exact; Gambatte states would need the quirk fixed or a different core) to hide latency
instead of input delay.

## Build

Part of the top-level CMake (`add_subdirectory(src/retrorun)`; needs `hubnet`, `hubcommon`, `n64run_lockstep_core`, `SDL2::SDL2`):

```
cmake -S . -B build
cmake --build build --config Release --target retrorun   # -> build/bin/Release/retrorun.exe (+ SDL2.dll)
```

## Testing (no window)

Cores: `deps\cores\mgba_libretro.dll`, `gambatte_libretro.dll` (libretro nightly buildbot). Test ROMs in `deps\testroms\` (gitignored): jsmolka/gba-tests (MIT) `sram.gba`, `arm.gba`, `thumb.gba`;
mattcurrie `dmg-acid2.gb`, `cgb-acid2.gbc` (MIT).

```
retrorun --core deps\cores\gambatte_libretro.dll --rom deps\testroms\dmg-acid2.gb --save-dir out\sv --state-dir out\st --headless --frames 600 --screenshot out\acid.png
retrorun --core deps\cores\mgba_libretro.dll --rom deps\testroms\sram.gba --save-dir out\sv --state-dir out\st --headless --frames 300 --selftest
# two peers (any order), real server:
retrorun ... --headless --frames 1800 --fuzz-input --net wss://smash-netplay.jumpcircuit.workers.dev --room KQ7ZMP-1 --slot 0 --players 2 --ports 0,1,-1,-1 --log a.log
retrorun ... --headless --frames 1800 --fuzz-input --net wss://smash-netplay.jumpcircuit.workers.dev --room KQ7ZMP-1 --slot 1 --players 2 --ports 0,1,-1,-1 --log b.log
```

Note: mGBA reports "Failed test" for jsmolka `arm.gba` #522 and `thumb.gba` #230 (obscure STMFD-with-base-in-list edge cases) - core accuracy, not a frontend issue; `sram.gba` shows "All tests passed".
Windowed code paths can be smoke-tested with `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy` (no window appears).

## On-screen display, volume and speed
- Every status message (state saved/loaded, slot, screenshot, disc, ...) is shown as an OSD toast (`src/common/osd.h`) as well as in the window title. `--osd-msg TEXT` plus `RETRORUN_OSD_SHOT=1` with `--screenshot` composites it into a QA picture. See RUNNER_CTL.md for the layer details.
- RetroHub steers a running retrorun through `control.json` (`src/common/runner_ctl.h`): `volume` (0-100), `mute`, `speed` (0.25 0.5 1 1.5 2 3 5 8 or max; shows "Speed 2x" on the OSD), `ff`, `osd_msg`. All but `osd_msg`, `screenshot` and `quit` are refused in online sessions. `rewind` (value 1/0) is implemented, `shader`, `overlay` and `clip` are reserved and answer `unsupported`. Full table: RUNNER_CTL.md.

## Limitations

- No hardware rendering, no sensors/camera, no core-options UI (use `--opt` / ini), no shaders.
- **Rewind and run-ahead (offline only; refused online, in `--link` and on PS2):** `--rewind auto|on|off` keeps a ring of XOR-delta compressed states (`rewind.h`): about 20 s for NES/SNES/GB/GBC/GBA, off by default for PS1/DS (`on` gives 5 s). Hold LT + View (not with Start or Y) or send runner_ctl `rewind`; the OSD corner shows `REWIND n.ns`. `--runahead off|1|2|auto` runs 1-2 speculative frames after the real one with audio muted and restores the state (`runahead.h`); Auto = 1 on NES/SNES/GB/GBC/GBA. Settings: per-game "Run-ahead" (play.runahead). Both measure their cost over the first 120 frames and turn themselves off with an OSD message if too slow (not in headless). QA: `--headless --rewind-selftest N,K` / `--runahead N --runahead-selftest N` (frame hashes after rewind-replay, and run-ahead output == normal shifted by N); `retrorun_rewind_test` covers the ring limits and the same checks on a fake core.
- Supported: cheats (`--cheats FILE`, live toggles offline), multi-disc swap (`--disc-ctl FILE`, libretro disk-control interface), rumble (libretro rumble -> `SDL_GameControllerRumble` on the pad of that local port).
- Zipped ROMs are not unpacked by retrorun itself (the hub library extracts them first) (cores that handle zip themselves via `need_fullpath` work).
- Core options changed while running are not supported (the core sees the values given at start).
- Cores with real-time clocks (e.g. mGBA with RTC games) may use wall-clock time, which can desync netplay; Gambatte state is not byte-exact after load (affects rollback later, not lockstep).

## Nintendo DS local wireless online (`--ds-link`)

Trades, battles and the Union Room are the DS's local wireless. The melonDS DS core emulates it by exchanging timestamped
MP frames through libretro's **netpacket interface** (`RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE`), so retrorun implements
that interface on top of `hubnet::Session` (STUN hole punch, WebSocket relay fallback; nobody port-forwards).
`retrorun ... --ds-link --net SERVER --room CODE-N --slot K --players N` (needs `--system nds`):

- No lockstep and no savestate sync: every PC runs its own emulator and its own real save; only wireless frames travel
  (DS client id = room slot, host = 0). Fast forward and state loading are off while linked.
- Core packets (up to ~2.3 KB) are split into <= 1000-byte datagrams (`ds_link.h`, tested by `retrorun_dslink_test`).
- Launch spec key `np_ds` = `{"v":1,"mode":"wireless"}` (written by the host; guests refuse a newer version).
- Why not standalone melonDS: its LAN/Netplay start only from GUI dialogs (no command line), so a managed install would
  need UI automation. The libretro core already exposes the same MP layer through netpacket.
- `qa\ds_link.ps1` runs N headless instances through the live server (`-Relay`, `-Players 3`). `RETRORUN_LINK_PROBE=1`
  adds synthetic 2.3 KB frames as a transport check. Real wireless traffic only appears once a game opens a wireless
  session, which needs a save inside a Pokemon Center; that was not done headlessly.
- Limit: the core waits only 25 ms per MP frame, so high-latency connections may drop frames.

## Link cable netplay (`--link`, Game Boy / Color Pokemon online)

`retrorun --core sameboy_libretro.dll --system gb|gbc --link --rom <rom> [--link-roms romP0,romP1] --net ... --ports 0,1,-1,-1 --players 2`

Every PC runs BOTH Game Boys in ONE deterministic SameBoy instance (`retro_load_game_special`, game type 0x101, `sameboy_link=enabled`,
`sameboy_rtc=accurate`, side-by-side frame). Port 0 drives console 1, port 1 console 2; inputs flow through the normal lockstep netplay, so the
emulated cable is part of the shared simulation and trades/battles cannot desync. Each PC shows only its own console (the frame is cropped) and
plays that console's audio. The periodic state hash covers both consoles (SameBoy states are exact: selftest passes, identical state hashes on both peers).
- **Saves:** before frame 0 each PC broadcasts its own save (`<stem>.srm` + `<stem>.rtc` in `--save-dir`) to the others (`link_sync.*`, all-to-all,
  loss tolerant, hash verified); every PC loads blob(s) into the console owned by that slot, so you trade with your own Pokemon. Only your own console's
  save is written back (never before the exchange succeeded). Offline `--link --players 2` loads console 2 from `<stem>.p2.srm`.
- **Launch spec:** `"np_link":{"core":"sameboy","consoles":2}`; the hub then passes the vault battery dir. Catalog: Red/Yellow/Gold/Silver/Crystal `online=true`, 2 players.
- **Limits:** both PCs need the same ROM unless `--link-roms` lists each console's ROM (cross-version trades Red<->Blue need both ROMs on both PCs);
  RTC is cycle-based (the in-game clock only advances while playing); SameBoy wants optional boot ROMs (`dmg_boot.bin`/`cgb_boot.bin`) in the system dir.
- **GBA link (`mgba_link_libretro.dll`, `src/mgba_link`):** mGBA's libretro cores expose no link port, so RetroHub builds its own libretro core from mGBA 0.10.5
  (CMake FetchContent, pinned tarball + SHA256, static `libmgba`, MPL-2.0). It creates 2 (up to 4) GBA cores in one process and joins them with mGBA's own
  `GBASIOLockstep` driver (multiplayer SIO mode, used by Pokemon Ruby/Sapphire/Emerald/FireRed/LeafGreen for trades and battles). mGBA's Qt frontend runs one thread
  per console; here the consoles are stepped cooperatively from one thread in a fixed order (wait/sleep are flags honoured after the event returns), so the
  result is deterministic. retrorun uses it exactly like SameBoy dual: `--core mgba_link_libretro.dll --system gba --link`, side-by-side 240N x 160 frame cropped
  to the own console, same memory-id layout (save exchange and own-console write-back unchanged), serialized state = all consoles + the lockstep bookkeeping
  (state hash). HLE BIOS; fake-epoch RTC (option `mgbalink_rtc_epoch`) advancing with emulated frames. `MGBALINK_DEBUG=1` prints SIO state.
  Catalog: Ruby/Sapphire/Emerald/FireRed/LeafGreen `online=true`, 2 players; spec `"np_link":{"core":"mgba","consoles":2}`.
  Limits: the Wireless Adapter (RFU, SIO normal-32) is not emulated; entering a real game's link room was not exercised headlessly (needs a save inside a
  Pokemon Center); the .srm is 128 KiB for Flash1M games (autodetected from the ROM).
- Tests: `qa\retrorun_gbalink.ps1` (two peers via a local `wrangler dev`) and `qa\make_siotest_rom.py` (hand-assembled multiplayer-SIO test ROM).
- Test: `qa\retrorun_link.ps1` (needs a local `wrangler dev`), `retrorun_link_test` (exchange logic with loss/reorder).

## Rollback netplay (`--rollback`, `--synctest N`)

GGPO-style rollback (`rollback.h/.cpp`, engine independent of SDL/libretro/hubnet; `retrorun_rollback_test` runs two engines over a simulated lossy link).
Remote inputs are predicted (repeat last), the state before every frame is saved (`retro_serialize` into a max_pred+4 ring, default 8 predicted frames),
a mispredicted input restores the state and re-simulates up to now with audio dropped (only the newest frame is presented), the sim stalls at the prediction
limit, and a PC that runs ahead waits an occasional frame (time sync). Input delay is 1 frame (`--delay 0..4`; the lockstep RTT negotiation is skipped).
Packets are cumulative and acked, so loss only costs latency. Desync check: every 120 confirmed frames a rolling hash of the confirmed video frames is compared
(`RB_HASH_INTERVAL` overrides). Final lines print `rollback-stats` (rollbacks/s, max/avg depth, stalls, sync-waits) and `rollback-timing`.

Safety: retrorun only rolls back on cores whose savestates passed SyncTest, otherwise it logs `staying on lockstep netplay` and falls back (`RETRORUN_FORCE_ROLLBACK=1` overrides for QA).
- Verified: `mgba_link_libretro` (hub's GBA link core). mGBA's `mgba_idle_optimization` is forced to `Don't Remove` for rollback (idle-loop state is not in the savestate).
- Verified: `sameboy_rb_libretro` (`src/sameboy_rb`, GB/GBC single and `--link` dual mode). Root causes of the old SameBoy failures, all state outside its savestate: joypad-bounce emulation (compares with the unsaved `gb->keys`), `joypad_is_stable`, power-on RAM from a global time-seeded RNG (must be off before the first `GB_init`), the pixel buffers / LCD-off retained frames (frames the PPU does not repaint show the previous timeline's pixels) and the link cable's in-flight serial bits. The core is libretro/SameBoy (MIT, pinned commit) patched in `src/sameboy_rb/patch_libretro.cmake` (patched state is 16 + 4x92 KB bigger), built with MinGW gcc (SameBoy needs GNU C; MSVC cannot compile it; set `-DSAMEBOY_RB_GCC=...`; without gcc the target is skipped and the hub stays on lockstep). Library name "SameBoy (rollback)".
- Not verified: stock mGBA libretro (non-link; savestates drift after frequent reloads), Gambatte (state not exact), stock SameBoy (joypad-bounce state outside the savestate).
- `HUBNET_LOOPBACK=1` (QA only; also needs `RETROHUB_QA=1`, as does `N64RUN_TEST_INPUT`) binds the UDP socket to 127.0.0.1 and skips STUN, so headless test peers never trigger a Windows firewall prompt.
- `--synctest N` (offline, `qa/retrorun_synctest.ps1`): a free-running reference pass records per-frame video + memory hashes, then a second pass rolls back N frames after every frame (also running half the frames first with wrong pads) and compares every frame with the reference.
- Cores start rollback after 12 neutral warmup frames (first boot frames cannot be restored faithfully).
- Test: `qa/retrorun_online.ps1 -Rollback -Core mgba_link_libretro.dll -Extra --link ...` against a local `wrangler dev`.

## PlayStation netplay (Beetle PSX)

`--system ps1` with `beetle_psx_libretro.dll` (docs/CORES.md, PS1 determinism). Same `--net/--room/--slot/--players/--ports` as the other systems, up to 4 ports (`--opt beetle_psx_enable_multitap_port1=enabled` for ports 2-3: the hub adds it when a third port is taken).
- **Input packet**: per port 16 button bits + the two sticks (lx ly rx ry, 8 bits each: a DualShock axis is one byte, so `axis >> 8` loses nothing). Lockstep: `LockstepCore` "wide" mode, a second u32 per sample (`n64run::LockstepOptions::wide`, `Lockstep::aux()`); rollback: `rb::Config::val_bytes = 6` (`uint64_t` values, 2 bytes for the other cores). Datagrams stay under `hubnet::kMaxPayload`: the redundancy window shrinks with the number of local ports.
- **Desync check**: the rolling hash of the output frames (never the serialized state, which differs between identical runs). Lockstep every 600 samples, rollback every 120 confirmed frames.
- **Setup fingerprint** (`kPktCfg` 0x60, before the start barrier): core build + `beetle_psx_*` options + the `scph*.bin` files of `--system-dir` (or `openbios`). A mismatch logs `FAIL: slot N runs a different PlayStation setup` and exits 7 on every PC.
- **Pinned online** (`--synctest` too): resolution, widescreen, PGXP, overclocks, CD speed/access, OSD, frame duping, crop, deinterlacer (`off`), analog toggle, memory card 2 (see the `pins` table in `Frontend::setup_paths`); `--opt` cannot override them.
- **Pacing** (rollback): `pred_budget` 3 (a PlayStation frame costs 3-8 ms, so the input delay takes over before re-simulation gets deep), `--rb-pred 6` (10 saved states, ~160 MB), non-forced packets at most every 30 ms, and when a peer is only reachable through the server relay the per-frame packet goes out every other frame (the relay caps a room at 400 frames/s and the host forwards every client packet: a 4-PC room costs 12 server frames per packet, so 60 packets/s per PC would be 720/s). Local 4-PC relay run: correct but about 0.6x real time on a laptop-local `wrangler dev` (client-to-client traffic crosses the relay twice, RTT ~215 ms); direct UDP is real time.
- **Rollback gate**: only a core named `Beetle PSX (rollback)` (the patched build) rolls back; a stock Beetle stays on lockstep.
- **PlayStation 2 (LRPS2) lockstep** (`--system ps2`, docs/PS2_ONLINE.md): setup fingerprint `kPktCfg2` 0x61 (core build, BIOS hash, disc, `pcsx2_*` options), exit 9 / 10 / 11 on a BIOS / disc / core-options mismatch; memory card exchange before the core boots (`--share-host-save`, sibling room `-(n+100000)`); `--setup-tag` adds the disc serial to the fingerprint; sticks + L2/R2 pressure in the wide packet; sync hash = EE RAM.
- **QA hooks**: `RB_SAVEFRAMES=<dir>` (every stepped frame as a BMP), `RB_SAVEBYHASH=<dir>` (each distinct frame named by its hash: find what differs after a rollback), `RETRORUN_DUMP_RAM=<file>` (system RAM at exit), `--fuzz-input` also fuzzes the sticks.
- **Tests**: `qa\ps1_synctest.ps1` (also `ctest -R ps1_synctest`), `qa\ps1_online.ps1 -Server ws://127.0.0.1:<port> -Scenarios quick|long|all` (direct, relay, bad network, 2 and 4 peers, host memory card, BIOS mismatch, peer kill; every peer must exit 0 with identical output + input hashes), unit tests in `n64run_lockstep_test` (wide packets under loss) and `retrorun_rollback_test` (6-byte inputs, stick packing).
- **Discs**: `qa\make_ps1_netdisc.py` (4-pad SIO0 homebrew, `--interlace` for 480i) builds a MODE2/2352 `.cue/.bin` with EDC/ECC that boots from OpenBIOS; `qa\make_ps1_testdisc.py` has the ISO builder (`raw_mode2`).


## NES / SNES online
Rollback (--rollback) via Nestopia UE / Snes9x, verified with --synctest 8/16; qa\nes_snes_online.ps1 runs direct, relay, 3-4 player (SNES multitap on port 2), guest drop and host leave. NES 3-4 players: retrorun sets nestopia_select_adapter=ntsc (Four Score), but the test ROM does not see pads 3-4 yet, so NES 3-4P is unverified. retro_init now runs before the set_* callbacks (RetroArch order).

## Screen filters and bezels (`present_fx.cpp`)

`--fx off|crt|lcd|dmg|sharp` picks a CPU post-process on the scaled frame (~1 ms at 1080p, benchmarked by `retrorun_fx_test`); `--bezel 1` paints a procedural per-console frame (GB, GBC, GBA, NES, SNES, DS, PS1, generic) in the pillarbox bars. Both default off, which keeps the classic path (including `--filter` and integer scaling) untouched. The OSD is drawn after both. Settings: `fx.filter` and `fx.bezel` in Per-game settings (console default or per game); runner_ctl `shader <name>` changes them live. Headless: `--screenshot` renders the filtered, bezelled window (`RETRORUN_FX_SHOT_SIZE=WxH`, default 1280x720).
