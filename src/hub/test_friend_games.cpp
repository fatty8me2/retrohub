// Pure-logic tests for the greyed-out friend games (friend_games.h, library_share.h). No network, no files.
#include "friend_games.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

using namespace hubui::fg;
using hubnet::LibGame;

int main()
{
    CHECK(norm_title("Mario Kart 64 (USA) (Rev 1)") == "mario kart 64");
    CHECK(norm_title("Mario Kart 64 (Europe)") == "mario kart 64");
    CHECK(norm_title("The Legend of Zelda: Ocarina of Time [!]") == "legend of zelda ocarina of time");
    CHECK(identity("n64", "mk64", "x") == "n64|c:mk64");
    CHECK(identity("gba", "", "Metroid Fusion (USA)") == identity("gba", "", "Metroid Fusion (Europe)"));
    CHECK(identity("gba", "", "A") != identity("gb", "", "A"));

    std::vector<Owned> mine = {{"n64", "n64:aaa", "mk64", "Mario Kart 64"}, {"gba", "gba:1", "", "Metroid Fusion (USA)"}};
    std::vector<LibGame> mickey = {{"n64", "n64:bbb", "mk64", "Mario Kart 64 (Europe)"},       // I own it (other region)
                                   {"gba", "gba:2", "", "Metroid Fusion (Europe)"},            // same game by title
                                   {"n64", "n64:ccc", "ssb", "Super Smash Bros."},
                                   {"gc", "gc:1", "", "Luigi's Mansion"}};
    std::vector<LibGame> camer = {{"n64", "n64:ddd", "ssb", "Super Smash Bros. (USA)"},
                                  {"gc", "gc:2", "", "Luigi's Mansion (Europe)"},
                                  {"gb", "gb:1", "", "Tetris"}};
    std::vector<std::pair<std::string, const std::vector<LibGame>*>> libs = {{"u1", &mickey}, {"u2", &camer}, {"u3", nullptr}};
    auto g = build_ghosts(mine, libs);
    CHECK(g.size() == 3);
    if (g.size() == 3) {
        CHECK(g[0].title == "Luigi's Mansion" && g[0].uids.size() == 2);
        CHECK(g[1].title == "Super Smash Bros." && g[1].uids.size() == 2 && g[1].cid == "ssb");
        CHECK(g[2].title == "Tetris" && g[2].uids.size() == 1 && g[2].uids[0] == "u2");
    }
    // catalogued + uncatalogued copy of the same title merge
    std::vector<LibGame> a = {{"n64", "k1", "cid1", "Game X"}}, b = {{"n64", "k2", "", "Game X (USA)"}};
    auto m = build_ghosts({}, {{"u1", &a}, {"u2", &b}});
    CHECK(m.size() == 1 && m[0].uids.size() == 2 && m[0].cid == "cid1");
    // same friend twice, same key owned by me
    std::vector<LibGame> dup = {{"n64", "n64:aaa", "", "Renamed"}, {"n64", "z", "", "Z"}, {"n64", "z2", "", "Z"}};
    auto d = build_ghosts(mine, {{"u1", &dup}});
    CHECK(d.size() == 1 && d[0].uids.size() == 1);
    // cap
    std::vector<LibGame> big;
    for (int i = 0; i < 6000; ++i) big.push_back({"n64", "k" + std::to_string(i), "", "T" + std::to_string(i)});
    CHECK(build_ghosts({}, {{"u1", &big}}).size() == kMaxGhosts);

    CHECK(badge_text({}) == "");
    CHECK(badge_text({"Mickey"}) == "Mickey has this");
    CHECK(badge_text({"Mickey", "camer"}) == "Mickey, camer have this");
    CHECK(badge_text({"A", "B", "C"}) == "A, B and 1 other have this");
    CHECK(badge_text({"A", "B", "C", "D"}) == "A, B and 2 others have this");

    // normalize / hash
    std::vector<LibGame> l1 = {{"n64", "b", "", "B"}, {"n64", "a", "", "A"}, {"n64", "a", "", "A"}, {"", "x", "", "bad"}};
    auto n1 = hubnet::lib_normalize(l1);
    CHECK(n1.size() == 2 && n1[0].key == "a");
    std::vector<LibGame> l2 = {{"n64", "a", "", "A"}, {"n64", "b", "", "B"}};
    CHECK(hubnet::lib_hash(true, n1) == hubnet::lib_hash(true, hubnet::lib_normalize(l2)));
    l2.push_back({"gb", "c", "", "C"});
    CHECK(hubnet::lib_hash(true, n1) != hubnet::lib_hash(true, hubnet::lib_normalize(l2)));
    CHECK(hubnet::lib_hash(false, n1) == hubnet::lib_hash(false, {}));   // withdrawn: the list does not matter
    CHECK(hubnet::lib_hash(false, {}) != hubnet::lib_hash(true, {}));
    CHECK(hubnet::lib_hash(true, n1).size() == 16);
    std::vector<LibGame> huge;
    for (int i = 0; i < 2500; ++i) huge.push_back({"n64", "k" + std::to_string(i), "", std::string(200, 'x')});
    auto nh = hubnet::lib_normalize(huge);
    CHECK(nh.size() == hubnet::kMaxLibGames && nh[0].title.size() == 80);

    // publish list skips the native entry; the debounce sends once, after a quiet period
    auto pl = publish_list({{"n64", "native", "smash64", "Smash"}, {"n64", "n64:aaa", "mk64", "MK"}}, "native");
    CHECK(pl.size() == 1 && pl[0].key == "n64:aaa");
    PublishGate gate;
    CHECK(!gate.poll("a", 0));
    CHECK(!gate.poll("a", 1000));
    CHECK(gate.poll("a", 3100));
    CHECK(!gate.poll("a", 9000));          // unchanged: never again
    CHECK(!gate.poll("b", 10000));         // changed: wait
    CHECK(!gate.poll("c", 12000));         // changed again: the quiet period restarts
    CHECK(!gate.poll("c", 14000));
    CHECK(gate.poll("c", 15100));

    CHECK(lists_held_text(2, 5) == "Game lists from friends: 2 of 5");
    CHECK(empty_reason(true, 3, 2, 4).empty());
    CHECK(empty_reason(false, 3, 2, 0) == "setting off");
    CHECK(empty_reason(true, 0, 0, 0) == "no friends");
    CHECK(empty_reason(true, 3, 0, 0) == "no lists held");
    CHECK(empty_reason(true, 3, 2, 0) == "friends own nothing you do not have");

    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("friend_games: ok\n");
    return 0;
}
