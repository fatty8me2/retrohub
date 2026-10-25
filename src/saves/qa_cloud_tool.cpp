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
 *     recovery                                prints the length of the recovery code and whether adopt round-trips (never the code)
 */
#include "cloud_internal.h"
#include "friends.h"
#include "savestore.h"

#include <cstdio>
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
        std::string resolve, wrel, wtext;
        for (size_t i = 3; i < rest.size(); i++) {
            if (rest[i] == "--resolve" && i + 1 < rest.size()) resolve = rest[++i];
            else if (rest[i] == "--write" && i + 2 < rest.size()) { wrel = rest[i + 1]; wtext = rest[i + 2]; i += 2; }
        }
        PrelaunchResult r = cloud_prelaunch(g, SyncMode::ReadWrite);
        std::printf("prelaunch action=%s offline=%d pulled=%d conflicts=%d held_by=%s\n",
                    r.action == PrelaunchResult::Proceed ? "proceed" : r.action == PrelaunchResult::NeedConflictDecision ? "conflict" : "lease",
                    (int)r.offline, r.files_pulled, (int)r.conflicts.size(), r.held_by.c_str());
        if (r.action == PrelaunchResult::NeedLeaseDecision) r = cloud_resolve_lease(g, LeaseChoice::PlayAnyway);
        if (r.action == PrelaunchResult::NeedConflictDecision && !resolve.empty()) {
            cloud_resolve_conflict(g, resolve == "local" ? ConflictChoice::UseLocal : resolve == "cloud" ? ConflictChoice::UseCloud :
                                      resolve == "later" ? ConflictChoice::DecideLater : ConflictChoice::UseNewer);
            std::printf("resolved=%s\n", resolve.c_str());
        }
        cloud_game_started(g, SyncMode::ReadWrite);
        if (!wrel.empty()) {
            std::string path = game_dir(g.system, g.key) + wrel;
            FILE* f = std::fopen(path.c_str(), "wb");
            if (f) { std::fwrite(wtext.data(), 1, wtext.size(), f); std::fclose(f); }
        }
        touch_game(g.system, g.key, g.key);
        cloud_game_exited(g);
        cloud_engine_pump(500);
        print_state(g);
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
        std::string code = cloud_recovery_code();
        std::string u2, s2, err;
        bool ok = recovery_decode(code, &u2, &s2, &err) && u2 == uid;
        std::printf("recovery_code_len=%d roundtrip=%d\n", (int)code.size(), (int)ok);
    } else {
        std::fputs("unknown command\n", stderr);
        rc = 2;
    }
    cloud_engine_stop();
    return rc;
}
