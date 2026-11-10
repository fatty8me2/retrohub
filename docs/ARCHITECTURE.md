# RetroHub — architecture

A Windows desktop hub for playing retro games with friends: a single library, room codes, and one-click launch.

Consoles: NES, SNES, N64, GB / GBC / GBA, DS, 3DS, GameCube, Wii U, PS1, PS2, PSP, Nintendo Switch and Nintendo Switch 2 (two consoles in the UI, one `System::Switch` underneath: see docs/EMULATORS.md "Two consoles in the UI").

- **Native ports** run as-is. Smash Classic (BattleShip) keeps its own rollback netplay.
- **N64 games** run through mupen64plus, embedded in our own runner (`n64run.exe`), with lockstep input netplay over our own transport.
- **NES, SNES, GB, GBC, GBA, DS and PS1** run through libretro cores in our own runner (`retrorun.exe`), with lockstep or rollback netplay.
- **GameCube, Wii U, 3DS, PS2, PSP and Switch** run in external emulators that RetroHub installs and configures (Dolphin, Cemu, Azahar, PCSX2, PPSSPP, Lichii). See `docs/EMULATORS.md`.
- **Switch runners:** Lichii is the default. Fritchii and Hookshot are opt-in, per game. See "Switch runners" below.

Users supply their own ROMs. Nothing copyrighted is shipped.

## Layout

```
RetroHub/
  CMakeLists.txt          top level; FetchContent: SDL2, Dear ImGui, mupen64plus-core (API headers only)
  src/common/             static lib `hubcommon`: games catalog, library scan, ROM identification, game settings, controller profiles (SDL half in controls_sdl.cpp)
  src/net/                static lib `hubnet`: UDP, WebSocket, STUN, room client (lobby + game session), friends, identity link
  src/saves/              static libs `hubsaves` (save vault, two-way folder sync, zip export/import, cloud saves) and `hubminiz`
  src/hub/                `RetroHub.exe`: ImGui launcher, split by concern: app/frame/lib_state/lobby (state and flow),
                          logic.cpp (pure, `hublogic`), launch*.cpp (game launch, online launch), emulators.cpp (3DS/Wii U),
                          netplay_3ds*.cpp, netplay_dolphin.cpp (GameCube), page_*.cpp and *_ui.cpp (screens), invite.cpp,
                          updater.cpp / update_apply.cpp, shell_integration.cpp. Tests are the `test_*.cpp` files beside them.
  src/n64run/             `n64run.exe`: mupen64plus frontend, in-exe input plugin, lockstep netplay, --match driver, save sync
  src/retrorun/           `retrorun.exe`: libretro frontend for NES/SNES/GBA/GB/GBC/DS/PS1 cores; link cable, lockstep and rollback netplay
  src/stream/             `streamrun.exe`: remote play host and guest (capture, H.264, UPnP) and the DSU pad server for Cemu; see docs/REMOTE_PLAY.md
  src/helper/             `rh-helper.exe`: tiny console program that runs one allow-listed bundled script (logic in rh_helper_logic.h)
  src/cheevos/            RetroAchievements login and cache data shared by the hub and retrorun
  src/mgba_link/          `mgba_link_libretro.dll`: N linked GBAs in one deterministic core, built from mGBA source
  src/sameboy_rb/         `sameboy_rb_libretro.dll`: SameBoy with two determinism patches, for rollback (MinGW gcc)
  src/beetle_psx/         `beetle_psx_libretro.dll`: Beetle PSX, software renderer, patched for deterministic states (MinGW gcc), plus the PS1 serial database
  assets/                 brand, controller database, icons
  scripts/fetch-emu.ps1   downloads the mupen64plus 2.6.0 win64 bundle + GLideN64 into deps/emu

The Cloudflare Worker (lobby, friends, saves, invite pages) lives in the BattleShip repo at `server/cloudflare`, not here.
qa/ holds the headless QA scripts; they take ROM and game folders as parameters or env vars.
docs/ holds these notes.
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
  BattleShip.exe  Smash Classic; the updater requires it next to RetroHub.exe
  n64run.exe
  retrorun.exe    libretro runner (cores/ holds its cores)
  streamrun.exe   remote play
  emu/        mupen64plus.dll, mupen64plus-rsp-hle.dll, mupen64plus-audio-sdl.dll,
              mupen64plus-video-GLideN64.dll, SDL2.dll, data files
  userdata/   hub.json (user settings: name, ROM folder, server URL, controller) and the rest of the hub's data
  games/      (dev layout only) smashClassic/BattleShip.exe; a packaged install finds BattleShip.exe beside RetroHub.exe
```

## Runner control, OSD and prefs (hub <-> runners)
- `src/common/runner_ctl.h`: the hub writes `control.json` in the runner's run dir (commands incl. `volume`, `mute`, `speed`, `ff`, `osd_msg`, `rewind`, `shader`, `overlay`, `clip`, `pause`, `osd_corner`, `handoff`; retrorun implements all of them, n64run answers `unsupported` to `rewind`, `shader`, `overlay` and `handoff`), the runner acks in `control_ack.json`. Spec: `docs/RUNNER_CTL.md`.
- `src/common/osd.h`: runner toast layer; drawn over the frame by retrorun, mirrored into the window caption by n64run (GL overlay behind `N64RUN_OSD_GL=1`).
- `src/common/prefs.h`: synced account prefs `<vault>/_account/prefs.json` (favorites, user_shelves, hidden, renames, covers, theme, profile.*), field-wise newest-wins merge. See `docs/SAVES.md`.
- `src/hub/shot_viewer.cpp`: full-size screenshot viewer opened from the screenshots strip (copy to clipboard, set as cover, delete, open in default viewer; "Send to a friend" is a stub until the server has DM attachments).

## Shell: never over a game, updates, first run, idle (Plan 4, Wave 1a)

**The house rule.** While any game or session runs (`session_active()`; `game_active()` adds another full-screen or busy app), RetroHub
never raises, focuses, covers or flashes anything over it, and never closes one. The policy is pure logic in `src/hub/popup_guard.h`; every
OS call that could break it goes through `window_guard.cpp`, and `hub_popup_guard_test` lints `src/hub` **and `src/common`**: an unmarked
raise/show/topmost/`MessageBox` call fails, the marked sites are pinned by file and count, and every place that injects synthetic input or
attaches to another thread's input (`keybd_event`, `SendInput`, `AttachThreadInput`, ...) is pinned too. **The one allowed exception is
View+Y**: the player pressed it, so `launch.cpp` `force_foreground()` taps Alt and attaches input to take the foreground for the game menu
(the "user hotkey" sites; `hub_menu_topmost` keeps the open menu above the game and clears on close). Anything else RetroHub wants to say
during a game goes to the bell, or to the runner's own OSD where one exists. (src/n64run and src/retrorun, the game windows themselves, are
not scanned.)

**No launch during an online session** (`launch_guard_logic.h`). `launch_allowed()` refuses every launch a person asks for (Play, Resume, Continue, invites, the palette, the boot flow) while ANY session is up, not only while `A.proc` is set. That covers GameCube NetPlay, a 3DS room, Wii U remote play and Kanto Together. The Play button is disabled with "An online game is running." (or "A game is already running."). A room's own game start passes `system_launch` and is only refused if a game is already running. Test: `hub_launchset_test`.

**Updates never close a game** (`updater.cpp`, `update_apply.cpp`):
- `start_update()` may download, verify and unpack while a game runs (it is idempotent, so the About page, the startup popup, the bell and a
  room's "UPDATE NOW" need no checks). The hand-off to the installer is driven by `hubupdate::tick()` from the main loop: it starts only
  when `hubguard::may_install_update`: no session of any kind (`session_active()`: `A.proc`, the job object, GameCube NetPlay, the 3DS room,
  remote play, Kanto), nothing left running from the install folder (a game started by an earlier RetroHub run), and the state has settled
  for 2.5 s (`detail::gate_step`, a pure state machine with injected time). Meanwhile `update_deferred()` is true, the status line reads
  "Update ready: installs when you finish playing", and the bell gets "Update ready: installs after your game" once. `main.cpp` quits for
  the installer only when `installer_started()` and no session runs.
- The applier (`RetroHub.exe --apply-update`, a TEMP copy) waits for the old RetroHub, then waits for anything else running from the install
  folder and **never terminates it**: after `--wait-secs` (default 20 min) it gives up, changes nothing, leaves a status note, relaunches
  the old version quietly and exits 3. Nothing is ever terminated, not even the old RetroHub (it may own a game through a kill-on-close
  job): after `--hub-wait-secs` (default 300 s) the apply is postponed too.
- Rollback: every replaced file is backed up first (`%TEMP%\RetroHub-update-<pid>\backup` with a `manifest.json` naming the install and the
  from/to versions). If a restore itself fails, the backup is KEPT (`Result::backup_kept`) and startup recovery (`recover_interrupted`, run
  by `startup_pass_async`) puts the files back, but only onto the version the backup came from (never a downgrade), never touches another
  install's or an unreadable leftover, and leaves a live applier alone. A restore is skipped when a failed copy never wrote a byte (a locked
  file).
- A failed or postponed update writes `userdata\update_status.json`; the next start shows it ONCE as a bell entry ("Update failed: a file
  was in use", "Update postponed: a game was running", ...), never a window. The unpacked files stay in `updates\staged` with
  `updates\staged.json` (version, file count, bytes) so a retry skips the download when they still match exactly; a successful install
  removes both. The first update that brings these changes is still applied by the previous version's applier.
- Tests: `hub_updater_test` (gate state machine, staged reuse), `hub_apply_test` (a fake game under the install folder survives the applier, failed restore keeps the backup, recovery cases, status note), `qa\update_during_game.ps1` (the same with the real `RetroHub.exe` and a real long-running process).

**First run** (`wizard_logic.h`, `main.cpp`). The installer pre-fills a games folder (`userdata\installer_games_dir.txt`, written only by a
fresh install, never an upgrade). `main()` decides "fresh" from the folders the PLAYER chose, before that folder counts (`fresh_install`),
so a fresh install always runs the setup wizard (controller, games, name, cloud), with the installer's folder as the Games step's pre-filled
answer. The hand-off file is kept until the wizard is finished or skipped, so quitting mid-wizard does not lose it; `should_autorun` (also
read by the "What's new" popup) honours the same flag.

**Idle throttle** (`idle_logic.h`, `main.cpp`). The loop used to draw and present at vsync even behind a game. Now every poller still runs
every iteration (at least about 60 Hz: the lobby and friends sockets, the Dolphin / 3DS / remote-play drivers, the pad watchers, the process
watcher), but with the hub in the background (not focused, hidden, or a game session running) a frame is drawn only when needed: always for
input, a repaint request, the View+Y menu, the start animation, toasts or a netplay driver session; at most every 33 ms for 400 ms after the
last input or visible change (a hash of the draw data detects change); otherwise every 100 ms. In between the loop waits with
`SDL_WaitEventTimeout` (input wakes it at once). Off for the QA drivers; `HUB_IDLE_THROTTLE=0|1`, `HUB_IDLE_SIM=bg[,noraster]`,
`HUB_FPS_REPORT=<file>`, `HUB_EXIT_AFTER=<s>`; `qa\idle_fps.ps1` runs the headless before/after.

**Smaller rules.** TV mode: the Settings checkbox (any non-F11 route) cannot fullscreen the hub over a running game (`may_enter_tv_mode`);
F11 in the focused hub still works. Windows toasts: none while a game runs, even when opted in (`hubtoast::should_toast` and the system
notifier's own probe; FRIENDS.md). Controller tips: the card is no longer a topmost window over a third-party game; for hub-launched
external emulators the hub shows the tips in its own window while the game is still starting (`draw_launch_tips`), retrorun draws them into
its own frame, and n64run's card is an owned, non-topmost window of its own game window (`src/common/hint_popup.cpp`).

## Lobby server: `/hub` endpoint (new; the Smash `/ws` endpoint is untouched)

The URL is `wss://smash-netplay.jumpcircuit.workers.dev/hub?room=CODE&role=host|join&name=NAME`.

- **Code format:** 6 characters, same alphabet as Smash (`[A-HJ-NP-Z2-9]`).
- **Room size:** up to 4 members.
- **Slots:** the host is slot 0. Joiners get the lowest free slot.
- **Room lifetime:** a lobby room lives until its last member leaves. **Host migration:** a client that connects with `&caps=hostmig` (every
  RetroHub since this feature) keeps the room alive when the host (slot 0) leaves: the lowest remaining opted-in slot is renumbered to slot
  0 (so "host = slot 0" stays true for the port plan and the hubnet game session), every remaining member gets `host_changed` then
  `members`, and the room code keeps working (friends' presence and invites point at the same code; a new joiner takes any free slot).
  Members that did NOT send `caps=hostmig` (older RetroHub) are closed with 4001 `host-left` as before, and the opted-in ones migrate; if
  nobody opted in, the room just closes. **Game rooms** (`<CODE>-N`) never migrate: the host leaving closes everyone with 4001, which ends
  the match. The host flag lives only in each socket's attachment (hibernation-safe); the Durable Object is single-threaded so there is
  never more than one slot 0. A RetroHub talking to an older Worker simply sees the old 4001 close.

Messages from the server (JSON text):

| Message | When |
|---------|------|
| `{type:"welcome", room, slot}` | On accept |
| `{type:"members", members:[{slot,name}]}` | On every join or leave, to everyone |
| `{type:"msg", from, body}` | A member's broadcast. `body` is any JSON object; the hub defines its meaning (see below) |
| `{type:"peers", peers:[{slot,public,local}]}` | Whenever a member reports its endpoint, to everyone |
| `{type:"host_changed", from_slot, slot:0, name}` | Host left and `from_slot` (named `name`) is now slot 0. Sent to every remaining opted-in member, just before the new `members`. The promoted member's own `my_slot` becomes 0 |
| `{type:"error", reason}` then close 4000 | `reason` is one of `room-in-use`, `no-such-room`, `room-full` (older RetroHub: also `bad-slot`, `host-reconnecting`, `spectators-full`, `spectate-busy`, `no-such-game`, `host-too-old`, `no-free-port`), or `server-busy` (the free daily budget is used up: no new rooms) or `rate-limited` (too many rooms or sockets from one network in a minute). `LobbyClient`/`Session` show every one of them in plain words |
| `{type:"relay_denied", reason, detail}` | A binary frame of the receiver was NOT relayed, at most once per 10 s per socket. `reason` is `server-busy` (daily budget: from 50% a room's first relayed frame, from 90% every frame) or `rate-limited` (the room sent more than 400 frames/s or the socket more than 1 MiB/s). Additive: older clients ignore it |

Messages from a client:

| Message | Effect |
|---------|--------|
| `{type:"msg", body}` | Broadcast to the other members |
| `{type:"endpoint", public, local}` | Stored and broadcast as `peers` |
| `{type:"keepalive"}` | Auto-response `keepalive_ack` |

**Binary relay (subject to the daily cost guard below):** byte 0 is the destination slot, or `0xFF` for all other members. The rest is the
payload. The server rewrites byte 0 to the sender's slot and forwards. The maximum frame size is 1200 bytes. A frame that is not forwarded
is answered with `relay_denied`; the hubnet `Session` then says "The free server is busy, so the relay is off right now. Direct connections
still work." instead of "Connection lost".

**Validation and details as implemented** (`BattleShip/server/cloudflare/src/hub.js`):

- `room` is `[A-HJ-NP-Z2-9]{6}` optionally followed by `-` and 1 to 6 digits; `slot` (join only) is 0-3, and a taken or zero slot gives `room-full`. A joiner without `slot` gets the lowest free slot. `name` is stripped to printable ASCII and cut to 24 characters.
- Text frames above 4096 bytes and binary frames above 1200 bytes are dropped. `endpoint` strings must match `ip:port` or `-`, otherwise they are stored as `-`. `msg.body` must be a JSON object.
- `peers` lists only the members that have reported an endpoint, and goes to everyone each time one reports.
- A joiner that connects before the host gets `no-such-room`. The hubnet `Session` retries that every 700 ms until its 15 s budget runs out, so game-room launch order does not matter.
- Binary relay frames are delivered to the destination slot (or every other member for `0xFF`); the sender never gets its own frame back.

### hubnet UDP framing (Session)

First byte is the type: `H` hello `[H,from]`, `A` helloack `[A,from]`, `D` data `[D,from,to,payload]` (`to` 0xFF = all), `P`/`Q` ping/pong
`[P|Q,from,ts u32]`, `R` roster `[R,0,mask]` (host to clients every 250 ms; bit s set = slot s reachable from the host). Relay frames carry
the same packet after the server-managed slot byte. `ready()` means the roster lists at least `expected` slots including mine.
`HUBNET_FORCE_RELAY=1` disables UDP.

**Who sent a frame (session token).** The sender byte inside a frame is only a claim. Over the relay the server stamps the real slot, and a frame whose claim does not match is dropped (the one exception: a guest hearing the host forward another guest's data). For direct UDP each peer sends a random per-session token to every other peer over the relay (`T` frame, relay only). Its direct `H`, `A`, `P` and `Q` frames then carry that token, and the address of a frame that proves it becomes the peer's bound address. Data from that peer must come from it. A peer that never sends a token (an older RetroHub) keeps the old rules, and its frames are only counted (`Session::unproven_frames`). Dropped frames are counted in `Session::spoofed_frames`. Tests: `hubreconnect_test`.

**Guest quits.** A guest's clean `bye` ends a 2-player host session, as before. In a 3 or 4 player room the host ends only when no other player is left (`other_player_remains`), so the rest keep playing. In n64run lockstep every PC handles a guest's clean quit like a crash: the others agree a cutoff, that port goes idle and the match continues, as long as another player besides the leaver is left (`bye_becomes_drop`). The host's quit, a 2-player match and the last guest leaving still end the match.

### Room roles

- **Lobby room** (the code users share): used by `RetroHub.exe`.
- **Game room** (`<CODE>` with a launch suffix, e.g. `ABC234-1`, which the server accepts as `room=` followed by `-` and digits): used by `n64run.exe` for the match itself. It joins with `slot=K` to request its lobby slot.

### Daily request cost guard (Worker, `reqbudget.js`, `reqbudget_core.js`)

Every Worker request counts 1 unit, every Durable Object request and alarm 1 unit, and each incoming WebSocket message 1/20 unit (Durable
Objects bill incoming WS messages 20:1). Units are counted per isolate and flushed to one ReqBudget DO at most every 30 s. The cap is
`DAILY_REQ_CAP` (default 100,000 units per UTC day), and the thresholds are fractions of it. Two more limits of the Workers Free plan are
folded into the same percentages: the estimated Durable Object duration (13,000 GB-s a day) and **Durable Object rows written** (100,000 a
day for the whole account; the guard's cap `DAILY_ROWS_CAP` is 60,000, 60% of it). Rows are the real `rowsWritten` of every UserHub and
Directory SQL statement plus 1 per KV put/delete and `setAlarm` elsewhere; a failed write is an outage (hello, presence, hosting and relay
start all write), so this axis trips the same gates before the wall. Limits as of 2026-11-01:
https://developers.cloudflare.com/durable-objects/platform/pricing/.

| Usage | Effect |
|-------|--------|
| >= 50% | No NEW relay sessions: on `/ws` the `relay` request, on `/hub` a room's first relayed frame. The sender gets `relay_denied` with `reason:"server-busy"`; direct UDP still works. No spectating. |
| >= 70% | No NEW rooms. `/ws` and `/hub` `role=host` are refused with `error` `server-busy` (RetroHub says "The free server is busy right now, so it can't open new rooms"). |
| >= 90% | Relayed frames are dropped (the sender is told with `relay_denied`). Relay sessions in progress stop; direct play is unaffected. New `/friends` sockets are refused, presence alarms pause until just after 00:00 UTC, and optional storage writes (the hub launch replay, the `/ws` relay flag) are skipped. |

Caps that apply whatever the budget:

- **Relay (`/hub`, like `/ws`):** each room 400 frames/s sustained (burst 2x), each socket 1 MiB/s (burst 4 MiB); over-budget frames are dropped and the sender told (`relay_denied` `rate-limited`, at most every 10 s); a socket over budget for 10 s without a pause is closed with 1008.
- **Text frames in a room socket:** 50/s per socket (burst 600: a joining RetroHub announces its game library in 3000-byte parts, again on every membership change); a flood is dropped and the socket closed with 1008 after 5 s.
- **Per address (`/hub`):** 30 new rooms and 120 sockets per minute (IPv6 counted per /64); refused with `error` `rate-limited`. Loopback and private LAN addresses (a local `wrangler dev`) are never limited.
- **Presence fan-out:** one presence event reaches at most 40 online friends (the Free plan allows 50 subrequests per invocation), always the least recently served first; the others re-sync on their own next hello, whose fan-out pulls our current status from the reply. The rest of the `/friends` hello is unchanged.
- **New accounts:** 6 per address (IPv6: per /64) and 100 service-wide per UTC day (test clients, `"test":true` or a `qa-` name: 30 and 100,
  counted apart). Refusal: `{type:"error", reason:"registration-limited", detail:"ip"|"global", retry_after:<seconds to 00:00 UTC>}`; the
  socket is left open on purpose (every shipped client reconnects after any close). Existing accounts are never affected. Local tests are
  not limited (loopback and private LAN addresses).
- **Sockets that have not said hello:** hello must arrive within 30 s and within 3 frames, otherwise 4002; at most 8 parked sockets per account (the oldest are closed); they never touch the account's own 300 messages/minute budget.

If the DO cannot be reached, the isolate's own count still applies (fails closed on the local number). The guard lives in BattleShip's Worker, so it covers RetroHub and native Smash alike. Tests: `server/cloudflare/test_reqbudget.mjs` (the maths), `test_rows_gate.mjs`, `test_hub_relay_caps.mjs`, `test_presence_cap.mjs`, `test_register_rl.mjs`, `test_friends_preauth.mjs`.

## Hub lobby messages (`msg.body`, defined by the hub)

| Body | Sent by | Meaning |
|------|---------|---------|
| `{kind:"hello", version, games:{<gameId>: <romMd5>}}` | Everyone, on join | Which games each player owns, and the exact ROM |
| `{kind:"select", game}` | Host | The host chose a game; everyone shows it |
| `{kind:"ready", ready}` | Anyone | Ready toggle |
| `{kind:"launch", game, launch, delay, seed}` | Host | Everyone starts the game. Native Smash uses its own lobby (the hub passes the code); emulator games use `n64run` with game room `<CODE>-<launch>` |
| `{kind:"chat", text}` | Anyone | Chat line |
| `{kind:"np_fail", reason}` | Guest | The guest could not start the host's launch (the reason is shown to the host). Sent by `launch_fail` in `lobby.cpp`. |
| `{kind:"pt_state", v, rev, ph, ...}` | Host | Party mode (Plan 4): the whole party State (`party_logic.h`, < 4 KB), sent on every change and when membership changes, never on a timer. Only slot 0 is believed; a guest applies only a newer `rev` (or a new night `seed`). Host migration: the new host re-broadcasts its last State with `rev + 16`. |
| `{kind:"pt_vote", round, what:"next"\|"winner", pick}` | Anyone | A vote for the next game (`pick` = game id) or for who won (`pick` = slot). The host folds it into the State; a vote for another round or kind of poll is ignored. |

### Party mode (Plan 4 P1-P3)

Host turns it on in a room (room page panel): playlist (games everyone owns, or Shuffle), rounds 3/5/7/10, points (auto / vote / none),
timer (off / 5 / 10 / 15 min), auto-ready. Rounds end on the first Smash result, the timer, or when the host's game closes (then a "Who
won?" vote; a tie is the host's call). Every PC closes its own game (OsdMsg, OsdCorner standings, Quit, window close as a last resort,
`party_closer.h`); the native Smash port has no control channel, so it only gets the window close. Standings and the podium show in the hub
only under the after-game rule (`prompt_allowed()`). Anyone can vote for the next game in any room (tie: Surprise-me style reel). Couch
party: same reducer, no room. Finished nights are stored as `party/<id>.json` (user data and `_account/party` in the vault;
`party_history.h`). Older RetroHubs ignore `pt_*`; the host sees "<name> needs to update for Party mode".

### Lobby room UI (v1.21.0)

- **Action strip:** the host's button reads **LAUNCH** and is enabled only when a game is selected, there is no launch blocker, and everyone is ready. Until then it says who it is waiting for ("Waiting for Kai..."). Guests get **READY** / **NOT READY**. The state comes from `room_action_state` in `src/hub/logic.cpp`, which is unit tested.
- **Rejoin:** if the connection drops, the room page shows a failure strip with a **Rejoin room** button in place of the action button. `rejoin_room()` reconnects with the same room code and the same role (`room_role_host`). A launch that failed on a guest is reported to the host with `np_fail`.

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

**Headless mode (`n64run --headless [--frames N] [--screenshot OUT.png] [--dump-ram OFFSET,LEN,FILE]...`):** for tests and QA; never shows a
window or opens an audio device. (1) Video: the real GLideN64 plugin still renders, but the runner installs a video extension
(`CoreOverrideVidExt`, `src/n64run/headless.cpp`) that creates the OpenGL context on a hidden SDL window (`SDL_WINDOW_HIDDEN`, vsync off,
default 640x480). (2) Audio: no audio plugin is attached, so the core uses its dummy audio. (3) Speed: the core speed limiter is switched
off on the first VI, so it runs as fast as the host allows (netplay still waits for peers' inputs). (4) Stop: the core frame callback counts
VIs and stops at `--frames N` (exit 0); with `--frames 0` (default) it runs until the session ends or the connection is lost. (5) Input:
pads and keyboard are not read, so only the (empty) lockstep input applies. (6) `--screenshot` reads the back buffer with `glReadPixels`
just before the swap of the last frame (PNG/BMP by extension; periodic grabs when `--frames` is not given). (7)
`--dump-ram 0xOFFSET,LEN,FILE` (repeatable) writes RDRAM bytes in N64 (big-endian) byte order after emulation stops, and the run logs
`headless: final RDRAM hash`. Limitations: needs a working OpenGL driver (no software fallback), the OSD is disabled, and `--frames` in
online mode must be identical on all peers (a peer that stops early makes the others report a lost connection after 10 s). See
`qa/n64run_online_qa.ps1` and `qa/n64run_headless_lib.ps1` (also asserts no visible top-level window).

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

**Slots and `--players`:** `--slot` may be any of 0..3; lobby slots can have gaps (members `{0,2}` after slot 1 left). `--players` is the
number of PCs, not max slot + 1. The session's slot set is the host (slot 0), `--slot`, and every owner named in `--ports`; if `--players`
is larger than that, the lowest free slots are added as port-less spectators. `--players` smaller than the PCs found is an error (exit 2).
The same applies to `retrorun`. Both `Session` readiness (a count of reachable PCs) and `LockstepCore` (`Config::slot_mask`) work with
sparse slot sets (`derive_slot_mask` in `lockstep_core.h`).

**Direct VS match (`--match "stage=<id>;stocks=<n>;[time=<min>;]p0=<fighter>:<costume>;p1=...;p2=...;p3=..."`,
`src/n64run/match.{h,cpp}`):** for Smash Remix (`--game remix`) and vanilla Smash 64 (`ssb64rom`; same addresses). Numbers are decimal or
`0x` hex; absent players are not present (at least two needed); all present players are human on controller port `pN`. Fighter/stage ids are
in `src/common/remix_data.h` (`docs/REMIX.md`). The core frame callback steps a `MatchDriver` once per VI (the VI count is identical on
every lockstep peer, so it is deterministic and desync-free). It waits until the scene byte (`0x800A4AD0`) is the title (`0x01`) for 30 VIs
(VI ~699 on Remix, ~3716 on vanilla), writes the VS block + player blocks + `SCCommonData.gkind`, sets `previous=0x01`, `current=0x16` and
`screen_interrupt=1`. Verified headless: the direct jump works for Remix-only fighters too (Wolf, Wario, Sonic, Marth, Banjo, Conker) and
Remix stages (e.g. 0x31 Battlefield); `stocks` is written 0-based. Optional `rules=<1 time|2 stock|3 both>` (default 2; QA: `rules=1;time=1`
is a one-minute match). Limits: Remix settings outside the VS block use their defaults; costumes are not validated (use <= `max_costume`).
`--match` works offline or online (pass the same spec on every peer).

**Hub control while blocked (runners-M1):** `hooks.idle` calls `ctl_idle`, so Quit, volume, mute and the OSD commands are served inside
every lockstep wait (stall, start barrier, desync recovery, drop-in barrier) and the connect loop; Quit calls `Lockstep::stop_waiting()` so
the wait returns at once (exit 0). Verified by `qa/n64run_quit_stall.ps1`: ack 0.3 s, exit 0.44 s inside an 8 s stall (the old build: 6.9
s). Not covered: delay negotiation, cheat check and save sync (bounded 8-20 s).

**Input delay (`--delay auto|N`, default `auto` with `--net`, for `n64run` and `retrorun`):** lockstep throughput is about delay / RTT, so a
fixed 2 frames stalls at 60-100 ms ping. With `auto`, after `Session::ready()` the host pings every client for about 1.5 s
(`Session::ping_stats`), takes the worst effective RTT (the slowest client, or the sum of the two slowest because client-to-client traffic
crosses the host), and computes `D = ceil((rtt/2 + 8 ms) / frame_ms) + 1` clamped to 1..10 (+1 if any path is relayed; `frame_ms` = 1000 /
core fps, 16.7 for N64 and GB/GBA). The host sends D to everyone (`kPktDelay`, clients ack with `kPktDelayAck`) and logs
`input delay auto: rtt=XXms -> D`. The host is always authoritative: a numeric host `--delay N` makes the host broadcast N (skipping the
measurement), and every client always waits for and adopts the host's D regardless of its own `--delay` (logging when it differs), so mixed
modes cannot disagree or hang. At the end of a session both runners log `netplay stalls: total X ms over Y stalled samples, Z samples`.

**Adaptive delay and catch-up (v1.21.0, `lockstep.cpp`, `DelayAdapter` in `lockstep_core.h`):** the negotiated D is only the starting point.
The host watches stalls and may order D up or down during play (at most one change per 1.5 s; lowering waits for 4 s of quiet). The cap is
`max(floor, min(8, floor + 3))`, never more than the hard cap `kMaxDelay` (20 samples). Every change is logged as
`input delay A -> B (sample N)`. When a poll gap shows a hiccup, the emulator runs unthrottled to repay the debt
(`catch-up: N ms behind, running unthrottled`, then `catch-up: done`). Catch-up starts above 40 ms of debt, is capped at 1.5 s of debt, and
ends after 4 s at most. Every 600 samples (about 10 s) both runners log
`latency: sample N wall ... lag ... vs 60 Hz, delay D (floor F), rtt R ms, stalled S ms, peer-lead P`.

**Spectating (v1.29.0, `src/n64run/spectate.{h,cpp}`):** `--spectate` (n64run and retrorun, with
`--net --room CODE-N --ports ... --players N`) joins a running online match as a port-less watcher; the hub's "Watch" button on a friend row
does it (`src/hub/spectate_ui.cpp`: a `role=watch` seat in the friend's lobby room, which hands over the stored launch message). A spectator
is NOT a lockstep slot, so it can never stall the players. The host (slot 0) records the inputs the emulation consumed (lockstep: sample n
once complete; rollback: a frame once confirmed, i.e. never rolled back; `Engine::set_confirmed_sink`) and streams them from sample 0 to
each spectator (`kPktData` 0x70 host to spectator, `kPktAck` 0x71 back: cumulative acks, a 1024-sample window above the spectator's consumed
position, resend from the acked position on stalled acks). A late joiner asks for a live snapshot (`kPktJoin`; retrorun, and n64run from
v1.29.2): the host sends its state with `rb::xfer` (`src/n64run/state_xfer.h`) and streams the inputs from that sample; n64run saves a mupen
savestate during the lead-port poll of sample n (the state before n + 1, taken by the core at its next interrupt) and the spectator loads it
before its first counted poll. Old hosts, young matches and too-big states fall back to the history from sample 0, replayed unthrottled
(`spectate: catching up`); either way it then follows live. Online n64run turns off GLideN64's RDRAM image dithering
(`RDRAMImageDitheringMode=0`): its noise depends on how many frames the plugin has drawn, so a snapshot-joined spectator's framebuffers in
RDRAM (part of the sync hash) would never match. QA: `qa/n64_spectate_live.ps1`. The host's periodic state hashes ride on the stream and the
spectator's go back in its acks, so both ends verify the same game (`DESYNC (spectator)`). The server (`role=watch`): 2 spectators per room,
refused with `spectators-full`, or `spectate-busy` when the day's REQ_BUDGET is over 50%; a game-room spectator takes a free slot 1..3 (so a
4-player game has no room), is flagged `spec:true` in `members`, never receives broadcasts, and may only send to slot 0. In `Session`,
spectators are excluded from `ready()`, `send(-1)` and the roster, and a spectator's bye does not end the session. Limits: games that share
the host's save (`--share-host-save`) or use cheats cannot be followed. QA: `qa/spectate_online.ps1` (players plus spectators through a
local worker; final hashes must be identical).

**Join fingerprint (retrorun spectators and drop-in, `src/retrorun/joinfp.h`, `joinid.h`).** The joiner's knock ends with a short trailer: the game id (`joinid.h`: the ROM CRC and size for cartridges, the boot serial and data track for discs), the core name and version, and a netcode version. The host compares it with its own before it sends a snapshot. A different game file, a different core or a different netcode version is refused with a plain message ("Your copy of this game is different from the host's...", "...different emulator cores...", "...different version..."). A knock with no trailer comes from an older RetroHub and is refused like an old peer. Tests: `retrorun_joinid_test`, `retrorun_dropin_test`.

**N64 drop-in (`src/n64run/dropin.h`, `Lockstep::di_*`, `frontend.cpp` namespace `live`):** a friend joins a running n64run lockstep match
into an empty controller port (hub: the same "Join game" button, lobby seat `role=watch&dropin=1` and launch `dropin` count as the NES/SNES
drop-in; `n64_dropin_game()` lists the party games: Mario Kart 64, Mario Party 1-3, Smash 64 ROM / Remix). *Controller presence:* the PIF
cannot be re-plugged mid-run without desyncing, so a drop-in room (hub flag `--dropin-room`, passed to every player and spectator of a
launch whose `dropin` count is >= 1) reports ALL four controllers as plugged in from boot (`LockstepCore::Config::reserved`,
`g_run.reserved`, no Controller Pak on the unowned ones); an unowned port reads neutral for everybody and the founders' behaviour is
otherwise unchanged (a 2-player Smash/MK64 menu simply shows the extra controllers as connected). *Joining* is a live spectator join
promoted to a player: the joiner (`--dropin --slot K --ports <plan> --name N`, a real game-room member, `kPktJoin` version 2 with a
capability byte and its name) gets the host's savestate (existing `live::` path: the core's worker thread writes it, no stall) plus the
confirmed input stream, loads it and catches up unthrottled; when it is a few samples behind it sends `Ready`, the host picks the join
sample J (~75 samples ahead), the first sample F the port is supplied from (`F = J + D + slack + rtt + joiner lag`) and broadcasts
`Admit{slot, port, J, F, D, owners, mask}` (`kPktDropIn` 13) until every player and the joiner acked. Every PC binds the port to the slot
from F on (`LockstepCore::join_port`: neutral before F, the slot gates and hashes from F, so nobody waits for the joiner before F); the
joiner turns into a player at its poll J (`Lockstep::dj_promote`), sends `Live`, and from there it is an ordinary slot (resync, adaptive
delay, drop). The host's barrier at J waits (at most 2.5 s) for missing acks, then `Abort`s and unbinds. *Leaving:* a bye or 3 s of silence
runs the existing mid-match drop agreement; once the cutoff is behind everybody the port is reserved again (`leave_port`) and the next
friend can drop in (the joiner cycles through the free slots when the plan's slot is taken; every slot taken = exit 9). Hub exit codes
mirror retrorun: 9 full, 10 a player's RetroHub too old (capability bit `kCapDropIn` missing, or no answer), 11 anything else. Refused for
games that share the host save or use cheats. QA: `qa/n64_dropin.ps1`; unit tests `n64run_dropin_test`.

**QA-only switches:** `HUBNET_LOOPBACK=1` (bind UDP to 127.0.0.1, skip STUN), the scripted input `N64RUN_TEST_INPUT`, `N64RUN_QA_AUDIO` and
`N64RUN_QA_STALL` are honoured only with `RETROHUB_QA=1`, so a stray variable cannot change real play. Online tests refuse the production
server unless `RETROHUB_ALLOW_PROD_TESTS=1`: `hubnet_test`, `hubfriends_test`, `qa/_guard.ps1` and the `RETROHUB_AUTOTEST` harness all check
the URL (`hubnet::is_production_server`), and the Session helpers exit with code 2 if `RETROHUB_TEST_SERVER` is unset.

**Network simulator (QA):** opt-in: nothing below has any effect unless `HUBNET_SIM_ENABLE=1` is also set (so a stray variable cannot
degrade real play). Set in the environment of any process that uses `hubnet` (applies to every outgoing datagram, direct UDP and relay
frames; relay frames stay in order): `HUBNET_SIM_LATENCY_MS` (added delay per send), `HUBNET_SIM_JITTER_MS` (extra uniform 0..J ms),
`HUBNET_SIM_LOSS_PCT` (random drop, 0..100), optional `HUBNET_SIM_SEED` (reproducible). Each PC applies it to what it sends, so RTT is about
2 x latency + jitter. Example:
`HUBNET_SIM_ENABLE=1 HUBNET_SIM_LATENCY_MS=80 HUBNET_SIM_JITTER_MS=20 HUBNET_SIM_LOSS_PCT=5 hubnet_test <server> 3 300`. `hubnet_test`
relaxes its delivery threshold to 100 - 2*loss - 4 % when loss is set.

**Exit codes (`n64run`, and the same for `retrorun --net`):**

| Code | Meaning |
|------|---------|
| 0 | Clean exit |
| 2 | Bad arguments (including `--slot/--players/--ports` inconsistencies) or invalid ROM |
| 3 | Emulator / core / plugin load failure (retrorun: only a real load failure now; "could not reach the lobby/peers" is 12) |
| 4 | Netplay connection failure, input-delay negotiation failure, or connection lost mid-game |
| 5 | Desync detected (`n64run`: with `--strict-sync`, or when a resync could not repair it: OSD message, match ends 4 s later; `retrorun`: always) |
| 12 | `retrorun --net` could not connect (room full, host never opened it, server busy, timeout); the plain-words reason is written to `$RETRORUN_REASON_FILE` and the hub shows it |
| 6 | Host-save sync failed (`--share-host-save`) |

**Saves in online sessions:** each session starts from a fresh, empty save directory on every PC. With `--share-host-save` (opt-in, every PC
must pass it) the host (slot 0) reads `--host-save-dir` and sends its battery saves (`.eep .sra .fla .mpk`, at most 8 files, 128 KiB each,
256 KiB total) to the other PCs before the ROM opens (exit code 6 if the sync fails). Transfer uses `src/n64run/save_sync.{h,cpp}`: chunked,
per-chunk check, whole-file FNV-1a, client ACK bitmaps, host resends. Without the flag the sessions start empty, as before.

**Lockstep in `GetKeys(Control, Keys)`:**

- Sample index `n` advances on each `GetKeys(0, …)` call.
- For each port this PC owns, it reads the local pad and schedules that input for sample `n + D`.
- It sends `{sample, port, buttons}` with redundancy: the last 8 samples in every packet.
- It blocks until inputs for `n` are known for every port. While waiting it keeps pumping `Session::update()` and SDL events.
- A 10 s timeout ends the game with "Connection lost".

**Desync check:** every 600 samples, each PC sends a hash of RDRAM (via `M64CMD_*` memory access, or the debugger API if needed). A mismatch is logged and shown on screen.

## Nintendo DS (system `nds`)

- **Library (`src/common/library.cpp`):** `.nds` files, also inside a `.zip` (extracted once to `userdata\rom-cache\`, entries up to 1 GB
  because DS images reach 512 MB). Header read: title 0x00 (12), game code 0x0C (the serial), unit code 0x12, ROM version 0x1E,
  Nintendo-logo CRC 0x15C (must be 0xCF56), header CRC16 0x15E. Key `nds:<code>-<version hex 2><header CRC16 hex 4>` (e.g.
  `nds:adae-05ca37`): no whole-file hash, so scanning stays fast and trimmed/padded dumps of one release share saves.
- **Catalog:** nine Pokemon DS games (Diamond, Pearl, Platinum, HeartGold, SoulSilver, Black, White, Black 2, White 2), `max_players` 4, `online` true (local wireless, see the DS row in Netplay modes), genre RPG, all region codes of each game.
- **Launch (`play_entry`, `System::NDS`):** `retrorun.exe --core melondsds_libretro.dll --system nds --save-dir <vault battery> --state-dir <vault states>`, no `--resume`. Saves are `<rom stem>.srm` in the vault battery folder and sync like every other game; states are never synced (and DS states are not written at all).
- **Online:** catalog games only, through local wireless (`supports_online()` is true for `nds` catalog entries; `retrorun --ds-link`). Not lockstep or rollback: the core's in-memory unserialize is not frame-exact and states are 19 MB.
- **Hub UI:** "Nintendo DS" shelf, filter chip, `NINTENDO DS` badge, covers from libretro-thumbnails `Nintendo_-_Nintendo_DS` (No-Intro DAT `Nintendo - Nintendo DS`, matched by game code). Details: `docs/RETRORUN.md`, `docs/CORES.md`.

## Netplay modes

Every mode is opt-in per game. Each one has a headless QA path under `qa/` and a console test where the logic is pure.

| Mode | Runner / code | Transport | Notes |
|---|---|---|---|
| N64 lockstep | `n64run` (`lockstep.cpp`, `lockstep_core.cpp`) | hubnet UDP, or WebSocket relay (`HUBNET_FORCE_RELAY=1`) | Input-only lockstep with auto input delay; desync detection with `--strict-sync`. Section above. |
| N64 host-save sharing | `n64run` (`save_sync*.cpp`, `mempak.cpp`) | same session | `--share-host-save`; Controller Pak policy and `.mpk` naming. |
| Smash Remix VS match | `n64run` (`match.cpp`) | same session | `--match` jumps straight into a VS battle from a spec string. |
| Retro lockstep | `retrorun` (`--net`, reuses `n64run` lockstep glue) | same as above | GBA/GB/GBC netplay through the lobby. |
| GB/GBC link cable | `retrorun` (`--link`, `link_sync.cpp`) | hubnet session | Exchanges save data over the link; console test with simulated loss and reorder. |
| DS local wireless | `retrorun` (`ds_link.h`, `dsview.h`) | hubnet session (`HUBNET_FORCE_RELAY` for relay) | Packet framing in `ds_link.h`; `qa/ds_link.ps1`. |
| 3DS rooms | `netplay_3ds.cpp`, `netplay_3ds_run.cpp` (Azahar) | Azahar's own ENet room server (`azahar-room.exe`), joined with a RetroHub room code | Azahar multiplayer config is generated per room; pure logic is tested in `test_netplay_3ds.cpp`. |
| GameCube Dolphin netplay | `netplay_dolphin.cpp` (Dolphin, `gcnp_live.cpp` manual driver) | Dolphin's own NetPlay, through a relay forwarder for the live run | Smoke-tested live (Double Dash, 100 s through the relay, no desync). See `docs/DOLPHIN.md`. |
| Smash Classic (BattleShip) | native port, own rollback netplay | its own Worker | Not run by RetroHub's transport; launched as a native game. |

## Games catalog (`src/common/games.cpp`)

Entries per system, with engine: n64 29 (Mupen64Plus 28, Native 1); gba 27 (Libretro); gb 1 and gbc 4 (Libretro); nds 9 (Libretro); gc 7 (Dolphin); 3ds 8 (Azahar); wiiu 1 (Cemu); ps1 16 (Libretro); ps2 12 (Pcsx2). The table below lists the N64 entries only.

| id | Title | Engine | USA ROM MD5 | Save |
|----|-------|--------|-------------|------|
| `smash64` | Super Smash Bros. (Smash Classic) | native BattleShip | (BattleShip validates) | — |
| `mp1` | Mario Party | mupen64plus | 8BC2712139FBF0C56C8EA835802C52DC | EEPROM 4K |
| `mp2` | Mario Party 2 | mupen64plus | 04840612A35ECE222AFDB2DFBF926409 | EEPROM 4K |
| `mp3` | Mario Party 3 | mupen64plus | 76A8BBC81BC2060EC99C9645867237CC | EEPROM 16K |
| `remix` | Smash Remix 2.0 (fan ROM hack) | mupen64plus | 43571020FA5DDB138BBD7F64A8C9C0D4 | — |

### 3DS and Wii U

`3ds` (Azahar, `Engine::Azahar`) and `wiiu` (Cemu, `Engine::Cemu`) entries live in the same catalog (8 Pokemon 3DS games, Twilight Princess HD).
They run as external emulators started by `launch_external` in `src/hub/launch.cpp`; everything else is in `src/hub/emulators.cpp`, with the header
parsers in `src/common/emu_formats.cpp` and the save mapping in `src/common/emu_saves.cpp`. See `docs/EMULATORS.md`.

ROM files can be `.z64` (big-endian), `.v64` (byteswapped) or `.n64` (little-endian). Normalize to `.z64` byte order before hashing. Identification uses the MD5 of the normalized image, with a fallback to the header CRC1/CRC2 at offsets 0x10/0x14.


**Socket blips (Plan 4 W2-D):** a game-room WebSocket loss after `ready` does not end the match. `Session` reconnects (`net/reconnect.h`, 25
s window) while UDP keeps playing, and keeps a peer that vanished from the roster as long as its direct UDP is fresh. The `/hub` Worker
holds slot 0 for `HOST_GRACE_MS` (10 s) after an ABNORMAL close (1006/1011) of the host (game room: nobody is closed; lobby: no host
migration) so the same host (same name) reclaims it; a clean close still migrates/closes at once.

### PSP (system `psp`, PPSSPP)
External emulator like PCSX2: `System::PSP`, `hubemu::Kind::Ppsspp` (pinned 1.20.4 zip from ppsspp.org, portable `memstick` under `<data>\emulators\ppsspp`), identification in `src/common/psp.{h,cpp}`
(PARAM.SFO of ISO / CSO / CHD / PBP, run BEFORE the PS1 / PS2 / GC claims; a PS1 eboot `.pbp` stays PS1), config writers in `src/hub/ppsspp.{h,cpp}`, SAVEDATA <-> vault mapping in `emulators.cpp`
(`SaveSession::psp_disc_id`). Offline only. Full description: `docs/EMULATORS.md` ("PlayStation Portable"). Tests: `hub_psp_test`, `qa/psp_play.ps1` (hidden desktop), `qa/psp_shots.ps1`.

## Switch runners (`nx_runner.*`, `lichii.*`, `hookshot*.*`)
Three programs can play a Switch game. The per-game setting `switch.runner` picks one (`lichii` | `fritchii` | `hookshot`; anything else, including the old `auto`, reads as Lichii).
- **Lichii** is the default. RetroHub installs, upgrades and configures it (`lichii.cpp`, `emulators.cpp`).
- **Fritchii** is opt-in. It is Frizti's Ocarina of Time fork of Lichii, built from source (a local build for v5.7, GitHub Actions before) in `fatty8me2/Fritchii` and pinned by size and hash in `emulators.cpp`.
  - It runs through all of Lichii's code, but has its own install folder (`<data>\emulators\fritchii`) and its own `user\` folder.
  - Nothing is linked between the two folders. Anything shared is copied, and anything that could replace data is backed up first.
  - Offline only: an online or room launch always gets Lichii (`nx_emulator_for`).
- **Hookshot** is opt-in. It is a native Windows build of Ocarina of Time (Switch 2), made on the player's PC from their own copy of the game by static recompilation (`hookshot_build.*`). No emulator. `launch.cpp` checks it before the two emulators (`hookshot::use_for`). Offline only. Full notes: `docs/HOOKSHOT.md`.
- Tests: `hub_nx_runner_test`, `hub_hookshot_build_test`.

## All Games layouts (`libview_ui.cpp`, `libview_logic.h`)
- **Six layouts** (`LibLayout` in `libview_logic.h`), switched by the icon cells next to the Sort chip (saved: `Settings::lib_layout` grid | list | compact | showcase | shelf | cases; the default is Cases):
  - Grid (classic).
  - List (dense sortable rows: thumbnail, title, console pill, players, last played, play time, online + favourite).
  - Compact (small covers, title on focus/hover).
  - Showcase (cover-flow carousel over a soft art backdrop, wheel / left-right to scroll).
  - Shelf (a wooden bookcase of cartridges, discs and cases; one shelf per console, or one flow that follows the Sort chip with `lib_shelf_mixed`; `shelf_logic.*`, `shelf_draw.*`).
  - Cases (real-looking cases and boxes; `cases_*.{cpp,h}`). It has two styles: Coverflow (the default carousel) and Classic (the shelf of turned cases), chosen by `Settings::lib_cases_classic`.
  - Search, chips, Sort, Genre and Group-by feed all of them (`rebuild_vis` -> `A.vis` / `A.secs`).
- **List headers** are nav items and sortable (click / A): `lib_list_col` + `lib_list_desc`; unset means the list follows the Sort chip and keeps its console/genre sections. Using the Sort chip (or RT elsewhere) clears the column.
- **Virtual rows**: every layout is laid out from numbers (`build_vlayout`); only rows within one page of the viewport submit items, and rows outside the viewport are bare nav stubs with the same ids as real tiles. Cover art is requested only for tiles really on screen (2000+ games stay cheap).
- **Fast scrolling**: A-Z strip (or console/genre labels when the order is not by title) at the right edge: click/drag, bubble shows the
  letter. Pad: right stick up/down = previous/next letter or group (keyboard: Ctrl+Up/Down). LT/RT page (`A.lib_pg`: main.cpp does not send
  PageUp/PageDown there, ImGui's own page move would land on the selected bar; All Games RT therefore no longer cycles Sort, use the chip).
- **Screenshot aids**: `HUB_SHOT_LIBVIEW=grid|list|compact|showcase|shelf|cases` (+ `HUB_SHOT_LIBCOL=last:desc`), `HUB_SHOT_FAKE_AZ=1` with `HUB_SHOT_FAKE_LIB=500` (titles over A-Z, favourites, play times), `HUB_SHOT_FAKE_ART=1` (procedural covers). `qa/libview_shots.ps1`; `HUB_SHOT_TRACE` logs `libview`, `libview-jump`, `libview-sort`, `libview-layout`. Test: `hub_libview_test`.

## Cover art and descriptions (art.cpp, gametdb.*, gameinfo.*, gameinfo_ui.cpp)
- **Art** (`hubart`): `Boxart / Title / Snap` come from libretro-thumbnails; a Boxart libretro lacks falls back to GameTDB's front cover
  (GameCube, Wii U, DS, 3DS) and, for a Switch `.nup`, the package's own icon (meta TLV 10 English / TLV 3, `nx::nup_icon_file`; a Switch
  title that matches GameTDB gets its real box first). `Kind::FullCover` is GameTDB's `coverfullHQ` (BACK|SPINE|FRONT) for GameCube / Wii U
  / DS / 3DS only. GameTDB pseudo names are `gametdb:<ID>:<region folder|*>`; ids come from the serial (`gm4e01`, `irbo`, `ECLA`) or, for
  Wii U (title ids carry no product code), from the GameTDB index by title. Region folders: the id's own region letter, a `(USA)` style
  hint, then US, EN, JA (ids with region letter A/O look in 8 folders). Cache: `art\<sys>\<kind>\`; JPEG bytes are stored under the same
  `.png` name (the decoder sniffs content). One request at a time per host, GameTDB >= 0.5 s apart, 429 / Retry-After honoured, misses
  remembered.
- **GameTDB database** (`gametdb.*`): the official `*tdb.zip` downloads, parsed once into `art\tdb\tdb_<db>.tsv` (refreshed after 60 days). Used for Wii U/Switch ids and as the description fallback (`gameinfo::resolve_description`).
- **Descriptions**: `gameinfo_prefetch(e, urgent)` queues a lookup (`gameinfo_queue.h`: urgent first, deduplicated); once the library loads, every game trickles through the one worker (stops offline for 5 min or when "Download cover art" is off). `gameinfo_generation()` ticks when a result lands. No description anywhere -> `gameinfo::generated_description` (facts only, `CaseInfo.desc_generated`).
- **Coverage tool**: `hub_metadata_prefetch <userdata copy> [--limit N] [--only sys] [--no-snaps] [--tdb]` runs the same code headlessly against a library_cache.json and prints a per-game table. Live network, so not a ctest; always point it at a copy.
