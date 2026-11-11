/* hub_crashlog_test: hub log rotation + header, privacy scrubbing, crash report formatter, diagnostics zip (non-ASCII temp dir), the
 * next-start report finder, and a child-process crash of hub_crashlog_test_helper.exe (argv[1]). Temp directories only. */
#include "crash_report.h"
#include "diag_logic.h"
#include "hub_log.h"
#include "hub_log_rotate.h"
#include "miniz.h"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static void spit(const fs::path& p, const std::string& s) { std::ofstream o(p, std::ios::binary); o << s; }
static bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

/* UTF-8 with non-ASCII letters; written as escapes so the source codepage cannot matter: "Zuerich-Ue-Euro" style */
static const char* kUni = "Tr\xC3\xA4" "mer-\xE2\x82\xAC-\xC3\x9C";

static fs::path fresh(const char* tag) {
    fs::path p = fs::temp_directory_path() / fs::u8path(std::string("hubcrashlog_") + tag + "_" + std::to_string(GetCurrentProcessId()) + "_" + kUni);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}
static std::string dir8(const fs::path& p) { return p.u8string() + "\\"; }

static void test_scrub() {
    const std::string prof = std::string("C:\\Users\\B") + "\xC3\x9C" + "b";
    std::string s = hublog::scrub("open C:\\Users\\B\xC3\x9C" "b\\AppData\\x and c:/users/b\xC3\x9C" "b/y peer 10.1.2.3:80 v1.40.10 build 1.2.3.4.5 C:\\Users\\B\xC3\x9C" "bob\\z", prof);
    CHECK(s == "open %USERPROFILE%\\AppData\\x and %USERPROFILE%/y peer <ip>:80 v1.40.10 build 1.2.3.4.5 C:\\Users\\B\xC3\x9C" "bob\\z");
    CHECK(hublog::scrub("nothing here", "") == "nothing here");
    CHECK(hublog::scrub("ip 192.168.0.1.", "") == "ip <ip>.");
}

static void test_rotate_pure() {
    fs::path d = fresh("rot");
    const std::wstring w = d.wstring() + L"\\";
    spit(d / "hub.log", "A"); spit(d / "hub.1.log", "B"); spit(d / "hub.2.log", "C");
    hublog::rotate(w, 3);
    CHECK(!fs::exists(d / "hub.log"));
    CHECK(slurp(d / "hub.1.log") == "A");
    CHECK(slurp(d / "hub.2.log") == "B");   // C (the oldest) is gone
    CHECK(!fs::exists(d / "hub.3.log"));
    CHECK(hublog::needs_rotation(900, 200, 1000));
    CHECK(!hublog::needs_rotation(0, 5000, 1000));
    CHECK(!hublog::needs_rotation(100, 200, 1000));
}

static void test_log_rotation_and_header() {
    fs::path d = fresh("log");
    hublog::Options o;
    o.max_bytes = 4000;
    o.worker = false;
    o.profile_dir = std::string("C:\\Users\\B") + "\xC3\x9C" + "b";
    hublog::init(dir8(d), "1.2.3", o);
    for (int i = 0; i < 400; ++i) {
        hublog::info("line %d path C:\\Users\\B\xC3\x9C" "b\\Docs peer 8.8.4.4", i);
        if (i % 10 == 9) hublog::flush();
    }
    hublog::warn("warn line");
    hublog::error("error line");
    // ring: the newest lines, oldest first
    char tail[8192];
    hublog::recent_lines(tail, sizeof tail, 5);
    std::string t = tail;
    CHECK(has(t, "ERROR error line") && has(t, "WARN  warn line") && has(t, "line 399"));
    size_t nl = 0;
    for (char c : t) nl += c == '\n';
    CHECK(nl == 5);
    hublog::shutdown();
    CHECK(fs::exists(d / "hub.log"));
    CHECK(fs::exists(d / "hub.1.log"));
    CHECK(fs::exists(d / "hub.2.log"));
    CHECK(!fs::exists(d / "hub.3.log"));
    for (const char* n : {"hub.log", "hub.1.log", "hub.2.log"}) CHECK(fs::file_size(d / n) <= 4000);
    const std::string all = slurp(d / "hub.log") + slurp(d / "hub.1.log") + slurp(d / "hub.2.log");
    CHECK(has(all, "%USERPROFILE%\\Docs"));
    CHECK(!has(all, "B\xC3\x9C" "b"));
    CHECK(!has(all, "8.8.4.4") && has(all, "<ip>"));
    CHECK(has(slurp(d / "hub.log"), "shutdown"));

    fs::path d2 = fresh("hdr");
    hublog::Options o2;
    o2.worker = true;   // the real path: worker thread + shutdown flush
    hublog::init(dir8(d2), "7.8.9", o2);
    hublog::info("hello");
    hublog::shutdown();
    const std::string h = slurp(d2 / "hub.log");
    CHECK(has(h, "RetroHub session started"));
    CHECK(has(h, "RetroHub 7.8.9"));
    CHECK(has(h, "Windows ") && has(h, " build "));
    CHECK(has(h, "CPU cores: ") && has(h, "RAM: ") && has(h, " MB"));
    CHECK(has(h, "GPU: ") && has(h, "Locale: "));
    CHECK(has(h, "INFO  hello"));
    // a second session appends below the first and starts with its own header
    hublog::init(dir8(d2), "7.8.10", o2);
    hublog::shutdown();
    const std::string h2 = slurp(d2 / "hub.log");
    CHECK(has(h2, "RetroHub 7.8.9") && has(h2, "RetroHub 7.8.10"));
}

static void test_formatter() {
    crashrep::Module mods[2] = {};
    std::snprintf(mods[0].name, sizeof mods[0].name, "RetroHub.exe"); mods[0].base = 0x140000000ull; mods[0].size = 0x500000;
    std::snprintf(mods[1].name, sizeof mods[1].name, "SDL2.dll"); mods[1].base = 0x7ff800000000ull; mods[1].size = 0x200000;
    const uint64_t stack[2] = {0x140001234ull, 0x7ff800000010ull};
    const char* rn[3] = {"rax", "rbx", "rip"};
    const uint64_t rv[3] = {1, 0xdeadbeefull, 0x1400abcdeull};
    crashrep::Input in;
    in.code = 0xC0000005u;
    in.address = 0x1400abcdeull;
    in.n_params = 2; in.params[0] = 1; in.params[1] = 0x18;
    in.thread_id = 4242;
    in.uptime_ms = 61000;
    in.version = "1.40.9";
    in.system = "Windows 10.0 build 26200\nCPU cores: 8\n";
    in.time_text = "2026-11-10 12:00:00";
    in.phase = "draw_frame";
    in.modules = mods; in.n_modules = 2;
    in.stack = stack; in.n_stack = 2;
    in.reg_names = rn; in.reg_values = rv; in.n_regs = 3;
    in.log_tail = "2026-11-10 11:59:59.000 INFO  last line\n";
    static char buf[16384];
    size_t n = crashrep::format_report(in, buf, sizeof buf);
    const std::string r = buf;
    CHECK(n == r.size());
    CHECK(has(r, "RetroHub crash report"));
    CHECK(has(r, "Version:      1.40.9"));
    CHECK(has(r, "0xc0000005  ACCESS_VIOLATION"));
    CHECK(has(r, "Address:      0x00000001400abcde  RetroHub.exe+0xabcde"));
    CHECK(has(r, "RetroHub.exe+0xabcde"));
    CHECK(has(r, "write to 0x18"));
    CHECK(has(r, "UI phase:     draw_frame"));
    CHECK(has(r, "RetroHub.exe+0x1234"));
    CHECK(has(r, "SDL2.dll+0x10"));
    CHECK(has(r, "rbx 0x00000000deadbeef"));
    CHECK(has(r, "last line"));
    CHECK(has(r, "Uptime:       61 s"));
    // truncation is safe and terminated
    char small[64];
    size_t sn = crashrep::format_report(in, small, sizeof small);
    CHECK(sn == std::strlen(small) && sn < sizeof small);
    // locate
    int idx = -1; uint64_t off = 0;
    CHECK(crashrep::locate(mods, 2, 0x7ff800000042ull, &idx, &off) && idx == 1 && off == 0x42);
    CHECK(!crashrep::locate(mods, 2, 0x1000, &idx, &off));
    CHECK(std::string(crashrep::exception_name(0xC00000FDu)) == "STACK_OVERFLOW");
}

static void test_diag_zip() {
    fs::path d = fresh("zip");
    fs::create_directories(d / "logs");
    spit(d / "logs" / "hub.log", "hub log text");
    spit(d / "logs" / "crash-20260101-000001.txt", "old1");
    for (int i = 2; i <= 8; ++i) { char nb[64]; std::snprintf(nb, sizeof nb, "crash-20260101-00000%d.txt", i); spit(d / "logs" / nb, "c"); }
    spit(d / "logs" / "identity.json", "SECRET");
    auto files = hubdiag::newest_crash_reports_only(hubdiag::collect_dir(d.u8string() + "\\logs", "logs/"));
    int crashes = 0; bool hub = false, old1 = false;
    for (auto& e : files) { crashes += e.zip_name.find("logs/crash-") == 0; hub |= e.zip_name == "logs/hub.log"; old1 |= e.zip_name.find("000001") != std::string::npos; }
    CHECK(crashes == 5 && hub && !old1);
    {   // the byte cap keeps the newest files that fit
        fs::create_directories(d / "cap");
        spit(d / "cap" / "old.log", std::string(600, 'o'));
        fs::last_write_time(d / "cap" / "old.log", fs::file_time_type::clock::now() - std::chrono::hours(48));
        spit(d / "cap" / "new.log", std::string(600, 'n'));
        auto capped = hubdiag::limit_total_bytes(hubdiag::collect_dir(d.u8string() + "\\cap", "cap/"), 1000);
        CHECK(capped.size() == 1 && capped[0].zip_name == "cap/new.log");
        CHECK(hubdiag::limit_total_bytes(hubdiag::collect_dir(d.u8string() + "\\cap", "cap/"), 2000).size() == 2);
    }
    const fs::path zip = d / fs::u8path(std::string("out-") + kUni + ".zip");
    std::string err;
    CHECK(hubdiag::write_zip(zip.u8string(), files, {{"versions.txt", "RetroHub 1"}}, &err));
    CHECK(err.empty());
    CHECK(fs::exists(zip));
    CHECK(!fs::exists(fs::path(zip).concat(".tmp")));
    const std::string bytes = slurp(zip);
    mz_zip_archive z;
    std::memset(&z, 0, sizeof z);
    CHECK(mz_zip_reader_init_mem(&z, bytes.data(), bytes.size(), 0));
    CHECK(mz_zip_reader_locate_file(&z, "versions.txt", nullptr, 0) >= 0);
    CHECK(mz_zip_reader_locate_file(&z, "logs/hub.log", nullptr, 0) >= 0);
    CHECK(mz_zip_reader_locate_file(&z, "logs/identity.json", nullptr, 0) < 0);
    mz_zip_reader_end(&z);
}

static void test_find_unseen() {
    fs::path d = fresh("seen");
    const std::string ds = dir8(d);
    crashrep::Found f;
    CHECK(!crashrep::find_unseen(ds, &f));
    spit(d / "crash-20260101-010101.txt", "a");
    spit(d / "hang-20260101-010101.txt", "not a crash");
    spit(d / "crash-notes.txt", "not a crash either");
    CHECK(crashrep::find_unseen(ds, &f) && f.name == "crash-20260101-010101.txt");
    CHECK(crashrep::read_report(f.path_utf8) == "a");
    crashrep::mark_all_seen(ds);
    CHECK(!crashrep::find_unseen(ds, &f));
    spit(d / "crash-20260102-020202.txt", "b");
    CHECK(crashrep::find_unseen(ds, &f) && f.name == "crash-20260102-020202.txt");
    CHECK(crashrep::list_reports(ds).size() == 2);
}

static bool run_helper(const std::wstring& exe, const fs::path& dir, const wchar_t* mode, DWORD* exit_code) {
    std::wstring cmd = L"\"" + exe + L"\" \"" + dir.wstring() + L"\" " + mode;
    STARTUPINFOW si;
    std::memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    std::memset(&pi, 0, sizeof pi);
    std::wstring mut = cmd;
    if (!CreateProcessW(nullptr, &mut[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    DWORD w = WaitForSingleObject(pi.hProcess, 60000);
    if (w != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 99);   // our own child, by handle
    GetExitCodeProcess(pi.hProcess, exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return w == WAIT_OBJECT_0;
}

static void test_child_crash(const std::wstring& helper, const wchar_t* mode, const char* expect_name) {
    fs::path d = fresh(expect_name);
    DWORD code = 0;
    CHECK(run_helper(helper, d, mode, &code));
    CHECK(code != 0 && code != 3 && code != 99);
    auto reports = crashrep::list_reports(dir8(d));
    CHECK(reports.size() == 1);
    if (reports.size() != 1) return;
    const std::string r = slurp(d / fs::u8path(reports[0]));
    std::printf("--- %ls: %zu bytes, exit 0x%lx\n", mode, r.size(), (unsigned long)code);
    CHECK(has(r, "RetroHub crash report"));
    CHECK(has(r, "Version:      9.9.9"));
    CHECK(has(r, expect_name));
    CHECK(has(r, "Address:      0x"));
    CHECK(has(r, "hub_crashlog_test_helper.exe+0x"));
    CHECK(has(r, "UI phase:     helper_phase"));
    CHECK(has(r, "rip ") && has(r, "rsp "));
    CHECK(has(r, "about to crash"));
    CHECK(has(r, "<ip>") && !has(r, "192.168.1.5"));
    CHECK(has(r, "Last hub log lines") && has(r, "Modules"));
    const char* up = std::getenv("USERPROFILE");
    if (up && std::strlen(up) > 3) CHECK(!has(r, up));
    CHECK(has(slurp(d / "hub.log"), "about to crash"));   // flushed from the crash path
    CHECK(!has(r, "dbghelp"));
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("scrub\n"); test_scrub();
    std::printf("rotate\n"); test_rotate_pure();
    std::printf("log\n"); test_log_rotation_and_header();
    std::printf("formatter\n"); test_formatter();
    std::printf("zip\n"); test_diag_zip();
    std::printf("seen\n"); test_find_unseen();
    if (argc > 1) {
        std::wstring helper = fs::u8path(argv[1]).wstring();
        test_child_crash(helper, L"av", "ACCESS_VIOLATION");
        test_child_crash(helper, L"terminate", "C++ exception");        // uncaught throw: the unhandled-exception filter sees it
        test_child_crash(helper, L"terminate-call", "STD_TERMINATE");   // std::terminate() / abort(): the terminate handler
        test_child_crash(helper, L"overflow", "STACK_OVERFLOW");
        {   // two crashes into one folder (usually the same second) never overwrite each other
            fs::path d = fresh("twice");
            DWORD c1 = 0, c2 = 0;
            CHECK(run_helper(helper, d, L"av", &c1));
            CHECK(run_helper(helper, d, L"av", &c2));
            CHECK(crashrep::list_reports(dir8(d)).size() == 2);
        }
    } else {
        std::printf("no helper given: child-crash checks skipped\n");
    }
    std::printf(g_fail ? "hub_crashlog_test: %d FAILED\n" : "hub_crashlog_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
