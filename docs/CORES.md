# Libretro cores for RetroHub

This document records which libretro core the hub uses for each system, why, and what each core offers a frontend. Cores are fetched by `scripts/fetch-cores.ps1` into `deps/cores/` (DLLs) and `deps/cores/info/` (`.info` metadata).

Verification key: **[verified]** = confirmed in the core's `.info` file or upstream README (sources listed). **[unverified]** = not confirmed from a source read here; treat as a lead to check.

## Systems and recommended cores

| System | Default core | Also available | Status |
|---|---|---|---|
| GBA | **mGBA** (`mgba_libretro`) | none needed | Recommended |
| GB | **SameBoy** (`sameboy_libretro`) | Gambatte, mGBA | Recommended |
| GBC | **SameBoy** (`sameboy_libretro`) | Gambatte, mGBA | Recommended |
| N64 | mupen64plus (not a libretro core here) | none | Shipped via `n64run.exe` |
| GameCube | not decided | none fetched | Future; no core chosen |
| Nintendo DS | **melonDS DS** (`melondsds_libretro`) | melonDS, DeSmuME (not fetched) | Recommended |

### GBA: mGBA
mGBA is the accuracy-focused GBA choice and is the only GBA core fetched. It also handles GB and GBC, so it can serve as a fallback. Its README describes it as a high-accuracy, high-performance emulator and the "generally good first choice" for GBA [verified: mGBA README, https://github.com/libretro/mgba].

### GB and GBC: SameBoy (recommended), Gambatte (fallback)
Recommendation: **SameBoy** as the default for GB and GBC.
- The SameBoy `.info` describes it as a port of a "highly accurate" Game Boy emulator with "the highest accuracy of any Game Boy emulator" [verified: `sameboy_libretro.info` description].
- Its upstream README lists save states and real-time clock emulation as features [verified: https://github.com/LIJI32/SameBoy README].
- Its costs: higher CPU requirements. The `.info` says lower-power machines are better served by Gambatte [verified: `sameboy_libretro.info`].

Fallback: **Gambatte** for weaker hardware. The `.info` describes it as accurate enough to run nearly all games with no visible bugs and "blisteringly fast" [verified: `gambatte_libretro.info`]. Keep it as a per-system override, not the default.

Note: mGBA also runs GB/GBC. Using it for everything would mean one core, but SameBoy is the better GB/GBC choice by the upstream accuracy claims.

### Nintendo DS: melonDS DS
`melondsds_libretro` (display version 1.4.0, GPLv3+, "Nintendo - DS (melonDS DS)", based on standalone melonDS) is the DS core, fetched by `scripts/fetch-cores.ps1` and shipped in `cores\` like the others.
- **No BIOS needed.** With `melonds_sysfile_mode = builtin` it uses its built-in FreeBIOS and generated firmware; the optional real `bios7.bin`, `bios9.bin`, `firmware.bin` (and DSi files) are never required [verified: core log "Falling back to FreeBIOS / built-in firmware", `.info` lists all firmware as `opt`].
- **Software renderer works.** `melonds_render_mode = software` (the default) needs no OpenGL context, so it runs under retrorun, which refuses `SET_HW_RENDER`. It has a threaded software renderer and an x64 JIT (about 140 fps headless on Pokemon Diamond).
- **Composes both screens itself** into one frame (`melonds_screen_layout1`, 17 layouts) and takes the stylus as a libretro pointer in frame coordinates (`melonds_touch_mode`), so the frontend only maps the mouse.
- Save RAM is exposed through `RETRO_MEMORY_SAVE_RAM` (`libretro_saves = true`); `melonds_battery_update_interval = 1` keeps that buffer current. Save states are about 19 MB, so the hub does not resume them.
- RTC follows the PC clock (`melonds_start_time_mode = real`), which Pokemon uses.
- Alternatives considered: the older `melonds_libretro` (fewer options, no standalone parity) and `desmume_libretro` (works, but slower/less accurate; no per-core stylus mapping advantage). Both were downloadable from the same buildbot and are not fetched.
- Option defaults retrorun applies for `--system nds` are in `src/retrorun/dsview.h` (`for_each_default_option`). Override with `retrorun.ini` `[nds]` or `--opt`.

## Core comparison

| Feature | mGBA (`mgba_libretro`) | Gambatte (`gambatte_libretro`) | SameBoy (`sameboy_libretro`) |
|---|---|---|---|
| Version (`display_version`) | 0.10-dev | v0.5.0 | 0.14.7 |
| License | MPLv2.0 [verified: `.info`, README] | GPLv2 [verified: `.info`, README] | MIT per `.info`; upstream README says Expat [verified, both; note the README adds an iOS-folder exception] |
| Systems | GB, GBC, GBA [verified] | GB, GBC [verified] | GB, GBC [verified] |
| Save states (libretro flag) | `savestate = true`, `savestate_features = deterministic` [verified: `.info`] | `savestate = true`, `savestate_features = deterministic` [verified: `.info`] | No `savestate` key in `.info` [unverified for libretro]; README lists save states as a feature [verified, README] |
| RTC | Real-time clock supported, MBC3+RTC mapper listed [verified: README] | Not mentioned in README [unverified] | "Real time clock emulation" listed as a feature, no accuracy detail [verified: README] |
| Link cable / multiplayer | Local (same-machine) link cable listed as current; networked link cable listed as **planned, not available** [verified: README] | Netlink not mentioned in README [unverified] | Dual-GB / link mode not mentioned in README [unverified] |
| Cheats | `cheats = true` [verified: `.info`] | not in `.info` [unverified] | not in `.info` [unverified] |
| Core options | Not listed in README [unverified names, see below] | Not listed in README [unverified names, see below] | Not listed in README [unverified] |
| BIOS / boot ROM | Optional: `gba_bios.bin`, `gb_bios.bin`, `gbc_bios.bin`, `sgb_bios.bin` [verified: `.info`] | Optional: `gb_bios.bin`, `gbc_bios.bin` [verified: `.info`] | Optional: `dmg_boot.bin`, `cgb_boot.bin` [verified: `.info`] |

Known BIOS MD5s from the `.info` notes, for validating user-supplied files: `gb_bios.bin` / `dmg_boot.bin` `32fbbd84168d3482956eb3c5051637f5`; `gbc_bios.bin` / `cgb_boot.bin` `dbfce9db9deaa2567f6a84fde55f9680`; `gba_bios.bin` `a860e8c0b6d573d191e4ec7db1b1e4f6` [verified: `.info` notes]. All BIOS files are optional, so the hub should not ship them.

## Core options relevant to a frontend (all unverified)

The option names below came from the brief, not from a source read here. The upstream READMEs don't list them, so check each core's `libretro_core_options` definitions before relying on them.
- mGBA: `mgba_gb_model`, `mgba_use_bios`, `mgba_skip_bios` [unverified].
- Gambatte: `gambatte_gbc_color_correction` [unverified].
- SameBoy: option names not identified [unverified].

Options a frontend will most likely need to set: GB model selection, BIOS use, and GBC color correction. Without verified names, the hub should not write these yet.

## Sources
- `deps/cores/info/mgba_libretro.info`, `gambatte_libretro.info`, `sameboy_libretro.info` (from https://buildbot.libretro.com/assets/frontend/info.zip, fetched by `scripts/fetch-cores.ps1`).
- mGBA README: https://github.com/libretro/mgba
- Gambatte README: https://github.com/libretro/gambatte-libretro
- SameBoy README: https://github.com/LIJI32/SameBoy
- Core downloads: https://buildbot.libretro.com/nightly/windows/x86_64/latest/

## Open items
- Confirm each core's option names from its libretro core-options source file.
- Confirm save-state support for SameBoy through the libretro build, not only the upstream README.
- Confirm link-cable support for Gambatte and SameBoy. The current RetroHub netplay design is lockstep input, not link cable, so this matters only if link-cable games are wanted.
- Decide the GameCube core. None is fetched.

## PlayStation: Beetle PSX (`beetle_psx_libretro`)

Built from source by `src/beetle_psx/CMakeLists.txt` (MinGW-w64 gcc, like `sameboy_rb`), from a pinned `libretro/beetle-psx-libretro` commit (`ea14d6a`, tarball SHA-256 verified). Output: `cores\beetle_psx_libretro.dll` (statically linked, about 10 MB). Without gcc the target is skipped and PS1 games stay listed but cannot start. GPL-2.0 (the hub ships it as a separate DLL that retrorun loads; see THIRD_PARTY.md).

**Why this core** (evaluated: Beetle PSX, SwanStation, PCSX-ReARMed):
- Pure C, bit-exact software GPU: retrorun refuses `SET_HW_RENDER`, so the HW renderers are out. The build has no GL/Vulkan code at all, and "internal resolution" 1x-8x is the software renderer's own option.
- **Deterministic CPU.** Built without lightrec (the JIT), so it is the interpreter: same input, same result on every PC. This is what online play needs; `cpu_dynarec` does not exist in this build.
- **Save states cover (nearly) everything** (CPU, GPU/VRAM, SPU, CD controller, DMA, timers): about 16 MB in retrorun's view of `retro_serialize_size` (the core pads; the hub never resumes PS1 states, the memory card is the save). The stock core is deterministic frame to frame (same input, same video/audio hashes) but **not exact across a state load**: `retrorun --synctest` failed, because the software video surface and the scan-out cache are host-side state outside the savestate (see "PS1 determinism" below). The serialized-state hash also differs between identical runs (padding bytes), so PS1 netplay compares a rolling **output (video) hash**, never the state hash.
- **CHD, CUE/BIN, ISO, PBP, M3U, EXE** are read by the core itself (libretro-common's clean-room `rchd`, with deflate, LZMA, FLAC, zstd hunks). Multi-disc via `.m3u` with disc swap.
- **Multitap**: `beetle_psx_enable_multitap_port1` gives four controllers on libretro ports 0-3 (Crash Bash, Tekken 3 with friends). DualShock analog and rumble work.
- **OpenBIOS** is compiled in: with no `scph*.bin` the core boots it (fewer games run). A real BIOS is used first when found.
- SwanStation: more accurate-looking and faster upscaling, but its software GPU is threaded and its states are not stable between runs in the way rollback needs, and it adds a C++/CMake dependency tree. PCSX-ReARMed: fast, but not cycle-exact.

Options retrorun/the hub set (names verified with `retrorun --list-options`): `beetle_psx_internal_resolution` (1x(native) 2x 4x 8x), `beetle_psx_widescreen_hack`, `beetle_psx_enable_multitap_port1`. Memory card 1 is the core's libretro save RAM (128 KB), written as `<game>.srm` in the vault battery folder; card 2 stays off.

### PS1 determinism (online play)

`src/beetle_psx/patch.cmake` patches the fetched tree in place at configure time (idempotent, marker `RETROHUB-RB-<id>`); the built DLL calls itself **"Beetle PSX (rollback)"**, which is how retrorun knows rollback is allowed (a stock Beetle PSX stays on lockstep).
- **Surface in the state.** Rows a frame does not rewrite (right after a display-mode change, outside the visible scanline range) kept whatever earlier frames *of that timeline* left in the software surface, so a re-simulated frame showed other pixels than the first run (false video mismatches at every mode switch, e.g. BIOS cube -> game). The surface (about 1.3-1.6 MB) is serialized with the MAIN section.
- **Scan-out cache dropped on load.** The cache that lets the scan-out skip re-zeroing a row's margins describes the previous frames of the old timeline.
- **Pinned options online / in SyncTest** (retrorun, after `--opt`): internal resolution 1x, no widescreen hack, PGXP off, dither/overclock/CD speed defaults, CD access `sync` (the async reader retries sectors by wall clock), OSD and FPS counter off (drawn into the frame), frame duping off, crop/scanline settings at their defaults, analog toggle/calibration off, memory card 2 off, and **deinterlacer = off** (rasterise both fields; weave/FastMAD read the previous field outside the savestate and failed SyncTest on a 480i picture, `off` and `bob` pass).
- **Input**: buttons (16 bits) + both sticks of every port. A DualShock axis is one byte, so sticks travel as 8-bit values (`axis >> 8`, `src/retrorun/ps1_input.h`) and every PC, the sender included, feeds the core the same quantised value. Lockstep carries them as a second u32 per sample ("wide" `LockstepCore`), rollback as 6 bytes per port (`rb::Config::val_bytes`).
- **Verified** (qa/ps1_synctest.ps1, qa/ps1_online.ps1): the homebrew `qa/make_ps1_netdisc.py` boots from OpenBIOS as a MODE2/2352 disc with EDC/ECC (the disc layer rejects unchecked 2048-byte sectors), switches the pads to analog through SIO0 and draws every frame from both sticks, all buttons and a hardware timer, so any divergence is visible and permanent. SyncTest (depth 6-8, mispredicted frames included) passes on a 240p and a 480i disc, 2/4 headless peers (direct, relay, 60 ms / 25 ms jitter / 4% loss) end with identical output + input hashes over 3+ minutes.
- **Not covered**: the matrix runs the homebrew, not commercial discs (none here): CD streaming/XA/MDEC video, memory-card writes and multi-disc play are exercised by the core's own state code but not by a SyncTest. Cost: 16 MB per saved state, about 0.5-1 ms to save and 1 ms to load; retrorun keeps `--rb-pred 6` (10 states) for PlayStation.
