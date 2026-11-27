#pragma once
/* Child processes, other processes, and other apps' windows on macOS (docs/MACOS.md). Windows hub code keeps its CreateProcessW /
 * job object / EnumWindows code under `#if RH_WIN`; macOS branches call this instead. Part of hubbase on macOS only
 * (plat_proc_mac.mm). Paths and arguments are UTF-8. */

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace plat {
namespace proc {

struct SpawnOptions {
    std::string exe;                                          // absolute path to an executable, or to a .app bundle
    std::vector<std::string> args;                            // argv[1..]; never shell-parsed
    std::string cwd;                                          // empty = inherit
    std::vector<std::pair<std::string, std::string>> env_set; // added / overridden on top of the hub's environment
    std::vector<std::string> env_unset;
    /* Put the child in its own process group, so kill_tree() ends it and everything it starts (the job-object equivalent). */
    bool own_group = true;
    /* Optional: redirect stdout / stderr to these files (appended; created if missing). Empty = inherit / /dev/null when hidden. */
    std::string stdout_path, stderr_path;
    /* A .app bundle (or exe inside one) is launched through LaunchServices-free posix_spawn of its CFBundleExecutable, so the
     * pid is ours to wait on. activate=false keeps it from taking focus at launch (hidden QA runs). */
    bool activate = true;
};

/* A started child. Copyable handle (the pid); destroying it never kills the child. */
struct Child {
    int pid = 0;
    int pgid = 0;                 // process group (== pid when own_group)
    uint64_t start_time_us = 0;   // process start time (from proc_pidinfo), 0 when unknown: pairs with pid to detect pid reuse
    bool valid() const { return pid > 0; }
};

/* false + *err (strerror text) when the child could not be started. */
bool spawn(const SpawnOptions& o, Child* out, std::string* err);

/* Waits up to timeout_ms (-1 = forever) for the child to exit. true when it exited; *exit_code = exit status, or 128+signal. Reaps it. */
bool wait(const Child& c, int timeout_ms, int* exit_code);
bool running(const Child& c);   // false once exited (reaps a zombie child)

/* SIGTERM to the group (or the pid), then SIGKILL after grace_ms if still alive. Returns once it is gone or the grace expired. */
void kill_tree(const Child& c, int grace_ms = 1500);

/* ---- any process (not only our children) ---- */
bool pid_alive(int pid);
uint64_t pid_start_time_us(int pid);                     // 0 when unknown
std::string pid_exe_path(int pid);                       // "" when unknown (proc_pidpath)
std::vector<int> pids_by_exe_name(const std::string& name);   // executable file name, case-insensitive (e.g. "Dolphin")
bool kill_pid(int pid, bool force);                      // SIGTERM, or SIGKILL when force

/* ---- the desktop ---- */
bool open_url(const std::string& url);          // default browser / handler
bool open_path(const std::string& path);        // Finder / default app
bool reveal_in_finder(const std::string& path);

/* ---- other apps' windows (NSRunningApplication; AXUIElement / CGEvent need the Accessibility permission) ---- */
bool activate_pid(int pid);                     // bring the app to the front (SetForegroundWindow equivalent)
bool hide_pid(int pid);                         // hide the app (ShowWindow SW_HIDE / minimise equivalent)
bool pid_has_window(int pid);                   // has at least one on-screen window (CGWindowListCopyWindowInfo; no permission needed)
std::string pid_window_title(int pid);          // title of its frontmost window, "" when unknown (needs Screen Recording on macOS 10.15+)
bool accessibility_trusted(bool prompt);        // AXIsProcessTrustedWithOptions
/* Sends a key press (down + up) to the app owning pid, using a CGEvent posted to that pid. Needs accessibility_trusted().
 * keycode: macOS virtual key code (kVK_*); mods: bit 0 shift, 1 control, 2 option, 3 command. */
bool send_key(int pid, int keycode, unsigned mods);

}  // namespace proc
}  // namespace plat
