#pragma once
/* Duplicate copies of one game (same library key, different files). Pure logic; the hub keeps one copy in the library
 * (the first scanned unless prefs.preferred_copy names another path) and remembers the others for the details modal. */
#include "library.h"

#include <map>
#include <string>
#include <vector>

namespace hubdupes {

/* key -> every path found for it, in scan order (only keys with 2+ copies). */
using Dupes = std::map<std::string, std::vector<std::string>>;

/* Collapse `lib` to one entry per key. `preferred` maps key -> path to keep (ignored when no copy has that path).
 * The kept entry takes the position of the first copy. Entries with an empty key are left alone. */
inline void collapse(std::vector<hub::GameEntry>* lib, const std::map<std::string, std::string>& preferred, Dupes* out) {
    out->clear();
    std::map<std::string, std::vector<size_t>> groups;
    for (size_t i = 0; i < lib->size(); ++i)
        if (!(*lib)[i].key.empty()) groups[(*lib)[i].key].push_back(i);
    std::vector<hub::GameEntry> res;
    std::map<std::string, size_t> pos;   // key -> index in res
    for (size_t i = 0; i < lib->size(); ++i) {
        const hub::GameEntry& e = (*lib)[i];
        if (e.key.empty()) { res.push_back(e); continue; }
        auto pit = pos.find(e.key);
        if (pit == pos.end()) {
            pos[e.key] = res.size();
            res.push_back(e);
            const auto& g = groups[e.key];
            if (g.size() > 1) {
                auto& paths = (*out)[e.key];
                for (size_t gi : g) paths.push_back((*lib)[gi].path);
                auto pf = preferred.find(e.key);
                if (pf != preferred.end())
                    for (size_t gi : g)
                        if ((*lib)[gi].path == pf->second) { res.back() = (*lib)[gi]; break; }
            }
        }
    }
    lib->swap(res);
}

} // namespace hubdupes
