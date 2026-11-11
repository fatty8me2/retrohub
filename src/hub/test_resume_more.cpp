// Console test for the auto-resume safety pieces behind resume_more.cpp (now in resume_logic.h): newest battery pick, remembered
// answers, per-system resume table and the resume-vs-newer-save decision. A wrong choice here can overwrite a battery save.
// Temp dir only, no network.
#include "resume_logic.h"
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
namespace fs = std::filesystem;
using namespace hubui;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void touch(const fs::path& p) { std::ofstream f(p, std::ios::binary); f << "x"; }

int main() {
    fs::path root = fs::temp_directory_path() / "retrohub_resume_more_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "a");
    fs::create_directories(root / "empty");
    fs::create_directories(root / "only_bak");
    fs::create_directories(root / "sub" / "nested");
    std::map<std::string, long long> times;   // file name -> fake mtime
    auto ft = [&](const std::string& path) -> long long {
        auto it = times.find(fs::u8path(path).filename().string());
        return it == times.end() ? 0 : it->second;
    };
    auto nb = [&](const char* d) { return newest_battery_in((root / d).u8string(), ft); };

    {   // newest battery: several files, the max wins
        touch(root / "a" / "game.srm"); touch(root / "a" / "game.sav"); touch(root / "a" / "game.eep");
        times["game.srm"] = 100; times["game.sav"] = 500; times["game.eep"] = 300;
        CHECK(nb("a") == 500);
        // equal mtimes: that time is returned (no crash, no preference needed)
        times["game.srm"] = 500; times["game.eep"] = 500;
        CHECK(nb("a") == 500);
        // .bak / .tmp (any case) are never the live save, even when newer
        touch(root / "a" / "game.srm.bak"); touch(root / "a" / "x.TMP"); touch(root / "a" / "y.Bak");
        times["game.srm.bak"] = 9000; times["x.TMP"] = 9500; times["y.Bak"] = 9900;
        CHECK(nb("a") == 500);
        // a folder named like a save is not a file
        fs::create_directories(root / "a" / "dir.sav");
        times["dir.sav"] = 99999;
        CHECK(nb("a") == 500);
        // unreadable times (0) never beat a real one
        times["game.srm"] = 0; times["game.sav"] = 0; times["game.eep"] = 0;
        CHECK(nb("a") == 0);
    }
    {   // missing / empty / only-backup folders: 0 ("nothing to protect")
        CHECK(nb("empty") == 0);
        CHECK(nb("does_not_exist") == 0);
        touch(root / "only_bak" / "g.srm.bak"); times["g.srm.bak"] = 777;
        CHECK(nb("only_bak") == 0);
        touch(root / "sub" / "top.srm"); touch(root / "sub" / "nested" / "deep.srm");
        times["top.srm"] = 10; times["deep.srm"] = 20;
        CHECK(nb("sub") == 10);   // not recursive
    }
    {   // per-system resume table
        const char* yes[] = {"n64", "gb", "gbc", "gba", "nes", "snes"};
        const char* no[] = {"", "N64", "ps1", "ps2", "psp", "gc", "wii", "wiiu", "3ds", "nds", "switch", "genesis", "n64 ", "smash"};
        for (auto s : yes) CHECK(resumes(s));
        for (auto s : no) CHECK(!resumes(s));
    }
    {   // answers: remembered only while the newest save is the one it was given for
        std::map<std::string, Answer> m;
        CHECK(answer_lookup(m, "g1", 100) == ResumeChoice::Ask);
        m["g1"] = {ResumeChoice::Newest, 100};
        m["g2"] = {ResumeChoice::ThisPc, 200};
        CHECK(answer_lookup(m, "g1", 100) == ResumeChoice::Newest);
        CHECK(answer_lookup(m, "g2", 200) == ResumeChoice::ThisPc);
        CHECK(answer_lookup(m, "g1", 101) == ResumeChoice::Ask);    // a newer save arrived: ask again
        CHECK(answer_lookup(m, "g1", 99) == ResumeChoice::Ask);     // any change, not just newer
        CHECK(answer_lookup(m, "g2", 100) == ResumeChoice::Ask);    // keyed per game
        CHECK(answer_lookup(m, "g3", 0) == ResumeChoice::Ask);
        // round trip
        std::map<std::string, Answer> r;
        parse_answers(serialize_answers(m), r);
        CHECK(r.size() == 2 && answer_lookup(r, "g1", 100) == ResumeChoice::Newest && answer_lookup(r, "g2", 200) == ResumeChoice::ThisPc);
        // CRLF, junk lines, missing columns, unknown word means Newest (the safe side), no trailing newline
        std::map<std::string, Answer> j;
        parse_answers("k1\tthis\t50\r\nbad line\r\nk2\tonly\n\nk3\tweird\t7\nk4\tthis\t9", j);
        CHECK(j.size() == 3);
        CHECK(answer_lookup(j, "k1", 50) == ResumeChoice::ThisPc);
        CHECK(answer_lookup(j, "k3", 7) == ResumeChoice::Newest);
        CHECK(answer_lookup(j, "k4", 9) == ResumeChoice::ThisPc);
        CHECK(j.find("k2") == j.end());
        std::map<std::string, Answer> e;
        parse_answers("", e);
        CHECK(e.empty());
        // a garbage time parses to 0 and so never matches a real battery time
        std::map<std::string, Answer> g;
        parse_answers("k\tthis\tabc\n", g);
        CHECK(answer_lookup(g, "k", 1000) == ResumeChoice::Ask);
    }
    {   // the decision: a state older than the battery save must NOT auto-resume
        ResumeInput in;
        in.game_key = "g";
        in.state_time = 1000; in.newest_battery_time = 1000;
        CHECK(decide_resume(in, ResumeChoice::Ask) == ResumeResult::Resume);                     // same session
        in.newest_battery_time = 1000 + kResumeSlackSec;
        CHECK(decide_resume(in, ResumeChoice::Ask) == ResumeResult::Resume);                     // exactly at the slack
        in.newest_battery_time = 1000 + kResumeSlackSec + 1;
        CHECK(decide_resume(in, ResumeChoice::Ask) == ResumeResult::NewerSave);                  // one second past it
        CHECK(resume_needs_choice(in, ResumeChoice::Ask));
        CHECK(decide_resume(in, ResumeChoice::Newest) == ResumeResult::NewerSave);               // answered "newest": still no --resume
        CHECK(!resume_needs_choice(in, ResumeChoice::Newest));
        CHECK(decide_resume(in, ResumeChoice::ThisPc) == ResumeResult::Resume);                  // only an explicit "this PC" resumes
        in.newest_battery_time = 5000;
        CHECK(decide_resume(in, ResumeChoice::Ask) == ResumeResult::NewerSave);
        // battery older than the state, or no battery: resume
        in.newest_battery_time = 10;
        CHECK(decide_resume(in, ResumeChoice::Ask) == ResumeResult::Resume);
        in.newest_battery_time = 0;
        CHECK(decide_resume(in, ResumeChoice::Ask) == ResumeResult::Resume);
        // no state: fresh, whatever the battery says
        in.state_time = 0; in.newest_battery_time = 5000;
        CHECK(decide_resume(in, ResumeChoice::ThisPc) == ResumeResult::Fresh);
        // online / real boot: always fresh, even with an answer
        in.state_time = 1000; in.newest_battery_time = 1000;
        in.online = true;
        CHECK(decide_resume(in, ResumeChoice::ThisPc) == ResumeResult::Fresh);
        in.online = false; in.real_boot = true;
        CHECK(decide_resume(in, ResumeChoice::ThisPc) == ResumeResult::Fresh);
    }
    {   // clock skew: the sidecar catches a newer save whose mtime looks older (only for this game on this PC)
        ResumeInput in;
        in.game_key = "g"; in.this_pc = "pcA";
        in.state_time = 10000; in.newest_battery_time = 9000;   // battery looks older than the state ...
        in.sidecar.valid = true; in.sidecar.pc = "pcA"; in.sidecar.game_key = "g"; in.sidecar.battery_time = 8000;   // ... but is newer than at exit
        CHECK(decide_resume(in, ResumeChoice::Ask) == ResumeResult::NewerSave);
        in.sidecar.pc = "pcB";                                  // sidecar from another PC says nothing
        CHECK(decide_resume(in, ResumeChoice::Ask) == ResumeResult::Resume);
        in.sidecar.pc = "pcA"; in.sidecar.game_key = "other";
        CHECK(decide_resume(in, ResumeChoice::Ask) == ResumeResult::Resume);
        in.sidecar.game_key = "g"; in.this_pc = "";
        CHECK(decide_resume(in, ResumeChoice::Ask) == ResumeResult::Resume);
        in.this_pc = "pcA"; in.sidecar.battery_time = 9000;     // same battery time as at exit: unchanged
        CHECK(decide_resume(in, ResumeChoice::Ask) == ResumeResult::Resume);
        in.sidecar.battery_time = 9000 - kResumeSlackSec;       // within slack
        CHECK(decide_resume(in, ResumeChoice::Ask) == ResumeResult::Resume);
    }
    fs::remove_all(root, ec);
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hub_resume_more_test: all passed\n");
    return 0;
}
