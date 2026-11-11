/* Test helper for hub_crashlog_test: a tiny windowless program that installs the hub log and the crash handler and then dies on purpose.
 * Not part of RetroHub. Usage: hub_crashlog_test_helper.exe <logs dir> <av|terminate|terminate-call|overflow> . Never shows a dialog. */
#include "crash_report.h"
#include "hub_log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <stdexcept>
#include <string>

static std::string to_utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 1 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

static const char* phase() { return "helper_phase"; }

#pragma optimize("", off)
static int recurse(volatile int depth) {
    volatile char pad[512];
    pad[0] = (char)depth;
    return recurse(depth + 1) + pad[0];
}
#pragma optimize("", on)

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);   // no Windows crash dialog, ever
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 3) return 2;
    const std::string dir = to_utf8(argv[1]);
    const std::wstring mode = argv[2];
    hublog::Options o;
    o.worker = false;   // exercise the crash-time flush
    hublog::init(dir, "9.9.9", o);
    crashrep::install(dir, "9.9.9", phase);
    wchar_t up[MAX_PATH * 2] = L"";
    GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH * 2);
    hublog::info("helper started, profile %s\\AppData peer 192.168.1.5", to_utf8(up).c_str());
    hublog::info("about to crash: %s", to_utf8(mode.c_str()).c_str());
    if (mode == L"av") {
        *(volatile int*)nullptr = 1;
    } else if (mode == L"terminate") {
        throw std::runtime_error("boom");
    } else if (mode == L"terminate-call") {
        std::terminate();
    } else if (mode == L"overflow") {
        return recurse(0);
    }
    return 3;
}
