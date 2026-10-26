#pragma once
/* "Find my games": a bounded, cancellable scan of the places game files usually live, and the pure ranking /
 * collapsing that turns per-directory counts into a short checklist of folders to add.
 * Counting is by file extension only (zip / 7z are counted as archives with no console); nothing is opened. */

#include <atomic>
#include <functional>
#include <thread>
#include <mutex>
#include <string>
#include <vector>

namespace hub { namespace fg {

enum Sys { N64, GC, GBA, GB, GBC, NDS, N3DS, WIIU, PS2, PS1, NES, SNES, kSysCount };
const char* sys_label(int s);                       // "N64", "GameCube", ...
/* Lower-case extension with the dot: a Sys value, kArchive for .zip/.7z, or kNone. */
constexpr int kNone = -1, kArchive = -2;
int classify_ext(const std::string& lower_ext);

struct Counts {
    int n[kSysCount] = {};
    int archives = 0;
    int games() const { int t = 0; for (int v : n) t += v; return t; }   // files with a console-specific extension
    int total() const { return games() + archives; }
    void add(const Counts& o) { for (int i = 0; i < kSysCount; ++i) n[i] += o.n[i]; archives += o.archives; }
};

/* A place to look. library = the folder itself is a candidate (a Games / ROMs / emulator game path);
 * otherwise (Desktop, Documents, Downloads) only the folders below it are candidates, never the folder itself. */
struct Root { std::string path; bool library = false; };

/* Games found directly inside one directory (not its subfolders). */
struct DirCount { std::string path; Counts c; };

struct Folder {
    std::string path;
    Counts c;            // including everything nested below it
    bool ticked = false; // pre-ticked in the checklist
};

/* Groups per-directory counts into checklist rows: one row per top folder below a generic root (narrowed to the deepest
 * folder that still holds all of that group's files) or per library root; rows nested in another row merge into it.
 * Rows with neither a console game nor 3+ archives are dropped. Best first. Pure: no disk access. */
std::vector<Folder> collapse_folders(const std::vector<DirCount>& dirs, const std::vector<Root>& roots);

bool skip_dir_name(const std::string& name);        // system / huge folders never entered (case-insensitive)
std::string norm_path(const std::string& p);        // lower case, '\' separators, no trailing slash (for comparing)

/* Game folders the emulators themselves remember. Text in, paths out (no disk access). */
std::vector<std::string> parse_dolphin_ini_paths(const std::string& text);    // [General] ISOPath0..
std::vector<std::string> parse_cemu_xml_paths(const std::string& text);       // <GamePaths><Entry>..</Entry>
std::vector<std::string> parse_qt_gamedirs(const std::string& text);          // Azahar/Citra "Paths\gamedirs\N\path="

struct Options {
    int max_depth_generic = 4;     // below Desktop / Documents / Downloads
    int max_depth_library = 6;     // below a Games / ROMs folder
    int time_ms = 15000;           // whole scan
    int max_dirs = 60000;          // directories visited, whole scan
    int max_dir_entries = 20000;   // entries read from one directory
};

/* Shared between the worker and the UI thread. */
struct Control {
    std::atomic<bool> cancel{false}, done{false};
    std::atomic<int> dirs{0}, games{0};
    std::mutex mu;
    std::string current;           // folder being read (guarded by mu)
};

/* Owns the worker thread of a scan. The destructor (and stop()) cancels and joins, so a scan still running at app
 * exit can never reach std::terminate through a joinable std::thread member. */
struct ScanRunner {
    Control ctl;
    std::thread th;
    ScanRunner() = default;
    ScanRunner(const ScanRunner&) = delete;
    ScanRunner& operator=(const ScanRunner&) = delete;
    ~ScanRunner() { stop(); }
    void start(std::function<void()> job) { stop(); ctl.cancel = false; ctl.done = false; th = std::thread(std::move(job)); }
    void join() { if (th.joinable()) th.join(); }
    void stop() { ctl.cancel = true; join(); }   // cancel + join (no-op when idle)
};

/* Walks the roots (breadth first; junctions / symlinks, system folders and already-visited folders are skipped;
 * stops at the time / directory caps or on ctl->cancel) and returns collapse_folders() of what it saw. Sets ctl->done. */
std::vector<Folder> scan_roots(const std::vector<Root>& roots, Control* ctl, const Options& opt = Options());

/* The likely places on this PC. HUB_FINDGAMES_HOME=<dir> (testing) replaces the user profile and turns off the drive,
 * AppData and emulator-config lookups. */
std::vector<Root> default_roots();

}} // namespace hub::fg
