#include "discover_logic.h"

#include "games.h"
#include "json.h"

#include <fstream>
#include <sstream>

namespace discover {

bool parse(const std::string& text, std::vector<Rec>* out, int* bad)
{
    if (out) out->clear();
    if (bad) *bad = 0;
    json::Value root;
    if (!json::parse(text, &root) || !root.is_object()) return false;
    const json::Value* games = root.get("games");
    if (!games || !games->is_array()) return false;
    for (const json::Value& g : games->a) {
        Rec r;
        r.title = g.str("title");
        r.sys = g.str("console");
        r.genre = g.str("genre");
        r.why = g.str("why");
        r.cid = g.str("id");
        r.also = g.str("also");
        r.year = (int)g.i64("year");
        r.players = (int)g.i64("players", 1);
        r.friends = g.boolean("friends");
        if (r.title.empty() || r.sys.empty() || r.genre.empty() || r.why.empty() || r.year <= 0 || r.players < 1) {
            if (bad) ++*bad;
            continue;
        }
        if (out) out->push_back(std::move(r));
    }
    return true;
}

const std::vector<Rec>& all()
{
    static const std::vector<Rec> recs = [] {
        std::vector<Rec> v;
        std::ifstream f(hub::exe_dir() + "data/recommended.json", std::ios::binary);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            parse(ss.str(), &v);
        }
        return v;
    }();
    return recs;
}

const std::vector<std::string>& consoles()
{
    static const std::vector<std::string> c = {"nes", "snes", "n64", "gb", "gbc", "gba", "nds", "3ds", "gc", "wiiu", "ps1", "ps2", "psp"};
    return c;
}

std::vector<const Rec*> for_console(const std::vector<Rec>& recs, const std::string& sys, bool friends_only)
{
    std::vector<const Rec*> v;
    for (const Rec& r : recs)
        if (r.sys == sys && (!friends_only || r.friends)) v.push_back(&r);
    return v;
}

static bool sys_in(const std::vector<std::string>& list, const std::string& s)
{
    for (const std::string& x : list) if (x == s) return true;
    return false;
}

Match classify(const Rec& r, const std::vector<hubui::fg::Owned>& owned, const std::vector<hubgp::PackGame>& pack)
{
    Match m;
    const std::vector<std::string> consoles = hubgp::pack_consoles(r.sys);
    if (!r.cid.empty()) {
        for (const auto& o : owned)
            if (!o.cid.empty() && (o.cid == r.cid || (!r.also.empty() && o.cid == r.also))) { m.st = Status::Owned; m.key = o.key; return m; }
    }
    const std::string want = hubgp::pack_key(r.title);
    {
        std::vector<std::string> keys;
        std::vector<const hubui::fg::Owned*> src;
        for (const auto& o : owned) {
            if (!sys_in(consoles, o.sys)) continue;
            keys.push_back(hubgp::pack_key(o.title));
            src.push_back(&o);
        }
        int i = hubgp::pack_pick(keys, want);
        if (i >= 0) { m.st = Status::Owned; m.key = src[(size_t)i]->key; return m; }
    }
    {
        std::vector<std::string> keys;
        std::vector<const hubgp::PackGame*> src;
        for (const auto& p : pack) {
            if (!sys_in(consoles, p.sys)) continue;
            keys.push_back(p.key);
            src.push_back(&p);
        }
        int i = hubgp::pack_pick(keys, want);
        if (i >= 0) {
            m.st = Status::InPack;
            m.pack_sys = src[(size_t)i]->sys;
            m.pack_title = src[(size_t)i]->title;
            m.pack_hint = src[(size_t)i]->hint;
        }
    }
    return m;
}

std::string players_text(int players)
{
    if (players <= 1) return "1 player";
    return "1-" + std::to_string(players) + " players";
}

}  // namespace discover
