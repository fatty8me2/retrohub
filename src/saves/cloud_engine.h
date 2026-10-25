#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace hub::saves {

enum class FileClass { Settings, Battery, Card, State, Shot };
enum class CloudState { Unknown, Synced, Syncing, Waiting, LocalOnly, Conflict, PlayingElsewhere, Paused, NeedsAttention };
enum class LocalOnlyReason { None, OverCap, TooLarge, User, DecideLater };
enum class SyncMode { ReadWrite, ReadOnly };            // ReadOnly = online session using a throwaway save dir

FileClass classify_path(const std::string& rel_in_game, int64_t size);   // pure; unit tested
bool is_protected(FileClass c, int64_t size);                            // settings, battery, card <= 512 KB

struct GameId { std::string system, key; std::string gkey() const { return system + "/" + key; } };

struct VersionInfo { int ver = 0; int64_t size = 0, mtime = 0, ts = 0; std::string device_name; };
struct FileSide { bool present = false; std::string sha; int64_t size = 0, mtime = 0; std::string device_name; };
struct ConflictItem { std::string path; FileClass cls = FileClass::Battery; FileSide local, cloud; };

struct GameCloudInfo {
    GameId id;
    CloudState state = CloudState::Unknown;
    LocalOnlyReason local_only = LocalOnlyReason::None;
    std::string message;                 // short human line for the details panel
    int64_t last_synced = 0;             // unix s, 0 = never
    int64_t cloud_bytes = 0;             // stored bytes in the cloud for this game
    int64_t pending_bytes = 0;           // waiting to upload/download
    int files_synced = 0, files_local_only = 0;
    bool pinned = false;
    std::string playing_on;              // device name when PlayingElsewhere
    std::vector<ConflictItem> conflicts; // non-empty when state == Conflict
};

struct CloudAccountInfo {
    bool known = false;                  // a status reply has been received this session
    bool online = false;
    bool paused_by_server = false;       // budget/maintenance
    bool needs_attention = false;
    std::string attention;               // "account", "identity-missing", ...
    int64_t used = 0, cap = 0, prot_used = 0, prot_cap = 0;
    std::string pool;                    // "ok" | "tight" | "full"
    int devices = 0;
    std::string device_name;
    int queued_games = 0;
};

struct CloudEvent {                      // popped by the UI thread; becomes a toast / banner
    enum Kind { Pushed, Pulled, WentLocalOnly, ConflictFound, ConflictResolved, OfflineStarted, BackOnline,
                CapAlmostFull, NeedsAttention, Restored } kind;
    GameId game;
    std::string text;                    // already user-ready English, e.g. "Cloud space is full: older save states of A, B stay on this PC only."
    bool good = true;
};

/* ---- pre-launch ---- */
struct PrelaunchResult {
    enum Action { Proceed, NeedConflictDecision, NeedLeaseDecision } action = Proceed;
    bool offline = false;                // proceeded without the cloud
    int files_pulled = 0;
    std::vector<ConflictItem> conflicts; // for NeedConflictDecision
    std::string held_by;                 // device name, for NeedLeaseDecision
    int64_t held_since = 0;
};
/* Blocking; call on the job thread. timeout_ms bounds the manifest request; the total pull budget is 15 s. */
PrelaunchResult cloud_prelaunch(const GameId& g, SyncMode mode, int timeout_ms = 4000);
enum class ConflictChoice { UseNewer, UseLocal, UseCloud, DecideLater };
enum class LeaseChoice { Cancel, PlayAnyway, TakeOver };
void cloud_resolve_conflict(const GameId& g, ConflictChoice c);          // applies, may enqueue a push/pull
PrelaunchResult cloud_resolve_lease(const GameId& g, LeaseChoice c);      // continues the prelaunch

/* ---- run + exit ---- */
void cloud_game_started(const GameId& g, SyncMode mode);  // starts the 5 min lease heartbeat (no-op when ReadOnly / single device)
void cloud_game_exited(const GameId& g);                   // touch_game must already have run; releases lease, enqueues push

/* ---- engine lifecycle (called from app startup/shutdown and the job loop) ---- */
void cloud_engine_start(const std::string& base_url);      // loads queue, schedules the one startup delta
void cloud_engine_stop();                                  // joins the worker; queue is already persisted
void cloud_sync_all_now();                                 // "Sync now"
void cloud_set_paused(bool paused);                        // Advanced > Pause cloud sync

/* ---- state for the UI (all thread-safe, cheap, no I/O) ---- */
CloudAccountInfo cloud_account();
std::vector<GameCloudInfo> cloud_games();                  // known games, any order
bool cloud_game(const GameId& g, GameCloudInfo* out);
bool cloud_pop_event(CloudEvent* out);

/* ---- actions ---- */
void cloud_set_pin(const GameId& g, bool pinned);
void cloud_set_local_only(const GameId& g, bool local_only);   // user "don't upload this game"
void cloud_remove_game(const GameId& g);                       // remove from cloud (keeps local)
std::vector<std::pair<std::string, std::vector<VersionInfo>>> cloud_versions(const GameId& g);  // blocking, job thread
bool cloud_restore_version(const GameId& g, const std::string& path, int ver, std::string* err);  // blocking
std::string cloud_recovery_code();                          // the saved account code XXXX-XXXX-XXXX ("" if none was made on this PC)
bool cloud_create_account_code(std::string* code, std::string* err);   // blocking: makes (or replaces) the account code on the server
bool cloud_adopt_recovery_code(const std::string& code, std::string* err);   // blocking: redeem a code (also 8-char link codes), switch account, pull saves

} // namespace hub::saves
