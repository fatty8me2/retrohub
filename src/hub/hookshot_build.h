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
 * package as data\hookshot\hookshot-src-137c423.zip (sha256 compiled in). Hookshot's own release zip / prebuilt tools and the
 * prebuilt wgpu-native are NEVER used (WGPU_NATIVE points at an empty folder: Vulkan back end only).
 * Visual Studio's C++ build tools + a Windows SDK must already be installed: Microsoft does not allow bundling them.
 *
 * The build: nx2/make_game.py --nup <file> (or --romfs/--exefs) --out <staging> --work <kit>\work --language <code>,
 * below-normal priority, RX_JOBS from memory (3.5 GB a job, at most 80% of RAM), SUSPENDED while any game runs and
 * resumed when it exits, cancellable. Progress comes from make_game.py's protocol lines (parse_protocol_line).
 * On @@done: write_stamp(), then swap the staging folder in for game_dir() keeping game_dir()\user (saves) and caches.
 *
 * Layout (docs/HOOKSHOT.md): <data_dir>\emulators\hookshot-kit\{downloads,src,python,llvm,cpm,work,logs}, the staging folder
 * <data_dir>\emulators\hookshot.staging, and the finished game in <data_dir>\emulators\hookshot (hookshot.h). */
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hub { struct GameEntry; }

namespace hookshot {

enum class BuildState { Idle, Preparing, Downloading, Checking, Building, Paused, Done, Failed, Cancelled };

struct Requirements {
    bool vs_cpp = false;      // Visual Studio's C++ tools (vswhere) AND a Windows SDK: vs_tools && win_sdk
    bool vulkan_mesh = false; // a Vulkan 1.3 device with VK_EXT_mesh_shader (vulkan-1.dll probe; NVIDIA tested only)
    std::string gpu_name;
    uint64_t ram_gb = 0;      // need >= 16
    uint64_t free_gb = 0;     // on the data_dir() drive; need >= 70 (60 build + headroom)
    std::string game_path;    // the player's .nup (or dump folder) for this game, from the library entry
    std::string problem;      // the first blocking problem in plain words ("" = can build)
    std::string fix_hint;     // what to do about it ("Install Visual Studio Build Tools with 'Desktop development with C++'")
    // the parts of the above, so the card can list every failure
    bool checked = false;     // a check ran (false = requirements_cached() before the first check)
    bool vs_tools = false, win_sdk = false;
    bool game_ok = false;
    std::string game_problem, game_fix;   // "" when game_ok
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
bool requirements_busy();                    // a refresh_requirements_async() is still running

/* The whole pipeline on a background thread (one at a time). false + err when it cannot start. */
bool start_build(const hub::GameEntry& e, const std::string& language, std::string* err);
void cancel_build();
bool build_running();
BuildStatus build_status();                  // thread-safe snapshot

/* RetroHub tells the build when a game is running (the build also looks for known emulator / game processes itself).
 * Called from the build's worker thread every few seconds: keep it to reading a flag. */
void set_game_running_probe(std::function<bool()> probe);

/* ---- the build tools (the kit): kept after a build so a rebuild is quick; the card offers to remove them ---- */
std::string kit_dir();                       // <data_dir>\emulators\hookshot-kit
bool kit_present();                          // the folder exists
uint64_t kit_size_bytes();                   // walks the folder: call from a worker (kit_size_async)
void kit_size_async();                       // computes kit_size_cached() in the background
int64_t kit_size_cached();                   // -1 until computed
bool remove_kit(std::string* err);           // false (+ err) while a build runs or when something cannot be deleted
void remove_kit_async();
bool kit_busy();                             // a remove_kit_async() is running

/* ---- pure helpers (unit-tested) ---- */
/* One stdout line of make_game.py: "@@stage <id> <text>" / "@@progress <x>" / "@@check ok|bad <text>" / "@@done <folder>" /
 * "@@error <text>". Updates *st and returns true when the line was a protocol line.
 *   stage    -> stage_id, stage, progress = 0, state = Building
 *   progress -> progress (clamped 0..1)
 *   check    -> stage_id = "check"; ok: stage = text, error cleared; bad: error = text
 *   done     -> stage_id = "done", stage = folder, progress = 1   (the caller then swaps the folders in: state is left alone)
 *   error    -> error = text, state = Failed
 * Anything else (including "@@progress abc") returns false and changes nothing. */
bool parse_protocol_line(const std::string& line, BuildStatus* st);
/* RX_JOBS for this much physical memory and these many logical cores (toolchain.default_jobs: 3.5 GB a job, 80%). */
int jobs_for(uint64_t ram_bytes, int cores);

/* The languages make_game.py accepts (its LANGUAGES list): code + English name. */
struct Language { const char* code; const char* name; };
const std::vector<Language>& languages();
bool language_valid(const std::string& code);

/* Where the build gets the game from: a .nup file, or a loose dump (exefs\main with the romfs folder beside exefs). */
struct GameSource {
    enum Kind { None, Nup, Dump } kind = None;
    std::string nup, exefs, romfs;        // paths for make_game.py (--nup, or --romfs with --exefs)
    std::string problem, fix;             // plain words when kind == None
};
GameSource resolve_game_source(const std::string& entry_path);
std::vector<std::string> game_source_args(const GameSource& g);   // {"--nup", f} or {"--romfs", r, "--exefs", x}

/* Each failed requirement with its words. id: game | vs | sdk | vulkan | ram | disk. url/button: a "go and fix it" link ("" = none). */
struct Problem { std::string id, text, hint, url, button; };
constexpr uint64_t kNeedRamGb = 16, kNeedFreeGb = 70;
std::vector<Problem> requirement_problems(const Requirements& r);
void fill_requirement_problem(Requirements* r);    // sets problem / fix_hint from the first of requirement_problems()

/* How far along the whole pipeline a stage is (0..1): the kit steps, the check, then make_game.py's own stages by id. */
float overall_for(const std::string& stage_id, float stage_progress);

/* ---- hashes (Windows BCrypt) ---- */
enum class HashAlg { Sha256, Sha512 };
std::string to_hex(const std::vector<uint8_t>& d);
std::string to_base64(const std::vector<uint8_t>& d);
bool hash_data(HashAlg a, const void* data, size_t n, std::vector<uint8_t>* out);
bool hash_file(HashAlg a, const std::string& path, std::vector<uint8_t>* out);     // streams the file
std::string hash_data_hex(HashAlg a, const std::string& bytes);                   // "" on error
std::string hash_file_hex(HashAlg a, const std::string& path);                    // "" on error / missing file

/* ---- the pinned downloads ---- */
struct Pin {
    const char* id;          // "python" | "llvm" | "cpm-vulkanheaders" ...
    const char* host;        // https only
    const char* path;
    HashAlg alg;
    const char* hash;        // lowercase hex, or standard base64 when b64
    bool b64;
    uint64_t bytes;          // exact size
    const char* file;        // name in <kit>\downloads
    const char* what;        // for the card: "Python 3.13.17"
};
const std::vector<Pin>& pins();                    // python, llvm, then the three cpm header packages
constexpr const char* kSourceZipName = "hookshot-src-137c423.zip";
constexpr const char* kSourceZipSha256 = "afd262616752aca42317fc2f53dbb1de3c9a12a1207b4d3b98c548ace0f228ba";

/* ---- the swap (pure file work; unit-tested with temp folders) ---- */
/* Puts `staging` in place of `game`: first MOVES game\user (the saves) and the cache files (hookshot_*.bin, mkw_vk_pipelines.bin,
 * hookshot_options.ini; the new build's own file wins) into staging, renames game -> `old`, staging -> game and only then deletes
 * `old` (once game\user is confirmed). Never deletes user\. On a failure both folders are kept (moves are rolled back) and *err
 * says why. */
bool swap_in(const std::string& staging, const std::string& game, const std::string& old, std::string* err);
/* After a crash in the middle of swap_in: puts a stranded staging\user back, and renames `old` back to `game` when game is gone.
 * Returns what it did ("" = nothing to do). */
std::string recover_swap(const std::string& staging, const std::string& game, const std::string& old);
/* Removes a leftover `staging` folder WITHOUT ever deleting saves: when staging\user holds anything it is first MOVED to
 * <recovered_parent>\hookshot-saves-recovered-<timestamp>\user. If that move fails staging is NOT deleted and false comes back.
 * *note (may be null) says what happened ("" = nothing special; else a sentence for the log / the failure message). */
bool remove_staging_keeping_saves(const std::string& staging, const std::string& recovered_parent, std::string* note);

/* True while a process runs from this exact exe path (case-insensitive full-path match, not just the file name). */
bool exe_running_from(const std::string& exe_path);
/* True while a finished build waits to be swapped in (or is being swapped in): launch_hookshot refuses to start meanwhile. */
bool swap_in_progress();

/* ---- the process runner the pipeline uses (tested with harmless commands) ----
 * Runs a program without a window, at below-normal priority, inside a kill-on-close job; every stdout/stderr line goes to on_line.
 * While *pause is true every process of the job is suspended. Once *cancel is true the job is terminated.
 * Returns the exit code, -1 when it could not start (*err says why) or -2 when cancelled. */
int run_command(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd, const std::atomic<bool>* cancel,
                const std::atomic<bool>* pause, const std::function<void(const std::string&)>& on_line, std::string* err);

/* ---- the kit stage on its own (also what the manual smoke test calls) ---- */
struct KitIO {
    std::string kit;                                   // kit folder to fill
    std::string data;                                  // folder holding hookshot-src-137c423.zip, unpack_tar.py, requirements.txt
    std::string sdl2_dll;                              // RetroHub's SDL2.dll
    const std::atomic<bool>* cancel = nullptr;
    const std::atomic<bool>* pause = nullptr;
    std::function<void(const std::string& stage_id, const std::string& text, float stage_progress)> report;   // may be empty
    std::function<void(const std::string& line)> log;                                                      // may be empty
};
bool kit_prepare(const KitIO& io, std::string* err);   // downloads + verifies + unpacks; reuses what is already there and valid

}   // namespace hookshot
