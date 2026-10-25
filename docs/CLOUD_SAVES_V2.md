# Cloud saves v2: automatic, per-game, capped (design)

Status: design for three parallel lanes (Server, Client engine, UI). Supersedes the opt-in v1 in `docs/CLOUD_SAVES.md`.
Hard rule carried over: everything stays inside Cloudflare's free tier. The global `R2Budget` guard (7 GB stored,
700k Class A/month, 7M Class B/month, 70k DO rows/day) and `REQ_BUDGET` stay, fail closed, and sit above every new route.

## 1. What exists today and its gaps

Today (v1, `src/saves/cloud.*`, `BattleShip/server/cloudflare/src/saves.js`, `r2budget.js`):
- Opt-in checkbox `Settings.cloud_saves` (default false) on the Saves page. `sync_enabled()` is true only if a folder or the checkbox is set.
- Auth: `Authorization: RHSIG uid,ts,sig`, HMAC key = `sha256hex(identity secret)`; the UserHub DO answers `saves_auth` and rate limits (120 writes / 600 reads per minute).
- Objects `saves/<uid>/<vault-relative path>`, 16 MB max, flat quota 200 MB / 1500 files, `If-Match` / `If-None-Match` on PUT, 412 on lost race.
- R2Budget DO holds the file index (list, quota, preconditions need no R2 op) and reserves Class A/B before each R2 call.
- Client = download changed objects into `<data>\cloud_mirror\<uid8>`, run `sync_folder(mirror)` (vault <-> mirror), upload changed mirror files.
- Sync runs on the job thread: at hub start (`auto_sync`), after a game exits (`run_pending_sync`, gated by `hubui::sync_request` so nothing touches batteries under a running emulator), and on the "Sync now" button.

Gaps:
1. Off by default and needs a checkbox; users with one PC never benefit, and a lost PC means lost saves.
2. No pull-before-launch. Playing on PC B after PC A only works if B happened to sync first. Sync is whole-vault, not per game.
3. A full local mirror doubles disk use and hashes the whole vault each run (cost grows with library size).
4. `include_states` is one global bool: states are all or nothing, so enabling them blows the 200 MB quota and the user gets a hard 507.
5. No history: an upload overwrites the object. The loser of a conflict survives only in a local `_conflicts/` folder, not in the cloud.
6. No priority: tiny battery saves and 8 MB cards/states compete equally; the quota error does not say which game stopped syncing.
7. No per-game status, no "playing on another PC" signal, no persistent retry queue (a failed upload waits for the next full sync), no conflict UI (silent newest-wins).
8. Deletions are never propagated (acceptable, but there is no way to remove a game from the cloud to free quota).
9. Identity risk: the cloud is only reachable with `identity.json`. If both `data_dir` and the vault are wiped the cloud saves are unreachable and nothing tells the player (see "Recovery code", section 2).
10. Every PUT is one Class A op and one DO call with no batching, no dedup, no daily per-account op ceiling (a buggy client loop could burn the global monthly budget).

## 2. Target user experience

Vocabulary. Class: `settings` (meta.json, `_account/*`), `battery` (SRAM/EEPROM/flash), `card` (memory cards, GCI), `state` (save states), `shot` (state thumbnails). "Protected" = settings + battery + small cards (<= 512 KB each); these always sync. "Large" = state, shot, and any card > 512 KB (PS2 8 MB, GC 16 MB); these sync only while under the cap.

| Situation | Behaviour |
|---|---|
| First launch / wizard | No cloud question. The friends identity already exists, so the account is the cloud. The wizard gets one new step "Your saves are backed up automatically", showing a green check, the device name (editable, default computer name) and a **Recovery code** (uid + secret encoded in 8 groups of 4, Copy / Save as text file / Print). Skippable; a toast reminds once after the first successful push if skipped. A "Link a PC" / "I have a recovery code" entry sits beside it. |
| Play a game on a 2nd PC | On Play: a short "Checking cloud saves..." line (<= 4 s for the manifest, <= 15 s total download, only the pulled game, protected class only). Newer cloud files are downloaded into the vault, then the emulator starts. States are not blocking: missing states download in the background after launch (never overwriting an existing local file). |
| Offline | Manifest request fails or times out: the game starts at once from local files. Details line: "Offline: will sync when you are back". Exit queues a push; the persistent queue retries (30 s, 2 min, 10 min, 1 h, then on hub start / next launch / manual). No dialog ever blocks play. |
| Conflict | Both the cloud and this PC changed the same file since the last sync (three-way compare, section 5). Detected before launch (cloud changed, local changed) or at push (CAS miss). Dialog before launch, never mid-game: "Saves for GAME differ. This PC: played 20 Oct 21:14, 412 KB. Cloud: PC-LIVINGROOM, 22 Oct 08:02, 410 KB." Buttons: **Use the newer one** (preselected, newest mtime), **Use this PC's**, **Use the cloud's**, **Decide later** (play local only, no push for that game, banner stays). Whatever loses is kept: as a cloud version (restorable) and locally in `_conflicts/`. States never prompt: newest wins silently, the replaced one goes to history if space allows. |
| Over the cap | Protected files always sync (up to the protected ceiling, 20 MB). A large item that does not fit evicts the least-recently-played games' large items first (never the game being saved, never protected). Evicted items stay on this PC and are marked "Local only (cloud full)". Toast, once per day: "Cloud space is full: older save states of A, B stay on this PC only." Players can pin a game ("Keep in cloud") to make it the last evicted, or "Remove from cloud" to free space. |
| Game running on another PC | The launch call takes a lease (only for accounts with 2+ registered devices). If held: dialog "GAME is open on PC-LIVINGROOM (since 21:02). Playing here may create a conflict." Buttons: **Cancel**, **Play anyway** (play and let normal conflict rules apply), **Take over** (lease moves, other PC's lease dies at next heartbeat and its push will conflict-check). Leases expire after 12 min without a heartbeat (crash safe). |
| Online-session guest saves | Shared-save lockstep/rollback modes use a throwaway dir (docs/SAVES.md "Online save policy"): vault untouched, so nothing is pushed and no lease is taken (`mode = readonly`). The host runs the normal pre-launch pull first, so the save it shares is the freshest. Modes that write the player's own vault save (DS, 3DS, GC, GB link cable, Kanto Together, Smash Classic) behave like a normal launch. |
| Uninstall / reinstall (vault kept) | Identity is mirrored in `<vault>\_identity`, so same account; first start diffs the manifest and heals missing files. |
| Uninstall / reinstall (everything wiped) | Wizard -> "I have a recovery code" (or Link a PC from another PC) adopts the identity (existing retire/alias logic in FRIENDS.md), then the first sync pulls: all settings + battery + cards (small, fast), then states newest-played first until the cap. Without a code, the cloud cannot be opened; the doc must say so plainly. |
| Link a PC | Existing LinkBox flow adopts the same uid + secret on the new PC. New PC registers as a new device; first sync is "restore mode": no push until the pull finished, so an empty vault can never overwrite the cloud. |
| Folder sync (OneDrive etc.) | Stays as an optional extra on the Saves page ("Also copy saves to a folder"). It runs after cloud sync and never owns the truth. |

## 3. Protocol / contract (Worker)

All routes under `/saves/v2/`. v1 routes (`/saves/list|obj`) stay until the migration window ends (section 7). Worker -> `R2_BUDGET` DO on every call, fail closed (`cloud-saves-unavailable`). Account state lives in the existing `R2Budget` SQLite DO (single global instance, already the quota authority); the manifest is DO rows, so reads cost zero R2 ops.

### 3.1 Auth
Unchanged scheme: `Authorization: RHSIG uid=<26 b32>,ts=<unix ms>,sig=<hex>`; sig = HMAC-SHA256(key = sha256hex(secret), `METHOD\nPATH?QUERY\nts\nX-RH-Sha\nIf-Match\nIf-None-Match\nContent-Length\nX-RH-Mtime`) with `-` for absent. For JSON POST bodies `X-RH-Sha` is mandatory and is the sha256 of the body, so the body is covered by the signature. Skew +-5 min; clients may use the `Date` of a 401 `stale-timestamp` reply (`serverTime` field) to correct once. UserHub keeps the per-account rate limiter (`saves_auth`). New header `X-RH-Device: <device id>` (not signed; informational, validated `[a-z0-9-]{8,40}`).

### 3.2 Data model (DO tables; all `uid`-scoped)
- `devices(uid, dev, name, first, last_seen)`  max 8 per account (oldest idle evicted).
- `games(uid, gkey, played_at, device, pin, lease_dev, lease_until, rev)`  `gkey = "<system>/<game key>"` (same sanitizer as the vault).
- `vers(uid, gkey, path, ver, sha, size, stored, enc, mtime, dev, ts, class, rkey, state)`  `state` = `cur` | `prev` | `evicted`. `ver` increments per path. `rkey` = R2 key of the blob.
- `accts(uid, used, prot_used, rev, day, a_ops, b_ops)` (extends the existing table). `rev` is a per-account monotonically increasing counter bumped by every commit/restore/evict/pin.
- Blobs are content addressed per account: `saves/<uid>/o/<sha256>`. Identical content (e.g. same save restored, or a reverted conflict) costs no new Class A op (`have`). A blob is deleted (free op) when no `vers` row references it. Cross-account dedup is never done.
- Hard limits: 16 MB per blob (`stored` size), 4000 `vers` rows per account, 600 paths, path rules = existing `cleanKey` (+ `_identity` and `.hubtmp` refused).

### 3.3 Manifest format
`GET /saves/v2/manifest?since=<rev>&game=<gkey optional>` (DO only, zero R2 ops, 1 DO request):
```json
{ "ok": true, "rev": 412, "full": false,
  "account": { "used": 8123456, "protUsed": 1204433, "cap": 52428800, "protCap": 20971520, "devices": 2,
               "pool": "ok|tight|full", "paused": false },
  "games": { "n64/smash-64": {
      "playedAt": 1760000000, "device": "PC-DESK", "pin": false, "rev": 411,
      "lease": { "dev": "d-ab12cd34", "name": "PC-LIVING", "until": 1760000900 } ,
      "files": { "battery/ssb64.sra": {
          "cur": 7, "cloud": "synced",
          "vers": [ { "ver": 7, "sha": "<64hex>", "size": 32768, "stored": 1210, "enc": "deflate",
                      "mtime": 1760000000, "dev": "d-ab12cd34", "devName": "PC-DESK", "ts": 1760000010, "class": "battery" } ] } } } } }
```
`since` returns only games with `rev > since` (`full:true` when since is absent or older than the retained window). Per file `vers` has the retained history (<= 3 for protected, 1 for large) newest first. `cloud` is `synced` or `evicted` (tombstone: bytes gone, row kept so the client knows why and does not retry). Classes: `settings|battery|card|state|shot`. `enc` is `none|deflate` (client compresses blobs > 4 KB with raw deflate when it saves >= 20%; `sha` and `size` are always of the plain file; `stored` is the compressed bytes and is what quota counts).

### 3.4 Endpoints

| Route | Purpose | R2 ops | DO rows |
|---|---|---|---|
| `GET /saves/v2/status` | quota, pool state, devices, server flags (cheap probe; replaces `/saves/list`) | 0 | 0 |
| `GET /saves/v2/manifest` | as above | 0 | 0 |
| `POST /saves/v2/device` `{dev,name}` | register/rename this PC, returns `{devices}` | 0 | 1 |
| `POST /saves/v2/plan` | body `{game, played_at, mode, files:[{path,sha,size,stored,enc,mtime,class,base}]}` (<= 200 files). Server compares to `cur`: per file answers `upload` (needs bytes), `have` (blob exists in account: commit will just add a version), `same` (cur already this sha), `conflict` (`cur != base`, returns `current` entry), `skip` (`reason: over-cap` or `too-large`). Returns `ticket` (opaque, 10 min, binds the accepted set, reserves Class A + bytes + eviction plan in the budget DO). | 0 | 0-1 |
| `PUT /saves/v2/blob?sha=<64hex>&ticket=T` | upload one blob (`Content-Length`, `X-RH-Sha` = sha of the stored bytes, `X-RH-Plain-Sha` header optional). Verified while streaming (`FixedLengthStream`), conditional put `etagDoesNotMatch:*`. Idempotent: existing blob returns `{unchanged:true}` without a put. | 1 A | 1 |
| `POST /saves/v2/commit` `{ticket}` | atomically: add `vers` rows, set `cur`, bump `rev`, apply retention (drop versions beyond 3 / 1, queue blob deletes), run eviction, return `{rev, results:[{path, ver, cloud}], evicted:[{game,path}]}`. Per file CAS on `base`; a miss turns that file into `conflict` and nothing else is affected. | 0 (+ free deletes) | ~3 per file |
| `GET /saves/v2/blob?game=&path=&ver=` | download a version (default `cur`); `Range: bytes=N-` resume; headers `X-RH-Sha` `X-RH-Plain-Sha` `X-RH-Enc` `X-RH-Size`. Only blobs referenced by this account's `vers`. | 1 B | 0 |
| `POST /saves/v2/restore` `{game,path,ver,base}` | make an older retained version current again (copies the row as a new ver, no R2 op); CAS on `base`. | 0 | 3 |
| `POST /saves/v2/game` `{game, pin?:bool, remove?:bool}` | pin / unpin; `remove` drops all `vers` for the game, deletes blobs (free), frees quota. | 0 (+ free deletes) | n |
| `POST /saves/v2/lease` `{game, mode:"rw"|"readonly", take?:bool}` | `{ok}` or `{held:{dev,name,since,until}}`; `take:true` steals. `readonly` never takes a lease. Skipped by the client when the account has < 2 devices. | 0 | 1 |
| `POST /saves/v2/lease/beat` `{game}` / `.../release` | extend to now+12 min every 5 min / clear at exit. | 0 | 1 |

Old `PUT /saves/obj` is still accepted for v1 clients; it writes through the same tables (v1 rows have `rkey = saves/<uid>/<rel>`, no history).

### 3.5 Quota accounting, priority, eviction
- Counted bytes = sum of `stored` over all retained `vers` of the account (history counts; shared blobs counted once per account). Per-account cap `ACCOUNT_CAP = 50 MB`; protected ceiling `PROT_CAP = 20 MB` (settings + battery + small cards, history included); the rest of the cap is the **large pool** (states, shots, big cards, their history).
- Protected writes are only refused by `PROT_CAP` (`507 too-big-protected`, a bug indicator, not an expected case) or by global `full`/`paused` budget states.
- Large write that does not fit in `ACCOUNT_CAP - used`: eviction candidates = this account's large items grouped by game, ordered by `games.played_at` ascending (pinned last), never the game in the ticket. Evict whole games' large items oldest first, within a game states before cards, oldest mtime first, until it fits. If it still does not fit, that file gets `skip: over-cap`. Eviction marks `vers.state='evicted'` and deletes the blob (free). Client keeps the local file.
- Global pressure (so the per-account number never promises more than 7 GB can hold): `pool` = `ok` below 70% of 7 GB, `tight` 70-90%, `full` above. In `tight` the large-pool cap for accounts above 10 MB of large items shrinks to `max(10 MB, 30 MB - ...)` in steps (30 MB, then 20 MB, then 10 MB at 90%); in `full` no new large items are accepted and the global least-recently-played large items of accounts above their fair share (`7 GB * 0.3 / accounts_with_large`) are evicted as writes arrive (checked on commit, at most 20 evictions per commit, so rows stay bounded). Protected items are never evicted for global pressure; the 7 GB R2Budget `full` response (`507 cloud-saves-full`) stays as the last resort.
- Inactivity: large items of accounts with no commit/manifest for 90 days are evicted when pool is `tight` or `full` (lazy, on a reconcile pass, max 200 rows per pass). Protected data stays until the account is idle for 24 months.

### 3.6 Rate limits and ceilings
- Per account per minute (UserHub limiter): 30 plan+commit, 120 blob PUT, 300 blob GET, 60 manifest/status/lease.
- Per account per UTC day: 1500 blob PUTs, 6000 blob GETs (`429 account-daily-limit`, `retryAfter` to next UTC midnight). A first full upload of a big library therefore finishes over a few days rather than hurting the global budget.
- Global: unchanged R2Budget caps; plus saves are given at most 35% of the `REQ_BUDGET` daily units; past that `429 cloud-saves-busy-today`. Local play is never affected by any server answer.
- Bodies: `plan` <= 64 KB JSON, `commit` <= 8 KB.

### 3.7 Error codes
`400 bad-key|bad-sha|bad-body|bad-plan`, `401 unauthorized|stale-timestamp`, `403 device-limit`, `404 not-found`, `409 busy|changed-during-read|ticket-expired`, `411 length-required`, `412 conflict` (body carries `current`), `413 object-too-large`, `416 bad-range`, `423 lease-held` (body `held`), `428 precondition-required`, `429 rate-limited|account-daily-limit|cloud-saves-busy-today|cloud-saves-paused|cloud-saves-unavailable` (all with `retryAfter`), `507 quota-exceeded|too-big-protected|too-many-files|cloud-saves-full`, `503 cloud-saves-disabled|cloud-saves-error|cloud-saves-retry`. Clients treat 429/503/507-full as "local only for now", never as a failure that blocks play.

## 4. Client sync engine

### 4.1 State
Per game and file the engine keeps the **base** (what both sides last agreed on) in `<vault>\.sync\cloud-<uid8>.json`: `{rev, device, games:{gkey:{files:{path:{ver,sha,size,mtime}}}}}`. Local scan results are cached by (size, mtime) so rehashing only happens for changed files. The `cloud_mirror` folder is dropped (files stream straight to `*.hubtmp` then atomic replace, as `sync_folder` does today). The persistent queue `<data>\cloud-queue.json` holds `{gkey, reason, tries, next_at}`.

### 4.2 Three-way rule (per file)
| local vs base | cloud vs base | Action |
|---|---|---|
| same | same | nothing |
| changed | same | push (plan/blob/commit) |
| same | changed | pull |
| changed | changed, same sha | adopt (update base only) |
| changed | changed, different sha | **conflict** |
| absent | present | pull (new game on this PC) |
| present | absent, base absent | push |
| present | evicted | keep local, status `LocalOnly(cap)` |
| absent locally, present in base | any | do not delete anything; re-pull if cloud has it (heals accidental loss) |
Classes `state`/`shot` never conflict: newest `mtime` wins, loser goes to history/`_conflicts`. `settings` conflicts also auto-resolve newest wins (as `meta.json` does today).

### 4.3 Flow
- **Pre-launch (`cloud_prelaunch`)**, job thread, called by the Play path before spawning the emulator: (1) register/ensure device (cached); (2) `manifest?game=` with a 4 s timeout (2 s connect); (3) if the lease check is needed, `lease` in the same step (dialog data returned to UI); (4) pull protected files that changed, total budget 15 s, resumable `Range`, sha verified, atomic replace; (5) three-way finds a conflict -> return `needs_decision` with both sides' details; (6) anything missed (timeout/offline) -> `proceed` with `offline=true` and the game queued. States/shots pulled afterwards by a background job (`Priority::Background`) that never overwrites an existing local file.
- **During the game**: only the lease heartbeat (5 min). No file I/O in the vault by the engine (existing `sync_gate` rule stays).
- **Post-exit (`cloud_postexit`)**: `touch_game`, then enqueue; the worker runs `plan` -> blob PUTs (protected first, then large by newest) -> `commit`. Compression is applied before hashing the stored form. Release lease. Unchanged files cost nothing: they are filtered locally by hash vs base before `plan`.
- **Background**: at hub start one `manifest?since=rev` delta (zero R2 ops); changed games are listed but only downloaded when the player launches or opens them (protected files of all changed games are fetched lazily in the background when idle, newest-played first, capped at 4 MB per minute). No periodic polling (Worker request budget, section 6).
- **Retry/backoff**: network error, 5xx, 429 (`retryAfter` honoured), 503: re-queue with 30 s, 2 min, 10 min, 1 h, then wait for a trigger. 507/over-cap/`too-large`: not retried, status `LocalOnly`. 401: stop, status `NeedsAttention(account)`. All steps are idempotent (hash-addressed blobs, CAS commit), so killing the process is safe.
- **Manual**: "Sync now" runs queue + whole-account delta once. "Restore previous version" lists `vers` (time, device, size) and calls `restore`, then pulls.

### 4.4 Per-game status states
`Synced`, `Syncing`, `Waiting` (queued/offline), `LocalOnly` (reason `cap|too-large|user|decide-later`), `Conflict`, `PlayingElsewhere` (lease by another device), `Paused` (server budget / maintenance), `NeedsAttention` (auth/identity), `Unknown` (not yet looked at). Each carries an optional message, last-synced time and byte counts. Aggregate account state feeds the title bar chip: all good (green check), working, offline, needs attention.

### 4.5 Settings migration
`Settings.cloud_saves` becomes `cloud_enabled` default **true** (a "Pause cloud sync" switch lives under Advanced; `sync_states` is removed, replaced by the class policy). `sync_folder` and `sync_auto` stay.

## 5. Contract C++ (shared by all lanes)

New header `src/saves/cloud_engine.h` (owned by the Client lane; UI and `save_jobs.cpp` code only against this). Existing `cloud.h`/`savestore.h` stay; `CloudConfig::include_states` is deprecated, and `cloud_status`/`sync_cloud` are kept as thin wrappers over the engine during migration.

```cpp
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
std::string cloud_recovery_code();                          // uid + secret, grouped; empty if no identity
bool cloud_adopt_recovery_code(const std::string& code, std::string* err);

} // namespace hub::saves
```

Server wire types are the JSON in section 3; the Client lane owns the parser (`cloud_wire.cpp`) with golden JSON tests.

## 6. Cost model (free tier)

Free tier (R2): 10 GB-month storage, 1 M Class A, 10 M Class B per month; Workers 100k requests/day; DO SQLite: 100k rows written/day, 5 M read/day. Our guards sit at 70% (7 GB, 700k, 7M, 70k rows).

Assumptions per active player: 20 play sessions/month, 3 files pushed per session (battery, meta, one state/shot), 50% of sessions follow a play on another PC, initial upload of 100 files. Light/typical/heavy = 0.5x / 1x / 3x. Blob history x1.8 for protected data (3 versions of tiny files), x1.3 for large.

| Metric (per month unless noted) | 100 players | 500 | 1000 | Cap used |
|---|---|---|---|---|
| Class A (puts): 60/mo + 100 once, plus weekly reconcile | ~8k | ~40k | ~80k (heavy mix: ~200k) | 700k: 1-29% |
| Class B (gets): 30/mo + 100 for new PCs | ~5k | ~25k | ~50k | 7M: <1% |
| Stored: protected ~3.6 MB avg, large pool ~5 MB avg capped at 50 MB per account | ~0.9 GB | ~4.4 GB | ~8.8 GB uncapped -> held at ~6.3 GB by pool rules | 7 GB |
| DO rows written/day: ~14 rows per session x 0.67 sessions/day | ~1k | ~5k | ~10k (+ leases ~5k) | 70k: 14-21% |
| Worker+DO units/day (REQ_BUDGET, double counted): ~20 per session x 0.67 | ~1.3k | ~6.7k | ~13k | 100k: 13% (saves limit 35k) |
| R2 duration / egress | free (R2 egress to Workers/Internet is free) | | | |

Reading it: Class A/B are non-issues; storage is the only constraint. At 1000 active players the average of 8.8 MB per player no longer fits in 7 GB, so the pool rules in 3.5 (`tight` at 4.9 GB shrinks the large-pool cap to 30/20/10 MB; `full` evicts least-recently-played large items of accounts above fair share) keep stored bytes below 7 GB while protected data (~3.6 GB at 1000) is never evicted. The 7 GB guard is still 30% below R2's 10 GB free tier. Beyond roughly 1500 active players protected data alone would exceed the guard; at that point `cloud-saves-full` applies and the owner decides (raise the guard toward 10 GB, still free, or shrink history). Any request-unit pressure (`REQ_BUDGET` at 50%+) switches saves to `cloud-saves-busy-today`, which only delays syncing. Compression (mostly-empty cards and SRAM compress 5-20x) is the main lever and is already in the contract (`enc: deflate`).

Why no periodic polling: a 15-minute poll while the hub is open (about 16/day/player) would add 32k units/day at 1000 players, more than all other saves traffic combined.

## 7. Migration

1. Server first (backwards compatible): deploy v2 routes, add tables, keep v1 routes. On the first v2 call for a uid the budget DO converts the existing `files` rows (v1 objects) into `vers` rows (`ver=1`, `rkey` = existing object key, class derived with the same `classify_path`, `stored=size`, `enc=none`); zero R2 ops. The v1 200 MB account quota is recomputed against the new caps; accounts over 50 MB get large items marked `evicted` using the normal rules (protected first stay), logged to `cloud_saves.log` on their client at next sync via the manifest `cloud:"evicted"`.
2. Client release: engine replaces `sync_cloud`. First run with the new build: set `cloud_enabled=true`, register device, then for every vault game compare to the manifest: local-only files push, cloud-only files pull, both different -> the normal conflict rule (decision dialog the first time they launch that game, not at startup). The old `cloud_mirror` folder is deleted after the first successful full pass. Existing `.sync\<root>.json` folder-sync manifests are untouched.
3. Existing vault + sync-folder users: nothing to do. The folder remains an extra target; because cloud and folder both hold copies, the order is cloud first, folder second, and the folder sync's conflict logic still applies to the folder side only.
4. Vault users who never enabled anything: the very first cloud pass would upload every game. To keep bandwidth and the daily PUT ceiling sane, the engine uploads protected data first (all games), then large items newest-played first, then stops at the cap; progress shows as "Backing up your saves: 12 of 41 games".
5. Existing identity edge cases follow FRIENDS.md (adopt/retire/aliases). On an identity change (link-a-PC, vault adopt) the cloud account switches with the identity; the old uid's cloud data is not merged (it stays under the retired uid and is deleted by the 24 month idle rule), and the engine enters restore mode.
6. Rollout switches (server vars, no redeploy of clients): `SAVES_V2=0|1`, `ACCOUNT_CAP`, `PROT_CAP`, `POOL_TIGHT`, `POOL_FULL`. Old clients keep working on v1 until the sunset; tests must cover a v1 client against the v2 server.

## 8. Implementation lanes

Lanes work in separate worktrees and meet only at section 3 (wire) and section 5 (header). The Client lane publishes `cloud_engine.h` first (stub implementation returning `Unknown`/`Proceed`) so UI can build immediately; the Server lane publishes a fixture folder `BattleShip/server/cloudflare/test_fixtures/v2/*.json` with example replies for the client's golden tests.

### Lane S: Server (Worker)
Owns: `BattleShip/server/cloudflare/src/saves.js` (v2 router, keep v1), `src/r2budget.js` (tables, accounting, eviction, leases, v1 conversion), `src/index.js` (route wiring only), `src/hub.js` (`saves_auth` limiter keys), `wrangler.jsonc` (vars only), new `src/saves_v2.js` and `src/saves_pool.js` (pure pool/eviction functions), tests `test_saves.mjs` (extend, v1 stays green), new `test_saves_v2.mjs`, `test_saves_pool.mjs` (pure eviction/priority property tests), extend `test_r2budget.mjs`. Docs: `RetroHub/docs/CLOUD_SAVES.md` becomes a pointer to this file. Done when: plan/commit CAS, dedup, retention 3/1, restore, eviction order, protected ceiling, lease expiry, per-account daily limits, 35% request share, fail-closed on budget errors and v1 conversion are all covered by tests, wrangler dev run with an isolated registry.

### Lane C: Client engine
Owns: `src/saves/cloud_engine.h/.cpp`, `cloud_wire.h/.cpp` (JSON, signing reuse from `cloud.cpp`), `cloud_queue.cpp`, `cloud_classify.cpp`, `cloud.h/.cpp` (reduced to signing helpers + deprecated wrappers), `savestore.*` (add per-game scan and `scan_game()`, keep `sync_folder` behaviour), `src/saves/CMakeLists.txt`, tests `test_cloud.cpp` (extend), new `test_cloud_engine.cpp` (fake in-process server: three-way table, offline, timeouts, resume, conflict, eviction tombstones, queue persistence, recovery code), `src/hub/save_jobs.cpp` (job plumbing only: call `cloud_prelaunch` before spawn, `cloud_game_started/exited`, remove mirror merge; keep `merge_reports`, folder sync order), `qa/saves_cloud.ps1`. Does not touch `page_saves.cpp`, `logic.h` settings fields (UI lane adds them from section 4.5) or any drawing code. Done when all `hubsaves_test` cases and the new engine tests pass and a two-PC headless scenario (qa script) shows pull-before-launch, offline play and conflict.

### Lane U: UI
Owns: `src/hub/page_saves.cpp` (account panel: usage bar, pool state, devices, "Restore previous version" list, per-game table with status chips, pin / remove / local-only, folder sync demoted to an "Also copy to a folder" panel, Advanced > pause), game details status line and chip (game details panel files), launch-time dialogs (conflict, lease), setup wizard step with recovery code (Copy / Save / Print), `logic.h` + settings load/save for `cloud_enabled` / removal of `sync_states` (with migration of the old `cloud_saves` key), toasts mapped from `CloudEvent`, title bar cloud chip, `hubui` pure logic and its console test (status -> chip text/colour, event -> toast, dialog state machine) in `logic.h` tests. Codes only against `cloud_engine.h`; until Lane C lands it links the stub. Screens verified headless with `HUB_SCREENSHOT`. Done when each status state and each dialog has a screenshot and the pure-logic tests pass.

### Cross-lane contracts
- The wire sections 3.3 and 3.7 are frozen after Server and Client agree; changes go through Opus.
- Strings shown to the user are produced in the UI lane except `CloudEvent::text`, which the Client lane writes (kept short, English, no jargon).
- `classify_path` exists once (C++, Client lane) and once (JS, Server lane, used only for v1 conversion); both share the golden table `docs/cloud_classify_cases.txt` (Server lane creates it, Client lane's test reads it).
- Order of merge: Server, Client (stub-compatible), UI. Deploy Server and verify with an old client before shipping the new client build.

## 9. Open questions for the owner
1. Recovery code is the only way back after a full wipe without a second PC; confirm showing it in the wizard (recommended) versus an optional email/Google sign-in later.
2. Inactivity policy (90 days for large items under pressure, 24 months for everything) is a proposal.
3. Is a 12 minute lease acceptable, given players on a single PC never see it?

## 10. Lane S implementation notes (server, as built)

Code: `BattleShip/server/cloudflare/src/{saves_v2,saves_pool,r2budget_v2}.js` (+ small hooks in `saves.js`, `r2budget.js`,
`friends.js` UserHub `saves_auth`). Tests: `test_saves_v2.mjs`, `test_saves_pool.mjs`, `test_saves.mjs` (v1), `test_r2budget.mjs`;
`run_all_tests.mjs` runs every server test sequentially on fresh isolated instances. Fixtures: `test_fixtures/v2/*.json` (real replies,
regenerate with `node gen_fixtures.mjs`).

Facts the client lane needs, where the build is more specific than sections 3-4:
- **Signature**: as v1. `Content-Length` is part of the signed string for `PUT` only (`-` for POST/GET). JSON POST bodies need
  `X-RH-Sha` = sha256 of the body (missing or wrong -> 400 `bad-sha`). `401 stale-timestamp` carries `serverTime` (ms).
- **Blob addressing**: `?sha=` on `PUT /blob` is the **plain** sha from the plan; `X-RH-Sha` on the request is the sha of the **bytes sent**
  (the same when `enc` is `none`). R2 verifies it while streaming. The server stores `saves/<uid>/o/<sha>` (`.z` suffix for deflate).
  `GET /blob` answers `X-RH-Sha` (stored bytes, when known), `X-RH-Plain-Sha`, `X-RH-Enc`, `X-RH-Size` (plain), `X-RH-Stored`, `X-RH-Ver`, `X-RH-Mtime`;
  `Range: bytes=N-` is over the stored bytes. A `have` reply carries the real `stored` and `enc` of the existing blob; commit uses those.
- **Plan**: `mode` must be `rw` (a readonly session pushes nothing, so no plan: 400 `bad-plan`). Per-file `skip.reason`:
  `over-cap`, `too-large`, `too-big-protected`, `too-many-files`. If the only outcome of a plan is protected-ceiling skips the reply is `507 too-big-protected` (with `files`).
  The protected ceiling counts **kept history**, so a protected file can be at most about `PROT_CAP / 3` once it has three versions.
  A reply without any `upload`/`have` has `ticket: null`. A plan that needs more PUTs than the account has left today is `429 account-daily-limit`
  (nothing is reserved); `429 cloud-saves-paused` when the global Class A budget cannot cover the uploads; `507 cloud-saves-full` when the 7 GB guard cannot.
- **Commit**: results per file are `{path, ver, cloud:"synced"}`, `{path, cloud:"conflict", current}`, `{path, cloud:"missing"}` (blob not uploaded: upload it
  and commit the same ticket again), or `{path, cloud:"skipped", reason}`. A repeated commit of a finished ticket returns the same results with `replay:true`.
  Tickets live 10 minutes. `evicted:[{game,path}]` lists what this commit pushed out of the cloud. Orphan blobs (lost races) are garbage collected after 1 hour.
- **Tombstones**: an evicted file stays in the manifest as `cloud:"evicted"` with `cur` = its last version and `vers: []`. Pushing it again needs `base = cur`.
  `GET /blob` of it is `404 {evicted:true}`.
- **Devices**: eight per account; a ninth is accepted only when the least recently seen device has been idle 24 h (it is then dropped), else `403 device-limit`.
  Lease calls need a valid `X-RH-Device`. Beat on a free/expired lease re-takes it; beat after a take-over is `423 lease-held`.
- **Rate limits** are per account per minute: `plan/commit/restore/game/recovery` 30, blob PUT 120, blob GET 300, status/manifest/device/lease 60.
  Daily per account 1500 blob PUTs / 6000 blob GETs (`DAY_PUT_MAX`, `DAY_GET_MAX` override). Saves are capped at 35% of the `REQ_BUDGET` daily units
  and stop (`429 cloud-saves-busy-today`) when the global request budget passes 50%. Server vars: `SAVES_V2`, `ACCOUNT_CAP`, `PROT_CAP`, `POOL_TIGHT`, `POOL_FULL`.
- **v1 compatibility**: v1 routes are unchanged for accounts that never called v2. The first v2 call converts the account's v1 objects to `ver 1` rows
  (zero R2 ops, classes by `classifyPath`, games `system/game`, `_account/_`, `_misc/_`) and applies the new caps (oversized accounts lose large items,
  least recently played game first). After conversion a v1 write goes through to the version table (no history), a v1 delete removes the file, and a v1 PUT over the
  v2 account cap is `507 quota-exceeded`. v1 never sees v2 blobs (`o/...` is reserved) .
- **DO schema tag**: `meta.schema = 2` in the R2Budget DO adds the v2 tables and columns once (no wrangler migration needed: same class).

### Recovery code (decision: yes) - one mechanism with the link code
There is deliberately **one** "add/recover this account" mechanism: the identity bundle `{salt, iv, ct}` that the link-a-PC flow already uses
(client derives an AES-GCM key from a code with PBKDF2; the server only stores ciphertext). The recovery code is a **long-lived link code**:
- The client generates a random code (160 bits, shown as 8 groups of 4, e.g. in the wizard), builds the same encrypted bundle as for `/link/put`,
  and stores it with `POST /saves/v2/recovery {hash, salt, iv, ct}` (signed like every v2 call). `hash = sha256hex("rh-recover:" + normalized code)`;
  the server keeps only that hash and the opaque bundle (never the code, the key or the identity). One slot per account; setting a new one replaces (revokes) the old
  code; `{remove:true}` deletes it. `GET /saves/v2/status` shows `account.recovery`.
- A new PC with no identity calls `POST /saves/v2/recover {hash}` **without** auth (throttled per IP, 10/hour, 200/hour overall) and gets `{salt, iv, ct}`,
  decrypts the identity locally, adopts it exactly as after a link redeem, and from then on is the same account (same uid), so it reads the same cloud data.
  Unlike a link code it is not single-use and does not expire; losing the code does not expose anything, replacing it does not touch the cloud data.
- Cloud data is always keyed by the internal account uid. The visible username (see the usernames lane) maps to the uid and can be renamed without moving any data.
  A PC added with a normal link code is the same uid with a new device id, so it needs nothing else to see the cloud saves.
- Integration with the usernames lane: the shape returned by `/saves/v2/recover` equals the one from `/link/redeem` (minus `username`), so the client can share the
  decrypt/adopt code. If the owner prefers a single endpoint, `recover` can be fronted by `POST /link/redeem {recovery: hash}` later without changing storage.
