#include "addgames.h"
#include "ps1.h"
#include "winutil.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fs = std::filesystem;

namespace hub::addgames {

namespace {
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
fs::path P(const std::string& u8) { return fs::path(winutil::to_wide(u8)); }
std::string U8(const fs::path& p) { return winutil::to_utf8(p.wstring()); }
std::string leaf_of(const std::string& p) {
    std::string t = p;
    while (t.size() > 3 && (t.back() == '\\' || t.back() == '/')) t.pop_back();
    size_t c = t.find_last_of("\\/");
    return c == std::string::npos ? t : t.substr(c + 1);
}
std::string pjoin(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    return (a.back() == '\\' || a.back() == '/') ? a + b : a + "\\" + b;
}
}  // namespace

const std::vector<std::string>& supported_exts() {
    static const std::vector<std::string> v = {".z64", ".n64", ".v64", ".gba", ".gb", ".gbc", ".nds", ".3ds", ".cci", ".cia", ".cxi",
                                               ".iso", ".gcm", ".rvz", ".ciso", ".wia", ".gcz", ".wua", ".wud", ".wux", ".rpx",
                                               ".chd", ".cso", ".cue", ".pbp", ".m3u", ".zip", ".7z"};
    return v;
}

bool is_game_file(const std::string& path) {
    std::string e = lower(U8(P(path).extension()));
    const auto& v = supported_exts();
    return std::find(v.begin(), v.end(), e) != v.end();
}

std::string console_folder(System s) {
    switch (s) {
        case System::N64: return "Nintendo 64";
        case System::GBA: return "Game Boy Advance";
        case System::GB: return "Game Boy";
        case System::GBC: return "Game Boy Color";
        case System::GC: return "GameCube";
        case System::N3DS: return "3DS";
        case System::WiiU: return "Wii U";
        case System::NDS: return "DS";
        case System::PS1: return "PlayStation";
        case System::PS2: return "PlayStation 2";
        default: return "Other";
    }
}

bool is_absolute_path(const std::string& p) {
    if (p.size() >= 3 && std::isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) return true;
    return p.size() >= 3 && p[0] == '\\' && p[1] == '\\';
}

std::set<std::string> index_keys(const std::string& root, const std::atomic<bool>* stop) {
    std::set<std::string> keys;
    std::error_code ec;
    if (root.empty() || !fs::is_directory(P(root), ec)) return keys;
    for (fs::recursive_directory_iterator it(P(root), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (stop && *stop) break;
        std::error_code fe;
        if (!it->is_regular_file(fe)) continue;
        std::string p = U8(it->path());
        if (!is_game_file(p)) continue;
        GameEntry g;
        if (identify_game(p, &g) && !g.key.empty()) keys.insert(g.key);
    }
    return keys;
}

std::string games_root(const std::vector<std::string>& rom_folders, const std::string& documents_dir, const std::string& custom_root) {
    if (is_absolute_path(custom_root)) {
        std::string t = custom_root;
        while (t.size() > 3 && (t.back() == '\\' || t.back() == '/')) t.pop_back();
        return t;
    }
    for (const std::string& f : rom_folders)
        if (lower(leaf_of(f)) == "retrohub games") {
            std::string t = f;
            while (t.size() > 3 && (t.back() == '\\' || t.back() == '/')) t.pop_back();
            return t;
        }
    return pjoin(documents_dir, "RetroHub Games");
}

std::string dest_dir(const std::string& root, System s) { return pjoin(root, console_folder(s)); }

std::string unique_name(const std::string& dir, const std::string& file_name, const std::function<bool(const std::string&)>& exists) {
    if (!exists(pjoin(dir, file_name))) return file_name;
    size_t dot = file_name.find_last_of('.');
    std::string stem = dot == std::string::npos || dot == 0 ? file_name : file_name.substr(0, dot);
    std::string ext = dot == std::string::npos || dot == 0 ? std::string() : file_name.substr(dot);
    for (int n = 2; n < 10000; ++n) {
        std::string c = stem + " (" + std::to_string(n) + ")" + ext;
        if (!exists(pjoin(dir, c))) return c;
    }
    return file_name;
}

int count_outcome(const std::vector<Item>& items, Outcome o) {
    int n = 0;
    for (const Item& i : items) n += i.outcome == o;
    return n;
}

std::string summary_text(const std::vector<Item>& items) {
    auto names = [&](Outcome o) {
        std::string s;
        int shown = 0, total = 0;
        for (const Item& i : items) {
            if (i.outcome != o) continue;
            ++total;
            if (shown < 4) { s += (shown ? ", " : "") + (i.title.empty() ? leaf_of(i.src) : i.title); ++shown; }
        }
        if (total > shown) s += " and " + std::to_string(total - shown) + " more";
        return s;
    };
    std::string out;
    int a = count_outcome(items, Outcome::Added), d = count_outcome(items, Outcome::Duplicate);
    int f = count_outcome(items, Outcome::Failed), u = count_outcome(items, Outcome::Unsupported), c = count_outcome(items, Outcome::Cancelled);
    auto line = [&](const std::string& s) { out += (out.empty() ? "" : "\n") + s; };
    if (a) line("Added " + std::to_string(a) + (a == 1 ? " game: " : " games: ") + names(Outcome::Added));
    int other = 0;
    for (const Item& i : items) other += i.outcome == Outcome::Added && i.other;
    if (other) line(std::to_string(other) + (other == 1 ? " file was" : " files were") + " not recognised and went to the Other folder");
    if (d) line("Already in your library: " + names(Outcome::Duplicate));
    if (u) line("Skipped (not a game file): " + names(Outcome::Unsupported));
    if (f) {
        std::string first;
        for (const Item& i : items) if (i.outcome == Outcome::Failed) { first = i.message; break; }
        line("Could not add " + names(Outcome::Failed) + (first.empty() ? "" : " - " + first));
    }
    if (c) line("Cancelled: " + names(Outcome::Cancelled));
    if (out.empty()) out = "Nothing to add.";
    return out;
}

bool copy_file(const std::string& src, const std::string& dst, uint64_t* copied_total,
               const std::function<bool(uint64_t, uint64_t)>& progress, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    FILE* in = _wfopen(winutil::to_wide(src).c_str(), L"rbN");
    if (!in) return fail("could not read the file");
    std::wstring part = winutil::to_wide(dst) + L".part";
    FILE* out = _wfopen(part.c_str(), L"wbN");
    if (!out) { std::fclose(in); return fail("could not write to the games folder"); }
    _fseeki64(in, 0, SEEK_END);
    uint64_t total = (uint64_t)_ftelli64(in);
    _fseeki64(in, 0, SEEK_SET);
    std::vector<char> buf(4u << 20);
    uint64_t done = 0;
    bool ok = true, cancelled = false;
    std::string e;
    while (done < total) {
        size_t want = (size_t)std::min<uint64_t>(buf.size(), total - done);
        size_t got = std::fread(buf.data(), 1, want, in);
        if (got != want) { ok = false; e = "the file could not be read to the end"; break; }
        if (std::fwrite(buf.data(), 1, got, out) != got) { ok = false; e = "not enough disk space (or the drive is not writable)"; break; }
        done += got;
        if (copied_total) *copied_total += got;
        if (progress && !progress(done, total)) { cancelled = true; break; }
    }
    std::fclose(in);
    if (std::fclose(out) != 0 && ok) { ok = false; e = "not enough disk space (or the drive is not writable)"; }
    if (ok && !cancelled && !MoveFileExW(part.c_str(), winutil::to_wide(dst).c_str(), 0)) { ok = false; e = "could not finish writing the file"; }
    if (!ok || cancelled) {
        DeleteFileW(part.c_str());
        return fail(cancelled ? "cancelled" : e);
    }
    return true;
}

void Job::start(std::vector<std::string> files, std::string root, std::set<std::string> known_keys) {
    join();
    done_ = false;
    cancel_ = false;
    bytes_done_ = bytes_total_ = 0;
    file_idx_ = 0;
    file_cnt_ = (int)files.size();
    items_.clear();
    th_ = std::thread([this, f = std::move(files), r = std::move(root), k = std::move(known_keys)]() mutable { run(std::move(f), std::move(r), std::move(k)); });
}

std::string Job::current_name() { std::lock_guard<std::mutex> lk(mu_); return cur_; }
std::vector<Item> Job::items() { std::lock_guard<std::mutex> lk(mu_); return items_; }

void Job::run(std::vector<std::string> files, std::string root, std::set<std::string> known_keys) {
    std::error_code ec;
    for (const std::string& f : files) {
        std::error_code e2;
        uint64_t sz = fs::file_size(P(f), e2);
        if (!e2) bytes_total_ += sz;
    }
    {   // fresh hash index of what is already in the destination, never an unfinished library scan
        std::set<std::string> fresh = index_keys(root, &cancel_);
        known_keys.insert(fresh.begin(), fresh.end());
    }
    for (size_t i = 0; i < files.size() && !cancel_; ++i) {
        const std::string& src = files[i];
        file_idx_ = (int)i + 1;
        Item it;
        it.src = src;
        std::string name = leaf_of(src);
        { std::lock_guard<std::mutex> lk(mu_); cur_ = name; }
        auto finish = [&](Outcome o, const std::string& m) {
            it.outcome = o;
            it.message = m;
            std::lock_guard<std::mutex> lk(mu_);
            items_.push_back(it);
        };
        if (!is_game_file(src)) { finish(Outcome::Unsupported, "not a supported game file"); continue; }
        std::error_code fe;
        uint64_t size = fs::file_size(P(src), fe);
        if (fe) { finish(Outcome::Failed, "could not read the file"); continue; }
        GameEntry g;
        bool known = identify_game(src, &g);
        it.system = known ? g.system : System::Unknown;
        it.other = !known || g.system == System::Unknown;
        it.title = known && !g.title.empty() ? g.title : U8(P(name).stem());
        if (known && !g.key.empty()) {
            if (known_keys.count(g.key)) { finish(Outcome::Duplicate, "already in your library"); bytes_done_ += size; continue; }
            known_keys.insert(g.key);   // also dedupes the same game appearing twice in one batch
        }
        std::string dir = dest_dir(root, it.system);
        /* A PlayStation .cue / .m3u needs its bins / discs next to it: the set goes into its own folder, game name as folder name. */
        std::vector<std::string> comps;
        if (known && g.system == System::PS1) {
            std::string ext = lower(U8(P(name).extension()));
            if (ext == ".cue" || ext == ".m3u") {
                for (const std::string& c : ps1::referenced_files(src)) {
                    std::error_code ce;
                    if (fs::is_regular_file(P(c), ce) && !ce) comps.push_back(c);
                }
            }
            if (!comps.empty()) {
                dir = pjoin(dir, U8(P(name).stem()));
                for (const std::string& c : comps) { std::error_code ce; uint64_t cs = fs::file_size(P(c), ce); if (!ce) bytes_total_ += cs; }
            }
        }
        fs::create_directories(P(dir), ec);
        auto exists = [](const std::string& p) { std::error_code e; return fs::exists(P(p), e); };
        std::string same = pjoin(dir, name);
        if (exists(same)) {
            std::error_code se;
            if (fs::file_size(P(same), se) == size && !se) { finish(Outcome::Duplicate, "already in your library"); bytes_done_ += size; continue; }
        }
        ULARGE_INTEGER freeb{};
        if (GetDiskFreeSpaceExW(winutil::to_wide(dir).c_str(), &freeb, nullptr, nullptr) && freeb.QuadPart < size + (64ull << 20)) {
            finish(Outcome::Failed, "not enough free disk space");
            continue;
        }
        it.dest = pjoin(dir, unique_name(dir, name, exists));
        uint64_t base = bytes_done_;
        uint64_t counted = 0;
        std::string err;
        bool ok = copy_file(src, it.dest, &counted, [&](uint64_t d, uint64_t) { bytes_done_ = base + d; return !cancel_; }, &err);
        bytes_done_ = base + size;
        for (size_t ci = 0; ok && ci < comps.size(); ++ci) {
            std::error_code ce;
            std::string cdst = pjoin(dir, leaf_of(comps[ci]));
            uint64_t cs = fs::file_size(P(comps[ci]), ce);
            if (ce) continue;
            if (fs::exists(P(cdst), ce) && fs::file_size(P(cdst), ce) == cs) { bytes_done_ += cs; continue; }
            uint64_t cbase = bytes_done_;
            ok = copy_file(comps[ci], cdst, &counted, [&](uint64_t d, uint64_t) { bytes_done_ = cbase + d; return !cancel_; }, &err);
            bytes_done_ = cbase + cs;
        }
        if (ok) finish(Outcome::Added, "");
        else if (err == "cancelled") finish(Outcome::Cancelled, "cancelled");
        else finish(Outcome::Failed, err);
    }
    done_ = true;
}

}  // namespace hub::addgames
