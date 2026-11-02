#include "diag_logic.h"

#include "miniz.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

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
    fs::path out = fs::u8path(out_zip);
    fs::path tmp = out; tmp += ".tmp";
    mz_zip_archive z;
    std::memset(&z, 0, sizeof z);
    if (!mz_zip_writer_init_file(&z, tmp.string().c_str(), 0)) return fail("Could not create the zip.");
    bool ok = true;
    for (auto& t : texts) {
        if (excluded(t.first)) continue;
        if (!mz_zip_writer_add_mem(&z, t.first.c_str(), t.second.data(), t.second.size(), MZ_DEFAULT_LEVEL)) { ok = false; break; }
    }
    for (auto& f : files) {
        if (!ok) break;
        if (excluded(f.zip_name) || excluded(f.src)) continue;
        if (!mz_zip_writer_add_file(&z, f.zip_name.c_str(), fs::u8path(f.src).string().c_str(), nullptr, 0, MZ_DEFAULT_LEVEL)) ok = false;
    }
    if (ok) ok = mz_zip_writer_finalize_archive(&z) != 0;
    mz_zip_writer_end(&z);
    if (!ok) { fs::remove(tmp, ec); return fail("Could not write the zip."); }
    fs::remove(out, ec);
    fs::rename(tmp, out, ec);
    if (ec) return fail("Could not save the zip.");
    return true;
}

}  // namespace hubdiag
