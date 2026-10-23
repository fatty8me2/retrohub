#include "findgames.h"

#include "winutil.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <set>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace hub { namespace fg {

const char* sys_label(int s) {
    static const char* const n[kSysCount] = {"N64", "GameCube", "GBA", "Game Boy", "GBC", "DS", "3DS", "Wii U"};
    return s >= 0 && s < kSysCount ? n[s] : "";
}

int classify_ext(const std::string& e) {
    struct M { const char* ext; int sys; };
    static const M map[] = {
        {".z64", N64}, {".v64", N64}, {".n64", N64},
        {".gcm", GC}, {".iso", GC}, {".rvz", GC}, {".gcz", GC}, {".ciso", GC}, {".wbfs", GC},
        {".gba", GBA}, {".gb", GB}, {".gbc", GBC}, {".nds", NDS},
        {".3ds", N3DS}, {".cci", N3DS}, {".cia", N3DS}, {".cxi", N3DS},
        {".wua", WIIU}, {".wud", WIIU}, {".wux", WIIU},
        {".zip", kArchive}, {".7z", kArchive},
    };
    for (const M& m : map) if (e == m.ext) return m.sys;
    return kNone;
}

static std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string norm_path(const std::string& p) {
    std::string s = lower(p);
    for (char& c : s) if (c == '/') c = '\\';
    while (!s.empty() && s.back() == '\\') s.pop_back();
    return s;
}

static std::vector<std::string> split_path(const std::string& p) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : p) {
        if (c == '\\' || c == '/') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static std::string join_path(const std::vector<std::string>& c, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && i < c.size(); ++i) { if (i) s += '\\'; s += c[i]; }
    if (n == 1 && !c.empty() && c[0].size() == 2 && c[0][1] == ':') s += '\\';   // "D:" -> "D:\"
    return s;
}

static bool is_under(const std::string& child, const std::string& parent) {   // both normalized; equal counts
    return child == parent || (child.size() > parent.size() && child.compare(0, parent.size(), parent) == 0 && child[parent.size()] == '\\');
}

std::vector<Folder> collapse_folders(const std::vector<DirCount>& dirs, const std::vector<Root>& roots) {
    struct Grp { std::string key; std::vector<std::string> comps; Counts c; bool fixed = false; size_t key_depth = 0; };
    std::vector<Grp> groups;
    for (const DirCount& d : dirs) {
        if (d.c.total() == 0) continue;
        const std::string nd = norm_path(d.path);
        int best = -1;
        size_t best_len = 0;
        for (size_t i = 0; i < roots.size(); ++i) {
            std::string nr = norm_path(roots[i].path);
            if (is_under(nd, nr) && (best < 0 || nr.size() > best_len)) { best = (int)i; best_len = nr.size(); }
        }
        std::vector<std::string> comps = split_path(d.path);
        size_t depth = comps.size();
        bool fixed = true;
        if (best >= 0) {
            size_t rd = split_path(roots[best].path).size();
            if (roots[best].library) depth = rd;
            else {
                if (nd == norm_path(roots[best].path)) continue;   // a stray file straight in Desktop / Downloads: not a library
                depth = rd + 1;
                fixed = false;
            }
        }
        if (depth > comps.size()) depth = comps.size();
        std::string key = norm_path(join_path(comps, depth));
        Grp* g = nullptr;
        for (Grp& x : groups) if (x.key == key) { g = &x; break; }
        if (!g) {
            groups.push_back(Grp());
            g = &groups.back();
            g->key = key;
            g->fixed = fixed;
            g->key_depth = depth;
            if (fixed && best >= 0) g->comps = split_path(roots[best].path);   // library root: its own spelling
            else g->comps.assign(comps.begin(), comps.begin() + (fixed ? depth : comps.size()));
        } else if (!g->fixed) {   // narrow to the deepest folder holding every file of the group
            size_t n = 0;
            while (n < g->comps.size() && n < comps.size() && lower(g->comps[n]) == lower(comps[n])) ++n;
            g->comps.resize(std::max(n, g->key_depth));
        }
        g->c.add(d.c);
    }
    std::stable_sort(groups.begin(), groups.end(), [](const Grp& a, const Grp& b) { return a.comps.size() < b.comps.size(); });
    std::vector<Folder> out;
    std::vector<std::string> out_norm;
    for (const Grp& g : groups) {
        std::string path = join_path(g.comps, g.comps.size());
        std::string np = norm_path(path);
        bool merged = false;
        for (size_t i = 0; i < out.size(); ++i)
            if (is_under(np, out_norm[i])) { out[i].c.add(g.c); merged = true; break; }
        if (merged) continue;
        Folder f;
        f.path = path;
        f.c = g.c;
        out.push_back(f);
        out_norm.push_back(np);
    }
    std::vector<Folder> res;
    for (Folder& f : out) {
        if (f.c.games() == 0 && f.c.archives < 3) continue;   // one or two stray zips are not a game library
        f.ticked = true;
        res.push_back(f);
    }
    std::stable_sort(res.begin(), res.end(), [](const Folder& a, const Folder& b) {
        if (a.c.games() != b.c.games()) return a.c.games() > b.c.games();
        if (a.c.archives != b.c.archives) return a.c.archives > b.c.archives;
        return norm_path(a.path) < norm_path(b.path);
    });
    return res;
}

bool skip_dir_name(const std::string& name) {
    static const char* const skip[] = {
        "windows", "program files", "program files (x86)", "programdata", "appdata", "$recycle.bin", "system volume information",
        "node_modules", ".git", ".svn", "windowsapps", "winsxs", "recovery", "steamapps", "$windows.~bt", "$windows.~ws",
        "msocache", "perflogs", "config.msi", "pictures", "music", "videos", "__pycache__", "venv", ".venv", "cache",
    };
    std::string n = lower(name);
    for (const char* s : skip) if (n == s) return true;
    return false;
}

/* ---- emulator config parsers ---- */

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

static bool abs_path(const std::string& p) {
    return p.size() >= 3 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && (p[2] == '\\' || p[2] == '/');
}

static void push_unique(std::vector<std::string>& v, std::string p) {
    p = trim(p);
    if (!abs_path(p)) return;
    for (char& c : p) if (c == '/') c = '\\';
    for (const std::string& q : v) if (norm_path(q) == norm_path(p)) return;
    v.push_back(p);
}

std::vector<std::string> parse_dolphin_ini_paths(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = trim(text.substr(pos, e - pos));
        pos = e + 1;
        if (line.compare(0, 7, "ISOPath") == 0) {
            size_t eq = line.find('=');
            if (eq != std::string::npos && line.find_first_not_of("0123456789", 7) == line.find_first_of(" =") ) push_unique(out, line.substr(eq + 1));
        }
    }
    return out;
}

std::vector<std::string> parse_cemu_xml_paths(const std::string& text) {
    std::vector<std::string> out;
    size_t gp = text.find("<GamePaths>");
    size_t ge = text.find("</GamePaths>");
    if (gp == std::string::npos) return out;
    if (ge == std::string::npos) ge = text.size();
    size_t pos = gp;
    while (true) {
        size_t a = text.find("<Entry>", pos);
        if (a == std::string::npos || a > ge) break;
        size_t b = text.find("</Entry>", a);
        if (b == std::string::npos) break;
        push_unique(out, text.substr(a + 7, b - a - 7));
        pos = b + 8;
    }
    return out;
}

std::vector<std::string> parse_qt_gamedirs(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = trim(text.substr(pos, e - pos));
        pos = e + 1;
        if (line.compare(0, 15, "Paths\\gamedirs\\") == 0 && line.find("\\path=") != std::string::npos)
            push_unique(out, line.substr(line.find("\\path=") + 6));
    }
    return out;
}

/* ---- the scan ---- */

namespace {

struct Item { std::string path; int depth; bool library; };

std::string ext_of(const std::wstring& name) {
    size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0) return "";
    return lower(winutil::to_utf8(name.substr(dot)));
}

}

std::vector<Folder> scan_roots(const std::vector<Root>& roots, Control* ctl, const Options& opt) {
    Control local;
    if (!ctl) ctl = &local;
    const auto t0 = std::chrono::steady_clock::now();
    auto out_of_time = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > opt.time_ms; };

    std::vector<DirCount> counted;
    std::set<std::string> visited;
    std::deque<Item> q;
    for (const Root& r : roots) q.push_back({r.path, 0, r.library});
    int dirs = 0;
    while (!q.empty() && !ctl->cancel && !out_of_time() && dirs < opt.max_dirs) {
        Item it = q.front();
        q.pop_front();
        if (!visited.insert(norm_path(it.path)).second) continue;
        ++dirs;
        ctl->dirs = dirs;
        { std::lock_guard<std::mutex> lk(ctl->mu); ctl->current = it.path; }

        std::wstring pat = winutil::to_wide(it.path);
        while (!pat.empty() && (pat.back() == L'\\' || pat.back() == L'/')) pat.pop_back();
        pat += L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        DirCount dc;
        dc.path = it.path;
        int seen = 0;
        const int max_depth = it.library ? opt.max_depth_library : opt.max_depth_generic;
        do {
            if (ctl->cancel || ++seen > opt.max_dir_entries) break;
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_SYSTEM)) continue;   // junctions / symlinks (loops), system folders
                std::string n = winutil::to_utf8(name);
                if (skip_dir_name(n) || it.depth >= max_depth) continue;
                std::string sub = it.path;
                if (!sub.empty() && sub.back() != '\\' && sub.back() != '/') sub += '\\';
                q.push_back({sub + n, it.depth + 1, it.library});
            } else {
                int s = classify_ext(ext_of(name));
                if (s >= 0) { ++dc.c.n[s]; ++ctl->games; }
                else if (s == kArchive) ++dc.c.archives;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (dc.c.total() > 0) counted.push_back(dc);
    }
    ctl->done = true;
    return collapse_folders(counted, roots);
}

/* ---- where to look ---- */

static bool dir_exists(const std::string& p) {
    DWORD a = GetFileAttributesW(winutil::to_wide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string env(const char* n) {
    const wchar_t* v = _wgetenv(winutil::to_wide(n).c_str());
    return v ? winutil::to_utf8(v) : std::string();
}

std::vector<Root> default_roots() {
    std::vector<Root> out;
    std::set<std::string> seen;
    auto add = [&](const std::string& p, bool library) {
        if (p.empty() || !dir_exists(p)) return;
        if (!seen.insert(norm_path(p)).second) return;
        out.push_back({p, library});
    };
    const std::string test_home = env("HUB_FINDGAMES_HOME");
    const std::string home = !test_home.empty() ? test_home : env("USERPROFILE");
    if (home.empty()) return out;
    for (const char* sub : {"Desktop", "Documents", "Downloads"}) add(home + "\\" + sub, false);
    for (const char* od : {"OneDrive", "OneDrive - Personal"})
        for (const char* sub : {"Desktop", "Documents"}) add(home + "\\" + od + "\\" + sub, false);
    for (const char* sub : {"Documents\\RetroHub Games", "Games", "ROMs", "Roms", "Emulation", "RetroArch\\roms"}) add(home + "\\" + sub, true);
    if (!test_home.empty()) return out;

    const std::string appdata = env("APPDATA"), local = env("LOCALAPPDATA");
    add(appdata + "\\RetroArch\\roms", true);
    auto cfg = [&](const std::string& path, std::vector<std::string> (*parse)(const std::string&)) {
        std::string text;
        if (!path.empty() && winutil::read_file(path, &text)) for (const std::string& p : parse(text)) add(p, true);
    };
    cfg(appdata + "\\Dolphin Emulator\\Config\\Dolphin.ini", parse_dolphin_ini_paths);
    cfg(home + "\\Documents\\Dolphin Emulator\\Config\\Dolphin.ini", parse_dolphin_ini_paths);
    cfg(local + "\\Cemu\\settings.xml", parse_cemu_xml_paths);
    cfg(appdata + "\\Cemu\\settings.xml", parse_cemu_xml_paths);
    cfg(appdata + "\\Azahar\\config\\qt-config.ini", parse_qt_gamedirs);
    cfg(appdata + "\\Citra\\config\\qt-config.ini", parse_qt_gamedirs);

    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        std::string root = std::string(1, char('A' + i)) + ":\\";
        UINT t = GetDriveTypeW(winutil::to_wide(root).c_str());
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;   // never wake a network share or an optical drive
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(winutil::to_wide(root + "*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        int n = 0;
        do {
            if (++n > 2000) break;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            std::string nm = lower(winutil::to_utf8(fd.cFileName));
            if (nm == "games" || nm == "rom" || nm == "roms" || nm == "emulation" || nm == "emulators" || nm == "retroarch" ||
                nm == "retro games" || nm == "game roms" || nm == "retrohub games")
                add(root + winutil::to_utf8(fd.cFileName), true);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}

}} // namespace hub::fg
