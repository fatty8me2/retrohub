#include "gametdb.h"
#include "gameinfo.h"

#include "miniz.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <thread>
#include <memory>
#include <mutex>

namespace fs = std::filesystem;

namespace gametdb {

namespace {

std::string upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

bool alnum_id(const std::string& s) {
    for (unsigned char c : s) if (!std::isalnum(c)) return false;
    return true;
}

void push_unique(std::vector<std::string>* v, const std::string& s) {
    if (s.empty()) return;
    if (std::find(v->begin(), v->end(), s) == v->end()) v->push_back(s);
}

std::string folder_for_letter(char c) {
    switch (std::toupper((unsigned char)c)) {
        case 'E': case 'O': return "US";
        case 'P': case 'X': case 'Y': case 'Z': return "EN";
        case 'J': return "JA";
        case 'K': return "KO";
        case 'D': return "DE";
        case 'F': return "FR";
        case 'S': return "ES";
        case 'I': return "IT";
        case 'H': return "NL";
        case 'U': return "AU";
        case 'R': return "RU";
        case 'W': return "ZH";
        case 'T': return "TW";
        default: return "";
    }
}

std::string folder_for_hint(const std::string& h) {
    std::string l = lower(h);
    if (l == "usa" || l == "us" || l == "world" || l == "canada") return "US";
    if (l == "europe" || l == "eur" || l == "uk") return "EN";
    if (l == "japan" || l == "jpn") return "JA";
    if (l == "korea") return "KO";
    if (l == "australia") return "AU";
    if (l == "germany") return "DE";
    if (l == "france") return "FR";
    if (l == "spain") return "ES";
    if (l == "italy") return "IT";
    if (l == "netherlands") return "NL";
    if (l == "russia") return "RU";
    if (l == "china") return "ZH";
    if (l == "taiwan") return "TW";
    return "";
}

}  // namespace

std::string db_name(const std::string& s) {
    if (s == "gc") return "wii";
    if (s == "wiiu") return "wiiu";
    if (s == "nds") return "ds";
    if (s == "3ds") return "3ds";
    if (s == "switch") return "switch";
    return "";
}

bool has_fullcover(const std::string& s) { return s == "gc" || s == "wiiu" || s == "nds" || s == "3ds"; }

std::string id_from_serial(const std::string& system, const std::string& serial) {
    std::string s = upper(serial);
    if (!alnum_id(s)) return "";
    if (system == "gc") return s.size() == 6 ? s : "";
    if (system == "wiiu") return s.size() == 6 ? s : s.size() == 4 ? s + "01" : "";
    if (system == "nds" || system == "3ds") return s.size() == 4 ? s : "";
    return "";
}

std::vector<std::string> region_folders(const std::string& id, const std::string& region_hint) {
    std::vector<std::string> v;
    if (id.size() >= 4) push_unique(&v, folder_for_letter(id[3]));
    push_unique(&v, folder_for_hint(region_hint));
    for (const char* r : {"US", "EN", "JA", "FR", "DE", "ES", "IT", "NL", "PT", "AU", "KO", "ZH", "RU", "TW"}) push_unique(&v, r);
    return v;
}

std::vector<std::pair<std::string, std::string>> regional_siblings(const std::string& id, const std::string& region_hint) {
    std::vector<std::pair<std::string, std::string>> out;
    if (id.size() < 4 || !alnum_id(id)) return out;
    std::string order;
    const std::string h = lower(region_hint);
    if (h == "europe" || h == "eur" || h == "uk" || h == "germany" || h == "france" || h == "spain" || h == "italy" || h == "australia") order += 'P';
    else if (h == "japan" || h == "jpn") order += 'J';
    else if (h == "korea") order += 'K';
    for (char c : std::string("EPJK")) if (order.find(c) == std::string::npos) order += c;
    const char own = (char)std::toupper((unsigned char)id[3]);
    for (char c : order) {
        if (c == own) continue;
        std::string sib = id;
        sib[3] = c;
        out.push_back({sib, c == 'E' ? "US" : c == 'P' ? "EN" : c == 'J' ? "JA" : "KO"});
    }
    return out;
}

std::string region_hint_from_name(const std::string& name) {
    static const char* words[] = {"USA", "Europe", "Japan", "World", "Korea", "Australia", "Germany", "France", "Spain", "Italy",
                                  "Netherlands", "Russia", "China", "Taiwan", "Canada"};
    size_t p = 0;
    while ((p = name.find('(', p)) != std::string::npos) {
        size_t e = name.find(')', p);
        if (e == std::string::npos) break;
        std::string tag = name.substr(p + 1, e - p - 1);
        for (const char* w : words)
            if (tag.compare(0, std::char_traits<char>::length(w), w) == 0) return w;
        p = e + 1;
    }
    return "";
}

std::vector<std::string> image_urls(const std::string& system, const std::string& id, Img what, const std::string& region) {
    std::vector<std::string> out;
    std::string db = db_name(system);
    if (db.empty() || id.empty() || region.empty() || !alnum_id(id) || !alnum_id(region)) return out;
    const char* ext = db == "wii" ? "png" : "jpg";
    std::vector<const char*> kinds;
    if (what == Img::Cover) { if (db != "wii") kinds.push_back("coverHQ"); kinds.push_back("cover"); }   // wii/coverHQ does not exist
    else { if (db == "switch") return out; kinds.push_back("coverfullHQ"); kinds.push_back("coverfull"); }
    for (const char* k : kinds) out.push_back("https://art.gametdb.com/" + db + "/" + k + "/" + region + "/" + id + "." + ext);
    return out;
}

std::string db_url(const std::string& system) {
    std::string db = db_name(system);
    if (db.empty()) return "";
    return "https://www.gametdb.com/" + db + "tdb.zip";
}


/* ============================ XML ============================ */

namespace {

/* Entities decode within a bounded window: an '&' without a ';' in the next 10 characters is literal (no scan to the end of the text). */
std::string decode_entities(std::string_view s) {
    if (s.find('&') == std::string_view::npos) return std::string(s);
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t e = std::string_view::npos;
        for (size_t k = i + 1; k < s.size() && k <= i + 10; ++k)
            if (s[k] == ';') { e = k; break; }
        if (e == std::string_view::npos) { o += s[i]; continue; }
        std::string ent(s.substr(i + 1, e - i - 1));
        if (ent == "amp") o += '&';
        else if (ent == "lt") o += '<';
        else if (ent == "gt") o += '>';
        else if (ent == "quot") o += '"';
        else if (ent == "apos") o += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? std::strtoul(ent.c_str() + 2, nullptr, 16) : std::strtoul(ent.c_str() + 1, nullptr, 10);
            if (cp < 0x80) o += (char)cp;
            else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x110000) { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        } else { o += s[i]; continue; }
        i = e;
    }
    return o;
}

/* Every search below runs inside ONE game's text (a string_view slice), so a missing tag can never scan into the next game or to the end of the file. */
using View = std::string_view;

View sub(View v, size_t from, size_t to) { return v.substr(from, to - from); }

/* Text of the first <tag>...</tag> in v ("" when absent or self-closing). */
std::string tag_text(View v, const char* tag) {
    const std::string open = std::string("<") + tag;
    size_t p = 0;
    while ((p = v.find(open, p)) != View::npos) {
        size_t gt = p + open.size();
        char c = gt < v.size() ? v[gt] : 0;
        if (c != '>' && c != ' ' && c != '/') { p = gt; continue; }
        size_t close = v.find('>', gt);
        if (close == View::npos) return "";
        if (v[close - 1] == '/') return "";
        size_t end = v.find(std::string("</") + tag + ">", close);
        if (end == View::npos) return "";
        return decode_entities(sub(v, close + 1, end));
    }
    return "";
}

/* Value of attr in the first <tag ... attr="v" ...> in v (must_have: that text has to be in the tag's head). */
std::string tag_attr(View v, const char* tag, const char* attr, const char* must_have = nullptr) {
    const std::string open = std::string("<") + tag;
    size_t p = 0;
    while ((p = v.find(open, p)) != View::npos) {
        size_t close = v.find('>', p);
        if (close == View::npos) return "";
        View head = sub(v, p, close);
        p = close;
        if (must_have && head.find(must_have) == View::npos) continue;
        std::string key = std::string(" ") + attr + "=\"";
        size_t a = head.find(key);
        if (a == View::npos) continue;
        a += key.size();
        size_t q = head.find('"', a);
        if (q == View::npos) continue;
        return decode_entities(sub(head, a, q));
    }
    return "";
}

int to_int(const std::string& s) { return s.empty() ? 0 : std::atoi(s.c_str()); }

}  // namespace

std::vector<Rec> parse_xml(const std::string& x, const std::string& type_filter) {
    std::vector<Rec> out;
    const View all(x);
    size_t p = 0;
    while ((p = all.find("<game ", p)) != View::npos) {
        size_t end = all.find("</game>", p);
        if (end == View::npos) break;
        const View g = sub(all, p, end);   // this game only
        p = end + 7;
        if (!type_filter.empty() && tag_text(g, "type") != type_filter) continue;
        Rec r;
        r.id = tag_text(g, "id");
        if (r.id.empty()) continue;
        r.region = tag_text(g, "region");
        /* locales: the English one, else the first */
        size_t loc = g.find("<locale lang=\"EN\"");
        if (loc == View::npos) loc = g.find("<locale ");
        if (loc != View::npos) {
            size_t le = g.find("</locale>", loc);
            View l = le == View::npos ? g.substr(loc) : sub(g, loc, le);
            r.title = tag_text(l, "title");
            r.synopsis = tag_text(l, "synopsis");
        }
        if (r.title.empty()) {   // the <game name="..."> attribute
            size_t q = g.find("name=\"");
            if (q != View::npos) { size_t q2 = g.find('"', q + 6); if (q2 != View::npos) r.title = decode_entities(sub(g, q + 6, q2)); }
        }
        r.developer = tag_text(g, "developer");
        r.publisher = tag_text(g, "publisher");
        r.genre = tag_text(g, "genre");
        r.year = to_int(tag_attr(g, "date", "year"));
        r.month = to_int(tag_attr(g, "date", "month"));
        r.players = to_int(tag_attr(g, "input", "players"));
        r.esrb = tag_attr(g, "rating", "value", "type=\"ESRB\"");
        std::string pegi = tag_attr(g, "rating", "value", "type=\"PEGI\"");
        if (!pegi.empty()) r.pegi = "PEGI " + pegi;
        out.push_back(std::move(r));
    }
    return out;
}

namespace {
std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\t') o += "\\t";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') {}
        else o += c;
    }
    return o;
}
std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) { char n = s[++i]; o += n == 't' ? '\t' : n == 'n' ? '\n' : n; }
        else o += s[i];
    }
    return o;
}

bool parse_line(const std::string& line, Rec* out) {
    if (line.empty() || line[0] == '#') return false;
    std::vector<std::string> f;
    size_t a = 0;
    while (true) {
        size_t t = line.find('\t', a);
        f.push_back(unesc(line.substr(a, t == std::string::npos ? std::string::npos : t - a)));
        if (t == std::string::npos) break;
        a = t + 1;
    }
    if (f.size() < 12) return false;
    Rec r;
    r.id = f[0]; r.region = f[1]; r.title = f[2]; r.synopsis = f[3]; r.developer = f[4]; r.publisher = f[5]; r.genre = f[6];
    r.esrb = f[7]; r.pegi = f[8]; r.year = to_int(f[9]); r.month = to_int(f[10]); r.players = to_int(f[11]);
    *out = std::move(r);
    return true;
}
}  // namespace

std::string to_tsv(const std::vector<Rec>& recs) {
    std::string o = "#gametdb-index v1\n";
    for (const Rec& r : recs)
        o += esc(r.id) + "\t" + esc(r.region) + "\t" + esc(r.title) + "\t" + esc(r.synopsis) + "\t" + esc(r.developer) + "\t" + esc(r.publisher) +
             "\t" + esc(r.genre) + "\t" + esc(r.esrb) + "\t" + esc(r.pegi) + "\t" + std::to_string(r.year) + "\t" + std::to_string(r.month) + "\t" +
             std::to_string(r.players) + "\n";
    return o;
}

std::vector<Rec> parse_tsv(const std::string& tsv) {
    std::vector<Rec> out;
    size_t p = 0;
    while (p < tsv.size()) {
        size_t nl = tsv.find('\n', p);
        if (nl == std::string::npos) nl = tsv.size();
        Rec r;
        if (parse_line(tsv.substr(p, nl - p), &r)) out.push_back(std::move(r));
        p = nl + 1;
    }
    return out;
}

/* ============================ index ============================ */

void Index::add(Slim s) {
    by_id_.emplace_back(s.id, s_.size());
    std::string n = gameinfo::normalize(gameinfo::clean_title(s.title));
    if (!n.empty()) by_title_.emplace_back(n, s_.size());
    s_.push_back(std::move(s));
}

void Index::finish() {
    std::sort(by_title_.begin(), by_title_.end());
    std::sort(by_id_.begin(), by_id_.end());
}

void Index::load(std::vector<Rec> recs) {
    s_.clear(); by_title_.clear(); by_id_.clear(); path_.clear();
    mem_ = std::move(recs);
    for (size_t i = 0; i < mem_.size(); ++i) {
        Slim s;
        s.id = mem_[i].id; s.region = mem_[i].region; s.title = mem_[i].title; s.off = i; s.syn = !mem_[i].synopsis.empty();
        add(std::move(s));
    }
    finish();
}

bool Index::load_file(const std::string& tsv_path) {
    s_.clear(); by_title_.clear(); by_id_.clear(); mem_.clear(); path_.clear();
    std::string text;
    {
        std::ifstream f(fs::u8path(tsv_path), std::ios::binary);
        if (!f) return false;
        text.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    size_t p = 0;
    while (p < text.size()) {
        size_t nl = text.find('\n', p);
        if (nl == std::string::npos) nl = text.size();
        Rec r;
        if (parse_line(text.substr(p, nl - p), &r)) {   // only id / region / title stay in memory; the rest is re-read from the file on demand
            Slim s;
            s.id = std::move(r.id); s.region = std::move(r.region); s.title = std::move(r.title);
            s.off = p; s.len = (uint32_t)std::min<size_t>(nl - p, 0xFFFFFFFFu); s.syn = !r.synopsis.empty();
            add(std::move(s));
        }
        p = nl + 1;
    }
    finish();
    path_ = tsv_path;
    return !s_.empty();
}

std::optional<Rec> Index::fetch(size_t i) const {
    if (i >= s_.size()) return std::nullopt;
    const Slim& s = s_[i];
    if (path_.empty()) return i < mem_.size() ? std::optional<Rec>(mem_[s.off]) : std::nullopt;
    std::ifstream f(fs::u8path(path_), std::ios::binary);
    if (!f) return std::nullopt;
    f.seekg((std::streamoff)s.off);
    std::string line(s.len, '\0');
    f.read(&line[0], (std::streamsize)s.len);
    Rec r;
    if (f.gcount() != (std::streamsize)s.len || !parse_line(line, &r) || r.id != s.id) return std::nullopt;   // file changed under us: no record
    return r;
}

std::optional<Rec> Index::by_id(const std::string& id) const {
    auto it = std::lower_bound(by_id_.begin(), by_id_.end(), std::make_pair(id, (size_t)0));
    if (it == by_id_.end() || it->first != id) return std::nullopt;
    return fetch(it->second);
}

std::optional<Rec> Index::by_title(const std::string& title, bool want_synopsis) const {
    std::string n = gameinfo::normalize(gameinfo::clean_title(title));
    if (n.empty()) return std::nullopt;
    auto it = std::lower_bound(by_title_.begin(), by_title_.end(), std::make_pair(n, (size_t)0));
    size_t best = (size_t)-1;
    int best_score = 1 << 30;
    for (; it != by_title_.end() && it->first == n; ++it) {
        const Slim& r = s_[it->second];
        int score = r.region.find("NTSC-U") != std::string::npos ? 0 : r.region.find("PAL") != std::string::npos ? 1 : 2;
        if (want_synopsis && !r.syn) score += 10;
        if (score < best_score) { best_score = score; best = it->second; }
    }
    return best == (size_t)-1 ? std::nullopt : fetch(best);
}

std::optional<Rec> find_record(const Index& idx, const std::string& system, const std::string& serial, const std::vector<std::string>& titles) {
    std::optional<Rec> r;
    std::string id = id_from_serial(system, serial);
    if (!id.empty()) r = idx.by_id(id);
    if (!r || r->synopsis.empty()) {
        for (const std::string& t : titles) {
            std::optional<Rec> t_rec = idx.by_title(t, true);
            if (t_rec && (!r || !t_rec->synopsis.empty())) { r = std::move(t_rec); break; }
        }
    }
    return r;
}

/* ============================ download / cache ============================ */

bool unzip_xml(const std::string& zip, std::string* xml) {
    xml->clear();
    mz_zip_archive za;
    std::memset(&za, 0, sizeof za);
    if (!mz_zip_reader_init_mem(&za, zip.data(), zip.size(), 0)) return false;
    bool ok = false;
    mz_uint n = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < n && !ok; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st) || st.m_is_directory) continue;
        std::string name = lower(st.m_filename);
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".xml") != 0) continue;
        if (st.m_uncomp_size > (256u << 20)) continue;
        size_t sz = 0;
        void* heap = mz_zip_reader_extract_to_heap(&za, i, &sz, 0);
        if (!heap) continue;
        xml->assign((const char*)heap, sz);
        mz_free(heap);
        ok = true;
    }
    mz_zip_reader_end(&za);
    return ok;
}

namespace {

std::mutex g_mu;           // guards the maps
std::mutex g_load_mu;      // one download / parse at a time
std::string g_dir;
std::map<std::string, std::shared_ptr<Index>> g_idx;
std::map<std::string, long long> g_failed;
std::set<std::string> g_downloading;

long long now_s() { return (long long)std::time(nullptr); }
const long long kRefreshSeconds = 60LL * 24 * 3600;
const long long kRetrySeconds = 300;

fs::path cache_path(const std::string& db) { return fs::u8path(g_dir) / ("tdb_" + db + ".tsv"); }

}  // namespace

void set_dir(const std::string& d) {
    std::lock_guard<std::mutex> l(g_mu);
    g_dir = d;
    if (!g_dir.empty() && g_dir.back() != '/' && g_dir.back() != '\\') g_dir += '/';
}
std::string dir() { std::lock_guard<std::mutex> l(g_mu); return g_dir; }

const Index* cached_index(const std::string& system) {
    std::string db = db_name(system);
    if (db.empty()) return nullptr;
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_idx.find(db);
        if (it != g_idx.end()) return it->second.get();
        if (g_dir.empty()) return nullptr;
    }
    /* an index file already on disk loads without any network (the slim in-memory form is cheap); never a download */
    std::lock_guard<std::mutex> ll(g_load_mu);
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_idx.find(db);
        if (it != g_idx.end()) return it->second.get();
    }
    std::error_code ec;
    if (!fs::exists(cache_path(db), ec)) return nullptr;
    auto idx = std::make_shared<Index>();
    if (!idx->load_file(cache_path(db).u8string())) return nullptr;
    std::lock_guard<std::mutex> l(g_mu);
    g_idx[db] = idx;
    return idx.get();
}

const Index* index_for(const std::string& system, const Get& get) {
    const std::string db = db_name(system);
    if (db.empty()) return nullptr;
    {   // a recent index file loads without any network; a stale one (> 60 days) is refreshed below, and still used if the refresh fails
        bool stale = false;
        {
            std::lock_guard<std::mutex> l(g_mu);
            if (g_idx.count(db)) return g_idx[db].get();
            if (g_dir.empty()) return nullptr;
            std::error_code ec;
            auto ft = fs::last_write_time(cache_path(db), ec);
            stale = !ec && std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::clock::now() - ft).count() > kRefreshSeconds;
        }
        if (!stale || !get)
            if (const Index* i = cached_index(system)) return i;
    }
    std::lock_guard<std::mutex> ll(g_load_mu);
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_idx.find(db);
        if (it != g_idx.end()) return it->second.get();
        if (g_dir.empty()) return nullptr;
        auto f = g_failed.find(db);
        if (f != g_failed.end() && now_s() < f->second) return nullptr;
    }
    if (get) {
        std::string zip, xml;
        int st = 0;
        if (get(db_url(system), &zip, &st) && st == 200 && zip.size() > 1000 && unzip_xml(zip, &xml)) {
            std::vector<Rec> fresh = parse_xml(xml, db == "wii" ? "GameCube" : "");
            if (!fresh.empty()) {
                std::error_code ec;
                fs::create_directories(fs::u8path(g_dir), ec);
                std::string tsv = to_tsv(fresh);
                fs::path tmp = cache_path(db);
                tmp += L".tmp";
                {
                    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                    f.write(tsv.data(), (std::streamsize)tsv.size());
                }
                fs::rename(tmp, cache_path(db), ec);
                if (ec) { fs::remove(cache_path(db), ec); fs::rename(tmp, cache_path(db), ec); }
            }
        }
    }
    auto idx = std::make_shared<Index>();
    std::lock_guard<std::mutex> l(g_mu);
    std::error_code ec;
    if (!fs::exists(cache_path(db), ec) || !idx->load_file(cache_path(db).u8string())) { g_failed[db] = now_s() + kRetrySeconds; return nullptr; }
    g_idx[db] = idx;
    return idx.get();
}

bool request_download(const std::string& system, const Get& get, std::function<void()> done) {
    const std::string db = db_name(system);
    if (db.empty() || !get) return false;
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_dir.empty() || g_idx.count(db) || !g_downloading.insert(db).second) return false;   // already loaded, or already on its way: once
    }
    std::thread([system, db, get, done]() {
        const Index* i = index_for(system, get);
        { std::lock_guard<std::mutex> l(g_mu); g_downloading.erase(db); }
        if (i && done) done();
    }).detach();
    return true;
}

} // namespace gametdb
