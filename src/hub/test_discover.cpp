// Tests for the Discover page's data and matching (discover_logic.h, data/recommended.json). No network, no UI.
#include "discover_logic.h"
#include "games.h"
#include "gameinfo.h"
#include <chrono>

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
        CHECK(r.players >= 1 && r.players <= 9);
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
        CHECK(n >= 15 && n <= 150);
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

    /* ---- browsing: paging ---- */
    {
        Pager p;
        CHECK(p.visible(103) == 24 && p.has_more(103));
        p.more(103); CHECK(p.visible(103) == 48 && showing_text(p.visible(103), 103) == "Showing 48 of 103");
        p.more(103); p.more(103);
        CHECK(p.visible(103) == 96 && p.has_more(103));
        p.more(103);
        CHECK(p.visible(103) == 103 && !p.has_more(103) && showing_text(103, 103) == "Showing 103 of 103");
        p.more(103); CHECK(p.shown == 120);   // never grows once everything shows
        p.reset(); CHECK(p.shown == kBatch);
        Pager q; CHECK(q.visible(10) == 10 && !q.has_more(10) && q.visible(0) == 0 && !q.has_more(0));
        CHECK(q.visible(24) == 24 && !q.has_more(24) && q.has_more(25));
    }

    /* ---- browsing: sort / filter / search over a small hand-made list ---- */
    auto mk = [](const char* t, const char* sys, const char* genre, int year, int pl, bool fr, const char* cid = "") {
        Rec r; r.title = t; r.sys = sys; r.genre = genre; r.year = year; r.players = pl; r.friends = fr; r.why = "w"; r.cid = cid; return r;
    };
    std::vector<Rec> small = {
        mk("The Legend of Zelda: Ocarina of Time", "n64", "Adventure", 1998, 1, false),   // n64 rank 0
        mk("Mario Kart 64", "n64", "Racing", 1996, 4, true),                              // n64 rank 1
        mk("Super Metroid", "snes", "Action", 1994, 1, false),                            // snes rank 0
        mk("Super Mario Kart", "snes", "Racing", 1992, 2, true),                          // snes rank 1
        mk("Pok\xC3\xA9mon Yellow", "gbc", "RPG", 1998, 2, true),                          // gbc rank 0
        mk("Advance Wars", "gba", "Strategy", 2001, 4, true),                             // gba rank 0
        mk("Metroid Fusion", "gba", "Action", 2002, 1, false),                            // gba rank 1
        mk("Tetris", "gb", "Puzzle", 1989, 2, true),                                      // gb rank 0
    };
    Browser br;
    br.set_curated(small);
    CHECK(br.curated_count() == 8 && !br.has_catalog());
    auto titles_of = [&](const Browser::Result& r) { std::vector<std::string> v; for (int i : r.ids) v.push_back(br.item(i).title); return v; };
    {
        Filter f;
        Browser::Result r = br.query(f);
        CHECK(r.ids.size() == 8 && r.total_all == 8);
        /* all consoles, Recommended: round robin by per-console rank, console picker order (nes snes n64 gb gbc gba ...) */
        std::vector<std::string> t = titles_of(r);
        CHECK(t[0] == "Super Metroid" && t[1] == "The Legend of Zelda: Ocarina of Time" && t[2] == "Tetris" && t[3].rfind("Pok", 0) == 0 && t[4] == "Advance Wars");
        CHECK(t[5] == "Super Mario Kart" && t[6] == "Mario Kart 64" && t[7] == "Metroid Fusion");
        /* one console keeps file order */
        f.sys = "n64";
        t = titles_of(br.query(f));
        CHECK(t.size() == 2 && t[0] == "The Legend of Zelda: Ocarina of Time" && t[1] == "Mario Kart 64");
        CHECK(br.query(f).by_console.size() == 5 && br.query(f).by_console["snes"] == 2 && br.query(f).total_all == 8);   // chip counts ignore the console
        /* Title A-Z ignores a leading "The" */
        f.sys.clear(); f.sort = Sort::Title;
        t = titles_of(br.query(f));
        CHECK(t.front() == "Advance Wars");
        CHECK(t[1] == "The Legend of Zelda: Ocarina of Time" && t.back() == "Tetris");
        /* Newest / Oldest */
        f.sort = Sort::Newest; t = titles_of(br.query(f));
        CHECK(t.front() == "Metroid Fusion" && t.back() == "Tetris");
        f.sort = Sort::Oldest; t = titles_of(br.query(f));
        CHECK(t.front() == "Tetris" && t.back() == "Metroid Fusion");
        /* Most players: 4 first (rank order among equals), then 2s, then 1s */
        f.sort = Sort::Players; t = titles_of(br.query(f));
        CHECK(t[0] == "Advance Wars" && t[1] == "Mario Kart 64" && t[2] == "Tetris" && t[3].rfind("Pok", 0) == 0 && t[4] == "Super Mario Kart");
        CHECK(br.item(br.query(f).ids.back()).players == 1);
    }
    {   // filters, one by one
        Filter f;
        f.genres = {"Racing"};
        CHECK(titles_of(br.query(f)).size() == 2);
        f.genres = {"Racing", "RPG"};
        CHECK(br.query(f).ids.size() == 3);
        f = Filter(); f.min_players = 2; CHECK(br.query(f).ids.size() == 5);
        f.min_players = 4; CHECK(br.query(f).ids.size() == 2);
        f.min_players = 3; CHECK(br.query(f).ids.size() == 2);
        f = Filter(); f.decade = 1990; CHECK(br.query(f).ids.size() == 5);
        f = Filter(); f.decade = 1980; CHECK(titles_of(br.query(f)) == std::vector<std::string>{"Tetris"});
        f.decade = 2000; CHECK(br.query(f).ids.size() == 2);
        f.decade = 2010; CHECK(br.query(f).ids.empty());
        f = Filter(); f.friends = true; CHECK(br.query(f).ids.size() == 5);
        f.catalog = true; CHECK(br.query(f).ids.empty());   // the catalog has none loaded yet, and ignores the friends flag anyway
        f = Filter(); f.min_players = 2; f.friends = true; f.genres = {"Racing"}; f.sys = "snes";
        CHECK(titles_of(br.query(f)) == std::vector<std::string>{"Super Mario Kart"});
        CHECK(filters_active(f));
        clear_filters(&f);
        CHECK(!filters_active(f) && f.sys == "snes");
        Filter g; g.query = "x"; CHECK(filters_active(g));
        CHECK(filter_sig(Filter()) != filter_sig(g));
    }
    {   // search: case, accents, several words, no match
        Filter f;
        f.query = "POKEMON"; CHECK(br.query(f).ids.size() == 1);
        f.query = "pok\xC3\xA9mon"; CHECK(br.query(f).ids.size() == 1);
        f.query = "kart"; CHECK(br.query(f).ids.size() == 2);
        f.query = "mario kart"; CHECK(br.query(f).ids.size() == 2);
        f.query = "kart super"; CHECK(br.query(f).ids.size() == 1);
        f.query = "  metroid "; CHECK(br.query(f).ids.size() == 2);
        f.query = "zzz"; CHECK(br.query(f).ids.empty());
        CHECK(fold_text("Pok\xC3\xA9mon \xC3\x89") == "pokemon e");
    }
    {   // genre counts honour the other filters but not the genre filter
        Filter f;
        auto g = br.genre_counts(f);
        int total = 0;
        for (auto& kv : g) total += kv.second;
        CHECK(total == 8);
        f.genres = {"Racing"};
        total = 0;
        for (auto& kv : br.genre_counts(f)) total += kv.second;
        CHECK(total == 8);
        f.min_players = 4;
        total = 0;
        for (auto& kv : br.genre_counts(f)) total += kv.second;
        CHECK(total == 2);
    }

    /* ---- status filter and the Classifier ---- */
    {
        std::vector<Owned> own = {{"n64", "n64:1", "", "Mario Kart 64 (USA)"}, {"gba", "gba:1", "", "Advance Wars"}};
        std::vector<hubgp::PackGame> pack = {{"snes", "Super Metroid", hubgp::pack_key("Super Metroid"), "Super Metroid (USA)"},
                                             {"gbc", "Tetris", hubgp::pack_key("Tetris"), "Tetris"}};
        br.set_sources(own, pack);
        Filter f;
        f.status = StatusFilter::Owned;
        CHECK(titles_of(br.query(f)).size() == 2);
        f.status = StatusFilter::InPack;
        std::vector<std::string> t = titles_of(br.query(f));
        CHECK(t.size() == 2 && t[0] == "Super Metroid" && t[1] == "Tetris");   // Tetris (gb) sits in the GBC pack
        f.status = StatusFilter::NotOwned;
        CHECK(br.query(f).ids.size() == 6);
        f.sys = "snes"; f.status = StatusFilter::Any;
        CHECK(br.query(f).ids.size() == 2);
        /* with a zero time budget the status filter reports what it could not check yet and a later call finishes it */
        br.set_sources(own, pack);
        f = Filter(); f.status = StatusFilter::Owned;
        Browser::Result r0 = br.query(f, 0.0);
        CHECK(r0.pending >= 0);
        Browser::Result r1 = br.query(f, -1.0);
        CHECK(r1.pending == 0 && r1.ids.size() == 2);
        /* Classifier == classify() for every recommendation, with a sloppy library and pack */
        std::vector<Owned> own2 = {{"n64", "a", "", "Legend of Zelda, The - Ocarina of Time (USA)"}, {"gb", "b", "", "Pokemon Yellow"},
                                   {"snes", "c", "", "Super Mario Kart"}, {"gba", "d", "", "Advance Wars 2"}};
        std::vector<hubgp::PackGame> pack2 = {{"gba", "Metroid - Fusion", hubgp::pack_key("Metroid - Fusion (USA)"), "Metroid - Fusion (USA)"},
                                              {"gbc", "Pokemon Yellow Special Pikachu Edition", hubgp::pack_key("Pokemon Yellow Special Pikachu Edition"), ""},
                                              {"n64", "Mario Kart 64", hubgp::pack_key("Mario Kart 64"), "Mario Kart 64 (USA)"}};
        Classifier c(own2, pack2);
        for (const Rec& s : small) {
            Rec p = s; p.key = hubgp::pack_key(p.title);
            Match a = classify(s, own2, pack2), b = c.classify(p);
            CHECK(a.st == b.st && a.key == b.key && a.pack_sys == b.pack_sys && a.pack_title == b.pack_title && a.pack_hint == b.pack_hint);
        }
        Rec ow = small[0]; ow.cid = "oot";
        Classifier c2({{"n64", "k", "oot", "whatever"}}, {});
        CHECK(c2.classify(ow).st == Status::Owned);
    }

    /* ---- the catalog: built from a small gameinfo.tsv, de-duplicated against the curated list ---- */
    {
        const std::string tsv =
            "#gameinfo v1 test\n#sys\ttitle\tserials\tdeveloper\tpublisher\tgenre\tyear\tmonth\tplayers\tesrb\tfamitsu\tedge\n"
            "n64\tLegend of Zelda, The - Ocarina of Time\t\tNintendo\tNintendo\tAdventure\t1998\t11\t1\tE\t40\t10\n"   // curated -> hidden
            "n64\tStarFox 64\t\tNintendo EAD\tNintendo\tShoot'em Up\t1997\t4\t4\tE\t35\t9\n"
            "n64\tAerofighters Assault\t\tParadigm\tVideo System\tShoot'em Up\t1997\t5\t2\t\t\t\n"
            "n64\tAerofighters Assault\t\t\t\t\t0\t0\t0\t\t\t\n"                                       // repeated: merged away
            "snes\tSuper Metroid\t\tNintendo R&D1\tNintendo\tAction\t1994\t3\t1\t\t\t\n"                // curated -> hidden
            "snes\tZ-Test Quest\t\tTest Dev\tTest Pub\tRole-playing (RPG)\t1995\t7\t2\t\t\t\n"
            "gb\tPokemon Red\t\tGame Freak\tNintendo\tRole-playing (RPG)\t1996\t2\t2\t\t\t\n"
            "gb\tAlleyway\t\tNintendo\tNintendo\tPuzzle\t1989\t4\t1\t\t\t\n"
            "gbc\tPokemon Yellow\t\tGame Freak\tNintendo\tRole-playing (RPG)\t1998\t9\t2\t\t\t\n"      // curated gbc -> hidden
            "gc\tPikmin\t\tNintendo\tNintendo\tStrategy\t2001\t10\t1\t\t\t\n"                           // gc has no catalog
            "ps1\tCrash Bandicoot\t\tNaughty Dog\tSony\tPlatform\t1996\t9\t1\t\t\t\n";
        gameinfo::Db db;
        db.load(tsv);
        std::vector<Rec> cur = small;
        cur.push_back(mk("Pokemon Red", "gbc", "RPG", 1998, 2, true));   // a gbc entry hides the gb row of the same title
        cur.push_back(mk("The Legend of Zelda: Ocarina of Time", "n64", "Adventure", 1998, 1, false));   // duplicate title is tolerated by the helper
        prepare(&cur);
        std::vector<Rec> cat = catalog_from(db, cur);
        std::set<std::string> got;
        for (const Rec& r : cat) got.insert(r.sys + ":" + r.title);
        CHECK(cat.size() == 5);
        CHECK(got.count("n64:StarFox 64") && got.count("n64:Aerofighters Assault") && got.count("snes:Z-Test Quest") && got.count("gb:Alleyway") && got.count("ps1:Crash Bandicoot"));
        CHECK(!got.count("n64:Legend of Zelda, The - Ocarina of Time") && !got.count("snes:Super Metroid") && !got.count("gb:Pokemon Red") && !got.count("gbc:Pokemon Yellow") && !got.count("gc:Pikmin"));
        for (const Rec& r : cat) {
            CHECK(!r.curated && r.why.empty() && !r.key.empty() && !r.hay.empty());
            if (r.title == "StarFox 64") CHECK(r.genre == "Shooter" && r.dev == "Nintendo EAD" && r.score > 80 && r.players == 4 && r.year == 1997);
            if (r.title == "Z-Test Quest") CHECK(r.genre == "RPG" && r.score == 0);
        }
        CHECK(catalog_covers("snes") && catalog_covers("ps2") && !catalog_covers("gc") && !catalog_covers("psp"));

        Browser b2;
        b2.set_curated(cur);
        b2.set_catalog(cat);
        CHECK(b2.has_catalog() && b2.curated_count() == cur.size() && b2.size() == cur.size() + 5);
        CHECK(b2.universe_size(true, "") == 5 && b2.universe_size(true, "n64") == 2 && b2.universe_size(false, "n64") == 3);
        Filter f;
        f.catalog = true;
        Browser::Result r = b2.query(f);
        CHECK(r.ids.size() == 5);
        for (int i : r.ids) CHECK(!b2.item(i).curated);
        CHECK(b2.item(r.ids[0]).title == "StarFox 64");   // Recommended in the catalog: best rated first, then A-Z
        f.sort = Sort::Title; r = b2.query(f);
        CHECK(b2.item(r.ids[0]).title == "Aerofighters Assault" && b2.item(r.ids.back()).title == "Z-Test Quest");
        f.sort = Sort::Oldest; r = b2.query(f);
        CHECK(b2.item(r.ids[0]).title == "Alleyway");
        f.sort = Sort::Newest; r = b2.query(f);
        CHECK(b2.item(r.ids[0]).title == "Crash Bandicoot" || b2.item(r.ids[0]).year == 1997 || b2.item(r.ids[0]).year == 1996);
        f = Filter(); f.catalog = true; f.sys = "n64";
        CHECK(b2.query(f).ids.size() == 2);
        f.sys.clear(); f.query = "naughty";   // developer search (catalog only)
        CHECK(b2.query(f).ids.size() == 1);
        f.query = "sony"; CHECK(b2.query(f).ids.size() == 1);
        f.catalog = false; CHECK(b2.query(f).ids.empty());   // curated games are searched by title only
        f = Filter(); f.catalog = true; f.min_players = 4; CHECK(b2.query(f).ids.size() == 1);   // unknown players (0) never pass
        f = Filter(); f.catalog = true; f.genres = {"Shooter"}; CHECK(b2.query(f).ids.size() == 2);
        f = Filter(); f.catalog = true; f.decade = 1990; CHECK(b2.query(f).ids.size() == 4);
        f = Filter(); f.catalog = true; f.friends = true; CHECK(b2.query(f).ids.size() == 5);   // friends is a curated-only flag
        b2.set_sources({{"gb", "g1", "", "Alleyway"}}, {{"ps1", "Crash Bandicoot", hubgp::pack_key("Crash Bandicoot"), "Crash Bandicoot (USA)"}});
        f = Filter(); f.catalog = true; f.status = StatusFilter::Owned; CHECK(b2.query(f).ids.size() == 1);
        f.status = StatusFilter::InPack; CHECK(b2.query(f).ids.size() == 1);
        f.status = StatusFilter::NotOwned; CHECK(b2.query(f).ids.size() == 4);
        b2.set_curated(cur);   // drops the catalog again
        CHECK(!b2.has_catalog());
        CHECK(genre_bucket("Role-playing (RPG)") == "RPG" && genre_bucket("Platform") == "Platformer" && genre_bucket("Sports / Basketball") == "Sports" &&
              genre_bucket("") == "Other" && genre_bucket("Board") == "Board & Card" && genre_bucket("Wacky") == "Other" && genre_bucket("Shoot'em Up") == "Shooter");
    }

    /* ---- sort setting keys round-trip ---- */
    for (int i = 0; i < kSortCount; ++i) CHECK((int)sort_from_key(sort_key((Sort)i)) == i && std::string(sort_label((Sort)i)).size() > 3);
    CHECK(sort_from_key("junk") == Sort::Recommended);

    /* ---- the real gameinfo.tsv against ~1,300 curated entries: correctness and speed ---- */
    if (argc > 2) {
        const std::string real = slurp(argv[2]);
        gameinfo::Db db;
        db.load(real);
        CHECK(db.size() > 10000);
        std::vector<Rec> big;
        for (const std::string& c : consoles()) {   // 100 per console, built from titles of that console's catalog rows, and the shipped picks
            int n = 0;
            for (const Rec& r : recs) if (r.sys == c) { big.push_back(r); ++n; }
            for (const gameinfo::Db::Row& row : db.rows()) {
                if (n >= 100) break;
                if (row.sys != c || row.year == 0) continue;
                Rec r = mk(row.title.c_str(), c.c_str(), "Action", row.year, 1 + (n % 4), n % 5 == 0);
                r.why = "Made-up reason";
                big.push_back(r);
                ++n;
            }
        }
        auto t0 = std::chrono::steady_clock::now();
        auto ms = [&] { double v = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); t0 = std::chrono::steady_clock::now(); return v; };
        Browser bb;
        bb.set_curated(big);
        std::vector<Rec> cat = catalog_from(db, big);
        double t_cat = ms();
        bb.set_catalog(cat);
        CHECK(cat.size() > 8000 && cat.size() + 200 < db.size());
        std::set<std::string> ckeys;
        for (const Rec& r : big) ckeys.insert(r.sys + "|" + hubgp::pack_key(r.title));
        std::set<std::string> dup;
        for (const Rec& r : cat) {
            CHECK(!ckeys.count(r.sys + "|" + r.key));
            CHECK(dup.insert(r.sys + "|" + r.key).second);   // no title twice within a console
        }
        std::vector<Owned> own;
        std::vector<hubgp::PackGame> pack;
        for (size_t i = 0; i < cat.size(); i += 7) own.push_back({cat[i].sys, "k" + std::to_string(i), "", cat[i].title});
        for (size_t i = 3; i < cat.size(); i += 5) pack.push_back({cat[i].sys, cat[i].title, hubgp::pack_key(cat[i].title), cat[i].title});
        bb.set_sources(own, pack);
        ms();
        Filter f;
        Browser::Result r = bb.query(f);
        double t_cur = ms();
        CHECK(r.ids.size() == big.size());
        f.catalog = true; f.sort = Sort::Newest; f.query = "mario"; f.genres = {"Action", "Platformer"};
        r = bb.query(f);
        double t_cf = ms();
        CHECK(!r.ids.empty());
        f = Filter(); f.catalog = true; f.status = StatusFilter::InPack;
        r = bb.query(f);
        double t_stat = ms();
        CHECK(!r.ids.empty() && r.pending == 0);
        f.sort = Sort::Title; f.status = StatusFilter::Any;
        r = bb.query(f);
        double t_sort = ms();
        CHECK(r.ids.size() == cat.size());
        std::printf("perf: catalog build %.1f ms (%zu games), curated query %.2f ms, filtered catalog %.2f ms, full status pass %.1f ms, full title sort %.1f ms\n",
                    t_cat, cat.size(), t_cur, t_cf, t_stat, t_sort);
        CHECK(t_cur < 50 && t_cf < 100 && t_sort < 150);
        /* the indexed classifier agrees with the plain one on a sample */
        Classifier cc(own, pack);
        for (size_t i = 0; i < cat.size(); i += 97) {
            Match a = classify(cat[i], own, pack), b = cc.classify(cat[i]);
            CHECK(a.st == b.st && a.key == b.key && a.pack_title == b.pack_title);
        }
        /* paging walks the whole catalog exactly once */
        Pager pg;
        size_t steps = 0;
        while (pg.has_more(r.ids.size())) { pg.more(r.ids.size()); ++steps; }
        CHECK(pg.visible(r.ids.size()) == r.ids.size() && steps == (r.ids.size() - 1) / kBatch);
    }

    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("ok\n");
    return 0;
}
