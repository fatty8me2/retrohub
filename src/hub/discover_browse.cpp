#include "discover_logic.h"

#include "gameinfo.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_map>

namespace discover {

/* ---------------------------------------------------------------- text helpers */

std::string fold_text(const std::string& s)
{
    static const char* kLow = "aaaaaaaceeeeiiiidnooooo*ouuuuyts";   // U+00C0..U+00DF
    static const char* kHigh = "aaaaaaaceeeeiiiidnooooo/ouuuuyty";  // U+00E0..U+00FF
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xC3 && i + 1 < s.size()) {
            unsigned char d = (unsigned char)s[i + 1];
            if (d >= 0x80 && d <= 0xBF) {
                o += d < 0xA0 ? kLow[d - 0x80] : kHigh[d - 0xA0];
                ++i;
                continue;
            }
        }
        o += (c < 0x80) ? (char)std::tolower(c) : (char)c;
    }
    return o;
}

static std::string sort_key_of(const std::string& title)
{
    std::string f = fold_text(title);
    size_t b = 0;
    while (b < f.size() && !std::isalnum((unsigned char)f[b]) && (unsigned char)f[b] < 0x80) ++b;
    f = f.substr(b);
    for (const char* art : {"the ", "an ", "a "}) {
        size_t n = std::char_traits<char>::length(art);
        if (f.compare(0, n, art) == 0) { f = f.substr(n); break; }
    }
    return f;
}

static int console_index(const std::string& sys)
{
    const std::vector<std::string>& c = consoles();
    for (size_t i = 0; i < c.size(); ++i) if (c[i] == sys) return (int)i;
    return 99;
}

static void fill_derived(Rec* r)
{
    r->cidx = console_index(r->sys);
    r->key = hubgp::pack_key(r->title);
    r->skey = sort_key_of(r->title);
}

void prepare(std::vector<Rec>* recs)
{
    std::map<std::string, int> n;
    for (Rec& r : *recs) {
        r.curated = true;
        r.rank = n[r.sys]++;
        fill_derived(&r);
        r.hay = fold_text(r.title);
    }
}

std::string genre_bucket(const std::string& g)
{
    std::string f = fold_text(g);
    size_t cut = f.find('/');
    if (cut != std::string::npos) f.resize(cut);
    while (!f.empty() && f.back() == ' ') f.pop_back();
    size_t b = f.find_first_not_of(' ');
    f = b == std::string::npos ? std::string() : f.substr(b);
    if (f.empty()) return "Other";
    static const std::map<std::string, std::string> m = {
        {"action", "Action"}, {"beat'em up", "Action"}, {"platform", "Platformer"}, {"platformer", "Platformer"},
        {"role-playing (rpg)", "RPG"}, {"rpg", "RPG"},
        {"shooter", "Shooter"}, {"shoot'em up", "Shooter"}, {"lightgun shooter", "Shooter"}, {"gun", "Shooter"},
        {"sports", "Sports"}, {"basketball", "Sports"}, {"football", "Sports"}, {"tennis", "Sports"}, {"boxing", "Sports"},
        {"wrestling", "Sports"}, {"sports with animals", "Sports"}, {"hunting and fishing", "Sports"},
        {"racing", "Racing"}, {"adventure", "Adventure"}, {"survival horror", "Adventure"}, {"strategy", "Strategy"},
        {"puzzle", "Puzzle"}, {"breakout", "Puzzle"}, {"thinking", "Puzzle"}, {"pinball", "Puzzle"},
        {"fighting", "Fighting"}, {"simulation", "Simulation"},
        {"party", "Party"}, {"mini games", "Party"}, {"quiz", "Party"}, {"casual game", "Party"}, {"various", "Party"},
        {"board", "Board & Card"}, {"card", "Board & Card"}, {"gambling", "Board & Card"}, {"chess", "Board & Card"},
        {"dancing", "Rhythm"}, {"music", "Rhythm"}, {"sing", "Rhythm"}, {"rhythm", "Rhythm"},
        {"educational", "Educational"}, {"compilation", "Compilation"},
    };
    auto it = m.find(f);
    return it == m.end() ? "Other" : it->second;
}

const std::vector<std::string>& catalog_consoles()
{
    static const std::vector<std::string> c = {"nes", "snes", "n64", "gb", "gbc", "gba", "nds", "ps1", "ps2"};
    return c;
}

bool catalog_covers(const std::string& sys)
{
    for (const std::string& c : catalog_consoles()) if (c == sys) return true;
    return false;
}

std::vector<Rec> catalog_from(const gameinfo::Db& db, const std::vector<Rec>& curated)
{
    std::map<std::string, std::set<std::string>> cur;   // sys -> title keys already curated
    for (const Rec& r : curated) cur[r.sys].insert(r.key.empty() ? hubgp::pack_key(r.title) : r.key);
    std::vector<Rec> out;
    std::unordered_map<std::string, size_t> seen;
    for (const gameinfo::Db::Row& row : db.rows()) {
        if (!catalog_covers(row.sys) || row.title.empty()) continue;
        Rec r;
        r.title = row.title;
        r.sys = row.sys;
        r.key = hubgp::pack_key(row.title);
        if (r.key.empty()) r.key = fold_text(row.title);
        bool dup = false;
        for (const std::string& c : hubgp::pack_consoles(row.sys)) {
            auto it = cur.find(c);
            if (it != cur.end() && it->second.count(r.key)) { dup = true; break; }
        }
        if (dup) continue;
        r.genre = genre_bucket(row.genre);
        r.year = row.year;
        r.month = row.month;
        r.players = row.players;
        r.dev = row.dev;
        r.pub = row.pub;
        int sum = 0, n = 0;
        if (row.famitsu > 0) { sum += row.famitsu * 100 / 40; ++n; }
        if (row.edge > 0) { sum += row.edge * 100 / 10; ++n; }
        r.score = n ? sum / n : 0;
        r.curated = false;
        const std::string id = row.sys + "|" + r.key;
        auto s = seen.find(id);
        if (s != seen.end()) {   // the same game listed twice: keep the first, fill what it lacks
            Rec& k = out[s->second];
            if (!k.year) { k.year = r.year; k.month = r.month; }
            if (!k.players) k.players = r.players;
            if (k.dev.empty()) k.dev = r.dev;
            if (k.pub.empty()) k.pub = r.pub;
            if (k.genre == "Other") k.genre = r.genre;
            k.score = std::max(k.score, r.score);
            continue;
        }
        r.cidx = console_index(r.sys);
        r.skey = sort_key_of(r.title);
        seen[id] = out.size();
        out.push_back(std::move(r));
    }
    for (Rec& r : out) r.hay = fold_text(r.title + " " + r.dev + " " + r.pub);
    return out;
}

/* ---------------------------------------------------------------- sort / filter plumbing */

const char* sort_key(Sort s)
{
    switch (s) {
        case Sort::Title: return "title";
        case Sort::Newest: return "new";
        case Sort::Oldest: return "old";
        case Sort::Players: return "players";
        default: return "rec";
    }
}
Sort sort_from_key(const std::string& k)
{
    if (k == "title") return Sort::Title;
    if (k == "new") return Sort::Newest;
    if (k == "old") return Sort::Oldest;
    if (k == "players") return Sort::Players;
    return Sort::Recommended;
}
const char* sort_label(Sort s)
{
    switch (s) {
        case Sort::Title: return "Title A-Z";
        case Sort::Newest: return "Newest";
        case Sort::Oldest: return "Oldest";
        case Sort::Players: return "Most players";
        default: return "Recommended";
    }
}

bool filters_active(const Filter& f)
{
    return !f.genres.empty() || f.min_players || f.decade || f.friends || f.status != StatusFilter::Any || !f.query.empty();
}
void clear_filters(Filter* f)
{
    f->genres.clear();
    f->min_players = 0;
    f->decade = 0;
    f->friends = false;
    f->status = StatusFilter::Any;
    f->query.clear();
}
std::string filter_sig(const Filter& f)
{
    std::string s = f.sys + (f.catalog ? "|C|" : "|R|");
    for (const std::string& g : f.genres) s += g + ",";
    s += "|" + std::to_string(f.min_players) + "|" + std::to_string(f.decade) + "|" + (f.friends ? "F" : "-") + "|" +
         std::to_string((int)f.status) + "|" + std::to_string((int)f.sort) + "|" + f.query;
    return s;
}
std::string showing_text(size_t visible, size_t total)
{
    char b[64];
    std::snprintf(b, sizeof b, "Showing %zu of %zu", visible, total);
    return b;
}

/* ---------------------------------------------------------------- Classifier */

static std::vector<std::string> split_words(const std::string& k)
{
    std::vector<std::string> w;
    std::string c;
    for (char ch : k) {
        if (ch == ' ') { if (!c.empty()) w.push_back(c); c.clear(); }
        else c += ch;
    }
    if (!c.empty()) w.push_back(c);
    return w;
}

Classifier::Classifier(std::vector<hubui::fg::Owned> owned, std::vector<hubgp::PackGame> pack) : owned_(std::move(owned)), pack_(std::move(pack)) {}

const Classifier::Group& Classifier::group(const std::string& sys) const
{
    auto it = groups_.find(sys);
    if (it != groups_.end()) return it->second;
    Group& g = groups_[sys];
    const std::vector<std::string> cons = hubgp::pack_consoles(sys);
    auto in = [&](const std::string& s) { return std::find(cons.begin(), cons.end(), s) != cons.end(); };
    for (size_t i = 0; i < owned_.size(); ++i) {
        if (!in(owned_[i].sys)) continue;
        int idx = (int)g.okeys.size();
        g.okeys.push_back(hubgp::pack_key(owned_[i].title));
        g.osrc.push_back(i);
        for (const std::string& w : split_words(g.okeys.back())) g.owords[w].push_back(idx);
    }
    for (size_t i = 0; i < pack_.size(); ++i) {
        if (!in(pack_[i].sys)) continue;
        int idx = (int)g.pkeys.size();
        g.pkeys.push_back(pack_[i].key);
        g.psrc.push_back(i);
        for (const std::string& w : split_words(g.pkeys.back())) g.pwords[w].push_back(idx);
    }
    return g;
}

/* pack_pick over only the keys that share a word with `want`: a loose match always shares one, so the answer is the same. */
static int pick_indexed(const std::vector<std::string>& keys, const std::unordered_map<std::string, std::vector<int>>& words, const std::string& want)
{
    if (want.empty()) return -1;
    std::vector<int> cand;
    for (const std::string& w : split_words(want)) {
        auto it = words.find(w);
        if (it != words.end()) cand.insert(cand.end(), it->second.begin(), it->second.end());
    }
    if (cand.empty()) return -1;
    std::sort(cand.begin(), cand.end());
    cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
    std::vector<std::string> sub;
    sub.reserve(cand.size());
    for (int c : cand) sub.push_back(keys[(size_t)c]);
    int i = hubgp::pack_pick(sub, want);
    return i < 0 ? -1 : cand[(size_t)i];
}

Match Classifier::classify(const Rec& r) const
{
    Match m;
    if (!r.cid.empty()) {
        for (const auto& o : owned_)
            if (!o.cid.empty() && (o.cid == r.cid || (!r.also.empty() && o.cid == r.also))) { m.st = Status::Owned; m.key = o.key; return m; }
    }
    const std::string want = r.key.empty() ? hubgp::pack_key(r.title) : r.key;
    const Group& g = group(r.sys);
    int i = pick_indexed(g.okeys, g.owords, want);
    if (i >= 0) { m.st = Status::Owned; m.key = owned_[g.osrc[(size_t)i]].key; return m; }
    i = pick_indexed(g.pkeys, g.pwords, want);
    if (i >= 0) {
        const hubgp::PackGame& p = pack_[g.psrc[(size_t)i]];
        m.st = Status::InPack;
        m.pack_sys = p.sys;
        m.pack_title = p.title;
        m.pack_hint = p.hint;
    }
    return m;
}

/* ---------------------------------------------------------------- Browser */

void Browser::set_curated(std::vector<Rec> recs)
{
    prepare(&recs);
    items_ = std::move(recs);
    ncur_ = items_.size();
    memo_.assign(items_.size(), Match());
    known_.assign(items_.size(), 0);
}

void Browser::set_catalog(std::vector<Rec> cat)
{
    items_.resize(ncur_);
    for (Rec& r : cat) items_.push_back(std::move(r));
    memo_.resize(items_.size());
    known_.resize(items_.size(), 0);
}

void Browser::set_sources(std::vector<hubui::fg::Owned> owned, std::vector<hubgp::PackGame> pack)
{
    cls_ = Classifier(std::move(owned), std::move(pack));
    std::fill(known_.begin(), known_.end(), (char)0);
}

const Match& Browser::match(int i)
{
    if (!known_[(size_t)i]) {
        memo_[(size_t)i] = cls_.classify(items_[(size_t)i]);
        known_[(size_t)i] = 1;
    }
    return memo_[(size_t)i];
}

bool Browser::passes(const Rec& r, const Filter& f, const std::vector<std::string>& words, bool skip_genre) const
{
    if (f.min_players && r.players < f.min_players) return false;
    if (f.decade && (r.year < f.decade || r.year >= f.decade + 10)) return false;
    if (f.friends && !f.catalog && !r.friends) return false;
    if (!skip_genre && !f.genres.empty() && !f.genres.count(r.genre)) return false;
    for (const std::string& w : words)
        if (r.hay.find(w) == std::string::npos) return false;
    return true;
}

size_t Browser::universe_size(bool catalog, const std::string& sys) const
{
    size_t lo = catalog ? ncur_ : 0, hi = catalog ? items_.size() : ncur_, n = 0;
    for (size_t i = lo; i < hi; ++i) if (sys.empty() || items_[i].sys == sys) ++n;
    return n;
}

Browser::Result Browser::query(const Filter& f, double budget_ms)
{
    Result res;
    const size_t lo = f.catalog ? ncur_ : 0, hi = f.catalog ? items_.size() : ncur_;
    const std::vector<std::string> words = split_words(fold_text(f.query));
    const auto t0 = std::chrono::steady_clock::now();
    bool out_of_time = false;
    std::vector<int> base;
    for (size_t i = lo; i < hi; ++i) {
        const Rec& r = items_[i];
        if (!passes(r, f, words, false)) continue;
        if (f.status != StatusFilter::Any) {
            if (!known_[i]) {
                if (!out_of_time && budget_ms >= 0 && (i & 31) == 0)
                    out_of_time = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > budget_ms;
                if (out_of_time) { ++res.pending; continue; }
                match((int)i);
            }
            const Status st = memo_[i].st;
            bool ok = f.status == StatusFilter::NotOwned ? st != Status::Owned : f.status == StatusFilter::InPack ? st == Status::InPack : st == Status::Owned;
            if (!ok) continue;
        }
        base.push_back((int)i);
    }
    const Sort so = f.sort;
    auto rec_less = [&](int a, int b) {
        const Rec &x = items_[(size_t)a], &y = items_[(size_t)b];
        if (x.curated != y.curated) return x.curated;
        if (x.curated) {
            if (x.rank != y.rank) return x.rank < y.rank;
            if (x.cidx != y.cidx) return x.cidx < y.cidx;
        } else {
            if (x.score != y.score) return x.score > y.score;
            if (x.skey != y.skey) return x.skey < y.skey;
        }
        return a < b;
    };
    std::sort(base.begin(), base.end(), [&](int a, int b) {
        const Rec &x = items_[(size_t)a], &y = items_[(size_t)b];
        switch (so) {
            case Sort::Title:
                if (x.skey != y.skey) return x.skey < y.skey;
                if (x.title != y.title) return x.title < y.title;
                return a < b;
            case Sort::Newest:
            case Sort::Oldest: {
                const bool up = so == Sort::Oldest;
                if ((x.year == 0) != (y.year == 0)) return y.year == 0;   // unknown years last either way
                if (x.year != y.year) return up ? x.year < y.year : x.year > y.year;
                if (x.month != y.month) return up ? x.month < y.month : x.month > y.month;
                return rec_less(a, b);
            }
            case Sort::Players:
                if (x.players != y.players) return x.players > y.players;
                return rec_less(a, b);
            default:
                return rec_less(a, b);
        }
    });
    for (int id : base) {
        ++res.by_console[items_[(size_t)id].sys];
        ++res.total_all;
        if (f.sys.empty() || items_[(size_t)id].sys == f.sys) res.ids.push_back(id);
    }
    return res;
}

std::vector<std::pair<std::string, int>> Browser::genre_counts(const Filter& f) const
{
    const size_t lo = f.catalog ? ncur_ : 0, hi = f.catalog ? items_.size() : ncur_;
    const std::vector<std::string> words = split_words(fold_text(f.query));
    std::map<std::string, int> n;
    for (size_t i = lo; i < hi; ++i) {
        const Rec& r = items_[i];
        if (!f.sys.empty() && r.sys != f.sys) continue;
        if (passes(r, f, words, true)) ++n[r.genre];
    }
    return std::vector<std::pair<std::string, int>>(n.begin(), n.end());
}

}  // namespace discover
