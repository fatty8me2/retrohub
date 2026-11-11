#include "crash_note.h"

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace hubcrash {

Verdict classify(uint32_t c) {
    if (c == kHubEndExitCode) return Verdict::HubEnded;   // 0: a clean exit, and also what the hang watch passes (both mean "not a crash")
    switch (c) {
        case 0xC000013Au:   // STATUS_CONTROL_C_EXIT: console closed / logoff / Ctrl+C: the user ended it
        case 0x40010004u:   // DBG_TERMINATE_PROCESS (a debugger ended it)
            return Verdict::Normal;
        case 0x80000001u:   // STATUS_GUARD_PAGE_VIOLATION
        case 0x80000002u:   // STATUS_DATATYPE_MISALIGNMENT
        case 0x80000003u:   // STATUS_BREAKPOINT (abort / assert / __debugbreak)
        case 0x80000004u:   // STATUS_SINGLE_STEP
        case 0xE06D7363u:   // unhandled C++ exception ("msc" + 0xE0)
        case 0xE0434352u:   // unhandled .NET exception
            return Verdict::Crash;
        default: break;
    }
    /* Every NTSTATUS error (severity 11, top bits 0xC) is a fault the process died of: 0xC0000005 access violation, 0xC0000409 stack buffer overrun
     * / fail-fast, 0xC00000FD stack overflow, 0xC000001D illegal instruction, 0xC0000374 heap corruption, 0xC0000094 divide by zero ... */
    if ((c & 0xF0000000u) == 0xC0000000u) return Verdict::Crash;
    return Verdict::Normal;   // 0, 1, 2 ... an ordinary exit code (also what End task gives)
}

std::string hex_code(uint32_t c) {
    static const char* d = "0123456789ABCDEF";
    std::string s = "0x00000000";
    for (int i = 0; i < 8; ++i) s[9 - i] = d[(c >> (4 * i)) & 0xF];
    return s;
}

std::string default_dumps_dir() {
    const char* la = std::getenv("LOCALAPPDATA");
    if (!la || !*la) return "";
    std::string p = la;
    if (p.back() != '\\' && p.back() != '/') p += '\\';
    return p + "CrashDumps\\";
}

static std::string lower(std::string s) { for (char& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; }

std::string find_dump(const std::string& dumps_dir, const std::string& exe_name, unsigned long pid) {
    if (dumps_dir.empty() || exe_name.empty() || pid == 0) return "";
    std::error_code ec;
    const fs::path dir = fs::u8path(dumps_dir);
    if (!fs::is_directory(dir, ec)) return "";
    const std::string want = lower(exe_name + "." + std::to_string(pid) + ".dmp");
    const fs::path exact = dir / fs::u8path(exe_name + "." + std::to_string(pid) + ".dmp");
    if (fs::is_regular_file(exact, ec)) return exact.u8string();
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {   // a differently cased exe name
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        if (lower(it->path().filename().u8string()) == want) return it->path().u8string();
    }
    return "";
}

static std::string time_text(long long t) {
    if (t <= 0) return "unknown";
    std::time_t tt = (std::time_t)t;
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tmv);
    return b;
}

std::string headline(const Crash& c) {
    std::string who = c.emulator.empty() ? std::string("The emulator") : c.emulator;
    return who + " stopped unexpectedly" + (c.game.empty() ? std::string(".") : " while running " + c.game + ".");
}

std::string details_text(const Crash& c) {
    std::string s;
    s += "Emulator: " + c.emulator + (c.version.empty() ? std::string() : " " + c.version) + "\n";
    if (!c.game.empty()) s += "Game: " + c.game + "\n";
    s += "Exit code: " + hex_code(c.exit_code) + "\n";
    s += "Time: " + time_text(c.time_unix) + "\n";
    if (!c.exe_name.empty()) s += "Process: " + c.exe_name + " (pid " + std::to_string(c.pid) + ")\n";
    s += "Crash dump: " + (c.dump_path.empty() ? std::string("none found") : c.dump_path) + "\n";
    if (!c.log_dir.empty()) s += "Emulator log folder: " + c.log_dir + "\n";
    return s;
}

std::string folder_of(const std::string& path) {
    const size_t p = path.find_last_of("\\/");
    return p == std::string::npos ? std::string() : path.substr(0, p);
}

}
