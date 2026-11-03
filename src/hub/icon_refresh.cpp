#include "app.h"
#include "icon_refresh_logic.h"
#include "hub_version.h"
#include "shell_integration.h"
#include "winutil.h"
#include <atomic>
#include <thread>

/* Taskbar / Start Menu icon refresh, once per version (icon_refresh_logic.h). Runs on a background thread, only when no game or session is
 * running, never under tests / headless / QA (shell_registration_allowed() also covers dev builds). It touches no window. */
namespace {
std::atomic<int> g_state{0};   // 0 not started, 1 running, 2 done
std::string g_recorded;
bool g_loaded = false;
std::string record_path() { return hub::data_dir() + "icon_refresh_version.txt"; }
bool env_on(const char* n) { const char* v = std::getenv(n); return v && *v && std::string(v) != "0"; }
}

void icon_refresh_tick() {
    if (g_state.load() == 2) return;
    if (g_state.load() == 1) return;
    if (g_headless || autotest_active() || env_on("RETROHUB_QA") || env_on("RETROHUB_TEST")) { g_state = 2; return; }
    if (!g_loaded) {
        g_loaded = true;
        winutil::read_file(record_path(), &g_recorded);
        while (!g_recorded.empty() && (g_recorded.back() == '\n' || g_recorded.back() == '\r' || g_recorded.back() == ' ')) g_recorded.pop_back();
    }
    const std::string cur = HUB_VERSION_STR;
    if (!hubicon::refresh_due(g_recorded, cur)) { g_state = 2; return; }
    static const double t0 = (double)GetTickCount64() / 1000.0;
    if ((double)GetTickCount64() / 1000.0 - t0 < 15.0) return;   // let the start-up work finish first
    if (game_active() || session_active()) return;               // never while a game runs: try again when it is over
    g_state = 1;
    const std::string path = record_path();
    std::thread([cur, path]() {
        std::string rec;
        const bool ran = hubicon::step(std::string(), cur, shell_registration_allowed(), false, [] { return shell_refresh_icons(); }, &rec);
        if (ran) winutil::write_file(path, rec);
        g_state = 2;   // a refused / failed attempt is retried on the next start, not in a loop
    }).detach();
}
