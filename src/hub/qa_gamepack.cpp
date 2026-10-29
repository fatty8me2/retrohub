// QA driver for GamePack against a local fake (RETROHUB_QA=1 RETROHUB_GP_BASE=127.0.0.1:<port>). Not a ctest.
//   qa_gamepack index  <links> <sys> <title>...      -> prints refresh latency, state, index_has per title
//   qa_gamepack install <links> <sys> <title> [cancel_after_bytes]
#include "gamepack.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
using namespace hubgp;
namespace fs = std::filesystem;
static long long ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int main(int argc, char** argv) {
    if (argc < 5) return 2;
    std::string mode = argv[1], links = argv[2], sys = argv[3];
    auto roots = parse_links(links);
    if (mode == "index") {
        long long t0 = ms(); index_refresh(roots); std::printf("refresh_call_ms=%lld\n", ms() - t0);
        for (int i = 0; i < 400 && index_state(sys) != IndexState::Ready; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::printf("state=%d after_ms=%lld source=%d\n", (int)index_state(sys), ms() - t0, (int)index_source(sys));
        for (int i = 4; i < argc; ++i) std::printf("has[%s]=%d\n", argv[i], (int)index_has(sys, argv[i]));
        index_shutdown();
        return 0;
    }
    std::string title = argv[4];
    uint64_t cancel_at = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 0;
    Installer in; long long t0 = ms(); in.start(sys, title, roots);
    long long maxpoll = 0; bool ok = false, cancelled = false;
    for (;;) {
        long long a = ms(); bool s = false; bool fin = in.poll(&s); maxpoll = std::max(maxpoll, ms() - a);
        if (fin) { ok = s; break; }
        if (cancel_at && !cancelled && in.got.load() >= cancel_at) { in.request_cancel(); cancelled = true; std::printf("cancel requested at got=%llu\n", (unsigned long long)in.got.load()); }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("ok=%d err=[%s] ms=%lld max_poll_ms=%lld installed=%d\n", (int)ok, in.error.c_str(), ms() - t0, maxpoll, (int)installed(sys, title));
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(fs::u8path(pack_root()), ec)) std::printf("  %s\n", e.path().u8string().c_str());
    return 0;
}
