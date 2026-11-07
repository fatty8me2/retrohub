# Hookshot: the native Ocarina of Time (Switch 2) on Windows

Hookshot (https://github.com/NotRexed/Hookshot, GPL-3.0-or-later) is a native Windows build of *The Legend of Zelda: Ocarina of
Time* (the Nintendo Switch 2 remake). It is made on the player's own PC, from the player's own copy of the game, by static
recompilation: no emulator, and a higher frame rate than Lichii. RetroHub builds it for the player with one button on the game's
page, and once it exists Play starts it instead of Lichii (`switch.runner` = auto). Lichii stays installed as the fallback.

Code: `src/hub/hookshot.*` (what is built: paths, stamp, `ready()`), `src/hub/hookshot_build.*` (the build pipeline),
`src/hub/hookshot_ui.*` (the card on the game page). Tests: `hub_hookshot_build_test`.

## What RetroHub shares, and what it never does

- The built game is made from the player's game file. RetroHub never offers it to friends or packs, never uploads it and never
  puts it in the save vault. Only the save goes in the vault, in the same folder Lichii uses, so any PC can switch between the two.
- RetroHub downloads no game data, keys or firmware. Hookshot's own release zip, its prebuilt tools and any prebuilt wgpu-native
  are never used.
- Nothing pops up over a running game. The build pauses while a game is running (see below).

## The audit (data/hookshot/SOURCE.md)

RetroHub ships the Hookshot source, unmodified, as `data\hookshot\hookshot-src-137c423.zip` (`git archive` of commit
`137c423589f178bb4274e339b17b4dfd466435e9`, sha256 `afd262616752aca42317fc2f53dbb1de3c9a12a1207b4d3b98c548ace0f228ba`, checked
before it is unpacked). It was audited on 2026-11-07, all 912 files:

- no network code and no telemetry;
- no process launching beyond the compiler and linker during the build;
- no registry, startup or security changes;
- no obfuscated or hidden code; the vendored Opus 1.5.2 and zstd 1.5.7 match upstream.

It writes beside its exe only: `user\` (saves), `language.txt`, `hookshot_options.ini`, the shader and pipeline caches
(`hookshot_*.bin`, `mkw_vk_pipelines.bin`) and `hookshot_log.txt`. If no AV1 video decoder is installed it shows a top-most message
box offering Microsoft's AV1 Video Extension; RetroHub therefore checks for the decoder itself before launch and asks in its own
window (that check belongs to the launch code, not the build).

## Requirements

The card checks these (on a worker thread, never the UI thread) and says in plain words what is missing, with a fix button where
one makes sense. **Build it** stays off until they all pass.

| Need | How it is checked |
| --- | --- |
| Visual Studio's C++ tools | `vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64`. Microsoft does not allow bundling them: the card offers "Get the build tools" (opens https://visualstudio.microsoft.com/downloads/ in the browser, on the player's click). RetroHub never installs them, never uses winget and never asks for elevation. |
| A Windows SDK | `%ProgramFiles(x86)%\Windows Kits\10\Include\10.*\um\windows.h` |
| A Vulkan 1.3 GPU with mesh shaders | `vulkan-1.dll` is loaded, an instance is created at API 1.3 (no window, no device) and destroyed, and a physical device with API >= 1.3 and `VK_EXT_mesh_shader` is looked for. Only NVIDIA has been tested. |
| 16 GB of RAM | `GlobalMemoryStatusEx` (rounded to the nearest GB, as a 16 GB PC reports a little less) |
| 70 GB free | on the drive holding `<data>\emulators` (the build needs about 60 GB while it works; about 25 GB stays afterwards) |
| The game file | A `.nup`, or a loose dump folder: `exefs\main` with a `romfs` folder next to `exefs` (the library lists a loose dump by its `exefs\main` file). `.nsp`/`.xci` are not supported by the Hookshot builder. The build refuses a game of another build than `e0f3ccfa` (`@@check bad ...`). |

## What is downloaded, from where

Plain HTTPS (WinHTTP), each file pinned by size and hash, downloaded into a `.part` file, verified, then renamed. A file already in
the kit that verifies is reused. Nothing else is downloaded by RetroHub; pip downloads the five wheels below from PyPI.

| What | Source | Pin |
| --- | --- | --- |
| CPython 3.13.17 (the Python Software Foundation's NuGet package; its `tools\` becomes `python\`) | `https://api.nuget.org/v3-flatcontainer/python/3.13.17/python.3.13.17.nupkg` (14,826,465 bytes) | SHA-512 (base64) `DmZ5KLtPGVc8bNiPZRM7LgeyQ6XHFvM1tWqma7nsb4IyglbSLDop4joCdqxcam14YhH+q1JIkK0eVmKRlsaynA==` |
| Python packages: capstone 5.0.9, lz4 4.4.5, numpy 2.5.4, unicorn 2.1.4, zstandard 0.25.0 | PyPI, `pip install --require-hashes --only-binary :all: --no-deps -r data\hookshot\requirements.txt` | sha256 of each Windows x64 CPython 3.13 wheel, in `requirements.txt` |
| LLVM 23.1.5 | `https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.5/clang+llvm-23.1.5-x86_64-pc-windows-msvc.tar.zst` (276,368,249 bytes) | SHA-256 `b2be33385ddc86e83511417a060de927ae8b09d1f3017cc3cdce5c73cd5029d7` |
| Vulkan-Headers v1.4.345 | `https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.345.tar.gz` (3,029,194 bytes) | SHA-512 `d2846ea2...b484b` (Eden's cpmfile pin) |
| SPIRV-Headers vulkan-sdk-1.4.341.0 | `https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.341.0.tar.gz` (561,287 bytes) | SHA-512 `d624371d...5ed23b` (Eden's cpmfile pin) |
| FFmpeg 9.0.2 (Hookshot's video code includes libavcodec; Eden's pinned CI build, `ffmpeg-ci` in Eden's cpmfile, built by GitHub Actions from FFmpeg's official source at commit `946fcce07b6dcd0331c8cc609192aeff5e1924f8` plus Eden's published patch) | `https://github.com/crueter-ci/FFmpeg/releases/download/9.0.2-1790726865-946fcce07b/ffmpeg-windows-amd64-9.0.2-1790726865-946fcce07b.tar.zst` (7,830,170 bytes) | SHA-256 `12d23634e609137e134cfd24148c9e27ed8a4104010f010b4cd92b68b1000367` (matches the release's `.sha512sum` too) |
| VulkanMemoryAllocator v3.3.0 | `https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz` (962,176 bytes) | SHA-512 `deb5902e...3344b` (Eden's cpmfile pin) |

The full hashes are in `src/hub/hookshot_build.cpp` (`pins()`), which the test checks for shape. All three header archives were
downloaded once while writing this and matched Eden's pins byte for byte. SDL2.dll is RetroHub's own (copied to
`src\third_party\sdl2\SDL2.dll` on every build). The Hookshot source is the audited zip above. `WGPU_NATIVE` points at a folder that
does not exist, so the build uses the Vulkan back end only.

### How the LLVM archive is unpacked

Windows' `tar.exe` refuses this archive's zstd window, so `data\hookshot\unpack_tar.py` (run with the kit's own Python, `-I`)
streams `zstandard` (`max_window_size=2**31`) into `tarfile` with `filter='data'`. Every member name is checked first (no absolute
path, no `..`, no drive letter or `:`, no backslash); symbolic links are skipped; a hard link becomes a copy. `python -I unpack_tar.py
--selftest` builds hostile archives and checks that each is refused. Only what the build runs is unpacked: `clang`, `clang++`, `lld`,
`lld-link`, `ld.lld`, a few `llvm-*` tools and `lib\clang` (headers and runtime): about 590 MB instead of 3.5 GB. `make_game.py`'s own
compiler self-test (`--check`) runs against this subset and passes.

The three header packages are unpacked with the same script (`--strip 1`) into the layout `nx2/build.py`'s `cpm_package()` reads:
`cpm\vulkanheaders\v1.4.345\include\...`, `cpm\spirv-headers\vulkan-sdk-1.4.341.0\include\...`,
`cpm\vulkanmemoryallocator\v3.3.0\include\...`, and (the FFmpeg archive's root already holds `include\` and `lib\`, so nothing is
stripped) `cpm\ffmpeg\9.0.2-1790726865-946fcce07b\{include,lib}` with `avcodec.lib`, `avutil.lib`, `swresample.lib`, `swscale.lib`.

## Where everything lives

All of it under `<data>\emulators\` (`<data>` is RetroHub's `userdata` folder):

| Folder | What | Size |
| --- | --- | --- |
| `hookshot\` | The built game: `hookshot.exe`, `hookshot_vk.dll`, `SDL2.dll`, `romfs\`, `user\` (the saves), caches, `retrohub_hookshot.json` (the stamp) | about 25 GB |
| `hookshot-kit\` | The build tools, kept after a build so a rebuild is quick | about 1.1 GB |
| `hookshot-kit\downloads\` | The pinned archives (283 MB) | |
| `hookshot-kit\src\` | The Hookshot source, unpacked (17 MB) | |
| `hookshot-kit\python\` | The private Python and its packages (173 MB) | |
| `hookshot-kit\llvm\` | The compiler subset (592 MB) | |
| `hookshot-kit\cpm\` | The header packages (45 MB) | |
| `hookshot-kit\work\` | The build's working files, tens of GB. Deleted when the build ends (done or cancelled); kept after a failure so *Try again* is quicker | up to ~60 GB |
| `hookshot-kit\logs\` | One log per build (`build-<date>-<time>.log`); the last five are kept | small |
| `hookshot.staging\` | The build's output, while it runs. Deleted on cancel or failure | |

The kit is marked per folder (`.retrohub-kit`, holding the pin it was made from), so a folder that was cut short is wiped and made
again, and one that is fine is reused.

## How a build runs

`hookshot::start_build` runs on one worker thread:

1. Requirements again (fresh), then any interrupted swap from a crash is put right (`recover_swap`).
2. The kit (`kit_prepare`): source, Python and packages, LLVM, headers.
3. `python -I nx2\make_game.py --check ...` (is this the right game and version?).
4. `python -I nx2\make_game.py --nup <file> --out <staging> --work <kit>\work --language <code> --jobs <n>`. Environment: `PATH` =
   `<kit>\llvm\bin;` + the old PATH, `EMSDK` = `<kit>\no-emsdk` (absent, so the tools come from PATH), `RX_CPM` = `<kit>\cpm`,
   `WGPU_NATIVE` = `<kit>\no-wgpu` (absent), `RX_JOBS` = `--jobs`, `PYTHONPATH` removed. `n` is 3.5 GB of RAM a job, at most 80% of
   the PC's memory, at most one a core.
5. The stamp is written and the folders are swapped (below). `<kit>\work` is deleted.

Every process is started without a window, at below-normal priority, inside a Job object with kill-on-close (closing RetroHub ends
the build). Output goes to the log and, line by line, to `parse_protocol_line` (`@@stage`, `@@progress`, `@@check`, `@@done`,
`@@error`) which drives the card.

**Pausing.** Every second or two RetroHub looks for a running game: its own game session, or a known emulator or game process
(Lichii, Dolphin, Cemu, Azahar, PCSX2, PPSSPP, retrorun, n64run, BattleShip, Hookshot). While one runs, every process in the job is
suspended (`NtSuspendProcess`) and downloads wait; when it ends they resume. The card says "Paused while you play". A download
that waited long enough for the server to drop the connection is simply retried (up to three attempts).

**Cancel** terminates the job, deletes the staging folder and the working files and keeps the downloads. Closing RetroHub does the
same to the processes.

**The swap** (`swap_in`): the old build's `user\` folder (the saves) and its cache files (`hookshot_*.bin`, `mkw_vk_pipelines.bin`,
`hookshot_options.ini`; where the new build made a file of the same name, the new one wins) are *moved* into the staging folder; the
old build is renamed to `hookshot.old`, staging becomes `hookshot`, and `hookshot.old` is deleted only once `user\` is confirmed in
the new folder. If any move fails, the moves are rolled back and both folders are kept. `user\` is never deleted: a `hookshot.old`
that holds saves is refused. If RetroHub is closed in the middle of the swap, the next start puts the saves back and renames
`hookshot.old` back to `hookshot`.

The saves themselves are in `user\save\5a3c9e1f0b7d24680000000000000001\`, the game's own raw files; the launch code syncs them with
the vault folder Lichii uses (`<vault>\switch\<key>\battery\save\0400486024DEC000\`).

## Removing it

- **Remove build tools (frees N GB)** on the game's card deletes `hookshot-kit\` (the game and its saves stay). Rebuilding downloads
  the tools again.
- To remove the built game, delete `<data>\emulators\hookshot\` yourself. Its `user\` folder holds the Hookshot-side saves: the
  vault copy is the safe one, but copy `user\` away first if you are unsure. Play then falls back to Lichii.

## Troubleshooting

- **"Build it" is off.** The card lists what is missing. Fix it and press *Check again*.
- **The build stopped.** *Open the log* opens the full log in Notepad. The tail of the log has the reason. *Try again* resumes with
  the tools already downloaded.
- **"The download did not match its checksum."** The file is deleted and tried again up to three times; a proxy or a security
  product that rewrites downloads causes this. Nothing is installed from a file that does not match.
- **Antivirus removes clang.exe or Python.** Allow `<data>\emulators\hookshot-kit` and build again.
- **Paths.** The deep build folders need Windows long paths if RetroHub's data folder is itself deeply nested; keep it short.
- **Wrong game version.** `@@check bad This is build ...`: Hookshot supports build `e0f3ccfa` (update v131072) only.
- **Slow PC while building.** The build is below-normal priority and pauses while a game runs. It takes 40-60 minutes on a PC with
  16+ GB and 6+ cores. A PC with little memory runs fewer compile jobs at once (`RX_JOBS`).
- **The Vulkan check fails on a PC with a suitable GPU.** Update the graphics driver; mesh shaders (`VK_EXT_mesh_shader`) need a
  recent one. Only NVIDIA cards have been tested.
