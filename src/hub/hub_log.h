#pragma once
/* RetroHub's single rotating log: <data_dir>\logs\hub.log (+ hub.1.log, hub.2.log, 1 MB each).
 *
 *   hublog::init(hub::data_dir() + "logs\\", HUB_VERSION_STR);     // once, early in WinMain
 *   hublog::info("game launched: %s", name);                        // printf style, any thread, never throws
 *   hublog::shutdown();                                              // before exit: flushes
 *
 * Lines are buffered in memory and written by a small worker thread every ~0.75 s (errors wake it at once), so a call never waits
 * on the disk. The last 64 lines are also kept in a fixed ring that the crash handler (crash_report.cpp) prints without touching files.
 * Privacy: every line is scrubbed before it is stored: the Windows profile directory becomes %USERPROFILE% and IPv4-looking numbers
 * become <ip>. Do not log account names, e-mail addresses, friend names or ids at all. Files are opened with wide paths (non-ASCII
 * profile names work). Calls before init() or after shutdown() are ignored. */
#include <cstddef>
#include <string>

namespace hublog {

enum class Level { Info, Warn, Error };

struct Options {
    unsigned long long max_bytes = 1ull << 20;   // rotate when hub.log would pass this
    int keep = 3;                                // files kept: hub.log, hub.1.log, ...
    std::string profile_dir;                     // scrubbed to %USERPROFILE%; empty = read %USERPROFILE%
    bool worker = true;                          // false: flushed only by flush()/shutdown() (tests)
    bool header = true;                          // write the session header
};

void init(const std::string& logs_dir_utf8, const std::string& version, const Options& opt = Options());
void shutdown();                  // flush + stop the worker + close
void flush();                     // synchronous drain to disk (UI thread, before a launch handoff, tests)

void write(Level lv, const std::string& msg);
void info(const char* fmt, ...);
void warn(const char* fmt, ...);
void error(const char* fmt, ...);

unsigned long long since_start_ms();   // time since init(), for "startup phase X took N ms" lines
const char* system_info();             // the header text (version, Windows, CPU, RAM, GPU, locale); never null

/* Crash-handler support. Lock-free and allocation-free: copies up to max_lines of the newest lines (oldest first, '\n' terminated)
 * into out (NUL terminated) and returns the byte count. */
size_t recent_lines(char* out, size_t cap, int max_lines);
/* Crash-handler support: writes what is still buffered to the open file if no thread holds the locks (try-lock only, never waits). */
void flush_from_crash();

}  // namespace hublog
