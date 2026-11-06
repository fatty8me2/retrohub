#pragma once
/* One process-wide politeness gate for every HTTP client in the app (cover art, GameTDB indexes, Wikipedia / Wikidata):
 *  - requests to the same site are spaced: gametdb.com (art. and www. share ONE limiter) 0.5 s, wikipedia.org / wikidata.org 1 s;
 *  - a 429 / 503 answer blocks the site until its Retry-After (30 s .. 1 h) has passed: acquire() then says "do not send".
 * Thread-safe. The Gate class takes the clock as an argument so a test can drive it without sleeping. */

#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace netgate {

using Clock = std::chrono::steady_clock;

/* "art.gametdb.com" -> "gametdb.com", "en.wikipedia.org" -> "wikipedia.org", anything else lower-cased as is. */
std::string site_of(const std::string& host);
int gap_ms(const std::string& site);   // 500 / 1000 / 0

class Gate {
public:
    /* false: the site is blocked (nothing may be sent). true: send after sleeping *wait (the slot is already reserved). */
    bool reserve(const std::string& host, Clock::time_point now, std::chrono::milliseconds* wait);
    void block(const std::string& host, Clock::time_point now, int retry_after_seconds);
private:
    std::mutex mu_;
    std::map<std::string, Clock::time_point> next_, blocked_;
};

/* The shared instance: reserve + sleep. false = blocked. */
bool acquire(const std::string& host);
void block(const std::string& host, int retry_after_seconds);
Gate& shared();

}  // namespace netgate
