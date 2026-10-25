#pragma once
/* Patch notes ("What's new"): parser for data/changelog.json and the show-once-per-version popup rule.
 * Pure logic, no UI or file access (the hub loads the file and passes the text in). */

#include <string>
#include <vector>

namespace hubui {
namespace changelog {

enum Section { New = 0, Improved = 1, Fixed = 2, kSections = 3 };
const char* section_title(int s);   // "New", "Improved", "Fixed"

struct Entry {
    std::string version;             // "1.22.0" (no leading v)
    std::string date;                // "2026-10-24"
    std::string title;
    std::vector<std::string> items[kSections];
    int total() const { return (int)(items[0].size() + items[1].size() + items[2].size()); }
};

/* data/changelog.json: {"versions":[{version,date,title,new:[..],improved:[..],fixed:[..]}, ...]}, newest first.
 * Entries without a version or without any bullet are dropped; a missing/invalid file gives false and an empty list.
 * Order is kept as written (the file is authored newest first). */
bool parse(const std::string& text, std::vector<Entry>* out);

/* The entry whose version equals `version` ("v1.22.0" and "1.22.0" match); null when absent. */
const Entry* find(const std::vector<Entry>& v, const std::string& version);

/* "2026-10-24" -> "Oct 24, 2026"; anything else is returned unchanged. */
std::string pretty_date(const std::string& iso);

/* The newest entry as Markdown (what scripts/release_notes.ps1 prints for `gh release create -F`). */
std::string to_markdown(const Entry& e);

enum class Popup { None, Show, MarkSeen };
/* What to do about the one-time "What's new in vX" popup.
 *  last_seen    settings value (the version whose notes were last shown or skipped; "" = never)
 *  current      this build's version ("v1.22.0", "dev", ...)
 *  has_entry    changelog.json has notes for `current`
 *  fresh_install  first-run setup is about to run or running: new players don't need patch notes, so just remember the version
 *  blocked      the setup wizard / another startup dialog is open: try again later
 * None = nothing now; MarkSeen = store `current` as seen without showing; Show = open the popup (store on dismiss). */
Popup popup_action(const std::string& last_seen, const std::string& current, bool has_entry, bool fresh_install, bool blocked);

}  // namespace changelog
}  // namespace hubui
