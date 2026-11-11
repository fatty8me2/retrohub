/* Exercises the real toast path's COM setup (MTA worker thread + XmlDocument load) without showing a toast. */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <windows.h>
#include <objbase.h>
#include "toast.h"
#include "toast_worker.h"

int main() {
    int fails = 0;
    std::string xml = hubtoast::build_invite_xml("Bob", "Smash 64", "ZXCV23");
    std::string err;
    // Also from a thread that already owns an STA (like the UI thread): must still work.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!hubtoast::toast_winrt_selftest(xml, &err)) { std::printf("FAIL: init+xml load (STA caller): %s\n", err.c_str()); ++fails; }
    if (hubtoast::toast_winrt_selftest("<toast><unclosed>", &err)) { std::printf("FAIL: malformed xml accepted\n"); ++fails; }
    if (!hubtoast::toast_winrt_selftest(xml, &err)) { std::printf("FAIL: second call\n"); ++fails; }
    // The scheduler refuses while a game runs (same gate as show()). Probe says "game up", so this can never create a toast.
    hubtoast::set_game_probe([]() { return true; });
    if (hubtoast::system_scheduler().schedule(xml, "gntest", "gamenight", 4102444800000LL)) { std::printf("FAIL: schedule accepted during a game\n"); ++fails; }
    hubtoast::set_game_probe(nullptr);
    CoUninitialize();
    // The toast worker: post() never waits for a running job, jobs run in order on ONE thread that is not the caller, stop() drains and joins.
    {
        std::atomic<int> seen{0}, order_ok{1};
        std::atomic<bool> release{false};
        std::thread::id worker_id;
        std::atomic<int> starts{0}, exits{0};
        {
            hubtoast::SerialWorker w(3, [&] { ++starts; }, [&] { ++exits; });
            const auto t0 = std::chrono::steady_clock::now();
            bool p1 = w.post([&] { worker_id = std::this_thread::get_id(); while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5)); seen = 1; });
            bool p2 = w.post([&] { if (seen != 1) order_ok = 0; seen = 2; });
            bool p3 = w.post([&] { if (seen != 2) order_ok = 0; seen = 3; });
            bool p4 = w.post([] {});   // cap 3 pending: the first may already be running, so allow either answer here
            (void)p4;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (!p1 || !p2 || !p3) { std::printf("FAIL: post refused\n"); ++fails; }
            if (ms > 500) { std::printf("FAIL: post blocked behind a running job (%lld ms)\n", (long long)ms); ++fails; }
            if (seen != 0) { std::printf("FAIL: job ran before release\n"); ++fails; }
            release = true;
            w.stop();   // drains the queue, then joins
            if (seen != 3 || !order_ok) { std::printf("FAIL: jobs not run in order / not drained (seen=%d)\n", seen.load()); ++fails; }
            if (worker_id == std::this_thread::get_id()) { std::printf("FAIL: job ran on the caller thread\n"); ++fails; }
            if (starts != 1 || exits != 1) { std::printf("FAIL: on_start/on_exit not once each\n"); ++fails; }
            if (w.post([] {})) { std::printf("FAIL: post accepted after stop\n"); ++fails; }
            w.stop();   // second stop is harmless
        }
        hubtoast::SerialWorker idle;   // never posted to: no thread, destructor is a no-op
    }
    std::printf(fails ? "hub_toast_win_test FAILED\n" : "hub_toast_win_test OK\n");
    return fails ? 1 : 0;
}
