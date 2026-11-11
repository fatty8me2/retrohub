#pragma once
/* Starts one child process the way RetroHub wants for an external emulator: created suspended, put in a job object BEFORE it runs
 * (a process it spawns in its first milliseconds would otherwise escape the job), then resumed. */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <string>

namespace hubproc {

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

} // namespace hubproc
