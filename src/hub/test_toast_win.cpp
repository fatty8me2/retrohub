/* Exercises the real toast path's COM setup (MTA worker thread + XmlDocument load) without showing a toast. */
#include <cstdio>
#include <string>
#include <windows.h>
#include <objbase.h>
#include "toast.h"

int main() {
    int fails = 0;
    std::string xml = hubtoast::build_invite_xml("Bob", "Smash 64", "ZXCV23");
    std::string err;
    // Also from a thread that already owns an STA (like the UI thread): must still work.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!hubtoast::toast_winrt_selftest(xml, &err)) { std::printf("FAIL: init+xml load (STA caller): %s\n", err.c_str()); ++fails; }
    if (hubtoast::toast_winrt_selftest("<toast><unclosed>", &err)) { std::printf("FAIL: malformed xml accepted\n"); ++fails; }
    if (!hubtoast::toast_winrt_selftest(xml, &err)) { std::printf("FAIL: second call\n"); ++fails; }
    CoUninitialize();
    std::printf(fails ? "hub_toast_win_test FAILED\n" : "hub_toast_win_test OK\n");
    return fails ? 1 : 0;
}
