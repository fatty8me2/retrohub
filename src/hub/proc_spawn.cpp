#include "proc_spawn.h"

#include <vector>

namespace hubproc {

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
    creation_time(pi.hProcess, &out->created);
    if (want_job) {   // follow every process it starts: in the job before its first instruction runs
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job && !AssignProcessToJobObject(job, pi.hProcess)) { CloseHandle(job); job = nullptr; }
        out->job = job;
    }
    if (before_resume) before_resume(*out);
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

} // namespace hubproc
