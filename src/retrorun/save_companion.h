/* Companion save files that must follow the save name (audit saves-F12). The .srm already adopts the one save in a per-game vault folder
 * when this PC's ROM file has another name; the files next to it (GB clock <stem>.rtc, PS1 memory card 2 <stem>.1.mcr, which the core
 * names after the ROM file) have to land under the name this PC will look for. Pure logic: the caller copies, never moves or overwrites. */
#pragma once
#include <string>
#include <vector>

namespace retrorun {

inline std::string sc_lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
inline bool sc_has_ext(const std::string& name, const std::string& ext) {
    std::string n = sc_lower(name), e = sc_lower(ext);
    return n.size() > e.size() && n.compare(n.size() - e.size(), e.size(), e) == 0;
}

/* `names` = file names in the per-game save folder. Returns the file to COPY to <target_stem><ext>, or "" for nothing to do:
 *  - <target_stem><ext> already exists: keep it, never overwrite;
 *  - else <primary_stem><ext> exists (the stem the .srm was adopted under): use that;
 *  - else exactly one *<ext> file in the folder: it is this game's (the folder is per game);
 *  - else nothing (zero, or several: do not guess). */
inline std::string companion_source(const std::vector<std::string>& names, const std::string& target_stem, const std::string& primary_stem,
                                    const std::string& ext) {
    const std::string want = sc_lower(target_stem + ext), prim = sc_lower(primary_stem + ext);
    std::string primary_hit, lone;
    int n = 0;
    for (const std::string& f : names) {
        std::string l = sc_lower(f);
        if (l == want) return "";
        if (!primary_stem.empty() && l == prim) primary_hit = f;
        if (sc_has_ext(f, ext)) { lone = f; n++; }
    }
    if (!primary_hit.empty()) return primary_hit;
    return n == 1 ? lone : std::string();
}

}  // namespace retrorun
