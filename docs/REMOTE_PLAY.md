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
          [--pad-port 26760] [--bitrate 8000] [--fps 60] [--max-height 720] [--no-audio] [--upnp]
streamrun --guest --net <wss://server> --room <CODE-N> --slot K [--fullscreen] [--overlay] [--headless]
          [--test-input] [--duration SECONDS] [--stats-file F]
```
`--capture-hwnd` takes a window handle (decimal/0x hex), or a PID (then the largest visible top-level window of that
process is captured; its audio is captured with WASAPI process loopback). Exit codes: 0 ok, 2 usage, 3 no direct
connection, 4 session failed/ended, 5 capture/encode/decode init failed.

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
