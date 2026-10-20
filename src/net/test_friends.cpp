/* hubfriends_test: FriendsClient identity/parsing checks + a multi-client flow against a /friends server.
 *   hubfriends_test [ws://127.0.0.1:8799]    (HUBFRIENDS_OFFLINE=1 runs only the offline checks) */
#include "friends.h"
#include "log.h"
#include "net_internal.h"
#include "session.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>

using namespace hubnet;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c, ...) do { if (c) printf("  ok   %s\n", #c); else { g_fail++; printf("  FAIL %s  ", #c); printf("" __VA_ARGS__); printf("\n"); } } while (0)

struct Peer {
    std::string name, dir, vault;
    FriendsClient c;
    std::vector<Event> log;
    void pump()
    {
        c.update();
        Event e;
        while (c.poll_event(&e)) log.push_back(e);
    }
    const Event* find(Event::Kind k, const std::string& uid = "") const
    {
        for (const Event& e : log) if (e.kind == k && (uid.empty() || e.uid == uid)) return &e;
        return nullptr;
    }
    void clear() { log.clear(); }
};

static std::vector<Peer*> g_all;
static bool wait_for(const std::function<bool()>& pred, int ms = 8000)
{
    uint32_t t0 = now_ms();
    while (now_ms() - t0 < (uint32_t)ms) {
        for (Peer* p : g_all) p->pump();
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
static void settle(int ms = 400) { wait_for([] { return false; }, ms); }

static void offline_tests(const std::string& root)
{
    printf("[offline]\n");
    Identity a = identity_generate();
    CHECK(a.user_id.size() == 26 && a.secret.size() == 52);
    std::string code = friend_code(a.user_id);
    CHECK(code.size() == 12 && code.compare(0, 3, "RH-") == 0 && code[7] == '-', "%s", code.c_str());
    CHECK(friend_code("7kq2m9xdaaaaaaaaaaaaaaaaaa") == "RH-7KQ2-M9XD");

    std::string d = root + "id1\\", v = root + "vault1\\";
    Identity i1 = identity_load_or_create(d, v);
    Identity i2 = identity_load_or_create(d, v);
    CHECK(i1.user_id == i2.user_id && i1.secret == i2.secret);
    CHECK(fs::exists(fs::path(v) / "_identity" / "identity.json"));
    fs::remove(fs::path(d) / "identity.json");
    Identity i3 = identity_load_or_create(d, v);
    CHECK(i3.user_id == i1.user_id, "vault adoption");
    CHECK(fs::exists(fs::path(d) / "identity.json"));

    /* JSON: escapes + unicode incl. surrogate pair, via the util parser */
    detail::Json j;
    CHECK(detail::json_parse("{\"t\":\"a\\\"b\\n\\u00e9\\ud83d\\ude00\\\\\",\"n\":[1,2.5,{\"x\":true}]}", &j));
    CHECK(j.s("t") == "a\"b\n\xC3\xA9\xF0\x9F\x98\x80\\", "got len %zu", j.s("t").size());
    CHECK(detail::json_parse("{\"t\":\"\\ud83dX\"}", &j) && j.s("t").size() >= 2);
    CHECK(!detail::json_parse("{\"t\":", &j));

    /* conversation persistence */
    {
        FriendsClient c;
        c.set_paths(root + "id2\\", "");
        // not started: conversation() lazily loads (empty)
        CHECK(c.conversation("aaaaaaaaaaaaaaaaaaaaaaaaaa").empty());
    }
}

static std::unique_ptr<Peer> make_peer(const std::string& root, const std::string& name)
{
    auto p = std::make_unique<Peer>();
    p->name = name;
    p->dir = root + name + "\\data\\";
    p->vault = root + name + "\\vault\\";
    p->c.set_paths(p->dir, p->vault);
    return p;
}

int main(int argc, char** argv)
{
    std::string server = argc > 1 ? argv[1] : "ws://127.0.0.1:8799";
    std::string root = (fs::temp_directory_path() / ("hubfriends_" + std::to_string(now_ms()))).string() + "\\";
    fs::create_directories(root);

    offline_tests(root);
    if (getenv("HUBFRIENDS_OFFLINE")) { printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail); return g_fail ? 1 : 0; }

    printf("[online] %s\n", server.c_str());
    auto A = make_peer(root, "alice"), B = make_peer(root, "bob"), C = make_peer(root, "carol");
    g_all = {A.get(), B.get(), C.get()};
    A->c.start(server, "Alice");
    B->c.start(server, "Bob");
    C->c.start(server, "Carol");
    bool up = wait_for([&] { return A->c.connected() && B->c.connected() && C->c.connected(); });
    CHECK(up, "A='%s' B='%s' C='%s'", A->c.status_text().c_str(), B->c.status_text().c_str(), C->c.status_text().c_str());
    if (!up) { printf("FAILED\n"); return 1; }
    printf("  codes: %s %s %s\n", A->c.my_code().c_str(), B->c.my_code().c_str(), C->c.my_code().c_str());

    printf("[friend flow]\n");
    A->c.add_friend(B->c.my_code());
    CHECK(wait_for([&] { return B->find(Event::RequestReceived); }));
    CHECK(B->c.incoming_requests().size() == 1 && B->c.incoming_requests()[0].uid == A->c.my_uid());
    B->c.accept(A->c.my_uid());
    CHECK(wait_for([&] { return A->find(Event::FriendAdded) && B->find(Event::FriendAdded); }));
    CHECK(A->c.friends().size() == 1 && B->c.friends().size() == 1);
    CHECK(A->c.incoming_requests().empty() && A->c.outgoing_requests().empty());

    A->clear();
    A->c.add_friend(C->c.my_code());
    CHECK(wait_for([&] { return C->find(Event::RequestReceived); }));
    C->c.decline(A->c.my_uid());
    CHECK(wait_for([&] { return A->find(Event::RequestResolved); }));
    CHECK(A->find(Event::RequestResolved)->text == "declined", "%s", A->find(Event::RequestResolved)->text.c_str());
    A->c.add_friend("RH-ZZZZ-ZZZZ");
    CHECK(wait_for([&] { return A->find(Event::Error); }));
    CHECK(A->find(Event::Error)->text == "unknown-code", "%s", A->find(Event::Error)->text.c_str());
    A->c.add_friend(A->c.my_code());
    CHECK(wait_for([&] { return A->find(Event::Error) && A->log.size() >= 2 && A->log.back().kind == Event::Error; }));

    printf("[presence]\n");
    A->clear(); B->clear();
    Status s; s.state = PresenceState::InLobby; s.game_id = "smash64"; s.game_title = "Super Smash Bros."; s.system = "n64";
    s.lobby_code = "ABC234"; s.joinable = true; s.players = 1; s.max_players = 4;
    B->c.set_status(s);
    CHECK(wait_for([&] { return A->find(Event::FriendOnline) || A->find(Event::PresenceChanged); }));
    const Friend* fb = A->c.find_friend(B->c.my_uid());
    CHECK(fb && fb->status.state == PresenceState::InLobby && fb->status.lobby_code == "ABC234" && fb->status.joinable &&
          fb->status.max_players == 4 && fb->name == "Bob");
    s.state = PresenceState::Online; s.lobby_code.clear(); s.joinable = false;
    B->c.set_status(s);
    CHECK(wait_for([&] { fb = A->c.find_friend(B->c.my_uid()); return fb && fb->status.state == PresenceState::Online; }));
    A->clear();
    B->c.rename("Bobby \xC3\xA9");
    CHECK(wait_for([&] { fb = A->c.find_friend(B->c.my_uid()); return fb && fb->name == "Bobby \xC3\xA9"; }));

    printf("[messages]\n");
    A->clear(); B->clear();
    const std::string txt = "hi \"bob\" \\ caf\xC3\xA9 \xF0\x9F\x98\x80\nline2";
    A->c.send_message(B->c.my_uid(), txt);
    CHECK(wait_for([&] { return B->find(Event::MessageReceived); }));
    CHECK(B->find(Event::MessageReceived) && B->find(Event::MessageReceived)->msg.text == txt);
    CHECK(B->c.unread(A->c.my_uid()) == 1 && B->c.total_unread() == 1);
    B->c.mark_read(A->c.my_uid());
    CHECK(B->c.unread(A->c.my_uid()) == 0);
    CHECK(A->c.conversation(B->c.my_uid()).size() == 1 && A->c.conversation(B->c.my_uid())[0].mine);

    /* offline inbox */
    B->c.stop();
    settle(500);
    B->clear();
    A->c.send_message(B->c.my_uid(), "while you were out");
    settle(500);
    B->c.start(server, "Bobby");
    CHECK(wait_for([&] { return B->find(Event::MessageReceived); }));
    CHECK(B->find(Event::MessageReceived) && B->find(Event::MessageReceived)->msg.text == "while you were out");
    CHECK(B->c.unread(A->c.my_uid()) == 1);
    /* ack: a third start must not redeliver */
    B->c.stop(); settle(200); B->clear();
    B->c.start(server, "Bobby");
    CHECK(wait_for([&] { return B->c.connected(); }));
    settle(800);
    CHECK(!B->find(Event::MessageReceived), "inbox redelivered");
    /* persistence across instances */
    {
        FriendsClient again;
        again.set_paths(B->dir, B->vault);
        const auto& conv = again.conversation(A->c.my_uid());
        CHECK(conv.size() == 2 && conv[0].text == txt, "size %zu", conv.size());
    }
    A->c.request_history(B->c.my_uid());
    settle(500);

    printf("[invite]\n");
    A->clear(); B->clear(); C->clear();
    Invite inv; inv.lobby_code = "XYZ789"; inv.game_id = "smash64"; inv.game_title = "Super Smash Bros."; inv.system = "n64";
    A->c.send_invite({B->c.my_uid()}, inv);
    CHECK(wait_for([&] { return B->find(Event::InviteReceived); }));
    const Event* ie = B->find(Event::InviteReceived);
    CHECK(ie && ie->invite.lobby_code == "XYZ789" && ie->invite.game_title == "Super Smash Bros." && ie->invite.expires_at > 0);
    A->c.send_invite({C->c.my_uid()}, inv);   // not friends
    CHECK(wait_for([&] { return A->find(Event::Error); }));
    CHECK(A->find(Event::Error)->text == "not-friends", "%s", A->find(Event::Error)->text.c_str());

    printf("[remove / block]\n");
    A->clear(); B->clear();
    A->c.remove_friend(B->c.my_uid());
    CHECK(wait_for([&] { return A->find(Event::FriendRemoved) && B->find(Event::FriendRemoved); }));
    CHECK(A->c.friends().empty() && B->c.friends().empty());

    /* befriend again, then block */
    B->c.add_friend(A->c.my_code());
    CHECK(wait_for([&] { return A->find(Event::RequestReceived); }));
    A->c.accept(B->c.my_uid());
    CHECK(wait_for([&] { return A->c.friends().size() == 1 && B->c.friends().size() == 1; }));
    A->clear(); B->clear();
    A->c.block(B->c.my_uid());
    CHECK(wait_for([&] { return B->find(Event::FriendRemoved); }));
    CHECK(A->c.blocked().size() == 1 && A->c.friends().empty());
    B->c.send_message(A->c.my_uid(), "still there?");
    settle(800);
    CHECK(!A->find(Event::MessageReceived), "blocked message delivered");
    A->c.unblock(B->c.my_uid());
    settle(300);
    CHECK(A->c.blocked().empty());

    /* reconnect: kill carol's socket by stopping and restarting is covered; check restart keeps identity */
    std::string cuid = C->c.my_uid();
    C->c.stop();
    C->c.start(server, "Carol");
    CHECK(wait_for([&] { return C->c.connected(); }) && C->c.my_uid() == cuid);

    for (Peer* p : g_all) p->c.stop();
    std::error_code ec;
    fs::remove_all(root, ec);
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
