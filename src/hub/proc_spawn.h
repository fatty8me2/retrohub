#pragma once
/* Starts one child process the way RetroHub wants for an external emulator: created suspended, put in a job object BEFORE it runs
 * (a process it spawns in its first milliseconds would otherwise escape the job), then resumed. */

#include "plat/plat.h"

#if RH_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include "plat/plat_proc.h"
#endif

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hubproc {

#if RH_WIN

struct SpawnResult {
    HANDLE process = nullptr;        // caller closes
    HANDLE job = nullptr;            // caller closes; null when no job was asked for or it could not be used
    DWORD pid = 0;
    unsigned long long created = 0;  // process creation time (FILETIME ticks), 0 when unreadable
    DWORD error = 0;                 // GetLastError() of a failed CreateProcessW
};

/* `cmdline` is the writable command line (CreateProcessW may modify it). `flags` are the creation flags; CREATE_SUSPENDED is added.
 * `before_resume` (optional) runs while the child is still suspended and already in its job: the place to record its id durably.
 * The thread handle is closed. Returns false when the process could not be created (error set) or could not be resumed (then it is ended). */
bool spawn(const std::wstring& exe, std::wstring cmdline, const std::wstring& cwd, DWORD flags, bool want_job,
           const std::function<void(const SpawnResult&)>& before_resume, SpawnResult* out);

/* Creation time of a process handle in FILETIME ticks, false when unreadable. */
bool creation_time(HANDLE process, unsigned long long* out);

#else
/* macOS: the same job, with plat::proc (posix_spawn into its own process group; kill_tree on the group is the job-object counterpart).
 *
 * "Record the child id before it runs" is NOT fully possible here: posix_spawn has no suspended start (plat::proc::spawn returns once the child exists and is
 * already running). `after_spawn` therefore runs right after the spawn returns, with the pid, its process group and its start time (proc_pidinfo), so the id is
 * recorded durably within microseconds of the exec. The small window this leaves (the child runs a few milliseconds before the journal line exists) matters only
 * if the hub itself is killed inside it; a process the emulator starts in that window still belongs to the child's own process group (the group is created at
 * spawn, before the first instruction), so kill_tree() reaches it either way. The start time pairs with the pid to detect pid reuse. */
struct Spawned {
    plat::proc::Child child;       // pid, pgid, start_time_us
    uint64_t created = 0;          // process start time in microseconds (== child.start_time_us), 0 when unreadable
    int error = 0;                 // errno-style code of a failed spawn (0 when it worked)
    std::string error_text;        // strerror text of a failed spawn
    int pid() const { return child.pid; }
};

/* `opt` as plat::proc::SpawnOptions (exe, args, cwd, env, own_group, stdout/stderr files). Returns false when the process could not be started (error_text set).
 * `after_spawn` (optional) runs immediately after the process exists, before spawn() returns: the place to record its id durably. */
bool spawn(const plat::proc::SpawnOptions& opt, const std::function<void(const Spawned&)>& after_spawn, Spawned* out);

/* Start time of any process in microseconds, false when unreadable (the pid is gone). */
bool creation_time(int pid, uint64_t* out);
#endif

} // namespace hubproc
