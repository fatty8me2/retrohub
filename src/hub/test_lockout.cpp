/* hub_lockout_test: the whole-app lock verdict state machine (pure logic, no network, no files). */
#include "lockout_logic.h"

#include <cstdio>

using namespace hublock;
using hubnet::Access;

static int g_fail = 0;
#define CHECK(c) do { if (c) printf("  ok   %s\n", #c); else { g_fail++; printf("  FAIL %s\n", #c); } } while (0)

static Cache cache(const char* verdict, const char* uid = "u1", bool admin = false) { Cache c; c.verdict = verdict; c.uid = uid; c.admin = admin; return c; }

int main()
{
    // upgrade from v1.31.10: an account exists, nothing cached -> trusted, even offline
    {
        Verdict v; v.init(Cache(), /*had_account=*/true, "u1");
        CHECK(v.lock() == Lock::None);
        v.on_live(Access::Ok, /*welcomed=*/false, false);   // still offline / connecting
        CHECK(v.lock() == Lock::None);
        Cache out;
        CHECK(!v.take_dirty(&out));                          // nothing learned yet: nothing written
        v.on_live(Access::Ok, true, false);
        CHECK(v.lock() == Lock::None && v.take_dirty(&out) && out.verdict == "ok" && out.uid == "u1");
        CHECK(!v.take_dirty(&out));                          // written once
    }
    // upgrade, then the server says pending (approval on, account too new to be grandfathered) or banned
    {
        Verdict v; v.init(Cache(), true, "u1");
        v.on_live(Access::Unavailable, false, false);
        CHECK(v.lock() == Lock::Unavailable);
        Cache out; CHECK(v.take_dirty(&out) && out.verdict == "unavailable");
    }
    // fresh install: no account, nothing cached -> locked ("connect to the internet") until the first Ok
    {
        Verdict v; v.init(Cache(), /*had_account=*/false, "u2");
        CHECK(v.lock() == Lock::Setup && v.locked());
        v.on_live(Access::Ok, false, false);                 // offline
        CHECK(v.lock() == Lock::Setup);
        v.on_live(Access::Pending, false, false);            // the server answered: waiting for approval
        CHECK(v.lock() == Lock::Pending);
        v.on_live(Access::Ok, true, false);                  // approved
        CHECK(v.lock() == Lock::None);
        Cache out; CHECK(v.take_dirty(&out) && out.verdict == "ok");
    }
    // cached verdicts apply at once, offline
    {
        Verdict p; p.init(cache("pending"), true, "u1");
        CHECK(p.lock() == Lock::Pending);
        p.on_live(Access::Ok, false, false);                 // not reachable: stays locked
        CHECK(p.lock() == Lock::Pending);
        p.on_live(Access::Ok, true, false);                  // reachable and welcomed: unlock
        CHECK(p.lock() == Lock::None);
        Verdict b; b.init(cache("unavailable"), true, "u1");
        CHECK(b.lock() == Lock::Unavailable);
        Verdict o; o.init(cache("ok"), true, "u1");
        CHECK(o.lock() == Lock::None);
        Verdict f; f.init(cache("ok"), false, "u1");         // cached Ok but no identity file: the cache still counts
        CHECK(f.lock() == Lock::None);
    }
    // ok -> banned -> ok
    {
        Verdict v; v.init(cache("ok"), true, "u1");
        v.on_live(Access::Ok, true, false);
        CHECK(v.lock() == Lock::None);
        v.on_live(Access::Unavailable, false, false);
        CHECK(v.lock() == Lock::Unavailable);
        v.on_live(Access::Unavailable, false, false);        // repeated: stays
        CHECK(v.lock() == Lock::Unavailable);
        v.on_live(Access::Ok, true, false);                  // unbanned
        CHECK(v.lock() == Lock::None);
    }
    // the owner is never locked, even by a cached or live refusal
    {
        Verdict v; v.init(cache("ok", "u1", true), true, "u1");
        v.on_live(Access::Ok, false, false);
        CHECK(v.admin() && v.lock() == Lock::None);
        Verdict w; w.init(cache("pending", "u1", true), true, "u1");
        CHECK(w.lock() == Lock::None);
        Verdict x; x.init(Cache(), false, "u1");
        x.on_live(Access::Ok, true, true);
        CHECK(x.admin() && x.lock() == Lock::None);
        x.on_live(Access::Pending, false, false);            // a refusal arrives while the admin flag is remembered
        CHECK(x.lock() == Lock::None);
        x.on_live(Access::Ok, true, false);                  // a welcome that says "not the owner" (ownership moved) ends the exemption
        CHECK(!x.admin());
    }
    // a verdict cached for another account is ignored
    {
        Verdict v; v.init(cache("unavailable", "other"), true, "u1");
        CHECK(v.lock() == Lock::None);
        Verdict w; w.init(cache("unavailable", "u1"), true, "u1");
        CHECK(w.lock() == Lock::Unavailable);
        w.reset_account("u9", true);                         // identity replaced
        CHECK(w.lock() == Lock::None);
        Verdict n; n.init(cache("pending", "u1"), false, "u1");
        n.reset_account("u9", false);                        // replaced by a brand-new one: needs a first Ok
        CHECK(n.lock() == Lock::Setup);
    }
    // the cache file round trips and survives garbage
    {
        Cache c = cache("pending", "abc123", true);
        Cache r = cache_parse(cache_json(c));
        CHECK(r.verdict == "pending" && r.uid == "abc123" && r.admin);
        CHECK(cache_parse("").verdict.empty() && cache_parse("{not json").verdict.empty() && cache_parse("{\"verdict\":\"weird\"}").verdict.empty());
        CHECK(!cache_parse("{\"verdict\":\"ok\",\"admin\":false}").admin);
    }
    // words
    CHECK(std::string(lock_title(Lock::Pending)) == "Waiting for approval");
    CHECK(std::string(lock_title(Lock::Unavailable)) == "This account can't use RetroHub");
    CHECK(std::string(lock_message(Lock::Unavailable, false)).empty());   // banned: no detail
    CHECK(std::string(lock_title(Lock::Setup, false)).find("Connect to the internet") == 0);
    CHECK(std::string(lock_title(Lock::None)).empty());

    printf(g_fail ? "FAILED (%d)\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
