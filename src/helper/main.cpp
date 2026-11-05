/* rh-helper: tiny console program that RetroHub launches instead of a script interpreter.
 *   rh-helper.exe run <script name> <args...>
 * Runs ONE bundled script from <this exe's dir>\data\helpers\ (plain file name, fixed allowlist, see rh_helper_logic.h) with the
 * Windows PowerShell host, waits for it and returns its exit code. The child lives in a kill-on-close Job object, so when the hub
 * kills this process by PID the script dies with it. It uses the desktop it was started on (the hub's hidden desktop) and has no
 * window of its own. Exit codes: 0.. the script's, 2 usage, 3 script not allowed, 4 script file missing, 5 could not start. */
#include <windows.h>

#include <cstdio>
#include <string>

#include "rh_helper_logic.h"

static std::wstring exe_dir() {
    std::wstring p(512, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, &p[0], (DWORD)p.size());
        if (n == 0) return L"";
        if (n < p.size()) { p.resize(n); break; }
        p.resize(p.size() * 2);
    }
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"" : p.substr(0, k + 1);
}

static std::wstring powershell_exe() {
    wchar_t sys[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring p = std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return L"powershell.exe";
}

int wmain() {
    std::string name;
    std::wstring tail;
    if (!rhhelper::parse_run(GetCommandLineW(), &name, &tail)) { fputs("usage: rh-helper run <script> [args]\n", stderr); return 2; }
    if (!rhhelper::script_allowed(name)) { fputs("rh-helper: script not allowed\n", stderr); return 3; }
    const std::wstring script = rhhelper::script_path(exe_dir(), name);
    if (script.empty() || GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) { fputs("rh-helper: script file missing\n", stderr); return 4; }

    std::wstring cmd = L"\"" + powershell_exe() + L"\" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + script + L"\"";
    if (!tail.empty()) cmd += L" " + tail;

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li)) { CloseHandle(job); job = nullptr; }
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;   // lpDesktop stays null: the child inherits this process's desktop (the hub's hidden one when it set one)
    PROCESS_INFORMATION pi{};
    /* CREATE_NO_WINDOW: a console child must not get a visible console (Windows Terminal as default host would pop one up).
     * CREATE_SUSPENDED: it joins the job before its first instruction, so nothing it spawns can escape. */
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        fputs("rh-helper: could not start the script host\n", stderr);
        return 5;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);   // if this fails the script still runs, just without kill-with-parent
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    if (!GetExitCodeProcess(pi.hProcess, &code)) code = 1;
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    return (int)code;
}
