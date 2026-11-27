#include "proc_spawn.h"

#include <vector>

namespace hubproc {

#if RH_WIN

bool creation_time(HANDLE process, unsigned long long* out) {
    FILETIME c, x, k, u;
    if (!process || !GetProcessTimes(process, &c, &x, &k, &u)) return false;
    *out = ((unsigned long long)c.dwHighDateTime << 32) | c.dwLowDateTime;
    return true;
}

bool spawn(const std::wstring& exe, std::wstring cmdline, const std::wstring& cwd, DWORD flags, bool want_job,
           const std::function<void(const SpawnResult&)>& before_resume, SpawnResult* out) {
    *out = SpawnResult();
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, flags | CREATE_SUSPENDED, nullptr, cwd.c_str(), &si, &pi);
    out->error = GetLastError();
    if (!ok) return false;
    out->process = pi.hProcess;
    out->pid = pi.dwProcessId;
    try {
        creation_time(pi.hProcess, &out->created);
        if (want_job) {   // follow every process it starts: in the job before its first instruction runs
            HANDLE job = CreateJobObjectW(nullptr, nullptr);
            if (job && !AssignProcessToJobObject(job, pi.hProcess)) { CloseHandle(job); job = nullptr; }
            out->job = job;
        }
        if (before_resume) {
            try { before_resume(*out); } catch (...) {}   // a failing observer must not leave the player's game suspended
        }
    } catch (...) {   // anything unexpected after the process exists: end it by handle, never leave it suspended
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (out->job) CloseHandle(out->job);
        *out = SpawnResult();
        out->error = ERROR_INTERNAL_ERROR;
        return false;
    }
    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        const DWORD err = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (out->job) CloseHandle(out->job);
        *out = SpawnResult();
        out->error = err;
        return false;
    }
    CloseHandle(pi.hThread);
    return true;
}

#else

bool creation_time(int pid, uint64_t* out) {
    const uint64_t t = plat::proc::pid_start_time_us(pid);
    if (!t) return false;
    *out = t;
    return true;
}

bool spawn(const plat::proc::SpawnOptions& opt, const std::function<void(const Spawned&)>& after_spawn, Spawned* out) {
    *out = Spawned();
    std::string why;
    if (!plat::proc::spawn(opt, &out->child, &why)) {
        out->error = 1;
        out->error_text = why.empty() ? "could not start the process" : why;
        return false;
    }
    out->created = out->child.start_time_us;
    if (!out->created) creation_time(out->child.pid, &out->created);
    if (after_spawn) {
        try { after_spawn(*out); } catch (...) {}   // a failing observer must not end the player's game (same as the Windows path)
    }
    return true;
}

#endif

} // namespace hubproc
