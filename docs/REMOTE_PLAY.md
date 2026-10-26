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
