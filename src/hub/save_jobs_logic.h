#pragma once
/* The wording of a save sync / import result, moved out of save_jobs.cpp so it can be tested (hub_save_jobs_logic_test).
 * save_jobs.cpp keeps the global report_summary() (declared in app.h) as a one-line wrapper. */
#include <string>

#include "savestore.h"

namespace hubsave {

/* sync_folder's warning (a game whose folder could not be read was left out this time) is part of the line: it used to be dropped,
 * so a game that silently did not sync read as "Everything is already up to date." */
inline std::string report_summary(const hub::saves::SyncReport& r, const char* sent_word, const char* got_word) {
    if (!r.ok) return "Failed: " + (r.error.empty() ? std::string("unknown error") : r.error);
    const std::string warn = r.warning.empty() ? std::string() : " " + r.warning;
    if (r.pushed == 0 && r.pulled == 0 && r.conflicts == 0) return (r.warning.empty() ? "Everything is already up to date." : "Nothing new to sync.") + warn;
    auto files = [](int n, const char* word) { return std::to_string(n) + (n == 1 ? " file " : " files ") + word; };
    std::string s;
    if (std::string(sent_word) == got_word) {   // an import: "restored" both ways is one number, not two
        if (r.pushed + r.pulled) s += files(r.pushed + r.pulled, sent_word);
    } else {
        if (r.pushed) s += files(r.pushed, sent_word);
        if (r.pulled) s += (s.empty() ? "" : ", ") + files(r.pulled, got_word);
    }
    if (r.conflicts) s += (s.empty() ? "" : ", ") + std::to_string(r.conflicts) + (r.conflicts == 1 ? " conflict" : " conflicts") + " (the older copy was kept as a backup)";
    return s + "." + warn;
}

/* The Saves page line after "Export". A zip that was written but left something out (export_zip's warning: files or a game folder
 * that could not be read) says so: it is a backup, and a silent gap is only found when it is needed. */
inline std::string export_summary(bool ok, const std::string& path, const std::string& err) {
    if (!ok) return "Export failed: " + (err.empty() ? std::string("unknown error") : err);
    if (err.empty()) return "Exported to " + path;
    return "Exported to " + path + ", but not everything: " + err;
}

}  // namespace hubsave
