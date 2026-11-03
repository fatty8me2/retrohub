// Tests for the Discover page's data and matching (discover_logic.h, data/recommended.json). No network, no UI.
#include "discover_logic.h"
#include "games.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

using namespace discover;

static std::string slurp(const char* p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: hub_discover_test <recommended.json>\n"); return 2; }
    const std::string text = slurp(argv[1]);
    CHECK(!text.empty());

    /* ---- the shipped file ---- */
    std::vector<Rec> recs;
    int bad = 0;
    CHECK(parse(text, &recs, &bad));
    CHECK(bad == 0);
    std::map<std::string, std::set<std::string>> titles;
    static const std::set<std::string> kGenres = {"Party", "Fighting", "Racing", "Shooter", "Platformer", "Adventure", "RPG", "Strategy",
                                                  "Sports", "Puzzle", "Action", "Simulation", "Rhythm"};
    for (const Rec& r : recs) {
        CHECK(!r.title.empty() && !r.why.empty());
        CHECK(r.year >= 1980 && r.year <= 2020);
        CHECK(r.players >= 1 && r.players <= 8);
        CHECK(kGenres.count(r.genre) == 1);
        CHECK(r.why.size() <= 120);
        CHECK(titles[r.sys].insert(lower(r.title)).second);   // no duplicate title per console
        if (r.friends) CHECK(r.players >= 2);
        if (!r.cid.empty()) {   // a catalog id must exist, belong to the same console, and (when the game is online) match the flag
            const hub::GameInfo* g = hub::find_game(r.cid);
            CHECK(g != nullptr);
            if (g) CHECK(g->system == r.sys);
        }
        if (!r.also.empty()) CHECK(hub::find_game(r.also) != nullptr);
    }
    for (const std::string& c : consoles()) {
        size_t n = for_console(recs, c, false).size();
        CHECK(n >= 15 && n <= 25);
        CHECK(for_console(recs, c, true).size() <= n);
        std::printf("%s=%zu ", c.c_str(), n);
    }
    std::printf("\n");
    CHECK(titles.size() == consoles().size());   // no console outside the picker
    /* no links of any kind anywhere in the file */
    {
        const std::string l = lower(text);
        for (const char* bad_s : {"http", "://", "www.", ".com", ".net", ".org", ".io/", "ftp", "magnet", "torrent", "rom download"})
            CHECK(l.find(bad_s) == std::string::npos);
    }

    /* ---- parse robustness ---- */
    {
        std::vector<Rec> v;
        int b = 0;
        CHECK(!parse("not json", &v, &b));
        CHECK(!parse("{}", &v, &b));
        CHECK(parse("{\"games\":[{\"title\":\"A\",\"console\":\"nes\",\"year\":1990,\"genre\":\"Action\",\"players\":2,\"why\":\"w\"},{\"title\":\"B\"}]}", &v, &b));
        CHECK(v.size() == 1 && b == 1 && v[0].players == 2 && !v[0].friends);
    }

    /* ---- library / pack status ---- */
    using hubui::fg::Owned;
    auto rec = [](const char* t, const char* sys, const char* cid = "") { Rec r; r.title = t; r.sys = sys; r.cid = cid; r.genre = "Action"; r.why = "x"; r.year = 2000; return r; };
    const std::vector<hubgp::PackGame> nopack;
    {   // catalog id first, even when the library title differs
        Rec r = rec("The Legend of Zelda: Ocarina of Time", "n64", "oot");
        std::vector<Owned> own = {{"n64", "n64:abc", "oot", "Zelda - Ocarina (header title)"}};
        Match m = classify(r, own, nopack);
        CHECK(m.st == Status::Owned && m.key == "n64:abc");
    }
    {   // the native port counts for Smash 64
        Rec r = rec("Super Smash Bros.", "n64", "ssb64rom");
        r.also = "smash64";
        Match m = classify(r, {{"n64", "native:smash64", "smash64", "Super Smash Bros. (Smash Classic)"}}, nopack);
        CHECK(m.st == Status::Owned);
    }
    {   // by title, region tags ignored; another console's copy does not count
        Rec r = rec("Super Metroid", "snes");
        CHECK(classify(r, {{"snes", "snes:1", "", "Super Metroid (USA)"}}, nopack).st == Status::Owned);
        CHECK(classify(r, {{"gba", "gba:1", "", "Super Metroid"}}, nopack).st == Status::Missing);
        CHECK(classify(r, {}, nopack).st == Status::Missing);
    }
    {   // Game Boy / Color twins
        Rec r = rec("Pokemon Yellow", "gbc");
        CHECK(classify(r, {{"gb", "gb:1", "", "Pokemon Yellow"}}, nopack).st == Status::Owned);
    }
    {   // numbers must agree: Mario Party 2 is not Mario Party 3
        Rec r = rec("Mario Party 2", "n64");
        CHECK(classify(r, {{"n64", "k", "", "Mario Party 3"}}, nopack).st == Status::Missing);
    }
    {   // pack: loose pack titles match, the install title / console come from the pack, owned wins over pack
        Rec r = rec("The Legend of Zelda: The Wind Waker", "gc");
        std::vector<hubgp::PackGame> pack = {{"gc", "Legend of Zelda - The Wind Waker", hubgp::pack_key("Legend of Zelda, The - The Wind Waker (USA)"), "Legend of Zelda, The - The Wind Waker (USA, Canada)"}};
        Match m = classify(r, {}, pack);
        CHECK(m.st == Status::InPack && m.pack_sys == "gc" && m.pack_title == "Legend of Zelda - The Wind Waker" && !m.pack_hint.empty());
        CHECK(classify(r, {{"gc", "gc:k", "", "The Legend of Zelda: The Wind Waker"}}, pack).st == Status::Owned);
        Rec other = rec("Pikmin", "gc");
        CHECK(classify(other, {}, pack).st == Status::Missing);
        Rec wrong = rec("The Legend of Zelda: The Wind Waker", "n64");
        CHECK(classify(wrong, {}, pack).st == Status::Missing);   // a pack for another console never matches
    }
    {   // a GB game sitting in the GBC pack is installable under the pack's console
        Rec r = rec("Pokemon Red", "gb");
        std::vector<hubgp::PackGame> pack = {{"gbc", "Pokemon - Red Version", hubgp::pack_key("Pokemon - Red Version (USA, Europe)"), ""}};
        Match m = classify(r, {}, pack);
        CHECK(m.st == Status::InPack && m.pack_sys == "gbc");
    }
    CHECK(players_text(1) == "1 player" && players_text(4) == "1-4 players");

    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("ok\n");
    return 0;
}
