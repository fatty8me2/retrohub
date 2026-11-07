// QA-only console harness (NOT an add_test): runs the real Hookshot build pipeline end to end on a game file.
// Usage: hub_hookshot_e2e <game file> [language]. data_dir() is <exe dir>\userdata, so run it from an isolated copy.
#include <windows.h>
#include "games.h"
#include "hookshot.h"
#include "hookshot_build.h"
#include "library.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;

static const char* state_name(hookshot::BuildState s) {
    using S = hookshot::BuildState;
    switch (s) {
        case S::Idle: return "Idle"; case S::Preparing: return "Preparing"; case S::Downloading: return "Downloading";
        case S::Checking: return "Checking"; case S::Building: return "Building"; case S::Paused: return "Paused";
        case S::Done: return "Done"; case S::Failed: return "Failed"; case S::Cancelled: return "Cancelled";
    }
    return "?";
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: hub_hookshot_e2e <game file> [language]\n"); return 2; }
    std::string lang = argc >= 3 ? argv[2] : "en-US";
    hub::GameEntry e;
    e.system = hub::System::Switch;
    e.key = "switch:0400486024dec000";
    e.path = argv[1];
    e.nx2 = true;
    e.title = "The Legend of Zelda: Ocarina of Time";

    auto r = hookshot::check_requirements(e);
    std::printf("requirements: vs=%d sdk=%d vulkan_mesh=%d gpu=%s ram=%llu free=%llu game_ok=%d problem='%s' fix='%s'\n", r.vs_tools, r.win_sdk,
                r.vulkan_mesh, r.gpu_name.c_str(), (unsigned long long)r.ram_gb, (unsigned long long)r.free_gb, r.game_ok, r.problem.c_str(),
                r.fix_hint.c_str());
    std::fflush(stdout);

    std::string err;
    if (!hookshot::start_build(e, lang, &err)) { std::printf("start_build failed: %s\n", err.c_str()); return 1; }
    auto t0 = std::chrono::steady_clock::now(), last_print = t0;
    std::string prev;
    hookshot::BuildStatus st;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        st = hookshot::build_status();
        char buf[512];
        std::snprintf(buf, sizeof buf, "%s|%s|%.3f|%.3f|%d", state_name(st.state), st.stage_id.c_str(), st.progress, st.overall, (int)st.paused_for_game);
        auto now = std::chrono::steady_clock::now();
        bool minute = now - last_print >= std::chrono::minutes(1);
        if (prev != buf || minute) {
            std::printf("[%5llds] state=%s stage=%s '%s' progress=%.3f overall=%.3f paused=%d\n",
                        (long long)std::chrono::duration_cast<std::chrono::seconds>(now - t0).count(), state_name(st.state), st.stage_id.c_str(),
                        st.stage.c_str(), st.progress, st.overall, (int)st.paused_for_game);
            std::fflush(stdout);
            prev = buf;
            last_print = now;
        }
        if (!hookshot::build_running() && (st.state == hookshot::BuildState::Done || st.state == hookshot::BuildState::Failed ||
                                           st.state == hookshot::BuildState::Cancelled))
            break;
    }
    std::string why;
    bool ok = hookshot::ready(&why);
    hookshot::Stamp stamp;
    bool has_stamp = hookshot::read_stamp(hookshot::game_dir(), &stamp);
    std::printf("final state=%s error='%s' log=%s\n", state_name(st.state), st.error.c_str(), st.log_path.c_str());
    std::printf("ready=%d why='%s'\n", ok, why.c_str());
    if (has_stamp) std::printf("stamp: commit=%s game_build=%s built=%lld size=%llu source=%s\n", stamp.commit.c_str(), stamp.game_build.c_str(),
                               (long long)stamp.built, (unsigned long long)stamp.size, stamp.source.c_str());
    else std::printf("stamp: none\n");
    std::error_code ec;
    for (auto& d : fs::directory_iterator(hookshot::game_dir(), ec)) std::printf("  %s%s\n", d.path().filename().string().c_str(), d.is_directory() ? "\\" : "");
    std::fflush(stdout);
    return (st.state == hookshot::BuildState::Done && ok) ? 0 : 1;
}
