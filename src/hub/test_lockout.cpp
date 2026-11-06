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
        w.reset_account("u9");                               // identity replaced
        CHECK(w.lock() == Lock::None);
        Verdict n; n.init(cache("pending", "u1"), false, "u1");
        n.reset_account("u9");                               // replaced (e.g. friend-code collision) before any Ok: still a new install
        CHECK(n.lock() == Lock::Setup);
        Cache o; CHECK(n.take_dirty(&o) && o.verdict == "setup" && o.uid == "u9");   // and the marker follows the new id
        Verdict k; k.init(Cache(), true, "u1");              // an upgrader whose identity is replaced stays trusted
        k.reset_account("u9");
        CHECK(k.lock() == Lock::None);
        Verdict c; c.init(Cache(), false, "u1");             // new install confirmed Ok, then adopts another PC's identity: not a new install any more
        c.on_live(Access::Ok, true, false);
        c.reset_account("u9");
        CHECK(c.lock() == Lock::None);
    }
    // the "setup" marker: a new install stays locked across restarts / offline until its first Ok; an upgrader is never marked
    {
        Verdict v; v.init(Cache(), false, "u2");             // first run: identity was just created
        Cache m; CHECK(v.take_dirty(&m) && m.verdict == "setup" && m.uid == "u2");
        CHECK(!v.take_dirty(&m));
        // quit offline; next launch: the identity file now exists (had_account=true) but access.json says setup
        Verdict v2; v2.init(m, /*had_account=*/true, "u2");
        CHECK(v2.lock() == Lock::Setup);
        Cache m2; CHECK(!v2.take_dirty(&m2));                // marker already on disk
        v2.on_live(Access::Ok, false, false);
        CHECK(v2.lock() == Lock::Setup);
        v2.on_live(Access::Ok, true, false);                 // first Ok
        CHECK(v2.lock() == Lock::None && v2.take_dirty(&m2) && m2.verdict == "ok");
        Verdict v3; v3.init(m2, true, "u2");
        CHECK(v3.lock() == Lock::None);
        // a marker for another account id (stale) is ignored, and an upgrader with no file is unlocked
        Verdict v4; v4.init(m, true, "other");
        CHECK(v4.lock() == Lock::None);
        Verdict up; up.init(Cache(), true, "u1");
        Cache none; CHECK(up.lock() == Lock::None && !up.take_dirty(&none));
        // a refusal over a marked install becomes Pending, then Ok
        Verdict v5; v5.init(m, true, "u2");
        v5.on_live(Access::Pending, false, false);
        CHECK(v5.lock() == Lock::Pending);
        Cache c5; CHECK(cache_parse(cache_json(m)).verdict == "setup");
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
