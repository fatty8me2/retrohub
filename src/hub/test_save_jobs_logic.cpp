// Console test for the save sync / import result wording (save_jobs_logic.h). Pure strings.
#include "save_jobs_logic.h"
#include <cstdio>
#include <string>
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
#define CHECK_EQ(a, b) do { std::string _a = (a), _b = (b); if (_a != _b) { std::printf("FAIL %s:%d: %s -> '%s' expected '%s'\n", __FILE__, __LINE__, #a, _a.c_str(), _b.c_str()); ++g_fail; } } while (0)
using hub::saves::SyncReport;

static SyncReport rep(int pushed, int pulled, int conflicts, bool ok = true, const char* err = "") {
    SyncReport r; r.pushed = pushed; r.pulled = pulled; r.conflicts = conflicts; r.ok = ok; r.error = err; return r;
}
static bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

int main() {
    using hubsave::report_summary;
    CHECK_EQ(report_summary(rep(0, 0, 0), "sent", "received"), "Everything is already up to date.");
    CHECK_EQ(report_summary(rep(2, 0, 0), "sent", "received"), "2 files sent.");
    CHECK_EQ(report_summary(rep(0, 3, 0), "sent", "received"), "3 files received.");
    CHECK_EQ(report_summary(rep(2, 3, 0), "sent", "received"), "2 files sent, 3 files received.");
    CHECK_EQ(report_summary(rep(0, 0, 1), "sent", "received"), "1 conflict (the older copy was kept as a backup).");
    CHECK_EQ(report_summary(rep(1, 1, 1), "sent", "received"), "1 file sent, 1 file received, 1 conflict (the older copy was kept as a backup).");
    CHECK_EQ(report_summary(rep(4, 0, 2), "restored", "restored"), "4 files restored, 2 conflicts (the older copy was kept as a backup).");
    CHECK_EQ(report_summary(rep(0, 5, 2), "sent", "received"), "5 files received, 2 conflicts (the older copy was kept as a backup).");
    // failures: wording starts with Failed, carries the error, never claims success, whatever the counters say
    CHECK_EQ(report_summary(rep(0, 0, 0, false, "the sync folder is not reachable"), "sent", "received"), "Failed: the sync folder is not reachable");
    CHECK_EQ(report_summary(rep(0, 0, 0, false), "sent", "received"), "Failed: unknown error");
    CHECK_EQ(report_summary(rep(3, 2, 1, false, "disk full"), "sent", "received"), "Failed: disk full");
    CHECK(!has(report_summary(rep(0, 0, 0, false), "sent", "received"), "up to date"));
    // large counts
    CHECK_EQ(report_summary(rep(100000, 0, 0), "sent", "received"), "100000 files sent.");
    // never an em / en dash, never a doubled full stop, always ends with a full stop except for Failed
    {
        SyncReport cases[] = {rep(0, 0, 0), rep(1, 0, 0), rep(0, 1, 0), rep(0, 0, 1), rep(1, 2, 3), rep(0, 0, 0, false, "x"), rep(0, 0, 0, false)};
        for (auto& c : cases) {
            std::string s = report_summary(c, "sent", "received");
            CHECK(!has(s, "\xE2\x80\x94") && !has(s, "\xE2\x80\x93") && !has(s, ".."));
            CHECK(!s.empty() && (s.back() == '.' || s.rfind("Failed: ", 0) == 0));
        }
    }
    // plain singular / plural wording, no folder names, no doubled "restored"
    CHECK_EQ(report_summary(rep(1, 0, 0), "sent", "received"), "1 file sent.");
    CHECK(!has(report_summary(rep(0, 0, 1), "sent", "received"), "_conflicts"));
    CHECK_EQ(report_summary(rep(1, 1, 0), "restored", "restored"), "2 files restored.");
    CHECK_EQ(report_summary(rep(1, 0, 0), "restored", "restored"), "1 file restored.");
    CHECK(!has(report_summary(rep(0, 0, 2), "sent", "received"), "file(s)"));
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hub_save_jobs_logic_test: all passed\n");
    return 0;
}
