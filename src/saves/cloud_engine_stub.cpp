/* FAKE cloud engine (Lane U placeholder). Implements cloud_engine.h with canned data for headless screenshots, driven by
 * HUB_SHOT_CLOUD2=<scenario>. Without that variable it reports "Unknown" and does nothing (no network, no files).
 * The Client lane's real cloud_engine.cpp replaces this file on merge: delete it and its line in CMakeLists.txt.
 * Scenarios: ok | mixed | offline | over | attention | paused | fresh */
#include "cloud_engine.h"
#include <cstdlib>
#include <ctime>
#include <mutex>

namespace hub::saves {
namespace {

std::mutex g_mu;
bool g_init = false;
CloudAccountInfo g_acct;
std::vector<GameCloudInfo> g_games;
std::vector<CloudEvent> g_events;

const char* scenario() { const char* s = std::getenv("HUB_SHOT_CLOUD2"); return s ? s : ""; }

GameCloudInfo mk(const char* sys, const char* key, CloudState st, int64_t ago_s, int64_t bytes) {
    GameCloudInfo g;
    g.id = {sys, key};
    g.state = st;
    g.last_synced = ago_s < 0 ? 0 : (int64_t)std::time(nullptr) - ago_s;
    g.cloud_bytes = bytes;
    return g;
}

void init_locked() {
    if (g_init) return;
    g_init = true;
    const std::string sc = scenario();
    if (sc.empty()) return;
    g_acct.known = true;
    g_acct.online = sc != "offline";
    g_acct.cap = 50LL << 20;
    g_acct.prot_cap = 20LL << 20;
    g_acct.used = 18LL << 20;
    g_acct.prot_used = 3LL << 20;
    g_acct.pool = "ok";
    g_acct.devices = 2;
    g_acct.device_name = "PC-DESK";
    if (sc == "fresh") { g_acct.used = 0; g_acct.prot_used = 0; g_acct.devices = 1; return; }
    if (sc == "attention") { g_acct.needs_attention = true; g_acct.attention = "account"; }
    if (sc == "paused") g_acct.paused_by_server = true;
    if (sc == "over") { g_acct.used = (int64_t)(49.6 * 1048576); g_acct.pool = "full"; }

    auto A = mk("n64", "smash64", CloudState::Synced, 600, 1 << 20);
    auto B = mk("gba", "pokemon-emerald", CloudState::Syncing, 30, 3 << 20);
    B.message = "Uploading";
    B.pending_bytes = 700000;
    auto C = mk("gba", "metroid-fusion", CloudState::Syncing, 3000, 2 << 20);
    C.message = "Downloading";
    auto D = mk("gc", "melee", CloudState::LocalOnly, 86400, 0);
    D.local_only = LocalOnlyReason::OverCap;
    D.files_local_only = 3;
    D.message = "Save states stay on this PC";
    auto E = mk("n64", "mario-kart-64", CloudState::Conflict, 200000, 2 << 20);
    ConflictItem ci;
    ci.path = "battery/mk64.eep";
    ci.local = {true, "a", 412 * 1024, (int64_t)std::time(nullptr) - 5 * 86400, "PC-DESK"};
    ci.cloud = {true, "b", 410 * 1024, (int64_t)std::time(nullptr) - 3 * 86400, "PC-LIVINGROOM"};
    E.conflicts.push_back(ci);
    auto F = mk("gb", "tetris", CloudState::PlayingElsewhere, 7200, 40000);
    F.playing_on = "PC-LIVINGROOM";
    auto G = mk("gbc", "zelda-oracle", CloudState::Waiting, 90000, 60000);
    auto H = mk("n64", "zelda-oot", CloudState::Synced, 172800, 4 << 20);
    H.pinned = true;
    if (sc == "ok") { g_games = {A, H, mk("gba", "pokemon-emerald", CloudState::Synced, 4000, 3 << 20), F}; g_games[3].state = CloudState::Synced; g_games[3].playing_on.clear(); }
    else if (sc == "offline") { G.state = B.state = CloudState::Waiting; g_games = {A, B, G, H}; g_games[0].state = CloudState::Waiting; }
    else if (sc == "over") { auto D2 = D; D2.id = {"gc", "wind-waker"}; D2.files_local_only = 5; g_games = {A, D, D2, H}; }
    else if (sc == "attention") { for (auto g : {A, H}) { g.state = CloudState::NeedsAttention; g.message = "Sign-in problem"; g_games.push_back(g); } }
    else if (sc == "paused") { for (auto g : {A, H}) { g.state = CloudState::Paused; g_games.push_back(g); } }
    else g_games = {A, B, C, D, E, F, G, H};   // mixed
}

}  // namespace

FileClass classify_path(const std::string&, int64_t) { return FileClass::Battery; }
bool is_protected(FileClass c, int64_t size) { return c != FileClass::State && c != FileClass::Shot && (c != FileClass::Card || size <= 512 * 1024); }

PrelaunchResult cloud_prelaunch(const GameId&, SyncMode, int) { return PrelaunchResult(); }
void cloud_resolve_conflict(const GameId& g, ConflictChoice) {
    std::lock_guard<std::mutex> l(g_mu);
    init_locked();
    for (auto& x : g_games) if (x.id.gkey() == g.gkey()) { x.state = CloudState::Synced; x.conflicts.clear(); }
}
PrelaunchResult cloud_resolve_lease(const GameId&, LeaseChoice) { return PrelaunchResult(); }
void cloud_game_started(const GameId&, SyncMode) {}
void cloud_game_exited(const GameId&) {}
void cloud_engine_start(const std::string&) {}
void cloud_engine_stop() {}
void cloud_sync_all_now() {}
void cloud_set_paused(bool) {}

CloudAccountInfo cloud_account() { std::lock_guard<std::mutex> l(g_mu); init_locked(); return g_acct; }
std::vector<GameCloudInfo> cloud_games() { std::lock_guard<std::mutex> l(g_mu); init_locked(); return g_games; }
bool cloud_game(const GameId& g, GameCloudInfo* out) {
    std::lock_guard<std::mutex> l(g_mu);
    init_locked();
    for (auto& x : g_games) if (x.id.gkey() == g.gkey()) { if (out) *out = x; return true; }
    return false;
}
bool cloud_pop_event(CloudEvent* out) {
    std::lock_guard<std::mutex> l(g_mu);
    init_locked();
    if (g_events.empty()) return false;
    *out = g_events.front();
    g_events.erase(g_events.begin());
    return true;
}

void cloud_set_pin(const GameId& g, bool pinned) {
    std::lock_guard<std::mutex> l(g_mu);
    for (auto& x : g_games) if (x.id.gkey() == g.gkey()) x.pinned = pinned;
}
void cloud_set_local_only(const GameId& g, bool lo) {
    std::lock_guard<std::mutex> l(g_mu);
    for (auto& x : g_games) if (x.id.gkey() == g.gkey()) { x.state = lo ? CloudState::LocalOnly : CloudState::Synced; x.local_only = lo ? LocalOnlyReason::User : LocalOnlyReason::None; }
}
void cloud_remove_game(const GameId& g) {
    std::lock_guard<std::mutex> l(g_mu);
    for (size_t i = 0; i < g_games.size(); ++i) if (g_games[i].id.gkey() == g.gkey()) { g_games.erase(g_games.begin() + i); break; }
}
std::vector<std::pair<std::string, std::vector<VersionInfo>>> cloud_versions(const GameId&) {
    if (!scenario()[0]) return {};
    const int64_t now = (int64_t)std::time(nullptr);
    VersionInfo a{3, 32768, now - 3600, now - 3600, "PC-DESK"}, b{2, 32768, now - 90000, now - 90000, "PC-LIVINGROOM"}, c{1, 30000, now - 400000, now - 400000, "PC-DESK"};
    return {{"battery/save.sra", {a, b, c}}};
}
bool cloud_restore_version(const GameId&, const std::string&, int, std::string* err) { if (err) err->clear(); return true; }
std::string cloud_recovery_code() { return scenario()[0] ? "K7M2-QX9P-3FTA-W8RD-HN4C-ZB6Y-J2VE-5GL7" : ""; }   // FAKE
bool cloud_adopt_recovery_code(const std::string&, std::string* err) { if (err) *err = "Recovery is not available in this build yet."; return false; }

} // namespace hub::saves
