#include "history.h"

#include "games.h"
#include "savestore.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "miniz.h"

namespace fs = std::filesystem;

namespace hub::saves {

namespace {

fs::path P(const std::string& u8) { return fs::u8path(u8); }
std::string S(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }

struct F { std::string rel; int64_t size; };

bool tmp_name(const std::string& n) { return n.size() > 7 && n.compare(n.size() - 7, 7, ".hubtmp") == 0; }

std::vector<F> scan(const fs::path& root, int64_t max_file) {
    std::vector<F> v;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return v;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string name = S(it->path().filename());
        if (name == ".info" || tmp_name(name)) continue;
        int64_t sz = (int64_t)it->file_size(ec);
        if (max_file > 0 && sz > max_file) continue;
        std::string rel = S(fs::relative(it->path(), root, ec));
        for (auto& c : rel) if (c == '\\') c = '/';
        v.push_back({rel, sz});
    }
    std::sort(v.begin(), v.end(), [](const F& a, const F& b) { return a.rel < b.rel; });
    return v;
}

bool same_bytes(const fs::path& a, const fs::path& b) {
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    if (!x || !y) return false;
    std::stringstream sx, sy;
    sx << x.rdbuf();
    sy << y.rdbuf();
    return sx.str() == sy.str();
}

bool same_tree(const std::vector<F>& a, const fs::path& ra, const std::vector<F>& b, const fs::path& rb) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].rel != b[i].rel || a[i].size != b[i].size || !same_bytes(ra / P(a[i].rel), rb / P(b[i].rel))) return false;
    return true;
}

bool copy_files(const std::vector<F>& fl, const fs::path& from, const fs::path& to) {
    std::error_code ec;
    for (auto& f : fl) {
        fs::path dst = to / P(f.rel);
        fs::create_directories(dst.parent_path(), ec);
        fs::path tmp = dst;
        tmp += ".hubtmp";
        fs::copy_file(from / P(f.rel), tmp, fs::copy_options::overwrite_existing, ec);
        if (ec) return false;
        fs::rename(tmp, dst, ec);
        if (ec) { fs::remove(tmp, ec); return false; }
    }
    return true;
}

void write_info(const fs::path& dir, int64_t ts, const std::string& device) {
    std::ofstream o(dir / ".info", std::ios::trunc);
    o << ts << "\n" << device << "\n";
}
bool read_info(const fs::path& dir, int64_t* ts, std::string* device) {
    std::ifstream i(dir / ".info");
    if (!i) return false;
    long long t = 0;
    i >> t;
    std::string d;
    std::getline(i, d);
    std::getline(i, d);
    while (!d.empty() && (d.back() == '\r' || d.back() == ' ')) d.pop_back();
    *ts = t;
    *device = d;
    return t > 0;
}

bool fill_entry(const fs::path& dir, HistEntry* e) {
    if (!read_info(dir, &e->ts, &e->device)) return false;
    e->id = S(dir.filename());
    auto fl = scan(dir, 0);
    e->files = (int)fl.size();
    e->size = 0;
    for (auto& f : fl) e->size += f.size;
    return true;
}

bool make_snapshot(const fs::path& dir, const fs::path& battery, int64_t ts, const std::string& device, int64_t max_file) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    auto fl = scan(battery, max_file);
    if (!copy_files(fl, battery, dir)) { fs::remove_all(dir, ec); return false; }
    write_info(dir, ts, device);
    return true;
}

std::string device_name() {
    char b[MAX_COMPUTERNAME_LENGTH + 2];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    return GetComputerNameA(b, &n) ? std::string(b, n) : std::string("this PC");
}

}  // namespace

bool history_snapshot(const std::string& hist_dir, const std::string& battery_dir, int64_t ts, const std::string& device, int keep,
                      int64_t max_file) {
    fs::path h = P(hist_dir), b = P(battery_dir);
    auto cur = scan(b, max_file);
    if (cur.empty()) return false;
    auto have = history_list(hist_dir);
    if (!have.empty()) {
        fs::path nd = h / P(have[0].id);
        if (same_tree(cur, b, scan(nd, 0), nd)) return false;
    }
    char id[32];
    std::snprintf(id, sizeof id, "%012lld", (long long)ts);
    std::string sid = id;
    for (auto& e : have) if (e.id == sid) { ts++; std::snprintf(id, sizeof id, "%012lld", (long long)ts); sid = id; break; }
    if (!make_snapshot(h / P(sid), b, ts, device, max_file)) return false;
    have = history_list(hist_dir);
    std::error_code ec;
    for (size_t i = (size_t)std::max(1, keep); i < have.size(); ++i) fs::remove_all(h / P(have[i].id), ec);
    return true;
}

std::vector<HistEntry> history_list(const std::string& hist_dir) {
    std::vector<HistEntry> v;
    std::error_code ec;
    fs::path h = P(hist_dir);
    if (!fs::is_directory(h, ec)) return v;
    for (fs::directory_iterator it(h, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        std::string n = S(it->path().filename());
        if (n.empty() || n[0] == '_') continue;
        HistEntry e;
        if (fill_entry(it->path(), &e)) v.push_back(e);
    }
    std::sort(v.begin(), v.end(), [](const HistEntry& a, const HistEntry& b) { return a.ts != b.ts ? a.ts > b.ts : a.id > b.id; });
    return v;
}

bool history_mark_undo(const std::string& hist_dir, const std::string& battery_dir, int64_t ts, const std::string& device, int64_t max_file) {
    return make_snapshot(P(hist_dir) / "_undo", P(battery_dir), ts, device, max_file);
}

bool history_restore(const std::string& hist_dir, const std::string& battery_dir, const std::string& id, int64_t ts, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    fs::path h = P(hist_dir), snap = h / P(id);
    if (id.empty() || id[0] == '_' || id.find_first_of("/\\") != std::string::npos) return fail("bad version");
    HistEntry e;
    if (!fill_entry(snap, &e)) return fail("That version is gone.");
    if (!history_mark_undo(hist_dir, battery_dir, ts, device_name())) return fail("Could not make an undo copy.");
    if (!copy_files(scan(snap, 0), snap, P(battery_dir))) return fail("Could not write the save files.");
    return true;
}

bool history_undo_info(const std::string& hist_dir, HistEntry* out) {
    HistEntry e;
    if (!fill_entry(P(hist_dir) / "_undo", &e)) { if (out) *out = HistEntry(); return false; }
    if (out) *out = e;
    return true;
}

bool history_undo(const std::string& hist_dir, const std::string& battery_dir, std::string* err) {
    fs::path u = P(hist_dir) / "_undo";
    HistEntry e;
    if (!fill_entry(u, &e)) { if (err) *err = "Nothing to undo."; return false; }
    if (!copy_files(scan(u, 0), u, P(battery_dir))) { if (err) *err = "Could not write the save files."; return false; }
    std::error_code ec;
    fs::remove_all(u, ec);
    return true;
}

std::vector<HistRow> history_merge(const std::vector<HistEntry>& local, const std::vector<CloudVer>& cloud) {
    std::vector<HistRow> v;
    for (auto& l : local) {
        HistRow r;
        r.id = l.id; r.ts = l.ts; r.size = l.size; r.device = l.device; r.files = l.files;
        v.push_back(r);
    }
    for (auto& c : cloud) {
        HistRow r;
        r.cloud = true; r.path = c.path; r.ver = c.ver; r.ts = c.ts; r.size = c.size; r.device = c.device; r.current = c.current; r.files = 1;
        v.push_back(r);
    }
    std::stable_sort(v.begin(), v.end(), [](const HistRow& a, const HistRow& b) { return a.ts != b.ts ? a.ts > b.ts : (!a.cloud && b.cloud); });
    return v;
}

bool export_game_zip(const std::string& game_dir, const std::string& zip_path, bool include_states, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    fs::path g = P(game_dir);
    std::vector<F> fl;
    for (auto& f : scan(g, 0)) {
        if (f.rel.rfind("states/", 0) == 0 && !include_states) continue;
        if (f.rel.rfind("playlog/", 0) == 0) continue;
        fl.push_back(f);
    }
    if (fl.empty()) return fail("There are no saves for this game yet.");
    fs::path dst = P(zip_path);
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::path tmp = dst;
    tmp += ".hubtmp";
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    std::string tmp8 = S(tmp);
    if (!mz_zip_writer_init_file(&z, tmp8.c_str(), 0)) return fail("Could not create the zip.");
    bool ok = true;
    for (auto& f : fl) {
        std::string src = S(g / P(f.rel));
        if (!mz_zip_writer_add_file(&z, f.rel.c_str(), src.c_str(), nullptr, 0, MZ_DEFAULT_LEVEL)) { ok = false; break; }
    }
    if (ok) ok = mz_zip_writer_finalize_archive(&z) != 0;
    mz_zip_writer_end(&z);
    if (ok) { fs::remove(dst, ec); fs::rename(tmp, dst, ec); ok = !ec; }
    if (!ok) { fs::remove(tmp, ec); return fail("Writing the zip failed."); }
    return true;
}

std::string game_history_dir(const std::string& system_short, const std::string& game_key) {
    fs::path g = P(game_dir(system_short, game_key));
    if (g.filename().empty()) g = g.parent_path();
    std::string key = S(g.filename()), sys = S(g.parent_path().filename());
    return hub::data_dir() + "save_history\\" + sys + "\\" + key + "\\";
}

bool history_snapshot_game(const std::string& system_short, const std::string& game_key) {
    return history_snapshot(game_history_dir(system_short, game_key), battery_dir(system_short, game_key), (int64_t)std::time(nullptr), device_name());
}

} // namespace hub::saves
