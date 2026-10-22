# RetroHub — architecture

A Windows desktop hub for playing N64 games with friends: a single library, room codes, and one-click launch.

- **Native ports** run as-is. Smash Classic (BattleShip) keeps its own rollback netplay.
- **Every other game** runs through mupen64plus, embedded in our own runner (`n64run.exe`), with lockstep input netplay over our own transport.

Users supply their own ROMs. Nothing copyrighted is shipped.

## Layout

```
Hub/
  CMakeLists.txt          top level; FetchContent: SDL2, Dear ImGui, mupen64plus-core (API headers only)
  src/net/                static lib `hubnet`: UDP, WebSocket, STUN, room client (lobby + game session)
  src/hub/                `RetroHub.exe`: ImGui launcher (library, ROMs, rooms, launch)
  src/n64run/             `n64run.exe`: mupen64plus frontend + built-in netplay input plugin
  src/common/             games catalog, ROM identification (MD5), paths, logging
  server/                 Cloudflare Worker additions (new /hub endpoint, HubRoom Durable Object)
  scripts/fetch-emu.ps1   downloads the mupen64plus 2.6.0 win64 bundle + GLideN64 into deps/emu
  docs/
```

Build with the VS 2026 bundled CMake:

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release -LE online --output-on-failure   # offline console tests (see CLAUDE.md)
```

The build is C++17, x64, with MSVC flags `/W3 /permissive- /utf-8`.

## Runtime layout (packaged)

```
RetroHub/
  RetroHub.exe
  n64run.exe
  emu/        mupen64plus.dll, mupen64plus-rsp-hle.dll, mupen64plus-audio-sdl.dll,
              mupen64plus-video-GLideN64.dll, SDL2.dll, data files
  games/      (optional) native ports, e.g. smashClassic/BattleShip.exe
  hub.json    user settings: name, ROM folder, server URL, controller
```

## Lobby server: `/hub` endpoint (new; the Smash `/ws` endpoint is untouched)

The URL is `wss://smash-netplay.jumpcircuit.workers.dev/hub?room=CODE&role=host|join&name=NAME`.

- **Code format:** 6 characters, same alphabet as Smash (`[A-HJ-NP-Z2-9]`).
- **Room size:** up to 4 members.
- **Slots:** the host is slot 0. Joiners get the lowest free slot.
- **Room lifetime:** a room ends when its host leaves. All members are then closed with code 4001.

Messages from the server (JSON text):

| Message | When |
|---------|------|
| `{type:"welcome", room, slot}` | On accept |
| `{type:"members", members:[{slot,name}]}` | On every join or leave, to everyone |
| `{type:"msg", from, body}` | A member's broadcast. `body` is any JSON object; the hub defines its meaning (see below) |
| `{type:"peers", peers:[{slot,public,local}]}` | Whenever a member reports its endpoint, to everyone |
| `{type:"error", reason}` then close 4000 | `reason` is one of `room-in-use`, `no-such-room`, `room-full` |

Messages from a client:

| Message | Effect |
|---------|--------|
| `{type:"msg", body}` | Broadcast to the other members |
| `{type:"endpoint", public, local}` | Stored and broadcast as `peers` |
| `{type:"keepalive"}` | Auto-response `keepalive_ack` |

**Binary relay (always available):** byte 0 is the destination slot, or `0xFF` for all other members. The rest is the payload. The server rewrites byte 0 to the sender's slot and forwards. The maximum frame size is 1200 bytes.

**Validation and details as implemented** (`BattleShip/server/cloudflare/src/hub.js`):

- `room` is `[A-HJ-NP-Z2-9]{6}` optionally followed by `-` and 1 to 6 digits; `slot` (join only) is 0-3, and a taken or zero slot gives `room-full`. A joiner without `slot` gets the lowest free slot. `name` is stripped to printable ASCII and cut to 24 characters.
- Text frames above 4096 bytes and binary frames above 1200 bytes are dropped. `endpoint` strings must match `ip:port` or `-`, otherwise they are stored as `-`. `msg.body` must be a JSON object.
- `peers` lists only the members that have reported an endpoint, and goes to everyone each time one reports.
- A joiner that connects before the host gets `no-such-room`. The hubnet `Session` retries that every 700 ms until its 15 s budget runs out, so game-room launch order does not matter.
- Binary relay frames are delivered to the destination slot (or every other member for `0xFF`); the sender never gets its own frame back.

### hubnet UDP framing (Session)

First byte is the type: `H` hello `[H,from]`, `A` helloack `[A,from]`, `D` data `[D,from,to,payload]` (`to` 0xFF = all), `P`/`Q` ping/pong `[P|Q,from,ts u32]`, `R` roster `[R,0,mask]` (host to clients every 250 ms; bit s set = slot s reachable from the host). Relay frames carry the same packet after the server-managed slot byte. `ready()` means the roster lists at least `expected` slots including mine. `HUBNET_FORCE_RELAY=1` disables UDP.

### Room roles

- **Lobby room** (the code users share): used by `RetroHub.exe`.
- **Game room** (`<CODE>` with a launch suffix, e.g. `ABC234-1`, which the server accepts as `room=` followed by `-` and digits): used by `n64run.exe` for the match itself. It joins with `slot=K` to request its lobby slot.

## Hub lobby messages (`msg.body`, defined by the hub)

| Body | Sent by | Meaning |
|------|---------|---------|
| `{kind:"hello", version, games:{<gameId>: <romMd5>}}` | Everyone, on join | Which games each player owns, and the exact ROM |
| `{kind:"select", game}` | Host | The host chose a game; everyone shows it |
| `{kind:"ready", ready}` | Anyone | Ready toggle |
| `{kind:"launch", game, launch, delay, seed}` | Host | Everyone starts the game. Native Smash uses its own lobby (the hub passes the code); emulator games use `n64run` with game room `<CODE>-<launch>` |
| `{kind:"chat", text}` | Anyone | Chat line |

## hubnet library: what `n64run` needs (`src/net/session.h`)

A connected game session among up to 4 PCs, using a star topology: the host (slot 0) is the hub, and clients connect to the host only.

1. **Connecting:** each client hole-punches to the host using STUN endpoints from `peers`, and falls back to the WebSocket binary relay.
2. **Forwarding:** the host forwards client-to-client traffic, so every PC sees every slot's messages.
3. **API (C++):**
   ```cpp
   struct SessionConfig { std::string server, room; int slot; int expected; };
   class Session {
     bool start(const SessionConfig&);   // non-blocking; call update() each tick
     void update();                       // drives WS, STUN, punch, relay, keepalive
     bool ready() const;                  // all `expected` slots reachable
     bool failed() const; std::string status() const;
     void send(int to_slot /*-1 = all*/, const void* data, int len);
     bool recv(int* from_slot, std::vector<uint8_t>* out);  // pops one message
     int  ping_ms(int slot) const;
   };
   ```

Delivery is unreliable, the same as UDP. The lockstep layer handles loss by repeating its inputs.

## n64run: emulator + lockstep netplay

**Command line:**

```
n64run --rom PATH --game ID [--net SERVER --room CODE-N --slot K --players N --ports P0,P1,P2,P3 --delay auto|D] [--save-dir DIR]
```

- `--ports` maps each N64 controller port to an owning slot. For example `0,1,-1,-1` means port 1 is played by slot 0, port 2 by slot 1, and ports 3 and 4 have no controller. The game fills those with CPUs.
- Offline (no `--net`): all ports are local controllers.

**Loading the emulator:** the runner loads `emu/mupen64plus.dll` and the RSP-HLE, audio-sdl and GLideN64 plugins. The **input plugin is `n64run.exe` itself**: it exports the input plugin API, and `CoreAttachPlugin(M64PLUGIN_INPUT, GetModuleHandle(NULL))`.

**Headless mode (`n64run --headless [--frames N] [--screenshot OUT.png] [--dump-ram OFFSET,LEN,FILE]...`):** for tests and QA; never shows a window or opens an audio device. (1) Video: the real GLideN64 plugin still renders, but the runner installs a video extension (`CoreOverrideVidExt`, `src/n64run/headless.cpp`) that creates the OpenGL context on a hidden SDL window (`SDL_WINDOW_HIDDEN`, vsync off, default 640x480). (2) Audio: no audio plugin is attached, so the core uses its dummy audio. (3) Speed: the core speed limiter is switched off on the first VI, so it runs as fast as the host allows (netplay still waits for peers' inputs). (4) Stop: the core frame callback counts VIs and stops at `--frames N` (exit 0); with `--frames 0` (default) it runs until the session ends or the connection is lost. (5) Input: pads and keyboard are not read, so only the (empty) lockstep input applies. (6) `--screenshot` reads the back buffer with `glReadPixels` just before the swap of the last frame (PNG/BMP by extension; periodic grabs when `--frames` is not given). (7) `--dump-ram 0xOFFSET,LEN,FILE` (repeatable) writes RDRAM bytes in N64 (big-endian) byte order after emulation stops, and the run logs `headless: final RDRAM hash`. Limitations: needs a working OpenGL driver (no software fallback), the OSD is disabled, and `--frames` in online mode must be identical on all peers (a peer that stops early makes the others report a lost connection after 10 s). See `qa/n64run_online_qa.ps1` and `qa/n64run_headless_lib.ps1` (also asserts no visible top-level window).

**Forced, identical config on every PC:**

| Setting | Value |
|---------|-------|
| `Core/R4300Emulator` | 2 (dynarec) |
| `Core/RandomizeInterrupt` | 0 |
| `Core/CountPerOp` | 0 (use the ROM database value) |
| `Core/DisableExtraMem` | 0 |
| `Core/SiDmaDuration` | -1 |
| `Core/EnableDebugger` | 0 |
| Save state / speed-limiter hotkeys | off |
| Controller pak | none (`Plugin = PLUGIN_NONE`, so no mempak serial randomness) |

**Slots and `--players`:** `--slot` may be any of 0..3; lobby slots can have gaps (members `{0,2}` after slot 1 left). `--players` is the number of PCs, not max slot + 1. The session's slot set is the host (slot 0), `--slot`, and every owner named in `--ports`; if `--players` is larger than that, the lowest free slots are added as port-less spectators. `--players` smaller than the PCs found is an error (exit 2). The same applies to `retrorun`. Both `Session` readiness (a count of reachable PCs) and `LockstepCore` (`Config::slot_mask`) work with sparse slot sets (`derive_slot_mask` in `lockstep_core.h`).

**Direct VS match (`--match "stage=<id>;stocks=<n>;[time=<min>;]p0=<fighter>:<costume>;p1=...;p2=...;p3=..."`, `src/n64run/match.{h,cpp}`):** for Smash Remix (`--game remix`) and vanilla Smash 64 (`ssb64rom`; same addresses). Numbers are decimal or `0x` hex; absent players are not present (at least two needed); all present players are human on controller port `pN`. Fighter/stage ids are in `src/common/remix_data.h` (`docs/REMIX.md`). The core frame callback steps a `MatchDriver` once per VI (the VI count is identical on every lockstep peer, so it is deterministic and desync-free). It waits until the scene byte (`0x800A4AD0`) is the title (`0x01`) for 30 VIs (VI ~699 on Remix, ~3716 on vanilla), writes the VS block + player blocks + `SCCommonData.gkind`, sets `previous=0x01`, `current=0x16` and `screen_interrupt=1`. Verified headless: the direct jump works for Remix-only fighters too (Wolf, Wario, Sonic, Marth, Banjo, Conker) and Remix stages (e.g. 0x31 Battlefield); `stocks` is written 0-based. Limits: Remix settings outside the VS block use their defaults; costumes are not validated (use <= `max_costume`). `--match` works offline or online (pass the same spec on every peer).

**Input delay (`--delay auto|N`, default `auto` with `--net`, for `n64run` and `retrorun`):** lockstep throughput is about delay / RTT, so a fixed 2 frames stalls at 60-100 ms ping. With `auto`, after `Session::ready()` the host pings every client for about 1.5 s (`Session::ping_stats`), takes the worst effective RTT (the slowest client, or the sum of the two slowest because client-to-client traffic crosses the host), and computes `D = ceil((rtt/2 + 8 ms) / frame_ms) + 1` clamped to 1..10 (+1 if any path is relayed; `frame_ms` = 1000 / core fps, 16.7 for N64 and GB/GBA). The host sends D to everyone (`kPktDelay`, clients ack with `kPktDelayAck`) and logs `input delay auto: rtt=XXms -> D`. The host is always authoritative: a numeric host `--delay N` makes the host broadcast N (skipping the measurement), and every client always waits for and adopts the host's D regardless of its own `--delay` (logging when it differs), so mixed modes cannot disagree or hang. At the end of a session both runners log `netplay stalls: total X ms over Y stalled samples, Z samples`.

**Network simulator (QA):** opt-in: nothing below has any effect unless `HUBNET_SIM_ENABLE=1` is also set (so a stray variable cannot degrade real play). Set in the environment of any process that uses `hubnet` (applies to every outgoing datagram, direct UDP and relay frames; relay frames stay in order): `HUBNET_SIM_LATENCY_MS` (added delay per send), `HUBNET_SIM_JITTER_MS` (extra uniform 0..J ms), `HUBNET_SIM_LOSS_PCT` (random drop, 0..100), optional `HUBNET_SIM_SEED` (reproducible). Each PC applies it to what it sends, so RTT is about 2 x latency + jitter. Example: `HUBNET_SIM_ENABLE=1 HUBNET_SIM_LATENCY_MS=80 HUBNET_SIM_JITTER_MS=20 HUBNET_SIM_LOSS_PCT=5 hubnet_test <server> 3 300`. `hubnet_test` relaxes its delivery threshold to 100 - 2*loss - 4 % when loss is set.

**Exit codes (`n64run`, and the same for `retrorun --net`):**

| Code | Meaning |
|------|---------|
| 0 | Clean exit |
| 2 | Bad arguments (including `--slot/--players/--ports` inconsistencies) or invalid ROM |
| 3 | Emulator / core / plugin load failure (retrorun: only "could not reach the lobby/peers"; negotiation and start-barrier loss are 4) |
| 4 | Netplay connection failure, input-delay negotiation failure, or connection lost mid-game |
| 5 | Desync detected (`n64run`: only with `--strict-sync`; `retrorun`: always) |
| 6 | Host-save sync failed (`--share-host-save`) |

**Saves in online sessions:** each session starts from a fresh, empty save directory on every PC. With `--share-host-save` (opt-in, every PC must pass it) the host (slot 0) reads `--host-save-dir` and sends its battery saves (`.eep .sra .fla .mpk`, at most 8 files, 128 KiB each, 256 KiB total) to the other PCs before the ROM opens (exit code 6 if the sync fails). Transfer uses `src/n64run/save_sync.{h,cpp}`: chunked, per-chunk check, whole-file FNV-1a, client ACK bitmaps, host resends. Without the flag the sessions start empty, as before.

**Lockstep in `GetKeys(Control, Keys)`:**

- Sample index `n` advances on each `GetKeys(0, …)` call.
- For each port this PC owns, it reads the local pad and schedules that input for sample `n + D`.
- It sends `{sample, port, buttons}` with redundancy: the last 8 samples in every packet.
- It blocks until inputs for `n` are known for every port. While waiting it keeps pumping `Session::update()` and SDL events.
- A 10 s timeout ends the game with "Connection lost".

**Desync check:** every 600 samples, each PC sends a hash of RDRAM (via `M64CMD_*` memory access, or the debugger API if needed). A mismatch is logged and shown on screen.

## Games catalog (`src/common/games.cpp`)

| id | Title | Engine | USA ROM MD5 | Save |
|----|-------|--------|-------------|------|
| `smash64` | Super Smash Bros. (Smash Classic) | native BattleShip | (BattleShip validates) | — |
| `mp1` | Mario Party | mupen64plus | 8BC2712139FBF0C56C8EA835802C52DC | EEPROM 4K |
| `mp2` | Mario Party 2 | mupen64plus | 04840612A35ECE222AFDB2DFBF926409 | EEPROM 4K |
| `mp3` | Mario Party 3 | mupen64plus | 76A8BBC81BC2060EC99C9645867237CC | EEPROM 16K |
| `remix` | Smash Remix 2.0 (fan ROM hack) | mupen64plus | 43571020FA5DDB138BBD7F64A8C9C0D4 | — |

ROM files can be `.z64` (big-endian), `.v64` (byteswapped) or `.n64` (little-endian). Normalize to `.z64` byte order before hashing. Identification uses the MD5 of the normalized image, with a fallback to the header CRC1/CRC2 at offsets 0x10/0x14.
