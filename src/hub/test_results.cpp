// Console test: Plan 4 shared match results (results.h): ranks, parse_round, the file watcher (scratch dir only), and the
// auto-resume guard (resume_logic.h).
#include "results.h"
#include "resume_logic.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubui::results;
namespace fs = std::filesystem;

static const char* kOnline =
    "{\"v\":1,\"game\":\"smash64\",\"id\":\"AB12-777\",\"code\":\"AB12\",\"players\":["
    "{\"slot\":0,\"char\":0,\"place\":1,\"stocks\":0,\"kos\":2,\"falls\":3},"
    "{\"slot\":2,\"char\":1,\"place\":0,\"stocks\":1,\"kos\":3,\"falls\":2},"
    "{\"slot\":3,\"char\":4,\"place\":1,\"stocks\":0,\"kos\":2,\"falls\":3}]}";
static const char* kLocal =
    "{\"v\":1,\"game\":\"remix\",\"id\":\"local-42\",\"code\":\"\",\"source\":\"n64run\",\"local\":true,\"players\":["
    "{\"slot\":0,\"char\":9,\"place\":0,\"stocks\":2,\"kos\":3,\"falls\":1},"
    "{\"slot\":1,\"char\":3,\"place\":1,\"stocks\":0,\"kos\":1,\"falls\":3}]}";

static void write(const fs::path& p, const std::string& s) { std::ofstream f(p, std::ios::binary | std::ios::trunc); f << s; }

int main() {
    RoundResult r;
    CHECK(parse_round(kOnline, &r) && r.id == "AB12-777" && r.game == "smash64" && r.source == "battleship" && !r.local);
    CHECK(r.ranks.size() == 2 && r.ranks[0] == std::vector<int>{2} && r.ranks[1] == (std::vector<int>{0, 3}));   // a tie shares a place
    CHECK(parse_round(kLocal, &r) && r.local && r.source == "n64run" && r.ranks.size() == 2 && r.ranks[0][0] == 0);
    CHECK(!parse_round("", &r) && !parse_round("{}", &r) && !parse_round("{\"game\":\"x\",\"id\":\"y\",\"players\":[{\"slot\":0}]}", &r));

    {   // the watcher returns each match once, survives rewrites of the same match and ignores half-written files
        fs::path d = fs::temp_directory_path() / "rh_results_test";
        fs::remove_all(d);
        fs::create_directories(d);
        fs::path f = d / "result.json";
        Watcher w;
        w.reset(f.u8string());
        std::string text;
        CHECK(!w.poll(&text, &r));                   // no file yet
        write(f, "{\"v\":1,\"game\":");                 // mid-write
        CHECK(!w.poll(&text, &r));
        write(f, kOnline);
        CHECK(w.poll(&text, &r) && r.id == "AB12-777" && text == kOnline);
        CHECK(!w.poll(&text, &r));                   // unchanged
        write(f, std::string(kOnline) + " ");          // same match rewritten: not a new match
        CHECK(!w.poll(&text, &r));
        write(f, kLocal);
        CHECK(w.poll(&text, &r) && r.id == "local-42");
        w.reset("");
        CHECK(!w.poll(&text, &r));
        fs::remove_all(d);
    }

    {   // saves-F1: the newer save wins over an older auto state
        using hubui::resume_allowed;
        CHECK(resume_allowed(0, 0) && resume_allowed(0, 1000) && resume_allowed(1000, 0));   // nothing to protect
        CHECK(resume_allowed(1000, 900));                                                     // save older than the state
        CHECK(resume_allowed(1000, 1000 + hubui::kResumeSlackSec));                           // same exit, written just after
        CHECK(!resume_allowed(1000, 1000 + hubui::kResumeSlackSec + 1));                      // another PC's save arrived later
        CHECK(!resume_allowed(1000, 1000 + 86400));
    }
    if (!fails) std::printf("hub_results_test: all passed\n");
    return fails ? 1 : 0;
}
