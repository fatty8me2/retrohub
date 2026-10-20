# Libretro cores for Classic Hub

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

### GBA: mGBA
mGBA is the accuracy-focused GBA choice and is the only GBA core fetched. It also handles GB and GBC, so it can serve as a fallback. Its README describes it as a high-accuracy, high-performance emulator and the "generally good first choice" for GBA [verified: mGBA README, https://github.com/libretro/mgba].

### GB and GBC: SameBoy (recommended), Gambatte (fallback)
Recommendation: **SameBoy** as the default for GB and GBC.
- The SameBoy `.info` describes it as a port of a "highly accurate" Game Boy emulator with "the highest accuracy of any Game Boy emulator" [verified: `sameboy_libretro.info` description].
- Its upstream README lists save states and real-time clock emulation as features [verified: https://github.com/LIJI32/SameBoy README].
- Its costs: higher CPU requirements. The `.info` says lower-power machines are better served by Gambatte [verified: `sameboy_libretro.info`].

Fallback: **Gambatte** for weaker hardware. The `.info` describes it as accurate enough to run nearly all games with no visible bugs and "blisteringly fast" [verified: `gambatte_libretro.info`]. Keep it as a per-system override, not the default.

Note: mGBA also runs GB/GBC. Using it for everything would mean one core, but SameBoy is the better GB/GBC choice by the upstream accuracy claims.

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
- Confirm link-cable support for Gambatte and SameBoy. The current Classic Hub netplay design is lockstep input, not link cable, so this matters only if link-cable games are wanted.
- Decide the GameCube core. None is fetched.
