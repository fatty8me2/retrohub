#include "match_result.h"

#include <algorithm>
#include <cstdio>

namespace n64match {
namespace {

constexpr uint32_t kPhysMask = 0x7FFFFFu;
/* Highest byte read: the sudden-death state's last player block (+ a little slack). */
constexpr size_t kNeeded = (size_t)((kSuddenDeathBlock & kPhysMask) + kPlayersOffset + kPlayers * kPlayerStride + 8);

struct BeReader {
    const uint8_t* p;
    uint8_t u8(uint32_t a) const { return p[a & kPhysMask]; }
};
struct HostReader {
    const uint8_t* p;
    uint8_t u8(uint32_t a) const { return p[(a & kPhysMask) ^ 3u]; }   // mupen64plus keeps RDRAM as host-endian words
};

template <class R> int32_t s32(const R& r, uint32_t a) {
    return (int32_t)(((uint32_t)r.u8(a) << 24) | ((uint32_t)r.u8(a + 1) << 16) | ((uint32_t)r.u8(a + 2) << 8) | (uint32_t)r.u8(a + 3));
}

uint32_t fnv(uint32_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) { h ^= (v >> (8 * i)) & 0xFFu; h *= 16777619u; }
    return h;
}

struct Row { int player; int score; int place; };   // one line of the results screen's ordering (a player, or a team)

/* mnVSResultsOrderResults + the numbering loop of mnVSResultsSetRoyalPlace / SetTeamPlaceAll: points descending; in sudden death equal
 * points are ordered by the sudden-death round's own place. Sorts `rows` in place (best first) and returns each sorted row's place
 * (0 = best, ties share a number). */
std::vector<int> rank_rows(std::vector<Row>& rows, bool sudden) {
    const size_t n = rows.size();
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (rows[i].score < rows[j].score || (sudden && rows[i].score == rows[j].score && rows[j].place < rows[i].place)) std::swap(rows[i], rows[j]);
    std::vector<int> out(n, 0);
    int place = 0;
    int score = n ? rows[0].score : 0, winner = n ? rows[0].place : 0;
    for (size_t i = 0; i < n; i++) {
        if (score != rows[i].score || (sudden && winner != rows[i].place)) { place++; score = rows[i].score; winner = rows[i].place; }
        out[i] = place;
    }
    return out;
}

template <class R> bool read_impl(const R& r, size_t size, std::vector<PlayerResult>* out, MatchInfo* info) {
    if (!out || size < kNeeded) return false;
    MatchInfo mi;
    mi.game_rules = r.u8(kVsBlock + 0x03);
    mi.team = r.u8(kVsBlock + 0x02) != 0;
    mi.stage = r.u8(kVsBlock + 0x01);
    mi.sudden_death = r.u8(kSceneBase + kSceneSuddenDeath) != 0;
    mi.no_contest = r.u8(kSceneBase + kSceneIsReset) != 0;

    std::vector<PlayerResult> v(kPlayers);
    int team_of[kPlayers] = {0, 0, 0, 0};
    int present = 0;
    for (int n = 0; n < kPlayers; n++) {
        const uint32_t b = kVsBlock + kPlayersOffset + (uint32_t)n * kPlayerStride;
        PlayerResult& p = v[n];
        p.port = n;
        const uint8_t pk = r.u8(b + 0x02);
        if (pk == 2) continue;          // nobody on this port
        if (pk > 1) return false;       // not a VS battle's player blocks
        p.present = true;
        p.cpu = pk == 1;
        p.ch = r.u8(b + 0x03);
        team_of[n] = r.u8(b + 0x04);
        p.stocks = (int8_t)r.u8(b + 0x0B);
        p.place = r.u8(b + 0x0D);
        p.falls = s32(r, b + 0x10);
        p.kos = s32(r, b + 0x14);
        if (p.ch > 0x60 || p.stocks < -1 || p.stocks > 99 || p.falls < 0 || p.falls > 100000 || p.kos < -100000 || p.kos > 100000 || team_of[n] > 7) return false;
        present++;
    }
    if (present < 2) return false;

    if (mi.game_rules != 2) {   // time / both: the results screen ranks by points (KOs - falls) itself; the blocks' place is not filled
        auto points = [&](int n) { return v[n].kos - v[n].falls; };
        auto sd_place = [&](int n) { return mi.sudden_death ? (int)r.u8(kSuddenDeathBlock + kPlayersOffset + (uint32_t)n * kPlayerStride + 0x0D) : 0; };
        if (!mi.team) {
            std::vector<Row> rows;
            for (int n = 0; n < kPlayers; n++) if (v[n].present) rows.push_back({n, points(n), sd_place(n)});
            const std::vector<int> pl = rank_rows(rows, mi.sudden_death);
            for (size_t i = 0; i < rows.size(); i++) v[rows[i].player].place = pl[i];
        } else {
            std::vector<Row> rows;       // one row per team; `player` holds the team id
            for (int t = 0; t <= 2; t++) {
                int first = -1, total = 0;
                for (int n = 0; n < kPlayers; n++) if (v[n].present && team_of[n] == t) { if (first < 0) first = n; total += points(n); }
                if (first >= 0) rows.push_back({t, total, sd_place(first)});
            }
            const std::vector<int> pl = rank_rows(rows, mi.sudden_death);
            for (size_t i = 0; i < rows.size(); i++)
                for (int n = 0; n < kPlayers; n++) if (v[n].present && team_of[n] == rows[i].player) v[n].place = pl[i];
        }
    } else {
        for (int n = 0; n < kPlayers; n++) if (v[n].present && v[n].place > 3) return false;
    }

    uint32_t h = 2166136261u;
    h = fnv(h, (uint32_t)mi.game_rules | (uint32_t)mi.stage << 8 | (mi.team ? 1u << 16 : 0u) | (mi.sudden_death ? 1u << 17 : 0u));
    for (int n = 0; n < kPlayers; n++) {
        const PlayerResult& p = v[n];
        h = fnv(h, (uint32_t)p.present | (uint32_t)p.cpu << 1 | (uint32_t)(p.ch & 0xFF) << 8 | (uint32_t)(p.place & 0xFF) << 16 | (uint32_t)(p.stocks & 0xFF) << 24);
        h = fnv(h, (uint32_t)p.kos);
        h = fnv(h, (uint32_t)p.falls);
        if (p.present) {   // what else the finished match left in the block: damage dealt and self-destructs differ between two matches that end with the same score
            const uint32_t b = kVsBlock + kPlayersOffset + (uint32_t)n * kPlayerStride;
            h = fnv(h, (uint32_t)s32(r, b + 0x30));
            h = fnv(h, (uint32_t)s32(r, b + 0x34));
        }
    }
    mi.signature = h;
    *out = v;
    if (info) *info = mi;
    return true;
}

void put_json_string(std::string* o, const std::string& s) {
    o->push_back('"');
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o->push_back('\\'); o->push_back((char)c); }
        else if (c < 0x20 || c >= 0x7F) o->push_back('?');
        else o->push_back((char)c);
    }
    o->push_back('"');
}

}  // namespace

bool read_results(const uint8_t* rdram, size_t size, std::vector<PlayerResult>* out, MatchInfo* info) {
    if (!rdram) return false;
    return read_impl(BeReader{rdram}, size, out, info);
}

bool read_results_host(const uint8_t* rdram, size_t size, std::vector<PlayerResult>* out, MatchInfo* info) {
    if (!rdram) return false;
    return read_impl(HostReader{rdram}, size, out, info);
}

bool Trigger::step(uint8_t scene) {
    bool fire = false;
    if (scene == 0x16) battle_ = true;                                    // a VS battle is running (remix::screen::kVsBattle)
    else if (scene == 0x18 && prev_ != 0x18 && battle_) {                 // its results screen (remix::screen::kVsResults)
        fire = true;
        battle_ = false;
        n_++;
    } else if (scene != 0x18 && scene != 0x16 && prev_ == 0x18) battle_ = false;
    prev_ = scene;
    return fire;
}

std::string match_id(const std::string& room, unsigned counter, uint32_t signature, long long unix_time) {
    char tail[40];
    std::snprintf(tail, sizeof tail, "-m%u-%08x", counter, (unsigned)signature);
    if (room.empty()) return "local-" + std::to_string(unix_time) + tail;
    std::string id;
    for (unsigned char c : room) id.push_back((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ? (char)c : '_');
    return id + tail;
}

std::string result_json(const std::string& game, const std::string& id, const std::string& room_code, bool local,
                        const std::vector<PlayerResult>& players, const std::vector<int>& port_to_slot) {
    std::vector<const PlayerResult*> rep;
    std::vector<int> slots;
    for (const PlayerResult& p : players) {
        if (!p.present || p.cpu || p.port < 0 || p.port >= kPlayers) continue;
        const int slot = local ? p.port : (p.port < (int)port_to_slot.size() ? port_to_slot[(size_t)p.port] : -1);
        if (slot < 0 || slot > 7) continue;
        rep.push_back(&p);
        slots.push_back(slot);
    }
    if (rep.size() < 2) return std::string();
    std::string o = "{\"v\":1,\"game\":";
    put_json_string(&o, game);
    o += ",\"id\":";
    put_json_string(&o, id);
    o += ",\"code\":";
    put_json_string(&o, room_code);
    o += ",\"source\":\"n64run\",\"local\":";
    o += local ? "true" : "false";
    o += ",\"players\":[";
    for (size_t i = 0; i < rep.size(); i++) {
        const PlayerResult& p = *rep[i];
        char b[160];
        std::snprintf(b, sizeof b, "%s{\"slot\":%d,\"char\":%d,\"place\":%d,\"stocks\":%d,\"kos\":%d,\"falls\":%d}", i ? "," : "", slots[i], p.ch, p.place, p.stocks, p.kos, p.falls);
        o += b;
    }
    o += "]}\n";
    return o;
}

}  // namespace n64match
