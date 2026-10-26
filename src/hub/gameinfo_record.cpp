/* Records test fixtures for game info: gameinfo_record <outdir> "<sys>|<title>[|<alt title>]" ...
 * Runs resolve_online() against the LIVE Wikidata/Wikipedia APIs (the only live-network tool; never run by tests or ctest) and saves every
 * response as <outdir>/<fnv1a64(url)>.json (or .404). Wikidata entity responses are pruned to the claims the parser reads, so fixtures stay small. */
#include "gameinfo.h"
#include "gameinfo_net.h"
#include "json.h"

#include <cstdio>
#include <fstream>
#include <set>
#include <string>

static std::string prune(const std::string& url, const std::string& body) {
    if (url.find("wbgetentities") == std::string::npos || url.find("claims") == std::string::npos) return body;
    json::Value v;
    if (!json::parse(body, &v)) return body;
    static const std::set<std::string> keep = {"P31", "P400", "P444", "P852", "P908", "P178", "P123", "P136", "P577"};
    if (json::Value* es = const_cast<json::Value*>(v.get("entities")))
        for (auto& kv : es->o)
            for (auto& m : kv.second.o)
                if (m.first == "claims") {
                    std::vector<std::pair<std::string, json::Value>> k;
                    for (auto& c : m.second.o) if (keep.count(c.first)) k.push_back(c);
                    m.second.o = k;
                }
    return json::stringify(v);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: gameinfo_record <outdir> \"sys|title[|alt]\" ...\n"); return 2; }
    std::string dir = argv[1];
    if (dir.back() != '/' && dir.back() != '\\') dir += '/';
    gameinfo::Fetcher f = [&](const std::string& url, std::string* body, int* st) {
        bool ok = gameinfo_net::fetch(url, body, st);
        if (!ok) return false;
        std::string base = dir + gameinfo_net::fixture_name(url);
        if (*st == 404) { std::ofstream(base + ".404", std::ios::binary); }
        else if (*st == 200) { std::string b = prune(url, *body); std::ofstream(base + ".json", std::ios::binary) << b; }
        return true;
    };
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i], p[3];
        int n = 0;
        size_t s = 0;
        while (n < 3) {
            size_t q = a.find('|', s);
            p[n++] = a.substr(s, q == std::string::npos ? std::string::npos : q - s);
            if (q == std::string::npos) break;
            s = q + 1;
        }
        gameinfo::Query q;
        q.system = p[0]; q.title = p[1]; q.alt_title = p[2];
        gameinfo::Online o;
        auto r = gameinfo::resolve_online(q, f, &o);
        std::printf("%s: %s qid=%s label=%s ratings=%zu esrb=%s pegi=%s dev=%s desc=%.60s\n", a.c_str(),
                    r == gameinfo::ResolveResult::Ok ? "OK" : r == gameinfo::ResolveResult::NotFound ? "NOT FOUND" : "NETWORK ERROR",
                    o.qid.c_str(), o.label.c_str(), o.ratings.size(), o.esrb.c_str(), o.pegi.c_str(), o.developer.c_str(), o.description.c_str());
    }
    return 0;
}
