/* Brackets pure logic (Plan 4 P7). Compiled into hublogic; see bracket_logic.h. */
#include "bracket_logic.h"

#include <algorithm>
#include <random>

#include "json.h"

namespace hubui {
namespace bracket {

int bracket_size(int n) { return n < 2 || n > kMaxEntrants ? 0 : n <= 4 ? 4 : n <= 8 ? 8 : 16; }

std::vector<int> seed_positions(int size) {
    std::vector<int> o = {0, 1};
    while ((int)o.size() < size) {
        std::vector<int> nx;
        int m = (int)o.size() * 2;
        for (int s : o) { nx.push_back(s); nx.push_back(m - 1 - s); }
        o = nx;
    }
    o.resize((size_t)size);
    return o;
}

std::vector<Entrant> seed_entrants(std::vector<Entrant> in, Seeding mode, uint32_t seed) {
    if (mode == Seeding::Rating)
        std::stable_sort(in.begin(), in.end(), [](const Entrant& a, const Entrant& b) { return a.rating > b.rating; });
    else if (mode == Seeding::Random) {
        std::mt19937 g(seed);   // own Fisher-Yates: std::shuffle's result differs between standard libraries
        for (size_t i = in.size(); i > 1; --i) std::swap(in[i - 1], in[g() % i]);
    }
    return in;
}

int winner_of_result(const results::RoundResult& r, int sa, int sb) {
    int ga = -1, gb = -1;
    for (size_t g = 0; g < r.ranks.size(); ++g)
        for (int s : r.ranks[g]) { if (s == sa) ga = (int)g; if (s == sb) gb = (int)g; }
    if (ga < 0 || gb < 0 || ga == gb) return -1;
    return ga < gb ? 0 : 1;
}

bool Bracket::init(Format f, bool reset, const std::vector<Entrant>& seeded) {
    size_ = bracket_size((int)seeded.size());
    seats_.clear(); nodes_.clear(); st_.clear(); reps_.clear();
    if (!size_) return false;
    fmt_ = f; reset_ = reset; rev_ = 1;
    for (auto e : seeded) { if (e.name.size() > kMaxName) e.name.resize(kMaxName); seats_.push_back(e); }
    build();
    recompute();
    return true;
}

void Bracket::build() {
    nodes_.clear();
    auto add = [&](char br, int round, int idx, Src a, Src b) {
        Node n; n.id = (int)nodes_.size(); n.br = br; n.round = round; n.idx = idx; n.a = a; n.b = b;
        nodes_.push_back(n);
        return n.id;
    };
    auto win = [](int m) { Src s; s.k = Src::Win; s.v = m; return s; };
    auto lose = [](int m) { Src s; s.k = Src::Lose; s.v = m; return s; };
    auto seat = [](int v) { Src s; s.k = Src::Seat; s.v = v; return s; };
    int k = 0;
    while ((1 << k) < size_) ++k;
    std::vector<std::vector<int>> wb(k + 2), lb(2 * k + 2);
    auto make_wb = [&](int r) {
        int cnt = size_ >> r;
        for (int i = 0; i < cnt; ++i) {
            if (r == 1) { auto pos = seed_positions(size_); wb[1].push_back(add('W', 1, i, seat(pos[2 * i]), seat(pos[2 * i + 1]))); }
            else wb[r].push_back(add('W', r, i, win(wb[r - 1][2 * i]), win(wb[r - 1][2 * i + 1])));
        }
    };
    make_wb(1);
    if (fmt_ == Format::Single) {
        for (int r = 2; r <= k; ++r) make_wb(r);
        return;
    }
    for (int j = 1; j <= k - 1; ++j) {
        int r = 2 * j - 1;
        int cnt = size_ >> (j + 1);
        for (int i = 0; i < cnt; ++i) {
            if (j == 1) lb[r].push_back(add('L', r, i, lose(wb[1][2 * i]), lose(wb[1][2 * i + 1])));
            else lb[r].push_back(add('L', r, i, win(lb[r - 1][2 * i]), win(lb[r - 1][2 * i + 1])));
        }
        make_wb(j + 1);
        r = 2 * j;
        for (int i = 0; i < cnt; ++i) lb[r].push_back(add('L', r, i, win(lb[r - 1][i]), lose(wb[j + 1][cnt - 1 - i])));
    }
    int gf1 = add('G', 1, 0, win(wb[k][0]), win(lb[2 * k - 2][0]));
    Src sa; sa.k = Src::SideA; sa.v = gf1;
    Src sb; sb.k = Src::SideB; sb.v = gf1;
    add('G', 2, 0, sa, sb);
}

void Bracket::recompute() {
    st_.assign(nodes_.size(), MState());
    size_t used = 0;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const Node& n = nodes_[i];
        MState& m = st_[i];
        auto resolve = [&](const Src& s) {
            Slot o;
            switch (s.k) {
                case Src::Seat: o.known = true; o.seat = s.v < players() ? s.v : -1; break;
                case Src::Win: if (st_[(size_t)s.v].done) { o.known = true; o.seat = st_[(size_t)s.v].winner; } break;
                case Src::Lose: if (st_[(size_t)s.v].done) { o.known = true; o.seat = st_[(size_t)s.v].loser; } break;
                case Src::SideA: if (st_[(size_t)s.v].done) { o.known = true; o.seat = st_[(size_t)s.v].a.seat; } break;
                case Src::SideB: if (st_[(size_t)s.v].done) { o.known = true; o.seat = st_[(size_t)s.v].b.seat; } break;
            }
            return o;
        };
        m.a = resolve(n.a);
        m.b = resolve(n.b);
        if (n.br == 'G' && n.round == 2) {   // reset final: only when the losers-bracket player took the first final
            const MState& g1 = st_[(size_t)n.a.v];
            if (!g1.done) continue;
            if (!reset_ || g1.winner != g1.b.seat || g1.b.seat < 0) { m.done = true; m.skipped = true; m.winner = -1; m.loser = -1; continue; }
        }
        if (!m.a.known || !m.b.known) continue;
        if (m.a.seat < 0 && m.b.seat < 0) { m.done = true; m.bye = true; continue; }
        if (m.a.seat < 0 || m.b.seat < 0) { m.done = true; m.bye = true; m.winner = m.a.seat >= 0 ? m.a.seat : m.b.seat; continue; }
        for (auto& r : reps_) {
            if (r.match != n.id) continue;
            if (r.winner != m.a.seat && r.winner != m.b.seat) continue;
            m.done = true; m.winner = r.winner; m.loser = r.winner == m.a.seat ? m.b.seat : m.a.seat;
            ++used;
            break;
        }
    }
    (void)used;
}

bool Bracket::playable(int match) const {
    if (match < 0 || match >= (int)nodes_.size()) return false;
    const MState& m = st_[(size_t)match];
    return !m.done && m.a.known && m.b.known && m.a.seat >= 0 && m.b.seat >= 0;
}
int Bracket::current() const {
    for (size_t i = 0; i < nodes_.size(); ++i) if (playable((int)i)) return (int)i;
    return -1;
}
int Bracket::next() const {
    int c = current();
    if (c < 0) return -1;
    for (size_t i = (size_t)c + 1; i < nodes_.size(); ++i) if (playable((int)i)) return (int)i;
    return -1;
}
bool Bracket::report(int match, int w) {
    if (!playable(match)) return false;
    const MState& m = st_[(size_t)match];
    if (w != m.a.seat && w != m.b.seat) return false;
    reps_.push_back({match, w});
    ++rev_;
    recompute();
    return true;
}
int Bracket::find_match(int x, int y) const {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (!playable((int)i)) continue;
        const MState& m = st_[i];
        if ((m.a.seat == x && m.b.seat == y) || (m.a.seat == y && m.b.seat == x)) return (int)i;
    }
    return -1;
}
bool Bracket::report_result(const results::RoundResult& r, int slot_a, int slot_b) {
    int c = current();
    if (c < 0) return false;
    int w = winner_of_result(r, slot_a, slot_b);
    if (w < 0) return false;
    return report(c, w == 0 ? st_[(size_t)c].a.seat : st_[(size_t)c].b.seat);
}
bool Bracket::undo() {
    if (reps_.empty()) return false;
    reps_.pop_back();
    ++rev_;
    recompute();
    return true;
}
int Bracket::champion() const {
    if (nodes_.empty()) return -1;
    if (fmt_ == Format::Single) { const MState& f = st_.back(); return f.done ? f.winner : -1; }
    const MState& g2 = st_.back();
    const MState& g1 = st_[st_.size() - 2];
    if (!g1.done) return -1;
    if (g2.skipped) return g1.winner;
    return g2.done ? g2.winner : -1;
}
std::string Bracket::seat_name(int s) const { return s < 0 || s >= players() ? std::string("BYE") : seats_[(size_t)s].name; }
std::string Bracket::match_label(int id) const {
    if (id < 0 || id >= (int)nodes_.size()) return "";
    const Node& n = nodes_[(size_t)id];
    if (n.br == 'G') return n.round == 1 ? "Grand final" : "Grand final reset";
    if (fmt_ == Format::Single) {
        int rounds = 0; while ((1 << rounds) < size_) ++rounds;
        int left = rounds - n.round;
        if (left == 0) return "Final";
        if (left == 1) return "Semifinal";
        return "Round " + std::to_string(n.round);
    }
    return std::string(n.br == 'W' ? "Winners R" : "Losers R") + std::to_string(n.round);
}

std::string Bracket::to_json() const {
    json::Value o = json::make_object();
    o.o.push_back({"kind", json::make_string("bk_state")});
    o.o.push_back({"rev", json::make_int(rev_)});
    o.o.push_back({"fmt", json::make_string(fmt_ == Format::Double ? "d" : "s")});
    o.o.push_back({"rs", json::make_bool(reset_)});
    json::Value sa = json::make_array();
    for (auto& s : seats_) {
        json::Value e = json::make_object();
        e.o.push_back({"n", json::make_string(s.name)});
        if (!s.uid.empty()) e.o.push_back({"u", json::make_string(s.uid)});
        e.o.push_back({"r", json::make_int(s.rating)});
        sa.a.push_back(e);
    }
    o.o.push_back({"seats", sa});
    json::Value ra = json::make_array();
    for (auto& r : reps_) { ra.a.push_back(json::make_int(r.match)); ra.a.push_back(json::make_int(r.winner)); }
    o.o.push_back({"rep", ra});
    std::string s = json::stringify(o);
    return s.size() > kMaxJson ? std::string() : s;
}

bool Bracket::from_json(const std::string& text, Bracket* out) {
    json::Value root;
    if (!json::parse(text, &root) || !root.is_object() || root.str("kind") != "bk_state") return false;
    const json::Value* seats = root.get("seats");
    const json::Value* rep = root.get("rep");
    if (!seats || !seats->is_array() || !rep || !rep->is_array() || rep->a.size() % 2) return false;
    std::vector<Entrant> es;
    for (auto& s : seats->a) {
        if (!s.is_object()) return false;
        Entrant e; e.name = s.str("n"); e.uid = s.str("u"); e.rating = (int)s.i64("r", 1000);
        if (e.name.empty()) return false;
        es.push_back(e);
    }
    Bracket b;
    if (!b.init(root.str("fmt") == "d" ? Format::Double : Format::Single, root.boolean("rs", true), es)) return false;
    for (size_t i = 0; i + 1 < rep->a.size(); i += 2) {
        if (!rep->a[i].is_number() || !rep->a[i + 1].is_number()) return false;
        if (!b.report((int)rep->a[i].i, (int)rep->a[i + 1].i)) return false;   // a corrupt history is refused whole
    }
    b.rev_ = (int)root.i64("rev", 1);
    if (out) *out = b;
    return true;
}

bool Bracket::accept_remote(int from_slot, const std::string& text) {
    if (from_slot != 0) return false;
    Bracket nb;
    if (!from_json(text, &nb)) return false;
    if (valid() && nb.rev_ <= rev_) return false;
    *this = nb;
    return true;
}

}  // namespace bracket
}  // namespace hubui
