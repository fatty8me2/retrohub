#pragma once
/* In-process mock of the cloud saves v2 Worker (docs/CLOUD_SAVES_V2.md section 3) over real loopback HTTP, for the engine
 * unit tests and qa\cloud_two_pc.ps1 (via cloud_mock_server.exe). Verifies the RHSIG signature like the Worker does.
 * Test code only: never shipped. */
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hub::saves {

class MockServer {
public:
    MockServer();
    ~MockServer();
    bool start(int port = 0);              // 0 = pick a free port
    void stop();
    int port() const { return port_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }
    void add_account(const std::string& uid, const std::string& secret);

    /* knobs (thread-safe) */
    std::atomic<int> delay_ms{0};          // sleep before answering every request
    std::atomic<int> fail_status{0};       // answer this status (JSON error) to the next fail_count requests
    std::atomic<int> fail_count{0};
    std::atomic<int> truncate_get{0};      // blob GET: send only N body bytes of the first such request, then drop the connection
    std::atomic<int> put_limit{0};         // blob PUTs allowed per account (0 = unlimited), then 429 account-daily-limit
    std::atomic<int64_t> account_cap{50ll << 20};
    std::atomic<int64_t> prot_cap{20ll << 20};
    std::atomic<int> lease_secs{720};
    std::atomic<int> sig_checks_failed{0};

    /* counters / inspection */
    std::atomic<int> n_manifest{0}, n_plan{0}, n_put{0}, n_get{0}, n_commit{0}, n_lease{0}, n_revoke{0}, n_devices{0};
    std::string cur_sha(const std::string& uid, const std::string& gkey, const std::string& path);   // "" when none
    int cur_ver(const std::string& uid, const std::string& gkey, const std::string& path);
    int versions(const std::string& uid, const std::string& gkey, const std::string& path);
    bool evicted(const std::string& uid, const std::string& gkey, const std::string& path);
    int64_t used(const std::string& uid);
    int devices(const std::string& uid);
    int revoked(const std::string& uid);   // device ids signed out with POST /saves/v2/device/revoke
    void seed_file(const std::string& uid, const std::string& gkey, const std::string& path, const std::string& plain,
                   int64_t mtime, const std::string& device_name, int64_t played_at);   // as if another PC had pushed it
    void set_played(const std::string& uid, const std::string& gkey, int64_t played_at);
    void expire_leases();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    int port_ = 0;
};

} // namespace hub::saves
