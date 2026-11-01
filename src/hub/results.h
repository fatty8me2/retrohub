#pragma once
/* Plan 4 shared match results: party mode (P1), brackets (P7) and the friends ladder (P8) all read finished matches here.
 * Pure parsing plus a tiny file watcher; no ImGui, no network.
 *
 * Writers (each writes the rivals JSON v1 of rivals_logic.h, one match per write, replacing the file):
 *   - Smash Classic (BattleShip, SSB64_RESULT_FILE): every finished online VS match; from Plan 4 also local VS matches, with
 *     "local":true, "code":"" and each player's "slot" = controller port 0-3.
 *   - n64run (Smash Remix / Smash 64 ROM, --result-file <path>): on the VS results screen, "source":"n64run" (n64run/match_result.h).
 * Optional fields added by Plan 4 (older readers ignore them): "source" (default "battleship"), "local" (default false).
 *
 * The game keeps running after a match, so the hub reads the file WHILE it runs (Watcher, polled from the frame loop) and once more
 * at exit; every match id counts once. Party mode and brackets use RoundResult; rivalries store the full Match (rivals_ingest_text). */
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include "json.h"
#include "rivals_logic.h"

namespace hubui {
namespace results {

struct RoundResult {
    std::string id, game, source;          // match id (unique per match), game id ("smash64", "remix"...), writer or "vote" / "timer"
    bool local = false;                    // couch match: the numbers in `ranks` are controller ports, not lobby slots
    std::vector<std::vector<int>> ranks;   // slots (or ports) grouped by finishing position, best first; a tie shares a group
};

/* Same order as rivals::build_match: place ascending, then more stocks left, then more KOs, then fewer falls. */
inline std::vector<std::vector<int>> ranks_of(const std::vector<rivals::RawPlayer>& ps) {
    std::vector<rivals::RawPlayer> v = ps;
    auto key = [](const rivals::RawPlayer& p) { return std::make_tuple(p.place, -p.stocks, -p.kos, p.falls); };
    std::stable_sort(v.begin(), v.end(), [&](const rivals::RawPlayer& a, const rivals::RawPlayer& b) { return key(a) < key(b); });
    std::vector<std::vector<int>> out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i == 0 || key(v[i]) != key(v[i - 1])) out.emplace_back();
        out.back().push_back(v[i].slot);
    }
    return out;
}

/* False for anything rivals::parse_result rejects (malformed, fewer than two players). */
inline bool parse_round(const std::string& text, RoundResult* out) {
    rivals::RawResult raw;
    if (!rivals::parse_result(text, &raw)) return false;
    json::Value root;
    if (!json::parse(text, &root) || !root.is_object()) return false;
    RoundResult r;
    r.id = raw.id;
    r.game = raw.game;
    r.source = root.str("source");
    if (r.source.empty()) r.source = "battleship";
    r.local = root.boolean("local", false);
    r.ranks = ranks_of(raw.players);
    if (out) *out = r;
    return true;
}

/* Watches one result file (the running game's). poll() is cheap (one stat) and returns each new match once, by id.
 * Keep one per run: reset(path) at launch, reset("") at exit. */
class Watcher {
  public:
    void reset(const std::string& path) { path_ = path; stamp_.clear(); seen_.clear(); }
    const std::string& path() const { return path_; }
    /* true once per new match: *text = the raw JSON (for rivals_ingest_text), *out = the parsed round. */
    bool poll(std::string* text, RoundResult* out) {
        if (path_.empty()) return false;
        std::error_code ec;
        auto p = std::filesystem::u8path(path_);
        auto t = std::filesystem::last_write_time(p, ec);
        if (ec) return false;
        auto sz = std::filesystem::file_size(p, ec);
        if (ec) return false;
        std::string st = std::to_string((long long)t.time_since_epoch().count()) + ":" + std::to_string((unsigned long long)sz);
        if (st == stamp_) return false;
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;   // being replaced: look again next poll
        std::stringstream ss;
        ss << f.rdbuf();
        RoundResult r;
        if (!parse_round(ss.str(), &r)) return false;   // mid-write: the stamp is not stored, so it is read again
        stamp_ = st;
        if (!seen_.insert(r.id).second) return false;
        if (text) *text = ss.str();
        if (out) *out = r;
        return true;
    }

  private:
    std::string path_, stamp_;
    std::set<std::string> seen_;
};

}  // namespace results
}  // namespace hubui
