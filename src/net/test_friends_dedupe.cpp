/* hubfriends_dedupe_test: the friends list is keyed by uid (no network). */
#include "friends.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

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
    CHECK(retire_error_is_terminal("retire_old", "bad-secret"));
    CHECK(!retire_error_is_terminal("retire_old", "retire-failed"));   // transient: keep resending
    CHECK(!retire_error_is_terminal("retire_old", "rate-limited"));
    CHECK(!retire_error_is_terminal("add_friend", "bad-secret"));
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
    {   // same name, different uid (a person's old identity that was never retired)
        const int64_t DAY = 86400 * 1000LL;
        std::vector<Friend> v = {mk("old", "Camer", PresenceState::Offline, 100 * DAY, 1 * DAY), mk("new", "camer ", PresenceState::Online, 200 * DAY, 90 * DAY),
                                 mk("solo", "Solo", PresenceState::Offline, 1, 1)};
        auto st = stale_same_name(v);
        CHECK(st.size() == 1 && st[0] == "old");
        std::vector<Friend> both_off = {mk("a", "Mike", PresenceState::Offline, 50 * DAY, 1), mk("b", "Mike", PresenceState::Offline, 51 * DAY, 1)};
        CHECK(stale_same_name(both_off).empty());   // two real Mikes seen a day apart: keep both
        std::vector<Friend> far = {mk("a", "Mike", PresenceState::Offline, 10 * DAY, 1), mk("b", "mike", PresenceState::Offline, 50 * DAY, 1)};
        auto sf = stale_same_name(far);
        CHECK(sf.size() == 1 && sf[0] == "a");      // one has been gone for 40 days
        std::vector<Friend> two_on = {mk("a", "Ann", PresenceState::Online, 1, 1), mk("b", "Ann", PresenceState::Online, 2, 1)};
        CHECK(stale_same_name(two_on).empty());     // both present: never hide
    }
    {   // every older identity on the PC is found (pending file, backups, vault), minus the current and the confirmed ones
        namespace fs = std::filesystem;
        fs::path root = fs::temp_directory_path() / "rh_older_ids_test";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "data", ec);
        fs::create_directories(root / "vault" / "_identity", ec);
        auto idjson = [](char c, char d) { return std::string("{\"user_id\":\"") + std::string(26, c) + "\",\"secret\":\"" + std::string(52, d) + "\",\"created\":1}"; };
        auto put = [](const fs::path& p, const std::string& t) { std::ofstream(p, std::ios::binary) << t; };
        put(root / "data" / "identity.json", idjson('c', 'z'));
        put(root / "data" / "pending-retire.json", idjson('b', 'y'));
        put(root / "data" / "identity-backup-20260101-000000.json", idjson('a', 'x'));
        put(root / "data" / "identity-backup-20260102-000000.json", idjson('b', 'y'));       // same uid as pending: once
        put(root / "data" / "identity-backup-20260103-000000.json", idjson('c', 'z'));       // the current one: skipped
        put(root / "vault" / "_identity" / "identity.json", idjson('d', 'w'));               // vault mirror, not an "older" one
        put(root / "vault" / "_identity" / "old-pc.json", idjson('e', 'v'));
        put(root / "data" / "retired-ids.txt", std::string(26, 'a') + '\n');                // a: already confirmed retired
        std::string dd = (root / "data").string(), vd = (root / "vault").string();
        auto ids = older_identities(dd, vd, std::string(26, 'c'));
        std::set<std::string> got;
        for (auto& i : ids) got.insert(i.user_id.substr(0, 1));
        CHECK(ids.size() == 2 && got.count("b") && got.count("e"));
        fs::remove_all(root, ec);
    }
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
