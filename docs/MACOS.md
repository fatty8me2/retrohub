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
| 1 | `src/common` hubbase (winutil, durable_write, u8file, cfg_write, hint_popup, osd, runner_ctl, start_hint, net_probe) | |
| 2 | `src/net` on `plat_sock.h` | |
| 3 | rest of `src/common` (hubcommon: bcrypt -> CommonCrypto, kanto, ...) | |
| 4 | `src/saves` (winhttp -> NSURLSession or libcurl, bcrypt) | |
| 5 | `src/retrorun`, `src/n64run`, `src/cheevos`, cores as `.dylib` | |
| 6 | `src/hub`: process spawn (posix_spawn + process groups), named pipes -> Unix sockets, data folders, toasts, window focus | |
| 7 | External emulators: macOS pins + config paths for Dolphin, PCSX2, PPSSPP, Azahar, Cemu, Lichii (official macOS arm64 build), Fritchii (build from source), Hookshot (check) | |
| 8 | `src/stream` (Media Foundation -> VideoToolbox) | |
| 9 | Packaging: `.app`, codesign + notarization, updater, release script | |
