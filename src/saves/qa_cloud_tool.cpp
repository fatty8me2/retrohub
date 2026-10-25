/* QA helper for qa\cloud_two_pc.ps1: drives the cloud saves v2 engine for one simulated PC (its own data and vault dirs) against a
 * LOCAL mock/wrangler server. One process = one action sequence; state persists in the dirs between calls. Console only, headless,
 * never prints the account secret.
 *
 *   qa_cloud_tool --data D --vault V --url U --uid UID --secret SEC [--name PC] <cmd> [args]
 *     prelaunch <system> <key> [--readonly]   pull before launch; prints action/offline/pulled/conflicts
 *     session <system> <key> [--resolve c] [--write <rel> <text>]   one play session: pre-launch, conflict choice, write, exit, drain
 *     flush                                   startup pass + drain the queue
 *     resolve <system> <key> <newer|local|cloud|later>
 *     status <system> <key>                   one line with the per-game state
 *     versions <system> <key>                 lists retained cloud versions per file (ver/size/dev)
 *     restore <system> <key> <path> <ver>     restore a previous cloud version, then pull it (prints ok/err)
 *     session extras: --readonly (guest session: SyncMode::ReadOnly) --lease play|take|cancel (other-PC lease choice, default play)
 *                     --writebig <rel> <bytes> (incompressible random file)
 *     hold <system> <key> <readyfile> <stopfile>   a running session: pre-launch (takes the lease), writes readyfile, waits for stopfile (max 120 s), then exits the game
 *     recovery                                prints the length of the recovery code and whether adopt round-trips (never the code)
 */
#include "cloud_internal.h"
#include "friends.h"
#include "savestore.h"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <thread>
#include <cstring>
#include <string>

using namespace hub::saves;

static const char* state_name(CloudState s) {
    switch (s) {
        case CloudState::Unknown: return "Unknown";
        case CloudState::Synced: return "Synced";
        case CloudState::Syncing: return "Syncing";
        case CloudState::Waiting: return "Waiting";
        case CloudState::LocalOnly: return "LocalOnly";
        case CloudState::Conflict: return "Conflict";
        case CloudState::PlayingElsewhere: return "PlayingElsewhere";
        case CloudState::Paused: return "Paused";
        case CloudState::NeedsAttention: return "NeedsAttention";
    }
    return "?";
}

static void print_state(const GameId& g) {
    GameCloudInfo gi;
    if (!cloud_game(g, &gi)) { std::printf("state=none\n"); return; }
    std::printf("state=%s local_only=%d files_synced=%d files_local_only=%d conflicts=%d cloud_bytes=%lld\n", state_name(gi.state), (int)gi.local_only,
                gi.files_synced, gi.files_local_only, (int)gi.conflicts.size(), (long long)gi.cloud_bytes);
}

int main(int argc, char** argv) {
    std::string data, vault, url, uid, secret, name = "qa-pc";
    std::vector<std::string> rest;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&](std::string& out) { if (i + 1 < argc) out = argv[++i]; };
        if (a == "--data") val(data);
        else if (a == "--vault") val(vault);
        else if (a == "--url") val(url);
        else if (a == "--uid") val(uid);
        else if (a == "--secret") val(secret);
        else if (a == "--name") val(name);
        else rest.push_back(a);
    }
    if (data.empty() || vault.empty() || url.empty() || uid.empty() || secret.empty() || rest.empty()) {
        std::fputs("usage: qa_cloud_tool --data D --vault V --url U --uid UID --secret SEC [--name PC] <cmd> ...\n", stderr);
        return 2;
    }
    if (url.rfind("http://127.0.0.1", 0) != 0 && url.rfind("http://localhost", 0) != 0) {
        std::fputs("refusing: the QA tool only talks to a local server\n", stderr);
        return 2;
    }
    set_vault_dir(vault);
    hubnet::Identity id;
    id.user_id = uid; id.secret = secret; id.created = 1;
    hubnet::identity_adopt(data, vault, id);                 // "link this PC to the account"
    EngineConfig ec;
    ec.data_dir = data;
    ec.device_name = name;
    ec.no_threads = true;
    cloud_engine_configure(ec);
    cloud_engine_start(url);

    const std::string cmd = rest[0];
    int rc = 0;
    if (cmd == "prelaunch" && rest.size() >= 3) {
        GameId g{rest[1], rest[2]};
        bool ro = rest.size() > 3 && rest[3] == "--readonly";
        PrelaunchResult r = cloud_prelaunch(g, ro ? SyncMode::ReadOnly : SyncMode::ReadWrite);
        std::printf("action=%s offline=%d pulled=%d conflicts=%d held_by=%s\n",
                    r.action == PrelaunchResult::Proceed ? "proceed" : r.action == PrelaunchResult::NeedConflictDecision ? "conflict" : "lease",
                    (int)r.offline, r.files_pulled, (int)r.conflicts.size(), r.held_by.c_str());
        print_state(g);
    } else if (cmd == "session" && rest.size() >= 3) {
        /* one whole play session in one process (the lease and the running state live in the engine): pre-launch, optional
         * conflict decision, "play" (write a file), exit, drain the push queue. */
        GameId g{rest[1], rest[2]};
        std::string resolve, wrel, wtext, lease = "play";
        bool readonly = false;
        std::vector<std::pair<std::string, int>> big;
        for (size_t i = 3; i < rest.size(); i++) {
            if (rest[i] == "--readonly") readonly = true;
            else if (rest[i] == "--lease" && i + 1 < rest.size()) lease = rest[++i];
            else if (rest[i] == "--writebig" && i + 2 < rest.size()) { big.push_back({rest[i + 1], std::atoi(rest[i + 2].c_str())}); i += 2; }
            else
            if (rest[i] == "--resolve" && i + 1 < rest.size()) resolve = rest[++i];
            else if (rest[i] == "--write" && i + 2 < rest.size()) { wrel = rest[i + 1]; wtext = rest[i + 2]; i += 2; }
        }
        const SyncMode sm = readonly ? SyncMode::ReadOnly : SyncMode::ReadWrite;
        PrelaunchResult r = cloud_prelaunch(g, sm);
        std::printf("prelaunch action=%s offline=%d pulled=%d conflicts=%d held_by=%s\n",
                    r.action == PrelaunchResult::Proceed ? "proceed" : r.action == PrelaunchResult::NeedConflictDecision ? "conflict" : "lease",
                    (int)r.offline, r.files_pulled, (int)r.conflicts.size(), r.held_by.c_str());
        if (r.action == PrelaunchResult::NeedLeaseDecision) {
            r = cloud_resolve_lease(g, lease == "take" ? LeaseChoice::TakeOver : lease == "cancel" ? LeaseChoice::Cancel : LeaseChoice::PlayAnyway);
            std::printf("lease_choice=%s -> action=%s\n", lease.c_str(), r.action == PrelaunchResult::Proceed ? "proceed" : "other");
            if (lease == "cancel") { print_state(g); cloud_engine_stop(); return 0; }
        }
        if (r.action == PrelaunchResult::NeedConflictDecision && !resolve.empty()) {
            cloud_resolve_conflict(g, resolve == "local" ? ConflictChoice::UseLocal : resolve == "cloud" ? ConflictChoice::UseCloud :
                                      resolve == "later" ? ConflictChoice::DecideLater : ConflictChoice::UseNewer);
            std::printf("resolved=%s\n", resolve.c_str());
        }
        cloud_game_started(g, sm);
        for (auto& b : big) {
            std::string data(b.second, 0); unsigned x = 2463534242u ^ (unsigned)b.second;
            for (auto& c : data) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; c = (char)x; }
            std::string path = game_dir(g.system, g.key) + b.first; std::filesystem::create_directories(std::filesystem::path(path).parent_path());
            FILE* f = std::fopen(path.c_str(), "wb"); if (f) { std::fwrite(data.data(), 1, data.size(), f); std::fclose(f); }
        }
        if (!wrel.empty()) {
            std::string path = game_dir(g.system, g.key) + wrel;
            std::filesystem::create_directories(std::filesystem::path(path).parent_path());
            FILE* f = std::fopen(path.c_str(), "wb");
            if (f) { std::fwrite(wtext.data(), 1, wtext.size(), f); std::fclose(f); }
        }
        touch_game(g.system, g.key, g.key);
        cloud_game_exited(g);
        cloud_engine_pump(500);
        print_state(g);
    } else if (cmd == "hold" && rest.size() >= 5) {
        GameId g{rest[1], rest[2]};
        PrelaunchResult r = cloud_prelaunch(g, SyncMode::ReadWrite);
        if (r.action == PrelaunchResult::NeedLeaseDecision) r = cloud_resolve_lease(g, LeaseChoice::PlayAnyway);
        cloud_game_started(g, SyncMode::ReadWrite);
        std::printf("hold action=%s\n", r.action == PrelaunchResult::Proceed ? "proceed" : "other");
        std::fflush(stdout);
        if (FILE* f = std::fopen(rest[3].c_str(), "wb")) { std::fputs("ready", f); std::fclose(f); }
        for (int i = 0; i < 1200 && !std::filesystem::exists(rest[4]); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        touch_game(g.system, g.key, g.key);
        cloud_game_exited(g);
        cloud_engine_pump(500);
        print_state(g);
    } else if (cmd == "versions" && rest.size() >= 3) {
        for (auto& pr : cloud_versions(GameId{rest[1], rest[2]})) {
            std::printf("file=%s", pr.first.c_str());
            for (auto& v : pr.second) std::printf(" [ver=%d size=%lld dev=%s]", v.ver, (long long)v.size, v.device_name.c_str());
            std::printf("\n");
        }
    } else if (cmd == "restore" && rest.size() >= 5) {
        std::string err;
        bool ok = cloud_restore_version(GameId{rest[1], rest[2]}, rest[3], std::atoi(rest[4].c_str()), &err);
        std::printf("restore ok=%d err=%s\n", (int)ok, err.c_str());
        cloud_engine_pump(500);
        print_state(GameId{rest[1], rest[2]});
    } else if (cmd == "flush") {
        cloud_wake_queue();                       // "Sync now": retries that were backing off run again
        cloud_engine_pump(500);
        CloudAccountInfo a = cloud_account();
        std::printf("online=%d queued=%d used=%lld devices=%d queue=%s\n", (int)a.online, a.queued_games, (long long)a.used, a.devices, cloud_queue_dump().c_str());
    } else if (cmd == "resolve" && rest.size() >= 4) {
        GameId g{rest[1], rest[2]};
        const std::string& c = rest[3];
        cloud_resolve_conflict(g, c == "local" ? ConflictChoice::UseLocal : c == "cloud" ? ConflictChoice::UseCloud :
                                  c == "later" ? ConflictChoice::DecideLater : ConflictChoice::UseNewer);
        cloud_engine_pump(500);
        print_state(g);
    } else if (cmd == "status" && rest.size() >= 3) {
        print_state(GameId{rest[1], rest[2]});
    } else if (cmd == "recovery") {
        /* the ONE account code: make it on the server, then redeem it in a second (empty) data dir via cloud_adopt_recovery_code */
        std::string code, err;
        bool made = cloud_create_account_code(&code, &err);
        std::printf("recovery made=%d code_len=%d err=%s\n", (int)made, (int)code.size(), err.c_str());
        if (made) std::printf("account_code=%s\n", code.c_str());
    } else if (cmd == "adopt" && rest.size() >= 2) {
        std::string err;
        bool ok = cloud_adopt_recovery_code(rest[1], &err);
        std::printf("adopt ok=%d err=%s\n", (int)ok, err.c_str());
    } else {
        std::fputs("unknown command\n", stderr);
        rc = 2;
    }
    cloud_engine_stop();
    return rc;
}
