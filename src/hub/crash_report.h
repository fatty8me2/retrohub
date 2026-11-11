#pragma once
/* Crash report for RetroHub.exe itself (crash_report.cpp). No dbghelp and no minidump: that got the exe flagged by Windows Defender
 * (see hang_watchdog.cpp). The report is plain text:
 *   crash-<yyyymmdd-hhmmss>.txt in the logs folder, with the exception code and name, the faulting address as module+offset, the
 *   registers, return-address-looking values found on the faulting thread's stack (module+offset, symbolise offline with the PDB),
 *   the RetroHub version, the loaded modules, the UI breadcrumb (hang watchdog phase) and the last 50 hub log lines.
 * The handler never shows a dialog, allocates nothing it can avoid, and returns EXCEPTION_CONTINUE_SEARCH so Windows Error Reporting
 * still runs. On the next start the hub shows one note per new report (crash_report_ui.cpp). */
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace crashrep {

/* ---- pure formatter (unit tested with a synthetic exception and a fake module list) ---- */
struct Module { char name[64]; uint64_t base; uint64_t size; };
struct Input {
    const char* kind = "Unhandled exception";
    uint32_t code = 0;
    uint64_t address = 0;
    int n_params = 0;                  // EXCEPTION_RECORD.NumberParameters
    uint64_t params[2] = {0, 0};       // access violation: [0] 0 read / 1 write / 8 execute, [1] the address touched
    uint32_t thread_id = 0;
    unsigned long long uptime_ms = 0;
    const char* version = "";
    const char* system = "";           // multi-line hub log header text
    const char* time_text = "";
    const char* phase = nullptr;       // UI breadcrumb
    const Module* modules = nullptr; int n_modules = 0;
    const uint64_t* stack = nullptr; int n_stack = 0;          // values from the stack that point into a module
    const char* const* reg_names = nullptr; const uint64_t* reg_values = nullptr; int n_regs = 0;
    const char* log_tail = "";
};
const char* exception_name(uint32_t code);                    // "ACCESS_VIOLATION", ... or "unknown"
bool locate(const Module* mods, int n, uint64_t addr, int* index, uint64_t* offset);   // which module holds addr
/* Writes the report into out (NUL terminated, truncated to cap); returns its length. Allocation free. */
size_t format_report(const Input& in, char* out, size_t cap);

/* ---- handler ---- */
/* Installs the unhandled-exception filter, the CRT invalid-parameter and pure-call handlers and a std::terminate handler. Reports go to
 * logs_dir_utf8. phase_fn (optional) returns the UI breadcrumb; hublog::recent_lines supplies the log tail. Keeps the newest 10 reports. */
void install(const std::string& logs_dir_utf8, const std::string& version, const char* (*phase_fn)() = nullptr);

/* ---- next start ---- */
struct Found { std::string name; std::string path_utf8; std::string dir_utf8; };
/* Newest crash-*.txt in logs_dir that has not been dismissed (logs_dir\crash-seen.txt lists dismissed names). */
bool find_unseen(const std::string& logs_dir_utf8, Found* out);
void mark_all_seen(const std::string& logs_dir_utf8);          // dismissing marks every existing report, so the note shows once
std::vector<std::string> list_reports(const std::string& logs_dir_utf8);   // names, newest first
std::string read_report(const std::string& path_utf8, size_t max_bytes = 256 * 1024);

}  // namespace crashrep
