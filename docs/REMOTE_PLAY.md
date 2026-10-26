# Remote play (Wii U "couch co-op" over RetroHub rooms)

Cemu has no netplay. The room host runs Cemu; guests receive a low-latency video + audio stream and send their
controller back. `streamrun.exe` (src/stream) does both ends. Wire format and the localhost pad API live in
`src/stream/stream_proto.h` (the contract with the `wiiu-input` DSU/cemuhook server).

## Cost rule
Video/audio/input go ONLY over direct UDP (the existing hubnet STUN + hole punch). The Cloudflare Worker is used for
signalling (the lobby WebSocket) only. `streamrun` calls `Session::set_no_relay(true)`, so a failed punch never
falls back to the WebSocket relay; it exits with code 3 and:

> Couldn't connect directly to the host - remote play needs a direct connection (try another network / enable UPnP)

`--upnp` tries a UPnP IGD port mapping (SSDP + SOAP AddPortMapping) for the Session's UDP port first.

## CLI
```
streamrun --host --capture-hwnd <hwnd|pid> | --test-pattern  --net <wss://server> --room <CODE-N> --slot 0 [--guests 1]
          [--dsu-port 26760] [--no-dsu] [--status-file F] [--bitrate 8000] [--fps 60] [--max-height 720] [--no-audio] [--upnp]
streamrun --guest --net <wss://server> --room <CODE-N> --slot K [--fullscreen] [--overlay] [--headless]
          [--test-input] [--test-menu] [--duration SECONDS] [--stats-file F]
streamrun --probe (--host|--guest) --net <wss://server> --room <CODE-N> --slot K --pcs N [--probe-ms 9000] [--stats-file F]
```
`--capture-hwnd` takes a window handle (decimal/0x hex), or a PID (then the largest visible top-level window of that
process is captured; its audio is captured with WASAPI process loopback). Exit codes: 0 ok, 2 usage, 3 no direct
connection, 4 session failed/ended, 5 capture/encode/decode init failed.

## Guest pads -> Cemu (DSU)
`streamrun --host` embeds the cemuhook/DSU server (`dsu_server.*`, 127.0.0.1:`--dsu-port`, default 26760). A guest in room slot K (1..3) is DSU
slot K-1 = Cemu player K+1 (a Pro Controller bound to `DSUController` slot K-1). The hub writes those Cemu profiles ONLY for a remote-play launch
(`RunInfo::dsu_players` -> `cemu_pads_with_dsu`, managed Cemu and the user's own Cemu with managed controls); every other launch keeps XInput/SDL
profiles. Button mapping is in `pad_convert.h` (the guest's Nintendo east button is the DSU "B" bit, which the default RetroHub profile maps to Wii U A).
`--pad-port P` still sends the old localhost RPAD/RGON datagrams, but it is off by default (the DSU server was the only consumer).
Tools: `dsu_feed` (scripted pads), `dsu_probe` (what a cemuhook client sees: connected + button bytes), `dsu_test`.

## Leaving cleanly
- The hub never `TerminateProcess`es a streamrun while it can ask: every streamrun creates the named event `Local\RetroHubStreamrunQuit_<pid>`; the hub sets it
  (`quit_gracefully`, 4 s grace, then kills its own child). Works for windowless viewers and the host alike.
- A leaving guest sends a `B` (bye) datagram, so the host drops that pad at once (DSU slot goes "disconnected", one final disconnected data packet) instead
  of after the 4 s silence timeout; the host keeps streaming and Cemu keeps running. The host ignores `Session::ended()` for exactly this reason.
- The host quitting (Cemu closed, hub quit event) ends every guest's session; a guest viewer exits 0 and the hub returns to the room ("The stream ended").

## Direct-connection pre-check
Before the host's Launch for a remote-play game the hub runs `streamrun --probe` on the host and, via the lobby message `rplay_probe`, on every guest. They join a
throwaway session room (`CODE-1xx`), the relay is disabled, and a guest counts as reachable when a direct UDP ping answered (`{"ok":..,"slots":{"1":rtt|-1}}`).
If anyone fails, Launch stays blocked ~25 s with "Can't reach Mickey directly ... try enabling UPnP ..." (`hubrp::probe_block_reason`); a pass is cached for 90 s
for the same room membership, and the real launch follows automatically.

## Guest menu (View + Y, F1 on the keyboard)
Resume / Stats (overlay) / Fullscreen / Leave, driven by the D-pad + A (B closes) while the menu owns the pad (the stream sees neutral input). `GuestMenu` /
`MenuPadEdge` in stream_core.h are unit tested; `--test-menu` scripts the presses for QA.

## Host status line
`streamrun --host --status-file F` writes `{"kbps":..,"guests":[{"slot":1,"rtt_ms":48,...}],"dsu_clients":N}` once a second; the hub shows
"streaming to Mickey · 12 Mbps · 48 ms" in the room banner and at the top of the in-game quick menu.

## End-to-end test (two hubs, hidden desktop)
`qa/wiiu_remote_room.ps1 -Scenario leave|hostquit` (run through `node qa/online_local.mjs <port>`): host hub with a scratch managed Cemu + the archived Super Mario 3D World
(unpacked by the hub), guest hub with no game and no Cemu, local worker for signalling. Covers room -> Launch -> probe -> Cemu start (DSU server already
listening) -> streamrun attaches -> guest streams (scripted pad) -> Cemu attaches to the DSU server as a client -> guest View+Y -> Leave (host drops the pad at once,
Cemu keeps running) -> host closes Cemu (quit event) / or host quits while the guest streams and the guest hub is back in the room. `qa/rplay_dsu_loopback.ps1` is the
console-only version (probe, DSU round trip via `dsu_probe`, menu, bye). The video in these runs is the TEST PATTERN: WGC/DXGI cannot capture Cemu on a hidden
desktop. One real run on the interactive desktop (host with Cemu visible, a second PC or this one as guest) is still needed to see Cemu's picture on the guest.
Cemu's DSU client does not retry after a failed first connect, which is why `streamrun --host` is started before Cemu (`--await-pid-file`; the hub writes Cemu's PID once its window exists).

## Pipeline
Host: Windows.Graphics.Capture (window; falls back to DXGI desktop duplication cropped to the window rect; test
pattern for CI) -> BGRA->NV12 -> Media Foundation H.264 MFT (hardware NVENC/AMF/QSV first, Microsoft software encoder
fallback; CBR, no B-frames, low-latency mode, 2 s GOP, keyframe on request) -> fragments (1000 B) + XOR parity per
group -> hubnet Session datagrams. Audio: WASAPI process loopback of the Cemu PID -> AAC-LC (ADTS) via MF (PCM
fallback) -> datagrams.

Guest: reassembler (parity recovery, NACK for what parity could not fix, keyframe request when a frame is lost for
good) -> MF H.264 decoder (D3D11/DXVA when available) -> SDL window (vsync off, fullscreen option, overlay toggled with
F3 shows fps / latency / bitrate / loss). Audio: jitter buffer (adaptive 20-60 ms) -> SDL audio.
Input: SDL GameController state sampled and sent at 250 Hz (state packets, idempotent, so loss costs nothing).

## Adaptive bitrate
Guest sends a stats datagram once per second (loss permille, jitter, rtt). Host controller: loss > 2% or rising
jitter -> multiply by 0.8 (min 3 Mbps); 5 clean seconds -> +10% (max 15 Mbps). Applied live through
`CODECAPI_AVEncCommonMeanBitRate`. FEC group size follows loss (every 4th fragment at >1% loss, else every 8th).

## Tests
`stream_core_test` (packetizer, FEC, reassembler, NACK, jitter buffer, input codec, bitrate controller) runs under
`ctest -LE online`. `qa/stream_loopback.ps1` runs host + guest on the hidden desktop through a LOCAL wrangler worker
(test pattern source, headless guest) and prints capture->present latency percentiles.

## Status and measured numbers (this PC: RTX 3090 Ti, other agents keeping the CPU ~85% busy during the runs)
Loopback, 1280x720 test pattern, NVIDIA NVENC through MF, local wrangler for signalling only, direct UDP:
- Software H.264 decode on the guest (default): capture->decoded avg 57 ms, p50 58, p95 84, p99 88 (headless); 38 fps (host thread starved by the
  machine load; unloaded selftest runs 60 fps with encode ~4.5 ms + decode ~8 ms = ~16 ms capture->decoded).
- Windowed SDL guest on a hidden desktop (latency measured after SDL_RenderPresent): avg 50 ms, p50 52, p95 72, p99 79.
- D3D11/DXVA hardware decode (`--hw-decode`) is slower here (~40 ms/frame under load, GPU read-back through `Lock2D`), so software decode is the default;
  720p software decode is ~5-8 ms. A zero-copy texture path to SDL is future work.
- Bitrate adapts 3-15 Mbps (ran ~9.7 Mbps on the busy test pattern), 0 NACKs and 0 lost frames on loopback; FEC/NACK recovery is covered by unit tests
  (2% random loss soak: 300/300 frames).
- Audio: tone -> AAC-LC -> 48 kHz stereo over the same session -> jitter buffer -> decode, ~21 ms per packet, 3-packet target.
- Input: 250 Hz state packets; the host forwarded every one to the localhost pad API (`pad_packets` ~= `inputs_sent`).

Latency is `now - capture time` on one machine's steady clock. Across two PCs it needs the clock offset; the guest overlay then shows decode+present
time and RTT instead (not yet implemented: the protocol carries the capture timestamp so it can be added).

## Capture notes
- `--selftest-wgc` proves Windows.Graphics.Capture on a real window (57 fps) and WASAPI process-loopback activation.
- WGC and DXGI duplication do NOT work on a hidden (CreateDesktop) desktop, so the Cemu 3D World test on the hidden desktop falls back to PrintWindow,
  which returns Cemu's blank canvas (Vulkan surface is not readable that way). The pipeline, window discovery and process audio path ran, but the video
  contents were a flat grey frame; a real Cemu capture has to be done on the interactive desktop (qa/stream_cemu.ps1 does the hidden-desktop variant; the interactive-desktop run was not done because it would put a Cemu window on the user's screen).
- Cemu must not be minimized (WGC delivers no frames) and the stream is of the whole window, so launch Cemu with `-f` (fullscreen) for a clean picture.
- Intra-refresh is not exposed by the Media Foundation H.264 MFTs; recovery uses a 2 s GOP plus keyframe-on-request.
