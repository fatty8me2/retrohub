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

std::string decode_entities(const std::string& s) {
    if (s.find('&') == std::string::npos) return s;
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t e = s.find(';', i);
        if (e == std::string::npos || e - i > 10) { o += s[i]; continue; }
        std::string ent = s.substr(i + 1, e - i - 1);
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

/* Text of the first <tag>...</tag> in [b, e) ("" when absent or self-closing). */
std::string tag_text(const std::string& x, size_t b, size_t e, const char* tag) {
    const std::string open = std::string("<") + tag;
    size_t p = b;
    while ((p = x.find(open, p)) != std::string::npos && p < e) {
        size_t gt = p + open.size();
        char c = gt < x.size() ? x[gt] : 0;
        if (c != '>' && c != ' ' && c != '/') { p = gt; continue; }
        size_t close = x.find('>', gt);
        if (close == std::string::npos || close >= e) return "";
        if (x[close - 1] == '/') return "";
        size_t end = x.find(std::string("</") + tag + ">", close);
        if (end == std::string::npos || end > e) return "";
        return decode_entities(x.substr(close + 1, end - close - 1));
    }
    return "";
}

/* Value of attr in the first <tag ... attr="v" ...> in [b, e). */
std::string tag_attr(const std::string& x, size_t b, size_t e, const char* tag, const char* attr, const char* must_have = nullptr) {
    const std::string open = std::string("<") + tag;
    size_t p = b;
    while ((p = x.find(open, p)) != std::string::npos && p < e) {
        size_t close = x.find('>', p);
        if (close == std::string::npos || close > e) return "";
        std::string head = x.substr(p, close - p);
        p = close;
        if (must_have && head.find(must_have) == std::string::npos) continue;
        std::string key = std::string(" ") + attr + "=\"";
        size_t a = head.find(key);
        if (a == std::string::npos) continue;
        a += key.size();
        size_t q = head.find('"', a);
        if (q == std::string::npos) continue;
        return decode_entities(head.substr(a, q - a));
    }
    return "";
}

int to_int(const std::string& s) { return s.empty() ? 0 : std::atoi(s.c_str()); }

}  // namespace

std::vector<Rec> parse_xml(const std::string& x, const std::string& type_filter) {
    std::vector<Rec> out;
    size_t p = 0;
    while ((p = x.find("<game ", p)) != std::string::npos) {
        size_t end = x.find("</game>", p);
        if (end == std::string::npos) break;
        size_t b = p;
        p = end + 7;
        if (!type_filter.empty() && tag_text(x, b, end, "type") != type_filter) continue;
        Rec r;
        r.id = tag_text(x, b, end, "id");
        if (r.id.empty()) continue;
        r.region = tag_text(x, b, end, "region");
        /* locales: the English one, else the first */
        size_t first = x.find("<locale ", b);
        size_t en = x.find("<locale lang=\"EN\"", b);
        size_t loc = (en != std::string::npos && en < end) ? en : first;
        if (loc != std::string::npos && loc < end) {
            size_t le = x.find("</locale>", loc);
            if (le == std::string::npos || le > end) le = end;
            r.title = tag_text(x, loc, le, "title");
            r.synopsis = tag_text(x, loc, le, "synopsis");
        }
        if (r.title.empty()) {   // the <game name="..."> attribute, region tags dropped
            size_t q = x.find("name=\"", b);
            if (q != std::string::npos && q < end) { size_t q2 = x.find('"', q + 6); r.title = decode_entities(x.substr(q + 6, q2 - q - 6)); }
        }
        r.developer = tag_text(x, b, end, "developer");
        r.publisher = tag_text(x, b, end, "publisher");
        r.genre = tag_text(x, b, end, "genre");
        r.year = to_int(tag_attr(x, b, end, "date", "year"));
        r.month = to_int(tag_attr(x, b, end, "date", "month"));
        r.players = to_int(tag_attr(x, b, end, "input", "players"));
        r.esrb = tag_attr(x, b, end, "rating", "value", "type=\"ESRB\"");
        std::string pegi = tag_attr(x, b, end, "rating", "value", "type=\"PEGI\"");
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
        std::string line = tsv.substr(p, nl - p);
        p = nl + 1;
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        size_t a = 0;
        while (true) {
            size_t t = line.find('\t', a);
            f.push_back(unesc(line.substr(a, t == std::string::npos ? std::string::npos : t - a)));
            if (t == std::string::npos) break;
            a = t + 1;
        }
        if (f.size() < 12) continue;
        Rec r;
        r.id = f[0]; r.region = f[1]; r.title = f[2]; r.synopsis = f[3]; r.developer = f[4]; r.publisher = f[5]; r.genre = f[6];
        r.esrb = f[7]; r.pegi = f[8]; r.year = to_int(f[9]); r.month = to_int(f[10]); r.players = to_int(f[11]);
        out.push_back(std::move(r));
    }
    return out;
}

/* ============================ index ============================ */

void Index::load(std::vector<Rec> recs) {
    recs_ = std::move(recs);
    by_title_.clear();
    by_id_.clear();
    for (size_t i = 0; i < recs_.size(); ++i) {
        by_id_.emplace_back(recs_[i].id, i);
        std::string n = gameinfo::normalize(gameinfo::clean_title(recs_[i].title));
        if (!n.empty()) by_title_.emplace_back(n, i);
    }
    std::sort(by_title_.begin(), by_title_.end());
    std::sort(by_id_.begin(), by_id_.end());
}

const Rec* Index::by_id(const std::string& id) const {
    auto it = std::lower_bound(by_id_.begin(), by_id_.end(), std::make_pair(id, (size_t)0));
    return it != by_id_.end() && it->first == id ? &recs_[it->second] : nullptr;
}

const Rec* Index::by_title(const std::string& title, bool want_synopsis) const {
    std::string n = gameinfo::normalize(gameinfo::clean_title(title));
    if (n.empty()) return nullptr;
    auto it = std::lower_bound(by_title_.begin(), by_title_.end(), std::make_pair(n, (size_t)0));
    const Rec* best = nullptr;
    int best_score = 1 << 30;
    for (; it != by_title_.end() && it->first == n; ++it) {
        const Rec& r = recs_[it->second];
        int score = r.region.find("NTSC-U") != std::string::npos ? 0 : r.region.find("PAL") != std::string::npos ? 1 : 2;
        if (want_synopsis && r.synopsis.empty()) score += 10;
        if (score < best_score) { best_score = score; best = &r; }
    }
    return best;
}

const Rec* find_record(const Index& idx, const std::string& system, const std::string& serial, const std::vector<std::string>& titles) {
    const Rec* r = nullptr;
    std::string id = id_from_serial(system, serial);
    if (!id.empty()) r = idx.by_id(id);
    if (!r || r->synopsis.empty()) {
        for (const std::string& t : titles) {
            const Rec* t_rec = idx.by_title(t, true);
            if (t_rec && (!r || !t_rec->synopsis.empty())) { r = t_rec; break; }
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

long long now_s() { return (long long)std::time(nullptr); }
const long long kRefreshSeconds = 60LL * 24 * 3600;
const long long kRetrySeconds = 300;

fs::path cache_path(const std::string& db) { return fs::u8path(g_dir) / ("tdb_" + db + ".tsv"); }

bool read_all(const fs::path& p, std::string* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

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
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_idx.find(db);
    return it == g_idx.end() ? nullptr : it->second.get();
}

const Index* index_for(const std::string& system, const Get& get) {
    const std::string db = db_name(system);
    if (db.empty()) return nullptr;
    if (const Index* i = cached_index(system)) return i;
    std::lock_guard<std::mutex> ll(g_load_mu);
    if (const Index* i = cached_index(system)) return i;   // another thread loaded it while we waited
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_dir.empty()) return nullptr;
        auto f = g_failed.find(db);
        if (f != g_failed.end() && now_s() < f->second) return nullptr;
    }
    std::string text;
    bool have = read_all(cache_path(db), &text);
    bool stale = false;
    if (have) {
        std::error_code ec;
        auto ft = fs::last_write_time(cache_path(db), ec);
        if (!ec) stale = std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::clock::now() - ft).count() > kRefreshSeconds;
    }
    std::vector<Rec> recs;
    if (have) recs = parse_tsv(text);
    if ((!have || stale || recs.empty()) && get) {
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
                recs = std::move(fresh);
            }
        }
    }
    std::lock_guard<std::mutex> l(g_mu);
    if (recs.empty()) { g_failed[db] = now_s() + kRetrySeconds; return nullptr; }
    auto idx = std::make_shared<Index>();
    idx->load(std::move(recs));
    g_idx[db] = idx;
    return idx.get();
}

} // namespace gametdb
