#pragma once
/* "Copy diagnostics": gather logs / versions / emulator list / hang reports into one zip on the Desktop.
 * Identity files never go in: any identity*.json and anything under an _identity folder is excluded, at every depth. */
#include <string>
#include <utility>
#include <vector>

namespace hubdiag {

bool excluded(const std::string& rel_path);   // '/' or '\' separated, case-insensitive

struct Entry { std::string zip_name, src; };
/* Every non-excluded file under dir (recursive, files over max_bytes skipped), named prefix + relative path. */
std::vector<Entry> collect_dir(const std::string& dir, const std::string& prefix, unsigned long long max_bytes = 25ull << 20);
/* Keeps every entry except crash-*.txt reports, of which only the newest `keep` stay (the file names carry the timestamp). */
std::vector<Entry> newest_crash_reports_only(std::vector<Entry> files, size_t keep = 5);
/* Keeps the newest files (by modified time) whose sizes add up to at most max_bytes; the bundle never reads more than that from disk. */
std::vector<Entry> limit_total_bytes(std::vector<Entry> files, unsigned long long max_bytes = 8ull << 20);
/* Writes out_zip from files + in-memory text entries. Excluded names are refused here too (defence in depth). */
bool write_zip(const std::string& out_zip, const std::vector<Entry>& files, const std::vector<std::pair<std::string, std::string>>& texts, std::string* err);

}  // namespace hubdiag
