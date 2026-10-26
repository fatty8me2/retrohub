/* Game info: offline metadata lookup, Wikidata/Wikipedia matching on RECORDED fixtures (no live network), cache, presentation. Console test. */
#include "gameinfo.h"
#include "gameinfo_net.h"
#include "winutil.h"

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

    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("hub_gameinfo_test: all passed (%zu games in the bundle)\n", n);
    return 0;
}
