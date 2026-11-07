#pragma once
/* Builds Hookshot (hookshot.h) on this PC from the player's own game file: about 40-60 minutes, ~70 GB free while
 * building (~25 GB after). Everything it downloads comes from the official upstream and is pinned by hash:
 *   - LLVM 23.1.5 (github.com/llvm/llvm-project release clang+llvm-23.1.5-x86_64-pc-windows-msvc.tar.zst,
 *     sha256 b2be33385ddc86e83511417a060de927ae8b09d1f3017cc3cdce5c73cd5029d7)
 *   - CPython 3.13.17, the Python Software Foundation's NuGet package "python" (SHA-512 from nuget.org's catalog)
 *   - the build's PyPI wheels (lz4, numpy, capstone, unicorn 2.1.x, zstandard) with pip --require-hashes
 *   - Vulkan-Headers v1.4.345, SPIRV-Headers vulkan-sdk-1.4.341.0, VulkanMemoryAllocator v3.3.0 (the tags and SHA-512s
 *     Eden's cpmfile.json pins), laid out as the CPM cache nx2/build.py reads (RX_CPM)
 * SDL2.dll is RetroHub's own. The Hookshot source is the audited commit (hookshot::kSourceCommit) shipped in RetroHub's
 * package as data\hookshot\hookshot-src.zip (sha256 compiled in). Hookshot's own release zip / prebuilt tools and the
 * prebuilt wgpu-native are NEVER used (WGPU_NATIVE points at an empty folder: Vulkan back end only).
 * Visual Studio's C++ build tools + a Windows SDK must already be installed: Microsoft does not allow bundling them.
 *
 * The build: nx2/make_game.py --nup <file> (or --romfs/--exefs) --out <staging> --work <kit>\work --language <code>,
 * below-normal priority, RX_JOBS from memory (3.5 GB a job, at most 80% of RAM), SUSPENDED while any game runs and
 * resumed when it exits, cancellable. Progress comes from make_game.py's protocol lines (parse_protocol_line).
 * On @@done: write_stamp(), then swap the staging folder in for game_dir() keeping game_dir()\user (saves) and caches. */
#include <cstdint>
#include <string>

namespace hub { struct GameEntry; }

namespace hookshot {

enum class BuildState { Idle, Preparing, Downloading, Checking, Building, Paused, Done, Failed, Cancelled };

struct Requirements {
    bool vs_cpp = false;      // vswhere: Microsoft.VisualStudio.Component.VC.Tools.x86.x64 + a Windows SDK
    bool vulkan_mesh = false; // a Vulkan 1.3 device with VK_EXT_mesh_shader (vulkan-1.dll probe; NVIDIA tested only)
    std::string gpu_name;
    uint64_t ram_gb = 0;      // need >= 16
    uint64_t free_gb = 0;     // on the data_dir() drive; need >= 70 (60 build + headroom)
    std::string game_path;    // the player's .nup (or dump folder) for this game, from the library entry
    std::string problem;      // the first blocking problem in plain words ("" = can build)
    std::string fix_hint;     // what to do about it ("Install Visual Studio Build Tools with 'Desktop development with C++'")
};

struct BuildStatus {
    BuildState state = BuildState::Idle;
    std::string stage_id, stage;   // from "@@stage <id> <text>"
    float progress = 0.f;          // "@@progress <0..1>" within the stage
    float overall = 0.f;           // rough 0..1 over the whole pipeline (downloads + stages)
    std::string error;             // Failed: the "@@error" text or RetroHub's own message
    std::string log_path;          // the full build log
    int64_t started_unix = 0;
    bool paused_for_game = false;  // suspended because a game is running
};

/* Quick checks only (no download, no build). Safe on the UI thread? NO: vswhere + Vulkan probe take ~1 s; call from a
 * worker and cache (requirements_cached()). */
Requirements check_requirements(const hub::GameEntry& e);
Requirements requirements_cached();          // last result ("" problem + vs_cpp false when never checked)
void refresh_requirements_async(const hub::GameEntry& e);

/* The whole pipeline on a background thread (one at a time). false + err when it cannot start. */
bool start_build(const hub::GameEntry& e, const std::string& language, std::string* err);
void cancel_build();
bool build_running();
BuildStatus build_status();                  // thread-safe snapshot

/* ---- pure helpers (unit-tested) ---- */
/* One stdout line of make_game.py: "@@stage <id> <text>" / "@@progress <x>" / "@@check ok|bad <text>" / "@@done <folder>" /
 * "@@error <text>". Updates *st and returns true when the line was a protocol line. */
bool parse_protocol_line(const std::string& line, BuildStatus* st);
/* RX_JOBS for this much physical memory and these many logical cores (toolchain.default_jobs: 3.5 GB a job, 80%). */
int jobs_for(uint64_t ram_bytes, int cores);

}   // namespace hookshot
