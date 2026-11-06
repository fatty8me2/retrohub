#include "netgate.h"

#include <algorithm>
#include <cctype>
#include <thread>

namespace netgate {

std::string site_of(const std::string& host) {
    std::string h = host;
    for (char& c : h) c = (char)std::tolower((unsigned char)c);
    for (const char* s : {"gametdb.com", "wikipedia.org", "wikidata.org"}) {
        std::string suf = s;
        if (h == suf || (h.size() > suf.size() && h.compare(h.size() - suf.size() - 1, suf.size() + 1, "." + suf) == 0)) return suf;
    }
    return h;
}

int gap_ms(const std::string& site) {
    if (site == "gametdb.com") return 500;
    if (site == "wikipedia.org" || site == "wikidata.org") return 1000;
    return 0;
}

bool Gate::reserve(const std::string& host, Clock::time_point now, std::chrono::milliseconds* wait) {
    const std::string site = site_of(host);
    std::lock_guard<std::mutex> l(mu_);
    auto b = blocked_.find(site);
    if (b != blocked_.end()) {
        if (now < b->second) return false;
        blocked_.erase(b);
    }
    Clock::time_point at = now;
    auto it = next_.find(site);
    if (it != next_.end() && it->second > at) at = it->second;
    const int gap = gap_ms(site);
    if (gap) next_[site] = at + std::chrono::milliseconds(gap);   // the next caller starts after this one + the gap
    *wait = std::chrono::duration_cast<std::chrono::milliseconds>(at - now);
    return true;
}

void Gate::block(const std::string& host, Clock::time_point now, int retry_after_seconds) {
    std::lock_guard<std::mutex> l(mu_);
    blocked_[site_of(host)] = now + std::chrono::seconds(std::max(30, std::min(retry_after_seconds, 3600)));
}

Gate& shared() {
    static Gate g;
    return g;
}

bool acquire(const std::string& host) {
    std::chrono::milliseconds w{0};
    if (!shared().reserve(host, Clock::now(), &w)) return false;
    if (w.count() > 0) std::this_thread::sleep_for(w);   // slept outside the lock: other sites are not held up
    return true;
}

void block(const std::string& host, int retry_after_seconds) { shared().block(host, Clock::now(), retry_after_seconds); }

}  // namespace netgate
