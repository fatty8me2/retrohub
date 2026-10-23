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
  `-1` = absent). Default delay is 2 frames.
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
  (`Core::set_option`, picked up on the core's next option poll) and remembers the choice in `userdataetrorun_layout.txt` (`nds=<layout>`). At startup retrorun checks every option key/value it relies on against the loaded core and logs `WARNING: DS core option ...` if one is missing (`qaetrorun_nds.ps1` asserts there is none). DS frames are scaled fractionally with linear filtering (other systems keep integer nearest scaling).
- **Touch:** the left mouse button over the frame is the stylus (`RETRO_DEVICE_POINTER`, coordinates normalised to the drawn frame, high-DPI aware); a press only touches if it began over the frame (dragging in does not); the OS cursor stays visible, also in fullscreen.
  Headless test: `--stylus U,V` holds the stylus down at that fraction of the frame (with `--opt melonds_show_cursor=always` the core draws the cursor there).
- **Pad:** same RetroPad mapping as GBA; X, Y, L and R are keyboard S, A, Q, W and the controller X, Y and shoulder buttons (see Controls).
- **Saves:** battery RAM goes to `<save-dir>\<rom basename>.srm` like every system (the file is the raw DS save, same bytes as a `.sav`; rename to import/export). The hub points `--save-dir` at the vault battery folder, so it syncs. The core itself gets a scratch save dir (`userdata\core-save\nds`), so nothing but the `.srm` lands in the vault. If a synced `.srm` has a different size than the core's save RAM, retrorun logs a warning and keeps the original as `<name>.srm.bak` (once) before its first write. Save states are not written or resumed for DS.
- **Online:** not offered (the hub only enables online for N64/GBA/GB/GBC). The DS core uses a JIT and 19 MB states; lockstep netplay has not been validated.
- ROMs inside `.zip` are extracted by the hub library to `userdataom-cache\` (DS images reach 512 MB), so retrorun always gets a plain `.nds`.

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
compared among peers; a mismatch logs `DESYNC ...` and the exit code is 5. Fast forward and state loading are disabled online.

**Desync hash.** By default the hash is of the `retro_serialize` buffer (`--sync-hash state`). Gambatte's serialized state differs between runs even though its
video+audio output is bit-identical (about 10-20 bytes of the 27-51 KB state; a core quirk), which would report false desyncs, so for Gambatte `auto` uses a rolling hash
of all video frames and audio samples instead (`--sync-hash output`). Each `final:` log line prints `state-hash`, `output-hash` and `input-hash` (all peers must agree on the last two).

**Planned upgrade: rollback** using `retro_serialize`/`retro_unserialize` (mGBA states are exact; Gambatte states would need the quirk fixed or a different core) to hide latency
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

## Limitations

- No hardware rendering, no cheats, no disk control (multi-disc), no rumble/sensors/camera, no core-options UI (use `--opt` / ini), no rewind, no shaders.
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
