#include "changelog.h"

#include "json.h"
#include "logic.h"

#include <cstdio>

namespace hubui {
namespace changelog {

static const char* kKeys[kSections] = {"new", "improved", "fixed"};
const char* section_title(int s) {
    static const char* t[kSections] = {"New", "Improved", "Fixed"};
    return s >= 0 && s < kSections ? t[s] : "";
}

bool parse(const std::string& text, std::vector<Entry>* out) {
    out->clear();
    json::Value root;
    if (!json::parse(text, &root) || !root.is_object()) return false;
    const json::Value* vs = root.get("versions");
    if (!vs || !vs->is_array()) return false;
    for (const json::Value& jv : vs->a) {
        if (!jv.is_object()) continue;
        Entry e;
        e.version = jv.str("version");
        if (!e.version.empty() && (e.version[0] == 'v' || e.version[0] == 'V')) e.version.erase(0, 1);
        e.date = jv.str("date");
        e.title = jv.str("title");
        for (int s = 0; s < kSections; ++s) {
            const json::Value* arr = jv.get(kKeys[s]);
            if (!arr || !arr->is_array()) continue;
            for (const json::Value& b : arr->a)
                if (b.is_string() && !b.s.empty()) e.items[s].push_back(b.s);
        }
        if (e.version.empty() || e.total() == 0) continue;
        out->push_back(std::move(e));
    }
    return !out->empty();
}

const Entry* find(const std::vector<Entry>& v, const std::string& version) {
    for (const Entry& e : v)
        if (compare_app_versions(e.version, version) == 0) return &e;
    return nullptr;
}

std::string pretty_date(const std::string& iso) {
    int y = 0, m = 0, d = 0;
    if (iso.size() < 10 || std::sscanf(iso.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return iso;
    static const char* mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char b[40];
    std::snprintf(b, sizeof b, "%s %d, %d", mon[m - 1], d, y);
    return b;
}

std::string to_markdown(const Entry& e) {
    std::string o = "## ";
    o += e.title.empty() ? "RetroHub v" + e.version : e.title;
    o += "\n";
    for (int s = 0; s < kSections; ++s) {
        if (e.items[s].empty()) continue;
        o += "\n**" + std::string(section_title(s)) + "**\n";
        for (const std::string& b : e.items[s]) o += "- " + b + "\n";
    }
    return o;
}

Popup popup_action(const std::string& last_seen, const std::string& current, bool has_entry, bool fresh_install, bool blocked) {
    if (compare_app_versions(current, "0") == 2) return Popup::None;                     // "dev" or unparsable build
    if (!last_seen.empty() && compare_app_versions(last_seen, current) >= 0) return Popup::None;   // already seen (or a downgrade)
    if (fresh_install) return Popup::MarkSeen;
    if (blocked) return Popup::None;
    return has_entry ? Popup::Show : Popup::MarkSeen;
}

}  // namespace changelog
}  // namespace hubui
