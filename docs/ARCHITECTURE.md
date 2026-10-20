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
cmake -S Hub -B Hub/build
cmake --build Hub/build --config Release
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
n64run --rom PATH --game ID [--net SERVER --room CODE-N --slot K --players N --ports P0,P1,P2,P3 --delay D] [--save-dir DIR]
```

- `--ports` maps each N64 controller port to an owning slot. For example `0,1,-1,-1` means port 1 is played by slot 0, port 2 by slot 1, and ports 3 and 4 have no controller. The game fills those with CPUs.
- Offline (no `--net`): all ports are local controllers.

**Loading the emulator:** the runner loads `emu/mupen64plus.dll` and the RSP-HLE, audio-sdl and GLideN64 plugins. The **input plugin is `n64run.exe` itself**: it exports the input plugin API, and `CoreAttachPlugin(M64PLUGIN_INPUT, GetModuleHandle(NULL))`.

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

**Saves in online sessions:** each session starts from a fresh, empty save directory on every PC, so the EEPROM is identical everywhere. Transferring the host's save comes in v2.

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
