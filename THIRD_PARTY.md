# Third-party components

RetroHub uses the components below. Each one keeps its own license. Components are either fetched at build time, downloaded by a script, or vendored in the source tree.

## Fetched at build time (CMake FetchContent)

| Component | Version | License | Use |
|---|---|---|---|
| SDL2 | 2.30.9 | zlib | Window, input and audio. Shipped as `SDL2.dll`. |
| Dear ImGui | 1.91.5 | MIT | Hub user interface. |
| mupen64plus-core | 2.6.0 | GPL-2.0-or-later | API headers only, at build time. |
| miniz | 3.0.2 | MIT | Zip handling for zipped ROMs and saves. |

## Downloaded by scripts

| Component | Version | License | Fetched by | Use |
|---|---|---|---|---|
| mupen64plus (core, RSP-HLE, audio-SDL) | 2.6.0 | GPL-2.0-or-later | `scripts/fetch-emu.ps1` | N64 emulation core, run by `n64run.exe`. |
| GLideN64 | a04081a | GPL-2.0-or-later | `scripts/fetch-emu.ps1` | N64 video plugin. |
| FreeType (runtime DLL) | as bundled | FreeType License (FTL) or GPL-2.0 | `scripts/fetch-emu.ps1` | Fonts for the mupen64plus on-screen display. |
| libpng (runtime DLL) | as bundled | libpng License | `scripts/fetch-emu.ps1` | Image loading for the mupen64plus bundle. |
| zlib (runtime DLL) | as bundled | zlib | `scripts/fetch-emu.ps1` | Compression for the mupen64plus bundle. |
| SDL2_net (runtime DLL) | as bundled | zlib | `scripts/fetch-emu.ps1` | Runtime library shipped with the mupen64plus bundle. |
| mGBA (libretro core) | nightly | MPL-2.0 | `scripts/fetch-cores.ps1` | Game Boy Advance, and fallback for GB and GBC. |
| SameBoy (libretro core) | nightly | MIT | `scripts/fetch-cores.ps1` | Game Boy and Game Boy Color. |
| Gambatte (libretro core) | nightly | GPL-2.0 | `scripts/fetch-cores.ps1` | Game Boy and Game Boy Color fallback. |
| libretro frontend info files | nightly | per file (MIT for most) | `scripts/fetch-cores.ps1` | Core metadata (`.info`). |
| Dolphin | 2509 | GPL-2.0-or-later | `scripts/fetch-dolphin.ps1` | GameCube emulation, downloaded on demand. |
| Azahar | 2126.2 | GPL-3.0 | `src/hub/emulators.cpp` (github.com/azahar-emu/azahar) | Nintendo 3DS emulation, downloaded on demand into the data folder; SHA-256 pinned. |
| Cemu | 2.6 | MPL-2.0 | `src/hub/emulators.cpp` (github.com/cemu-project/Cemu) | Wii U emulation. An existing install is used; otherwise downloaded on demand into the data folder; SHA-256 pinned. |

The libretro cores are downloaded from the libretro nightly build server. The Dolphin build is downloaded from dolphin-emu.org, and the Azahar and Cemu builds from their GitHub releases. RetroHub does not redistribute these files in its source tree.

## Vendored in the source tree

| File | License | Notes |
|---|---|---|
| `src/hub/third_party/stb_image.h` | MIT or public domain (dual) | Image decoding. |
| `src/retrorun/third_party/stb_image_write.h` | MIT or public domain (dual) | Image encoding for screenshots. |
| `src/retrorun/third_party/libretro.h` | MIT | libretro frontend API header. |

The stb libraries are public domain in the sense that you may use them without restriction. The dual license lets you choose between MIT and public domain.

## Fonts and artwork

RetroHub does not include game cover art, box art or screenshots. Art is fetched at run time from the libretro thumbnail and database repositories, which are third-party resources with their own terms.

## Licensing notes

- RetroHub itself is GPL-3.0-or-later (see `LICENSE`).
- The mupen64plus and GLideN64 components are GPL-2.0-or-later, so they can be used under GPL-3.0-or-later.
- Gambatte and mupen64plus are used as separate programs or plugins, not compiled into RetroHub.
