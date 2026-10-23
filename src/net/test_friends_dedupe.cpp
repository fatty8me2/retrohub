/* hubfriends_dedupe_test: the friends list is keyed by uid (no network). */
#include "friends.h"

#include <cstdio>

using namespace hubnet;

static int g_fail = 0;
#define CHECK(c) do { if (c) printf("  ok   %s\n", #c); else { g_fail++; printf("  FAIL %s\n", #c); } } while (0)

static Friend mk(const char* uid, const char* name, PresenceState st, int64_t updated, int64_t since)
{
    Friend f;
    f.uid = uid; f.name = name; f.status.state = st; f.status.updated = updated; f.since = since;
    return f;
}
static Request rq(const char* uid, const char* name, int64_t at)
{
    Request r;
    r.uid = uid; r.name = name; r.at = at;
    return r;
}

int main()
{
    {   // same uid twice: one entry, most present status wins, earliest "since" kept
        std::vector<Friend> v = {mk("a", "Mia", PresenceState::Offline, 10, 500), mk("b", "Jo", PresenceState::Online, 1, 5),
                                 mk("a", "Mia", PresenceState::InGame, 5, 900), mk("a", "Mia", PresenceState::Online, 99, 100)};
        dedupe_friends(v);
        CHECK(v.size() == 2);
        CHECK(v[0].uid == "a" && v[1].uid == "b");
        CHECK(v[0].status.state == PresenceState::InGame);
        CHECK(v[0].since == 100);
    }
    {   // equal presence: newer update wins; empty uid and our own uid are dropped
        std::vector<Friend> v = {mk("a", "old", PresenceState::Online, 1, 0), mk("a", "new", PresenceState::Online, 2, 7), mk("", "x", PresenceState::Online, 1, 1),
                                 mk("me", "Me", PresenceState::Online, 1, 1)};
        dedupe_friends(v, "me");
        CHECK(v.size() == 1 && v[0].name == "new" && v[0].since == 7);
    }
    {   // idempotent
        std::vector<Friend> v = {mk("a", "A", PresenceState::Online, 1, 1), mk("a", "A", PresenceState::Offline, 1, 1)};
        dedupe_friends(v);
        std::vector<Friend> w = v;
        dedupe_friends(w);
        CHECK(v.size() == 1 && w.size() == 1 && w[0].uid == "a");
    }
    {   // requests: duplicates collapse; a friend has no request; incoming wins over outgoing
        std::vector<Friend> fr = {mk("f", "Fay", PresenceState::Online, 1, 1)};
        std::vector<Request> in = {rq("x", "Xi", 1), rq("x", "Xi", 5), rq("f", "Fay", 1), rq("both", "Bo", 1)};
        std::vector<Request> out = {rq("both", "Bo", 2), rq("f", "Fay", 2), rq("y", "Yu", 1), rq("y", "Yu", 2), rq("me", "Me", 1)};
        dedupe_requests(fr, in, out, "me");
        CHECK(in.size() == 2 && in[0].uid == "x" && in[0].at == 5 && in[1].uid == "both");
        CHECK(out.size() == 1 && out[0].uid == "y");
    }
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
