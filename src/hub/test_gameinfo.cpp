/* Game info: offline metadata lookup, Wikidata/Wikipedia matching on RECORDED fixtures (no live network), cache, presentation. Console test. */
#include "gameinfo.h"
#include "gameinfo_net.h"
#include "gameinfo_queue.h"
#include "gametdb.h"
#include "winutil.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

using namespace gameinfo;

static Query Q(const char* sys, const char* title, const char* serial = "", const char* alt = "") {
    Query q;
    q.system = sys; q.title = title; q.serial = serial; q.alt_title = alt;
    return q;
}

int main() {
    /* ---- normalisation ---- */
    CHECK(normalize("Pok\xC3\xA9mon: Emerald Version") == "pokemonemerald");
    CHECK(normalize("Pokemon - Emerald") == "pokemonemerald");
    CHECK(normalize("The Legend of Zelda") == normalize("Legend of Zelda"));   // leading "the" ignored
    CHECK(normalize("Super Smash Bros. Melee") == "supersmashbrosmelee");
    CHECK(normalize("Banjo & Kazooie") == normalize("Banjo and Kazooie"));
    CHECK(normalize("Link's Awakening") == "linksawakening");
    CHECK(normalize_serial("slus_200.62") == "SLUS-20062");
    CHECK(normalize_serial("NALE") == "NALE");
    CHECK(clean_title("Legend of Zelda, The - Ocarina of Time (USA) (Rev A)") == "The Legend of Zelda: Ocarina of Time");
    CHECK(clean_title("Crash Bash (USA)") == "Crash Bash");

    /* ---- offline db (the real bundled data) ---- */
    std::string tsv;
    CHECK(winutil::read_file(GAMEINFO_DB, &tsv));
    Db db;
    size_t n = db.load(tsv);
    CHECK(n > 5000 && db.loaded());
    Meta m = db.lookup(Q("n64", "SUPER SMASH BROS", "NALE"));
    CHECK(m.found && m.by_serial && m.developer == "HAL Laboratory" && m.publisher == "Nintendo" && m.genre == "Fighting");
    CHECK(m.year == 1999 && m.month == 4 && m.players == 4 && m.esrb == "E");
    m = db.lookup(Q("n64", "ZELDA OOT", "", "The Legend of Zelda: Ocarina of Time"));   // by (catalog) title
    CHECK(m.found && !m.by_serial && m.famitsu == 40 && m.developer.find("Nintendo") == 0);
    m = db.lookup(Q("gba", "POKEMON EMER", "", "Pokemon Emerald"));
    CHECK(m.found && m.developer == "Game Freak" && m.year == 2005 && m.esrb == "E");
    m = db.lookup(Q("ps1", "Crash Bash", "SCUS_945.70"));
    CHECK(m.found && m.by_serial && m.developer.find("Eurocom") == 0 && m.year == 2000 && m.players == 2);
    CHECK(!db.lookup(Q("n64", "Zzqx Nonexistent Game")).found);
    CHECK(!db.lookup(Q("gc", "Super Smash Bros. Melee")).found);                 // no libretro data for GameCube: online only
    CHECK(!db.lookup(Q("gba", "Super Smash Bros.")).found);                      // title exists only on N64: never cross systems
    CHECK(db.lookup(Q("gbc", "Pokemon: Red Version")).found);                    // GB <-> GBC fall back
    CHECK(!db.lookup(Q("n64", "")).found);

    /* ---- online resolve on fixtures ---- */
    gameinfo_net::set_fixture_dir(GAMEINFO_FIXTURES);
    Fetcher f = [](const std::string& u, std::string* b, int* s) { return gameinfo_net::fetch(u, b, s); };
    Online o;
    CHECK(resolve_online(Q("n64", "Super Smash Bros."), f, &o) == ResolveResult::Ok);
    CHECK(o.qid == "Q870981" && o.status == Online::Ok);
    CHECK(o.description.find("1999") != std::string::npos && o.page_url == "https://en.wikipedia.org/wiki/Super_Smash_Bros._(video_game)");
    CHECK(o.developer == "HAL Laboratory" && o.esrb == "E");     // the N64 ESRB statement, not the Wii U one
    CHECK(resolve_online(Q("gc", "Super Smash Bros. Melee"), f, &o) == ResolveResult::Ok && o.qid == "Q1052131" && o.esrb == "T" && o.year == 2001);
    CHECK(resolve_online(Q("gba", "POKEMON EMER", "", "Pokemon Emerald"), f, &o) == ResolveResult::Ok && o.qid == "Q2045645" && o.pegi == "PEGI 3");
    CHECK(resolve_online(Q("ps1", "Crash Bash"), f, &o) == ResolveResult::Ok && o.qid == "Q597808" && o.description.find("Crash Bash") == 0);
    CHECK(resolve_online(Q("n64", "The Legend of Zelda: Ocarina of Time"), f, &o) == ResolveResult::Ok);
    CHECK(o.ratings.size() == 1 && o.ratings[0].source == "Metacritic" && o.ratings[0].text == "99");
    Online zelda = o;
    /* Wii U: the library title (meta.xml long name) and the archive file name both resolve against the Wii U platform */
    CHECK(resolve_online(Q("wiiu", "The Legend of Zelda: Twilight Princess HD"), f, &o) == ResolveResult::Ok && o.qid == "Q23008476" && o.esrb == "T" && o.year == 2016);
    CHECK(resolve_online(Q("wiiu", "Super Mario 3D World (USA) (En,Fr,Es) (Rev 2)"), f, &o) == ResolveResult::Ok && o.qid == "Q13426947" && o.pegi == "PEGI 3");
    CHECK(resolve_online(Q("3ds", "Super Mario 3D World"), f, &o) == ResolveResult::NotFound);   // right title, wrong console
    /* title right, platform wrong -> not confident, nothing shown */
    CHECK(resolve_online(Q("gba", "Super Smash Bros."), f, &o) == ResolveResult::NotFound && o.status == Online::NotFound && o.description.empty());
    CHECK(resolve_online(Q("n64", "Zzqx Nonexistent Game"), f, &o) == ResolveResult::NotFound);
    CHECK(resolve_online(Q("xbox", "Halo"), f, &o) == ResolveResult::NotFound);   // unknown system: no request at all
    /* title that is only similar: no fixture needed, search results must match exactly (here: no fixture -> network error) */
    CHECK(resolve_online(Q("n64", "Never Recorded Title"), f, &o) == ResolveResult::NetworkError);
    Fetcher dead = [](const std::string&, std::string*, int*) { return false; };
    CHECK(resolve_online(Q("n64", "Super Smash Bros."), dead, &o) == ResolveResult::NetworkError);
    Fetcher busy = [](const std::string&, std::string* b, int* s) { b->clear(); *s = 429; return true; };
    CHECK(resolve_online(Q("n64", "Super Smash Bros."), busy, &o) == ResolveResult::NetworkError);
    /* API etiquette: a descriptive User-Agent with contact */
    CHECK(gameinfo_net::user_agent().find("RetroHub/") == 0 && gameinfo_net::user_agent().find("(https://github.com/fatty8me2/retrohub)") != std::string::npos);
    CHECK(url_search("A B").find("search=A%20B") != std::string::npos && url_summary("Crash Bash").find("/Crash_Bash") != std::string::npos);

    /* ---- cache ---- */
    zelda.fetched = 1000000;
    Online back;
    CHECK(cache_from_json(cache_to_json(zelda), &back));
    CHECK(back.status == Online::Ok && back.qid == zelda.qid && back.description == zelda.description && back.ratings.size() == 1 &&
          back.ratings[0].text == "99" && back.esrb == zelda.esrb && back.developer == zelda.developer && back.fetched == 1000000);
    CHECK(!cache_from_json("not json", &back) && !cache_from_json("{\"v\":2}", &back));
    CHECK(!cache_stale(back, 1000000 + 86400 * 30) && cache_stale(back, 1000000 + 86400 * 91));
    Online none;
    none.status = Online::NotFound;
    none.fetched = 5000;
    none.rev = kCacheRev;   // entries from before the matching fixes (rev 0) are always looked up again
    CHECK(!cache_stale(none, 5000 + 86400 * 6) && cache_stale(none, 5000 + 86400 * 8));
    CHECK(cache_stale(Online(), 0));
    CHECK(cache_name(Q("gba", "POKEMON EMER", "", "Pokemon: Emerald Version")) == "gba_pokemonemerald.json");
    CHECK(cache_name(Q("n64", "Super Smash Bros. (USA)")) == "n64_supersmashbros.json");
    {   // file round trip through a scratch dir
        auto dir = std::filesystem::temp_directory_path() / "hub_gameinfo_test";
        std::filesystem::create_directories(dir);
        std::string p = (dir / cache_name(Q("n64", "Zelda"))).string();
        CHECK(winutil::write_file_atomic(p, cache_to_json(zelda)));
        std::string t;
        CHECK(winutil::read_file(p, &t) && cache_from_json(t, &back) && back.qid == zelda.qid);
        std::filesystem::remove_all(dir);
    }

    /* ---- presentation ---- */
    Meta zm = db.lookup(Q("n64", "x", "", "The Legend of Zelda: Ocarina of Time"));
    auto chips = rating_chips(zm, zelda);
    CHECK(chips.size() == 2 && chips[0].label == "Metacritic" && chips[0].value == "99" && chips[1].label == "Famitsu" && chips[1].value == "40/40");
    CHECK(rating_chips(Meta(), Online()).empty());
    Online many;
    for (const char* s : {"IGN", "Zed", "Metacritic", "GameRankings", "Foo", "Bar"}) many.ratings.push_back({s, "9"});
    auto c2 = rating_chips(Meta(), many, 4);
    CHECK(c2.size() == 4 && c2[0].label == "Metacritic" && c2[1].label == "GameRankings" && c2[2].label == "IGN");
    CHECK(age_badge(zm, zelda) == "ESRB E");
    Online pe;
    pe.pegi = "PEGI 12";
    CHECK(age_badge(Meta(), pe) == "PEGI 12" && age_badge(Meta(), Online()).empty());
    Online ee;
    ee.esrb = "T";
    CHECK(age_badge(Meta(), ee) == "ESRB T");
    CHECK(players_text(0).empty() && players_text(1) == "1 player" && players_text(4) == "1-4 players");
    CHECK(release_text(zm, zelda) == "November 1998" && release_text(Meta(), ee).empty());
    Online yr;
    yr.year = 2001;
    CHECK(release_text(Meta(), yr) == "2001");

    /* ---- title variants, Switch 2, redirected sitelinks, GameTDB fallback, generated text (synthetic responses, no network) ---- */
    {
        auto has = [](const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); };
        auto tv = title_variants(Q("gc", "Zelda: The Wind Waker"));
        CHECK(tv.size() == 2 && tv[0] == "Zelda: The Wind Waker" && has(tv, "The Legend of Zelda: The Wind Waker"));
        tv = title_variants(Q("ps2", "Dragon Ball Z - Budokai 3"));
        CHECK(tv.size() == 1 && tv[0] == "Dragon Ball Z: Budokai 3");
        tv = title_variants(Q("n64", "Zelda: Ocarina of Time Master Quest"));
        CHECK(has(tv, "The Legend of Zelda: Ocarina of Time Master Quest") && has(tv, "The Legend of Zelda: Ocarina of Time"));
        tv = title_variants(Q("gc", "The Legend of Zelda: Twilight Princess (GameCube)", "", "The Legend of Zelda: Twilight Princess"));
        CHECK(tv.size() == 1 && tv[0] == "The Legend of Zelda: Twilight Princess");   // the curated title wins and the cleaned duplicate folds into it
        tv = title_variants(Q("nds", "Pokemon Black", "", "Pokemon Black Version"));
        CHECK(tv.size() == 1);                                                         // "version" is ignored by normalize(): one lookup, not two
        CHECK(platform_qids("switch").size() == 2 && platform_qids("switch")[1] == "Q122761124" && platform_qids("n64").size() == 1 && platform_qids("zz").empty());

        struct Fake { std::string plat, label, qid, sitelink, page_item; };
        auto make_fetcher = [](const Fake& fk) -> Fetcher {
            return [fk](const std::string& u, std::string* body, int* st) {
                *st = 200;
                if (u.find("action=wbsearchentities") != std::string::npos) {
                    bool hit = false;
                    for (const char* t : {"Pokemon Black", "The Legend of Zelda: The Wind Waker", "Some Switch Game"})
                        if (u == url_search(t)) hit = true;
                    *body = hit ? "{\"search\":[{\"id\":\"" + fk.qid + "\",\"label\":\"" + fk.label + "\",\"match\":{\"text\":\"" + fk.label + "\"}}]}" : "{\"search\":[]}";
                } else if (u.find("props=claims") != std::string::npos) {
                    *body = "{\"entities\":{\"" + fk.qid + "\":{\"claims\":{\"P31\":[{\"mainsnak\":{\"datavalue\":{\"value\":{\"id\":\"Q7889\"}}}}],"
                            "\"P400\":[{\"mainsnak\":{\"datavalue\":{\"value\":{\"id\":\"" + fk.plat + "\"}}}}]},\"labels\":{\"en\":{\"value\":\"" + fk.label +
                            "\"}},\"sitelinks\":{\"enwiki\":{\"title\":\"" + fk.sitelink + "\"}}}}}";
                } else if (u.find("props=labels") != std::string::npos) {
                    *body = "{\"entities\":{}}";
                } else if (u.find("page/summary") != std::string::npos) {
                    *body = "{\"type\":\"standard\",\"wikibase_item\":\"" + fk.page_item + "\",\"extract\":\"A role-playing game.\",\"content_urls\":{\"desktop\":{\"page\":\"https://en.wikipedia.org/wiki/X\"}}}";
                } else { *st = 404; body->clear(); }
                return true;
            };
        };
        Online fo;
        // "Pokemon Black": the sitelink is a redirect to a page about the pair, whose own Wikidata item differs -> the description must still be used
        CHECK(resolve_online(Q("nds", "Pokemon Black"), make_fetcher({"Q170323", "Pok\xC3\xA9mon Black", "Q16683542", "Pok\xC3\xA9mon Black", "Q816034"}), &fo) == ResolveResult::Ok);
        CHECK(fo.qid == "Q16683542" && fo.description == "A role-playing game." && fo.source == "Wikipedia");
        // the short library title finds the full-title item through the variants
        CHECK(resolve_online(Q("gc", "Zelda: The Wind Waker"), make_fetcher({"Q182172", "The Legend of Zelda: The Wind Waker", "Q852563", "X", "Q852563"}), &fo) == ResolveResult::Ok && fo.qid == "Q852563");
        // Switch 2 items match a Switch library entry; a PC-only item does not
        CHECK(resolve_online(Q("switch", "Some Switch Game"), make_fetcher({"Q122761124", "Some Switch Game", "Q1", "X", "Q1"}), &fo) == ResolveResult::Ok);
        CHECK(resolve_online(Q("switch", "Some Switch Game"), make_fetcher({"Q1406", "Some Switch Game", "Q1", "X", "Q1"}), &fo) == ResolveResult::NotFound);

        // GameTDB fills what Wikipedia has not: a NotFound becomes Ok with the synopsis, the key-features list is cut, the source says GameTDB
        gametdb::Rec rec;
        rec.id = "ARDE01"; rec.title = "Super Mario 3D World"; rec.developer = "Nintendo EAD Tokyo"; rec.publisher = "Nintendo"; rec.esrb = "E"; rec.pegi = "PEGI 3";
        rec.genre = "action,platformer"; rec.year = 2013;
        rec.synopsis = "Leap into the first multiplayer Mario platformer set in a 3D world! Play as Mario, Luigi, Princess Peach, and Toad in the all-new Sprixie Kingdom.\n\n"
                       "Key Features:\nThe first multiplayer Mario platformer set in a 3D world\nCat Mario can climb high";
        Fetcher none = [](const std::string&, std::string* b, int* st) { b->clear(); *st = 404; return true; };
        TdbFinder finder = [&](const Query&) { return &rec; };
        Online od;
        CHECK(resolve_description(Q("wiiu", "Super Mario 3D World"), none, finder, &od) == ResolveResult::Ok);
        CHECK(od.status == Online::Ok && od.source == "GameTDB" && od.description.find("Leap into") == 0 && od.description.find("Key Features") == std::string::npos);
        CHECK(od.description.find('\n') == std::string::npos && od.page_url == "https://www.gametdb.com/WiiU/ARDE01");
        CHECK(od.developer == "Nintendo EAD Tokyo" && od.publisher == "Nintendo" && od.esrb == "E" && od.pegi == "PEGI 3" && od.genre == "Action" && od.year == 2013);
        // no record: stays NotFound; a record without a synopsis still supplies the facts
        CHECK(resolve_description(Q("wiiu", "Super Mario 3D World"), none, [](const Query&) { return (const gametdb::Rec*)nullptr; }, &od) == ResolveResult::NotFound && od.description.empty());
        gametdb::Rec bare = rec;
        bare.synopsis.clear();
        CHECK(resolve_description(Q("wiiu", "X"), none, [&](const Query&) { return &bare; }, &od) == ResolveResult::Ok && od.description.empty() && od.developer == "Nintendo EAD Tokyo");
        // a network failure is never papered over by GameTDB
        Fetcher down = [](const std::string&, std::string*, int* st) { *st = 0; return false; };
        CHECK(resolve_description(Q("wiiu", "Super Mario 3D World"), down, finder, &od) == ResolveResult::NetworkError);
        // Wikipedia wins when it has a description
        CHECK(resolve_description(Q("nds", "Pokemon Black"), make_fetcher({"Q170323", "Pok\xC3\xA9mon Black", "Q16683542", "Pok\xC3\xA9mon Black", "Q816034"}), finder, &od) == ResolveResult::Ok && od.source == "Wikipedia");
        // very long synopsis: cut at a sentence end near 650 characters
        gametdb::Rec lng = rec;
        lng.synopsis.clear();
        for (int i = 0; i < 40; ++i) lng.synopsis += "This is a sentence about the game. ";
        CHECK(resolve_description(Q("wiiu", "Y"), none, [&](const Query&) { return &lng; }, &od) == ResolveResult::Ok && od.description.size() <= 650 && od.description.size() > 400 && od.description.back() == '.');

        // generated description: factual, from metadata only
        Meta gm;
        gm.found = true; gm.year = 2001; gm.genre = "Racing"; gm.developer = "Rare"; gm.players = 4;
        CHECK(generated_description(Q("n64", "Diddy Kong Racing"), gm, Online()) == "A 2001 racing game for the Nintendo 64 by Rare, for 1-4 players.");
        gm.genre = "Action"; gm.players = 1; gm.year = 0;
        CHECK(generated_description(Q("gba", "X"), gm, Online()) == "An action game for the Game Boy Advance by Rare, for 1 player.");
        Online wd;
        wd.genre = "Role-playing video game"; wd.developer = "Game Freak"; wd.year = 2010;
        CHECK(generated_description(Q("nds", "Pokemon Black"), Meta(), wd) == "A 2010 role-playing game for the Nintendo DS by Game Freak.");
        wd = Online();
        wd.publisher = "Nintendo";
        CHECK(generated_description(Q("gc", "X"), Meta(), wd) == "A game for the Nintendo GameCube by Nintendo.");
        CHECK(generated_description(Q("ps2", "X"), Meta(), Online()) == "A game for the PlayStation 2.");
        CHECK(generated_description(Q("switch", "X"), Meta(), Online(), true) == "A game for the Nintendo Switch 2.");
        CHECK(generated_description(Q("n64", "Smash Remix 2.0"), Meta(), Online(), false, "Super Smash Bros.") == "A fan-made modification of Super Smash Bros. for the Nintendo 64.");
        CHECK(generated_description(Q("gb", "X"), Meta(), Online(), false, "*") == "A fan-made modification for the Game Boy.");
        Meta nm;
        nm.genre = "Fighting"; nm.developer = "HAL Laboratory";
        CHECK(generated_description(Q("n64", "X"), nm, Online()).find("fighting game") != std::string::npos);
        CHECK(system_display_name("wiiu") == "Wii U" && system_display_name("3ds") == "Nintendo 3DS");

        // the cache keeps the source and rev; entries from before the matching fixes are looked up again
        Online c;
        c.status = Online::Ok; c.fetched = 1000; c.description = "d"; c.source = "GameTDB"; c.rev = kCacheRev;
        Online cb;
        CHECK(cache_from_json(cache_to_json(c), &cb) && cb.source == "GameTDB" && cb.rev == kCacheRev && !cache_stale(cb, 1000 + 86400));
        Online old;
        old.status = Online::Ok; old.fetched = 1000;   // ok but empty, written by the old code (rev 0)
        CHECK(cache_stale(old, 1000 + 60));
        old.description = "text";
        CHECK(!cache_stale(old, 1000 + 60));            // an old result that has a description is kept
        old.status = Online::NotFound; old.description.clear();
        CHECK(cache_stale(old, 1000 + 60));
        std::string legacy = "{\"v\":1,\"status\":\"ok\",\"fetched\":5,\"description\":\"hello\",\"url\":\"u\"}";
        CHECK(cache_from_json(legacy, &cb) && cb.source == "Wikipedia" && cb.rev == 0);

        // the background queue: urgent first (newest urgent in front), background keeps arrival order, one entry per name, handled names stay handled
        PrefetchQueue<int> pq;
        CHECK(pq.add("bg1", 1, false) && pq.add("bg2", 2, false) && pq.add("bg3", 3, false));
        CHECK(!pq.add("bg2", 2, false));                  // duplicate ignored
        CHECK(pq.add("u1", 10, true));
        CHECK(pq.add("bg3", 3, true));                    // a waiting background entry becomes urgent and jumps ahead
        CHECK(!pq.add("u1", 10, true));                   // already urgent: keeps its place
        CHECK(pq.size() == 4 && pq.front_urgent());
        auto e1 = pq.pop();
        CHECK(e1.name == "bg3" && e1.urgent);
        CHECK(pq.pop().name == "u1");
        CHECK(!pq.front_urgent());
        CHECK(pq.pop().name == "bg1" && pq.pop().name == "bg2" && pq.empty());
        CHECK(!pq.add("bg1", 1, false) && !pq.add("bg1", 1, true) && pq.handled("bg1"));   // handled this session: not looked up again, even when urgent
        pq.requeue(e1);                                   // offline: back at the end, open again
        CHECK(pq.size() == 1 && !pq.handled("bg3") && !pq.add("bg3", 3, false));
        pq.pop();
        pq.forget("bg3");
        CHECK(pq.add("bg3", 3, true) && pq.size() == 1);
        CHECK(!PrefetchQueue<int>().front_urgent());
    }

    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("hub_gameinfo_test: all passed (%zu games in the bundle)\n", n);
    return 0;
}
