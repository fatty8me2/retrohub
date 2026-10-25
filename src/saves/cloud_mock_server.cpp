/* Standalone mock Worker for qa\cloud_two_pc.ps1:  cloud_mock_server --port N --account uid:secret [--cap bytes]
 * Prints "listening <port>" and serves until killed (the script kills its own PID). Loopback only. */
#include "cloud_mock.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

int main(int argc, char** argv) {
    int port = 0;
    hub::saves::MockServer s;
    std::vector<std::pair<std::string, std::string>> accts;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--cap") && i + 1 < argc) s.account_cap = std::atoll(argv[++i]);
        else if (!std::strcmp(argv[i], "--account") && i + 1 < argc) {
            std::string a = argv[++i];
            size_t c = a.find(':');
            if (c != std::string::npos) accts.push_back({a.substr(0, c), a.substr(c + 1)});
        }
    }
    if (!s.start(port)) { std::fprintf(stderr, "cannot listen on %d\n", port); return 1; }
    for (auto& a : accts) s.add_account(a.first, a.second);
    std::printf("listening %d\n", s.port());
    std::fflush(stdout);
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
