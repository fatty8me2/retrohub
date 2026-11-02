# GameCube online speed: progress (agent/hotfix-gclag)

## Built so far
- `src/hub/gc_speed.cpp` → `hub_gc_speed.exe` measures speed. It runs 1-2 Dolphin 2509 instances on a private hidden desktop with No Audio Output and drives them with a scripted DSU pad (`dsu::DsuServer`, R3 = screenshot hotkey).
  - **Speed sources:** each second it reads the game's own VI-rate counter from MEM1. It also reads Dolphin's own `User\Logs\vblank_times.txt` and `render_times.txt`, which come from `LogRenderTimeToFile`. The two sources agreed in every run.
  - **Modes:** `offline`, `offline2` (two solo instances, same CPU load as NetPlay), `np-direct` (Dolphin NetPlay over 127.0.0.1 with no forwarder), `np-fw` (the hub forwarders over a hubnet::Session against a local wrangler; `HUBNET_FORCE_RELAY` and `HUBNET_SIM_*` work here) and `np-trav` (Dolphin's traversal server).
  - **Options:** `--stock`, `--ini`/`--gfx` overrides, `--buffer N` (typed into the host's NetPlay dialog), and `--gci` (copies a save into the scratch card; the library is never written).
  - **Gameplay:** with the user's save copied in, MSB and Strikers reach their intro, title and attract-demo gameplay. Screenshots confirm it.
- `qa/gc_online_speed.ps1` runs a list of matrix rows and appends everything to a results file.
- `src/hub/test_gcfw_latency.cpp` → `hub_gcfw_latency` (ctest, label `online`) measures loopback RTT through both forwarders.
- **Fix work.** Some of this was committed in 0d312d5 and some is WIP in e1ab5cc:
  - The GC forwarder runs on its own thread (1 ms wake). `RETROHUB_GCFW_NOTHREAD=1` restores the old UI-tick pump for A/B tests.
  - The host pad buffer comes from the session ping. gc_tick writes `gc_host_buffer.txt` and the UIA driver types it into Dolphin's Buffer box before Start. It also re-applies it live during the game, raising at once and lowering only with hysteresis.
  - The driver reports Dolphin's real room buffer as `padbuffer:N`, parsed from the chat line "Buffer size changed to N".
  - New formula: `pad_buffer_for_ping = ceil(rtt/15)+1`. Unknown ping falls back to 8, or 10 when relayed.

## Evidence
- **Owner's real session** (11-02 10:13, Strikers, v1.31.2, `userdata\gcnp\gc_host_status.txt`): Dolphin ping to Mickey was **177 ms** and the chat said **"Buffer size changed to 5"**. Dolphin also reported a desync at frame 480.
- **Dolphin source:** each SI poll blocks in `NetPlayClient::GetNetPads` until the other player's pad arrives. The host's buffer is the room's buffer, and guests ignore their local `BufferSize`. NetPlay forces nothing that would explain the slowdown.
- **Forwarder RTT on loopback** (`hub_gcfw_latency`, local wrangler):

  | Pump | Direct | Relay |
  |---|---|---|
  | UI tick | 49.5 ms | 66.2 ms |
  | Thread | 3.0 ms | 4.3 ms |

## Matrix: mean emulation speed over 90 s, host and guest identical (lockstep)

### MSB (GYQE01)
- A offline: 100 %. A2 (two offline instances at once): 100 %.
- B np-direct (RetroHub config): 100 % (one earlier run showed 94 %).
- C np-direct, stock: 100 %.
- D forwarder, direct, loopback: 100 % with either pump. The old pump had 7 hitches over 50 ms.
- E relay through the local wrangler: 95 % with the old pump (546 hitches over 50 ms), 100 % threaded.
- F, simulated latency (sim 80 one-way + 10 jitter gives a session RTT of about 170 ms), threaded:

  | Buffer | Speed |
  |---|---|
  | 8 | 75 % |
  | 10 | 92 % |
  | 12 | 100 % |
  | 16 | 100 % |

  With the old pump at buffer 12 it was 76 %.
- **Before/after** (before = v1.31.2: old pump and buffer 5; after = this branch):

  | Row | Before | After |
  |---|---|---|
  | F40 | 66 % | 100 % (buffer 7) |
  | F80 | **41 %** | **100 %** (buffer 14) |
  | E | 100 % | 100 % |

- H, Dolphin traversal with both instances on one PC: the host got a code but the guest could not join (hairpin NAT). This row can't be measured on one PC.

### Strikers (G4QE01)
Before = v1.31.2: single core, old pump, buffer 5.

| Row | Before | After (dual core, threaded, auto buffer) |
|---|---|---|
| A offline | 100 % | 100 % |
| E | 99.5 % | 95 % (one 1.4 s relay outage: forwarder datagrams fell to 3/s at second 72) |
| F40 | 66 % | 100 % (buffer 7) |
| F80 | **41 %** | **100 %** (buffer 13) |

Not finished: post F80 with single core (did the CPU mode matter?).

## Root cause so far (measured)
1. The room's pad buffer was always Dolphin's default of 5, because nothing set it. With a round trip of about 170 ms, both Dolphins block on every pad poll. Measured speed was 41 %, with roughly 740 stalls over 50 ms in 90 s, which is "super slow, from launch".
2. The forwarder ran on the hub's ~60 Hz UI loop. That added about 45 ms of round trip plus bursty delivery. At the same buffer, measured speed was 76 % with the old pump vs 100 % threaded.

The CPU mode is not the cause: MSB was already dual core and was still 41 % before the fix.

## Resumed after the PC restart
- **Strikers post F80 on single core: 99.9 %.** The CPU mode is not the cause on this PC.
- **Strikers post E repeats: 100 % and 100 %.** The earlier 1.4 s relay outage did not reproduce.
- **Found and fixed: the forwarder thread's 1 ms select/Sleep ran at about 16 ms** once Windows stopped granting 1 ms timers after the restart. Loopback RTT went from 3 ms to 30 ms.
  - The forwarder now wakes on data: WSAEventSelect on the Session UDP socket and Dolphin's local sockets, plus an event set when a relay frame is queued (`Session::set_wake_event`, `WsClient::set_wake`).
  - `hub_gcfw_latency` now gives **0.3 ms direct and 4.9 ms relay** (UI-tick pump: 62.9 ms).
  - The test's echo loss came from SO_RCVTIMEO and is fixed by using select waits.
- **Final build, simulated RTT about 170 ms, auto buffer 13-14:**

  | Row | MSB | Strikers |
  |---|---|---|
  | F80 | 99.9 % | 100 % |
  | E80 (relay + latency) | 99.5 % | 99.8 % |

- `ctest -LE online`: 120/120 passed. `hub_gcfw_latency` (online) passes for direct and relay.

## Architecture recommendation
- **Keep the hub transport:** direct UDP hole punch first, with the Worker relay only as a fallback.
  - With the threaded, event-driven forwarder it adds 0.3 ms (direct) to about 5 ms (relay, local) of round trip.
  - The matrix shows no speed difference between Dolphin-direct (B/C) and the forwarder (D/E) once the buffer is right.
- **Dolphin traversal (H) is not a better path:** it could not even connect two instances on one PC (hairpin NAT). It would add a second NAT-traversal system that has no relay fallback.
- **The open real-world cost is Internet RTT itself**, which sets the buffer and therefore the input lag (about 1 frame per 15 ms of RTT).

## Next
- Not done: the old-tag rebuild (row G). The GC data path is unchanged since v1.27.6: same UI-tick forwarder, Dolphin default buffer 5. The 140-game stress run only checked liveness on title screens.
- Owner test: a real 2-PC online session. Check `online.log` for the `gc host: pad buffer N (ping X ms)` line, and `gc_host_status.txt` for `buffer:N` / `padbuffer:N`.
