#pragma once
/* Connection test runner (Settings > Online). One background job: STUN against two servers from ONE local UDP port,
 * then the lobby ping. Servers are parameters so tests run against loopback fakes. */
#include "nettest_logic.h"

#include <atomic>
#include <mutex>
#include <string>

namespace hubnt {

struct Report {
    bool done = false;
    Mapped a, b;
    Result verdict;
    int ping_ms = -1;
    std::string lobby;
};

/* Blocking: sends up to `tries` requests (timeout_ms each) to host:port and returns the mapped address; `sock` is shared between calls. */
Mapped stun_query(unsigned long long sock, const std::string& host_port, int tries = 3, int timeout_ms = 800);
/* Blocking whole test. lobby_url: ws(s)/http(s) URL of the lobby ("" = skip the ping). */
Report run_test(const std::string& stun1, const std::string& stun2, const std::string& lobby_url);

class Job {
public:
    void start(const std::string& lobby_url);   // real servers; no-op while running
    bool running() const { return running_; }
    bool has_report();
    Report report();
private:
    std::atomic<bool> running_{false};
    std::mutex mu_;
    Report rep_;
    bool have_ = false;
};

}  // namespace hubnt
