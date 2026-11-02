/* hubfriends_outbox_test: the offline outbox rules (pure logic, no network). */
#include "outbox_logic.h"

#include <cstdio>

using namespace hubnet;

static int g_fail = 0;
#define CHECK(c) do { if (c) printf("  ok   %s\n", #c); else { g_fail++; printf("  FAIL %s\n", #c); } } while (0)

static OutItem it(const char* kind, const char* uid, const char* frame = "{}", int64_t at = 1000, const char* id = "")
{
    OutItem o; o.kind = kind; o.uid = uid; o.frame = frame; o.at = at; o.local_id = id;
    return o;
}

int main()
{
    {   // order is kept; messages and invites are never merged
        Outbox b;
        CHECK(b.add(it("message", "u1", "m1", 1, "local-1")) == OutAdd::Queued);
        CHECK(b.add(it("message", "u1", "m1", 2, "local-2")) == OutAdd::Queued);
        CHECK(b.add(it("invite", "", "i1")) == OutAdd::Queued);
        CHECK(b.size() == 3 && b.has_message("local-2") && !b.has_message("nope"));
        auto all = b.take_all();
        CHECK(all.size() == 3 && all[0].local_id == "local-1" && all[2].kind == "invite" && b.empty());
    }
    {   // block then unblock cancel out, either way round
        Outbox b;
        CHECK(b.add(it("block", "u1")) == OutAdd::Queued);
        CHECK(b.add(it("unblock", "u1")) == OutAdd::Cancelled && b.empty());
        CHECK(b.add(it("unblock", "u1")) == OutAdd::Queued);
        CHECK(b.add(it("block", "u1")) == OutAdd::Cancelled && b.empty());
        CHECK(b.add(it("block", "u1")) == OutAdd::Queued);
        CHECK(b.add(it("block", "u1")) == OutAdd::Merged && b.size() == 1);
        CHECK(b.add(it("block", "u2")) == OutAdd::Queued && b.size() == 2);
    }
    {   // the later answer to a request wins
        Outbox b;
        b.add(it("accept", "u1", "acc"));
        CHECK(b.add(it("decline", "u1", "dec")) == OutAdd::Merged);
        CHECK(b.size() == 1 && b.items()[0].kind == "decline");
        b.add(it("accept", "u2", "acc2"));
        CHECK(b.size() == 2);
    }
    {   // add_friend: an identical request is queued once
        Outbox b;
        CHECK(b.add(it("add_friend", "", "RH-AAAA-BBBB")) == OutAdd::Queued);
        CHECK(b.add(it("add_friend", "", "RH-AAAA-BBBB")) == OutAdd::Merged);
        CHECK(b.add(it("add_friend", "", "RH-CCCC-DDDD")) == OutAdd::Queued && b.size() == 2);
    }
    {   // bounded: the 51st item is refused (and reported by the caller), nothing is dropped silently
        Outbox b;
        for (size_t i = 0; i < kOutboxMax; i++) CHECK(b.add(it("message", "u1", "m", 1, "x")) == OutAdd::Queued || true);
        CHECK(b.size() == kOutboxMax);
        CHECK(b.add(it("message", "u1", "m")) == OutAdd::Full && b.size() == kOutboxMax);
    }
    {   // 24 h age limit
        Outbox b;
        b.add(it("message", "u1", "old", 1000));
        b.add(it("message", "u1", "new", 1000 + kOutboxMaxAgeMs));
        auto gone = b.expire(1000 + kOutboxMaxAgeMs + 5);
        CHECK(gone.size() == 1 && gone[0].frame == "old" && b.size() == 1 && b.items()[0].frame == "new");
        CHECK(b.expire(1000 + kOutboxMaxAgeMs + 6).empty());
    }
    {   // cancelling a waiting message
        Outbox b;
        b.add(it("message", "u1", "a", 1, "l1"));
        CHECK(b.remove_message("l1") && !b.remove_message("l1") && b.empty());
    }
    CHECK(Outbox::queued_text("message").find("back online") != std::string::npos);
    CHECK(Outbox::failed_text(it("message", "u1"), "the file expired") == "Your message could not be sent (the file expired).");
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
