#pragma once
/* Internals of the cloud saves v2 engine (docs/CLOUD_SAVES_V2.md). Not part of the UI contract (that is cloud_engine.h):
 * pure helpers kept here so the unit tests can reach them. */
#include "cloud_engine.h"

#include <map>
#include <string>
#include <vector>

namespace hub::saves {

/* ---- wire (cloud_wire.cpp): parsers for the frozen server JSON, section 3 of the design ---- */
struct WVer {
    int ver = 0;
    std::string sha, enc = "none", dev, dev_name, cls;
    int64_t size = 0, stored = 0, mtime = 0, ts = 0;
};
struct WFile {
    std::string path;
    int cur = 0;
    std::string cloud = "synced";          // synced | evicted
    std::vector<WVer> vers;                // newest first
    const WVer* current() const;           // the entry whose ver == cur (or the first), null when none
    bool evicted() const { return cloud == "evicted"; }
};
struct WLease { bool present = false; std::string dev, name; int64_t since = 0, until = 0; };
struct WGame {
    std::string gkey, device;
    int64_t played_at = 0, rev = 0;
    bool pin = false;
    WLease lease;
    std::vector<WFile> files;
    const WFile* find(const std::string& path) const;
};
struct WAccount {
    int64_t used = 0, prot_used = 0, cap = 0, prot_cap = 0;
    int devices = 0;
    std::string pool = "ok";
    bool paused = false;
};
struct WManifest { int64_t rev = 0; bool full = false; WAccount account; std::vector<WGame> games; };
bool parse_manifest(const std::string& json, WManifest* out, std::string* err);

struct PlanFile { std::string path, sha, enc = "none", cls; int64_t size = 0, stored = 0, mtime = 0; int base = 0; };
std::string build_plan_body(const std::string& gkey, int64_t played_at, const std::string& mode, const std::vector<PlanFile>& files);
struct PlanAnswer { std::string path, action /* upload|have|same|conflict|skip */, reason; WVer current; bool has_current = false; int ver = 0; /* "same": the version the cloud holds (the Worker sends no `current` for it, F8) */ };
struct PlanReply { std::string ticket; std::vector<PlanAnswer> files; };
bool parse_plan(const std::string& json, PlanReply* out, std::string* err);
/* cloud: "synced" | "conflict" | "skipped" (reason over-cap / too-big-protected) | "missing" (blob not there yet) | "evicted". Only "synced" is an upload. */
struct CommitResult { std::string path, cloud, reason; int ver = 0; bool conflict = false; WVer current; bool has_current = false; };
struct CommitReply { int64_t rev = 0; std::vector<CommitResult> results; std::vector<std::pair<std::string, std::string>> evicted; /* game, path */ };
bool parse_commit(const std::string& json, CommitReply* out, std::string* err);
struct LeaseReply { bool ok = false, held = false; std::string dev, name; int64_t since = 0, until = 0; };
bool parse_lease(const std::string& json, LeaseReply* out);
struct ServerError { std::string code; int64_t retry_after = 0; };
ServerError parse_error(const std::string& json);        // {"error":"code","retryAfter":N}
/* The Worker rejects the WHOLE plan (400 bad-plan) when one path breaks its cleanKey rules (saves_pool.js): more than 8 segments, more than
 * 240 characters, a segment over 120 characters or with \ : * ? " < > | or control characters, a trailing '.' or ' ', a first segment
 * "_identity", a ".hubtmp" suffix. The client leaves such files out of the plan (they stay on this PC) so the rest of the game still syncs.
 * `why` = "too-deep" | "too-long" | "bad-name" when false. */
bool plan_path_ok(const std::string& rel, const char** why = nullptr);
constexpr int kMaxPlanSegments = 8;

/* ---- classification (cloud_classify.cpp) ---- */
const char* class_name(FileClass c);                     // settings|battery|card|state|shot
FileClass class_from_name(const std::string& s, FileClass def = FileClass::Battery);
constexpr int64_t kProtectedCardMax = 512 * 1024;        // classify_path: an unknown layout above this is a (large) card
constexpr int64_t kProtectedBigCardMax = 16 * 1024 * 1024;   // is_protected: a memory card up to the 16 MB blob limit is a save, whatever its size (PS2 Mcd001.ps2 is 8 MB)

/* ---- three-way rule (cloud_engine.cpp), section 4.2 ---- */
struct Side { bool present = false; std::string sha; int64_t size = 0, mtime = 0; };
enum class Act { Nothing, Push, Pull, Adopt, Conflict, LocalOnlyCap };
/* local = file in the vault, base = last agreed state, cloud = manifest current (present=false for none), cloud_evicted = tombstone. */
Act three_way(const Side& local, const Side& base, const Side& cloud, bool cloud_evicted, FileClass cls);

struct BFile;
/* How one base entry is read (F3). A normal entry is the last agreement with the cloud. An entry that the server REFUSED (over the cap,
 * too large) carries the refused local content in `lsha` and leaves the agreement alone. Builds before the fix instead wrote the refused
 * LOCAL sha into the agreement (ver 0, or a stale ver whose cloud sha differs), which made the next pass "pull" the older cloud copy over
 * the newer local save: those legacy shapes are recognised here (cloud_* = the manifest's current version, have_cloud = false when none). */
struct BaseRead { Side base; int ver = 0; std::string refused_sha; bool legacy = false; };
BaseRead read_base(const BFile& bf, bool have_cloud, int cloud_ver, const std::string& cloud_sha);

/* ---- queue + base state (cloud_queue.cpp) ---- */
struct QItem { std::string gkey, reason; int tries = 0; int64_t next_at = 0; };
/* Backoff after `tries` failures: 30 s, 2 min, 10 min, 1 h, then -1 (wait for a trigger). */
int backoff_secs(int tries);
class CloudQueue {
public:
    bool load(const std::string& path);
    bool save() const;
    void set_path(const std::string& p) { path_ = p; }
    /* Adds or refreshes (gkey, reason); keeps tries of an existing item unless reset. */
    void put(const std::string& gkey, const std::string& reason, int64_t next_at, bool reset_tries = false);
    void remove(const std::string& gkey, const std::string& reason);
    void fail(const std::string& gkey, const std::string& reason, int64_t now, int64_t retry_after = 0);   // backoff or park
    void park(const std::string& gkey, const std::string& reason);   // waits for a trigger (next_at = -1); a retry cannot help until something changes
    bool next_due(int64_t now, QItem* out) const;             // earliest item with next_at <= now (parked items have next_at = -1)
    int64_t earliest(int64_t now) const;                      // next_at of the first not-yet-due item, or 0
    void wake_parked(int64_t now, bool only_parked = false);  // trigger (manual / hub start): backing-off items become due; only_parked = just the ones waiting for a trigger
    bool has(const std::string& gkey) const;
    size_t size() const { return items_.size(); }
    const std::vector<QItem>& items() const { return items_; }
private:
    std::string path_;
    std::vector<QItem> items_;
};

struct BFile {
    int ver = 0; int64_t size = 0, mtime = 0; std::string sha;   // the last agreement with the cloud (ver 0 / empty sha = none yet)
    std::string lo;                                              // "cap"|"too-large" local only marker
    std::string lsha;                                            // with lo: sha of the LOCAL content the server refused (not an agreement); the file stays local only while it is unchanged
};
struct BGame {
    std::map<std::string, BFile> files;
    bool lo_user = false, decide_later = false, pinned = false;
    int64_t played = 0, last_synced = 0;
};
struct BaseStore {
    int64_t rev = 0;
    std::string device;
    bool restore = false;           // restore mode: no push until the first full pull finished
    bool migrated = false;
    int64_t cap_toast_day = 0;
    std::map<std::string, BGame> games;
    bool load(const std::string& path);
    bool save(const std::string& path) const;
};


/* ---- engine hooks for the app and tests (not part of the UI contract) ---- */
struct EngineConfig {
    std::string data_dir;            // default hub::data_dir()
    std::string device_name;         // default computer name
    int beat_ms = 5 * 60 * 1000;     // lease heartbeat
    int pull_budget_ms = 15000;      // total pre-launch pull budget
    bool no_threads = false;         // tests: do not start the worker (call cloud_engine_pump())
};
void cloud_engine_configure(const EngineConfig& cfg);            // before cloud_engine_start
void cloud_engine_pump(int max_jobs = 100);                       // runs queued jobs on the calling thread (tests)
bool cloud_engine_idle();                                         // no queued job running or due
void cloud_set_device_name(const std::string& name);
std::string cloud_device_name();
bool cloud_recovery_acknowledged();
void cloud_recovery_set_acknowledged(bool v);
bool cloud_take_recovery_reminder();                              // true once after the first successful push while not acknowledged
void cloud_backup_progress(int* done, int* total);                // initial upload "12 of 41 games"
PrelaunchResult cloud_prelaunch_ex(const GameId& g, SyncMode mode, int timeout_ms, int pull_budget_ms);
std::string cloud_queue_dump();                                      // "gkey:reason:tries:next_at;..." for tests and the log
void cloud_account_changed();                                     // identity.json now holds another account (link code, recovery code, vault adopt): switch, restore mode, pull
bool cloud_adopt_account(const std::string& uid, const std::string& secret, std::string* err);   // THE add/recover-this-account path (recovery code decodes to this)
void cloud_wake_queue();                                          // manual trigger: parked retries run again
void cloud_set_identity(const std::string& uid, const std::string& secret);   // tests / after an identity change; resets restore mode

/* Text for a failed sync pass: status = the HTTP status (0 = no network), code = the server's error code or a local one ("file-locked", "disk-full", "deadline"). */
std::string sync_failure_text(int status, const std::string& code);
/* Crash-safe file replace used for the engine's state files (flush + one write-through MoveFileExW; a failure leaves the old file intact). */
bool cloud_write_atomic(const std::string& path_utf8, const std::string& data);

} // namespace hub::saves
