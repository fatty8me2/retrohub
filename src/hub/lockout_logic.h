#pragma once
/* Whole-app lock: pure verdict state machine (no network, no UI, no files). See docs/ADMIN.md "Locking the whole app".
 *
 * The server is the real gate. This is the client side of it: a banned or not-yet-approved account sees only a lock screen and nothing
 * can start a game. A modified client can skip it; the server still refuses that account everything. */

#include "admin_logic.h"

#include <string>

namespace hublock {

using hubnet::Access;

enum class Lock { None, Pending, Unavailable, Setup };

/* What is remembered between runs (access.json in the data dir). verdict: "ok" | "pending" | "unavailable" | "setup" | "" (nothing known).
 * "setup" = a brand-new install that has not yet been told Ok: it is written the moment the account is created, so the Setup lock survives
 * a restart or going offline before the first Ok. An upgrader (identity but no access.json) never gets one. */
struct Cache {
    std::string verdict;
    std::string uid;        // the account the verdict belongs to
    bool admin = false;     // this account has been the owner: never locked
};

inline std::string cache_json(const Cache& c)
{
    return "{\"verdict\":\"" + c.verdict + "\",\"uid\":\"" + c.uid + "\",\"admin\":" + (c.admin ? "true" : "false") + "}";
}
inline std::string cache_field(const std::string& t, const char* key)
{
    const std::string k = std::string("\"") + key + "\":\"";
    size_t p = t.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    size_t e = t.find('"', p);
    return e == std::string::npos ? std::string() : t.substr(p, e - p);
}
/* Lenient: anything unreadable gives an empty cache (= nothing known). */
inline Cache cache_parse(const std::string& t)
{
    Cache c;
    const std::string v = cache_field(t, "verdict");
    if (v == "ok" || v == "pending" || v == "unavailable" || v == "setup") c.verdict = v;
    c.uid = cache_field(t, "uid");
    c.admin = t.find("\"admin\":true") != std::string::npos;
    return c;
}

class Verdict {
public:
    /* At startup, BEFORE the friends client creates an identity.
     *   cache        what access.json said (empty verdict = never had one)
     *   had_account  an identity already existed on this PC (an install upgrading from before the lock)
     *   uid          the identity's id ("" if unknown yet); a cache written for another account is ignored */
    void init(const Cache& cache, bool had_account, const std::string& uid)
    {
        had_account_ = had_account;
        uid_ = uid;
        state_ = State::Unknown;
        admin_ = false;
        bool marked = false;   // access.json already says "setup" for this account
        if (cache.uid.empty() || uid.empty() || cache.uid == uid) {
            if (cache.verdict == "setup") { had_account_ = false; marked = true; }   // never confirmed: still a new install, whatever the identity file says now
            else if (cache.verdict == "ok") state_ = State::Ok;
            else if (cache.verdict == "pending") state_ = State::Pending;
            else if (cache.verdict == "unavailable") state_ = State::Unavailable;
            admin_ = cache.admin;
        }
        if (state_ == State::Ok) had_account_ = true;
        dirty_ = state_ == State::Unknown && !had_account_ && !marked;   // a new install: write the "setup" marker now
    }

    /* Every tick, from the friends client: `welcomed` = the server accepted our hello, `live` = why it refused it (Ok when it did not). */
    void on_live(Access live, bool welcomed, bool admin)
    {
        State n = state_;
        bool a = admin_;
        if (welcomed) {
            n = State::Ok;
            had_account_ = true;   // confirmed by the server: from now on an account change is not a new install
            a = admin;     // the welcome says who the owner is: an account that stopped being one is no longer exempt
        } else if (live == Access::Pending) n = State::Pending;
        else if (live == Access::Unavailable) n = State::Unavailable;
        if (n != state_ || a != admin_) dirty_ = true;
        state_ = n;
        admin_ = a;
    }

    /* The account changed (identity adopted / regenerated): what was known no longer applies. */
    void reset_account(const std::string& uid)
    {
        init(Cache(), had_account_, uid);   // a new install stays a new install (e.g. a friend-code collision during setup)
        dirty_ = true;
    }

    Lock lock() const
    {
        if (admin_) return Lock::None;
        switch (state_) {
            case State::Pending: return Lock::Pending;
            case State::Unavailable: return Lock::Unavailable;
            case State::Ok: return Lock::None;
            case State::Unknown: break;
        }
        return had_account_ ? Lock::None : Lock::Setup;   // an existing account is trusted until the server says otherwise; a new install must be told Ok first
    }
    bool locked() const { return lock() != Lock::None; }

    /* True once after a change worth writing to disk; fills `out`. */
    bool take_dirty(Cache* out)
    {
        if (!dirty_) return false;
        dirty_ = false;
        out->uid = uid_;
        out->admin = admin_;
        out->verdict = state_ == State::Ok ? "ok" : state_ == State::Pending ? "pending" : state_ == State::Unavailable ? "unavailable" : !had_account_ ? "setup" : "";
        return !out->verdict.empty();
    }
    bool admin() const { return admin_; }

private:
    enum class State { Unknown, Ok, Pending, Unavailable };
    State state_ = State::Unknown;
    bool had_account_ = true, admin_ = false, dirty_ = false;
    std::string uid_;
};

/* ---- words ---- */
inline const char* lock_title(Lock l, bool trying = false)
{
    return l == Lock::Pending ? "Waiting for approval" : l == Lock::Unavailable ? "This account can't use RetroHub" : l == Lock::Setup ? (trying ? "Setting up RetroHub" : "Connect to the internet to finish setting up") : "";
}
inline const char* lock_message(Lock l, bool trying)
{
    if (l == Lock::Pending) return "The owner needs to approve this account before you can use RetroHub. Tell them the name and friend code below so they know it is you.";
    if (l == Lock::Unavailable) return "";
    if (l == Lock::Setup) return trying ? "Setting up your account..." : "RetroHub needs to reach the internet once to set up your account. Check your connection, then try again.";
    return "";
}

} // namespace hublock
