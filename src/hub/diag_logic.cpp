#include "diag_logic.h"

#include "miniz.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace hubdiag {

bool excluded(const std::string& rel) {
    std::string comp;
    auto bad = [](std::string c) {
        std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
        if (c == "_identity") return true;
        return c.rfind("identity", 0) == 0 && c.size() >= 5 && c.compare(c.size() - 5, 5, ".json") == 0;
    };
    for (char ch : rel) {
        if (ch == '/' || ch == '\\') { if (!comp.empty() && bad(comp)) return true; comp.clear(); }
        else comp += ch;
    }
    return !comp.empty() && bad(comp);
}

std::vector<Entry> collect_dir(const std::string& dir, const std::string& prefix, unsigned long long max_bytes) {
    std::vector<Entry> out;
    std::error_code ec;
    fs::path root = fs::u8path(dir);
    if (!fs::is_directory(root, ec)) return out;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string rel = it->path().lexically_relative(root).u8string();
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (excluded(rel)) continue;
        if (it->file_size(ec) > max_bytes) continue;
        out.push_back({prefix + rel, it->path().u8string()});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.zip_name < b.zip_name; });
    return out;
}

bool write_zip(const std::string& out_zip, const std::vector<Entry>& files, const std::vector<std::pair<std::string, std::string>>& texts, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    std::error_code ec;
    fs::path out = fs::u8path(out_zip);   // std::filesystem keeps these as UTF-16 on Windows; never go through .string() (ANSI)
    fs::path tmp = out; tmp += ".tmp";
    /* The archive is built in memory and written through a wide-path stream: miniz's own file functions take ANSI paths, which
     * fails for a Windows profile name with non-ASCII letters. */
    mz_zip_archive z;
    std::memset(&z, 0, sizeof z);
    if (!mz_zip_writer_init_heap(&z, 0, 64 * 1024)) return fail("Could not create the zip.");
    bool ok = true;
    for (auto& t : texts) {
        if (excluded(t.first)) continue;
        if (!mz_zip_writer_add_mem(&z, t.first.c_str(), t.second.data(), t.second.size(), MZ_DEFAULT_LEVEL)) { ok = false; break; }
    }
    for (auto& f : files) {
        if (!ok) break;
        if (excluded(f.zip_name) || excluded(f.src)) continue;
        std::ifstream in(fs::u8path(f.src), std::ios::binary);
        if (!in) continue;   // vanished or locked: skip it, the rest of the bundle is still useful
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!mz_zip_writer_add_mem(&z, f.zip_name.c_str(), data.data(), data.size(), MZ_DEFAULT_LEVEL)) ok = false;
    }
    void* buf = nullptr;
    size_t size = 0;
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&z, &buf, &size) != 0;
    if (ok) {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write((const char*)buf, (std::streamsize)size);
        o.flush();
        ok = (bool)o;
    }
    mz_zip_writer_end(&z);   // frees buf
    if (!ok) { fs::remove(tmp, ec); return fail("Could not write the zip."); }
    fs::remove(out, ec);
    fs::rename(tmp, out, ec);
    if (ec) return fail("Could not save the zip.");
    return true;
}

std::vector<Entry> limit_total_bytes(std::vector<Entry> files, unsigned long long max_bytes) {
    struct Item { Entry e; fs::file_time_type t; unsigned long long size; };
    std::vector<Item> items;
    std::error_code ec;
    for (auto& e : files) {
        fs::path p = fs::u8path(e.src);
        Item it{e, fs::last_write_time(p, ec), 0};
        if (ec) { ec.clear(); it.t = fs::file_time_type::min(); }
        it.size = fs::file_size(p, ec);
        if (ec) { ec.clear(); it.size = 0; }
        items.push_back(it);
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.t > b.t; });   // newest first
    std::vector<Entry> out;
    unsigned long long total = 0;
    for (auto& it : items) {
        if (total + it.size > max_bytes) continue;   // too big for what is left: a smaller older file may still fit
        total += it.size;
        out.push_back(it.e);
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.zip_name < b.zip_name; });
    return out;
}

std::vector<Entry> newest_crash_reports_only(std::vector<Entry> files, size_t keep) {
    std::vector<Entry> reports, rest;
    for (auto& e : files) {
        const size_t sl = e.zip_name.find_last_of('/');
        const std::string base = sl == std::string::npos ? e.zip_name : e.zip_name.substr(sl + 1);
        (base.rfind("crash-", 0) == 0 && base.size() > 4 && base.compare(base.size() - 4, 4, ".txt") == 0 ? reports : rest).push_back(e);
    }
    std::sort(reports.begin(), reports.end(), [](const Entry& a, const Entry& b) { return a.zip_name > b.zip_name; });   // names carry the timestamp
    if (reports.size() > keep) reports.resize(keep);
    rest.insert(rest.end(), reports.begin(), reports.end());
    std::sort(rest.begin(), rest.end(), [](const Entry& a, const Entry& b) { return a.zip_name < b.zip_name; });
    return rest;
}

}  // namespace hubdiag
