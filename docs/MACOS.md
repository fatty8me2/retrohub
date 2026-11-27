# macOS port

Status: in progress on branch `mac-port`. Target: Apple Silicon (arm64) only, macOS 14+, full console parity with Windows
(Switch / Switch 2 included). Mac and Windows do not need to interoperate yet (no cross-platform rooms or shared vault).

## Build (macOS)

```
brew install cmake ninja
cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac
ctest --test-dir build-mac -LE "online|gui" --output-on-failure
```

Windows is still built and released exactly as in CLAUDE.md. Every change on `mac-port` must keep the Windows build and its
tests green: push the branch and check the Windows CI run (`.github/workflows/ci.yml`) before merging into `main`.

## Rules for ported code

- `src/common/plat/plat.h`: `RH_WIN` / `RH_MAC`, `plat::kSep`, `plat::join`, monotonic clock, `sleep_ms`, `lib_open` / `lib_sym`.
- `plat.h` file helpers: `plat::fopen_path(std::filesystem::path, "rb")` (replaces `_wfopen(p.wstring().c_str(), L"rb")`),
  `plat::fseek64` / `plat::ftell64` (replace `_fseeki64` / `_ftelli64`), `plat::local_tm` / `plat::utc_tm` (localtime_s / gmtime_s).
- `src/common/plat/plat_crypto.h` (in hubbase): `plat::Hasher` / `plat::hash` / `plat::hmac` (MD5, SHA-1, SHA-256, SHA-512),
  `plat::random_bytes`, `plat::hex`. BCrypt on Windows, CommonCrypto on macOS. Replacing a BCrypt hash sequence with it is fine.
- `src/common/plat/plat_http.h` (macOS only, NSURLSession): `plat::http::perform` (blocking request, optional streaming /
  progress) and `plat::ws::Socket`. Every WinHTTP user keeps its Windows code under `#if RH_WIN` and calls this on macOS.
  Never hand-roll TLS.
- `src/common/plat/plat_sock.h`: include it instead of `<winsock2.h>`; `plat::socket_t`, `kBadSocket`, `sock_init`, `sock_close`,
  `sock_error`, `sock_would_block`, `sock_set_nonblocking`, `udp_ignore_connreset`, `sock_setopt`, `sock_poll`.
- Windows behaviour stays byte-identical: same API calls, same paths, same file names. Put Windows-only code under `#if RH_WIN`
  and give macOS a real implementation (or an explicit "not supported on this platform" result, never a fake success).
- Paths are UTF-8 `std::string` (or `std::filesystem::path`) on macOS. No hard-coded `'\\'` in new code; use `plat::kSep` /
  `plat::join` / `std::filesystem`.
- Windows system libraries named in `target_link_libraries` are empty targets off Windows (top-level CMakeLists.txt). Link the
  macOS frameworks a module needs under `if(APPLE)`.
- Data folder on macOS: `~/Library/Application Support/RetroHub` (Windows keeps its current location).

## Lanes

| # | Area | State |
|---|---|---|
| 0 | CMake for Apple arm64, `plat.h`, `plat_sock.h`, this doc | done |
| 1 | `src/common` hubbase (winutil, durable_write, u8file, cfg_write, hint_popup, osd, runner_ctl, start_hint, net_probe) | done |
| 2 | `src/net` on `plat_sock.h` | |
| 3 | rest of `src/common` (hubcommon: bcrypt -> CommonCrypto, kanto, ...) | |
| 4 | `src/saves` (winhttp -> NSURLSession or libcurl, bcrypt) | |
| 5 | `src/retrorun`, `src/n64run`, `src/cheevos`, cores as `.dylib` | |
| 6 | `src/hub`: process spawn (posix_spawn + process groups), named pipes -> Unix sockets, data folders, toasts, window focus | |
| 7 | External emulators: macOS pins + config paths for Dolphin, PCSX2, PPSSPP, Azahar, Cemu, Lichii (official macOS arm64 build), Fritchii (build from source), Hookshot (check) | |
| 8 | `src/stream` (Media Foundation -> VideoToolbox) | |
| 9 | Packaging: `.app`, codesign + notarization, updater, release script | |

## N64 on macOS

`n64run` (src/n64run) builds and its tests pass on macOS arm64 (`ctest -R n64run -LE "online|gui"`). What changed and what is still missing:

**Code.** Windows paths are unchanged (`#if RH_WIN`). Shared helpers added to `src/common/plat/plat.h` (same style as the rest):
`plat::tick_ms` (GetTickCount64), `plat::perf_counter` / `perf_freq` (QueryPerformanceCounter), `plat::lib_self` (GetModuleHandle(NULL) /
`dlopen(NULL)`), `plat::set_env`, `plat::strdup_c`. The core and plugins load with `plat::lib_open` / `lib_sym`
(`libmupen64plus.dylib`, `mupen64plus-<kind>-<name>.dylib`, in the emu dir `bin/emu` next to the exe; same layout as Windows' `emu\`).
The input plugin is still the executable itself: `PLUGIN_EXPORT` is `visibility("default")` and CMake sets `ENABLE_EXPORTS` so the
core's `dlsym(dlopen(NULL), "GetKeys")` finds it (verified with `nm -gU bin/n64run`). Not ported on macOS yet: the game-window caption
watcher and the start-tips popup (Win32 `EnumWindows`; the OSD drawn by `osd_gl.cpp` still shows corner text and toasts), and the
`--net-probe` firewall prompt is the generic one in `net_probe.h`.

**OpenGL.** `osd_gl.cpp` and the clip read-back use `<OpenGL/gl3.h>` with `GL_SILENCE_DEPRECATION`: macOS ships deprecated OpenGL up
to 4.1 core and nothing newer. The OSD is pure core profile (`#version 330 core`, VAO, no fixed function), so it works on the SDL GL
context the plugin creates. Anything that needs a compatibility profile above GL 2.1 cannot work on macOS: the headless video
extension (`headless.cpp`) maps a plugin's "compatibility profile" request to a core profile on macOS. Plugins must request a core
context (3.3 or 4.1) with forward-compat, which SDL2 does on macOS when the profile mask is CORE.

**What Windows uses** (`scripts/fetch-emu.ps1`, see THIRD_PARTY.md): the official `mupen64plus-bundle-win64-2.6.0.zip` from
github.com/mupen64plus/mupen64plus-core releases (core `mupen64plus.dll`, `mupen64plus-rsp-hle.dll`, `mupen64plus-audio-sdl.dll`,
plus SDL2 / libpng / zlib / FreeType / SDL2_net DLLs, `mupen64plus.ini`, `mupencheat.txt`, `font.ttf`) and the GLideN64 CI build
`GLideN64-a04081a-Windows-Mupen64Plus-CLI-x64.zip` (github.com/gonetz/GLideN64, `mupen64plus-video-GLideN64.dll`, `GLideN64.custom.ini`).
`n64run` forces `R4300Emulator=2` (dynarec), plugin names `video-GLideN64`, `audio-sdl`, `rsp-hle` and its own input plugin.

**Core on macOS.** There is no official macOS bundle for 2.6.0. The fetched 2.6.0 source builds cleanly with CMake: target
`mupen64plus_core` (top-level CMakeLists.txt, `if(APPLE)`) produces `build/bin/emu/libmupen64plus.dylib` and copies `mupen64plus.ini`,
`mupencheat.txt`, `font.ttf` beside it. It needs libpng for screenshots (`brew install libpng`; the target is skipped when missing; for
a release, link libpng statically or take it from a pinned source, since the Homebrew dylib is not redistributable as is) and shares the
SDL2 dylib with `n64run` (`@loader_path/..`). Built without the core OSD (needs OGLFT + FreeType; RetroHub draws its own) and without
netplay (RetroHub has its own lockstep). Checked: it loads, `CoreGetAPIVersions` returns 0x20302 / 0x20001 / 0x30300, and
`n64run --headless` with the dylib runs through ROM load and stops at the missing video plugin (expected).

**Dynarec.** 2.6.0 has an arm64 new_dynarec (`src/device/r4300/new_dynarec/arm64`, `NEW_DYNAREC=4`), but its upstream Makefile sets
`NO_ASM=1` on Apple Silicon ("assembly compilation not yet supported on macOS with Apple Silicon"), and the code maps its code buffer
`PROT_READ|PROT_WRITE|PROT_EXEC` with `mprotect`, which macOS arm64 refuses without `MAP_JIT` + `pthread_jit_write_protect_np` + the
`com.apple.security.cs.allow-jit` entitlement. So the dylib above has no dynarec: `R4300Emulator=2` silently falls back to the **cached
interpreter** (`#if defined(DYNAREC)` is off; value 1). Expect roughly a third to a half of dynarec speed: fine for most 2D / light 3D
games on an M-series chip, borderline for the heaviest (Conker, Perfect Dark, Smash Remix with big mods) in full-speed netplay.
Getting a dynarec means patching the core: allocate the buffer with `MAP_JIT`, toggle write/execute per translation block, sign `RetroHub`
with the allow-jit entitlement, and drop `NO_ASM` for the `.S` linkage (`linkage_arm64.S` needs the leading-underscore symbol
convention). Not attempted; uncertain whether the 2.6.0 arm64 dynarec is stable enough. Lockstep needs both peers to run the same CPU
mode only if they cross-play with Windows, which is not a goal now (a mixed dynarec / interpreter pair could desync).

**Plugins on macOS arm64 (nothing is downloaded or built yet; sources are from memory, mark as uncertain until tried).**
- *Video.* GLideN64 (github.com/gonetz/GLideN64) is the plugin RetroHub's settings, determinism list (`det_gfx.h`) and
  `Video-GLideN64` config keys assume. It has an upstream CMake build for the Mupen64Plus target and macOS support exists in
  `src/GLideNUI` / `osal` (uncertain how current; Apple Silicon needs a test), and it needs GL 3.3+ core, which macOS provides (4.1).
  Output name must be `mupen64plus-video-GLideN64.dylib`. Fallbacks, in order: `mupen64plus-video-rice` (simplest, built by the stock
  mupen64plus-video-rice Makefile with `OS=OSX`; GL 2.1 fixed function, which macOS still has, but a different config section and
  looks/timing differ from GLideN64); `mupen64plus-video-glide64mk2` (3.3 core, OSX supported). If the plugin changes, `frontend.cpp`
  (the `Video-GLideN64` dither key and the plugin name) and `det_gfx.h` need a per-plugin switch.
- *RSP.* `mupen64plus-rsp-hle` (mupen64plus/mupen64plus-rsp-hle, pure C, `make OS=OSX`, no GL): builds on arm64 as is (the
  `mupen64plus-rsp-cxd4` alternative has SSE paths, avoid). Output `mupen64plus-rsp-hle.dylib`.
- *Audio.* `mupen64plus-audio-sdl` (the audio-sdl plugin is SDL2 based, despite the name; pure C, `make OS=OSX`, needs `sdl2-config`
  or the shared SDL2 target): builds on arm64. Output `mupen64plus-audio-sdl.dylib`. `audio-sdl2` is not a separate upstream plugin.
- *Input.* None needed: `n64run` itself.
All plugins must link the same SDL2 dylib as `n64run` (one copy beside the exe) and be ad-hoc / Developer-ID signed together with the
`.app` (lane 9). A `scripts/fetch-emu-mac.sh` that builds these three plugins from pinned tags (like the Windows `fetch-emu.ps1`) is the
next step.

**To run an N64 game on macOS now:** (1) build `n64run` and `mupen64plus_core`; (2) put `mupen64plus-rsp-hle.dylib`,
`mupen64plus-audio-sdl.dylib` and `mupen64plus-video-GLideN64.dylib` (built from source, see above) into `build/bin/emu/`; (3) run
`n64run --rom game.z64 --game id --emu-dir build/bin/emu`; the hub (lane 6) still has to launch it and pass the macOS paths. Expected
first-run risks: GLideN64's GL context request on macOS, the interpreter's speed, and window-title / tips features that are not ported.
