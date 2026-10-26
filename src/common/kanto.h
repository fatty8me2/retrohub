#pragma once
/* Kanto Together (FireRed co-op ROM hack by a friend of the user) inside RetroHub.
 *
 * The user's own package (KantoTogether-ROM-*.zip, or an extracted KantoTogether-ROM folder) is NEVER shipped by RetroHub and never
 * modified. RetroHub detects it in the ROM folders, "installs" a copy into <data>\apps\KantoTogether (first Play), builds the patched game
 * from the user's FireRed (USA) v1.0 and runs the package's own Node launcher (hidden, no console) next to mGBA:
 *
 *   node app\coop-launch.mjs host|join [CODE] --rom ... --saves ... --mgba ... [--unmute]   (stdin: trade N | confirm | cancel | code NEW | quit)
 *
 * The launcher's console output is parsed (feed_line) into a small State the hub draws as a status panel and a trade panel.
 * Pure, testable pieces first; the Win32 pieces (Session, Prep) at the end. See docs/KANTO_TOGETHER.md. */

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace kanto {

constexpr const char* kKey = "gba:kanto-together";   // library key (same on every PC: the vault keeps the saves under it)
constexpr const char* kCatalogId = "kanto";           // games.cpp catalog id / room id
constexpr const char* kTitle = "Kanto Together (FireRed co-op)";
constexpr const char* kBaseSha1 = "41cb23d8dccc8ebd7c649cd8fbb58eeace6e2fdc";   // Pokemon FireRed (USA) v1.0
constexpr const char* kIssue = "kanto";              // GameEntry::issue marker

/* ------------------------------ hashing ------------------------------ */
std::string sha1_hex(const void* data, size_t len);
std::string sha1_file(const std::string& path_utf8);   // "" on error

/* ------------------------------ package detection ------------------------------ */
struct Package {
    std::string path;          // the .zip or the extracted folder (UTF-8)
    bool is_zip = false;
    uint64_t size = 0;         // zip size (0 for a folder)
    uint64_t stamp = 0;        // zip mtime (seconds, 0 for a folder): with size it tells when a newer package replaces the installed one
    std::string prefix;        // zip only: folder inside the archive that holds app\, patch\, runtime\ ("KantoTogether-ROM/" or "")
    std::string content_id;    // patch.json patchSha1 + "@" + built: identifies the build whether it comes as a zip or a folder ("" = unknown)
    std::string built;         // patch.json "built" (ISO time; sorts chronologically)
};
/* "<patchSha1>@<built>" from patch.json text ("" if neither key is present); *built gets the built value. */
std::string patch_content_id(const std::string& patch_json_text, std::string* built = nullptr);
bool inspect_dir(const std::string& dir, Package* out);            // is_package_dir + content id
/* Index of the package to use: newest build (patch.json built), else newest zip mtime; a tie prefers the extracted folder. -1 if empty. */
int pick_package(const std::vector<Package>& v);
/* Files of an installed package that the user's own runs rewrite (mGBA portable config): never overwritten by a reinstall once present. */
bool keep_on_reinstall(const std::string& rel);
/* Zip entry names that must exist for a usable package (relative to the package root). */
const std::vector<std::string>& required_files();
bool is_package_dir(const std::string& dir);                      // has app\coop-launch.mjs, patch\patch.json, runtime\node\node.exe, runtime\mgba\mGBA.exe
bool inspect_zip(const std::string& zip_path, Package* out);       // reads the central directory only
/* Walk `folders` (depth-limited, names containing "kanto" only; never opens other files) and return every package found, zips and folders. */
std::vector<Package> find_packages(const std::vector<std::string>& folders, int max_depth = 4);
/* Archive entry -> path below the install dir ("" = skip). Rejects "..", absolute and drive paths and the package's own rom\ and saves\. */
std::string install_rel_path(const std::string& entry, const std::string& prefix);

/* Where the package is installed to (<data>\apps\KantoTogether\) and whether a given package is already there. */
std::string install_dir(const std::string& data_dir);
bool is_installed(const std::string& dir);                         // files present
bool install_current(const std::string& dir, const Package& p);    // files present AND from this very package (marker size/stamp)
/* Copy the package into `dir`. progress(done_bytes, total_bytes) returns false to cancel. Overwrites package files, deletes nothing. */
bool install(const Package& p, const std::string& dir, const std::function<bool(uint64_t, uint64_t)>& progress, std::string* err);

/* ------------------------------ package version (both players must run the same build) ------------------------------ */
struct PkgVer { std::string id, built; };           // id = patch.json patchSha1 (lower case), built = ISO time
enum class VerMatch { Same, MineOlder, MineNewer, Different, Unknown };   // Unknown: one side has no version (e.g. an older RetroHub)
PkgVer parse_pkg_ver(const std::string& patch_json_text);
PkgVer read_pkg_ver(const std::string& dir);       // the INSTALLED package's patch.json
VerMatch compare_versions(const PkgVer& mine, const PkgVer& theirs);
/* Fixed reason code for a mismatch ("pkg_old": my package is older than the other one, "pkg_new", "pkg_diff"; "" for Same/Unknown). */
std::string version_reason(VerMatch m);
/* Host-side sentence for the Launch block: "Mickey has an older Kanto Together package - they need to update". `m` is from the HOST's point of view. */
std::string version_block_text(VerMatch m, const std::string& partner_name);
bool valid_ver_id(const std::string& id);          // 40 hex chars (lobby input validation)
/* The installed launcher knows the start-up barriers (--no-barriers / PH sync): app\coop-launch.mjs mentions "no-barriers". */
bool has_barriers(const std::string& dir);

/* ------------------------------ ROM ------------------------------ */
struct PatchManifest { std::string base_sha1, target_sha1; uint64_t base_size = 0; };
bool read_manifest(const std::string& patch_json_path, PatchManifest* out);

struct RomCandidate { std::string path, serial, catalog_id; uint64_t size = 0; };   // library entries of the GBA system (size 0 = unknown)
enum class RomState { Ok, NoFireRed, WrongRevision };
struct RomPick { RomState state = RomState::NoFireRed; std::string path; std::string seen_sha1; };
/* First candidate whose SHA-1 is the base ROM. Candidates with serial BPRE or catalog id pokefirered that hash differently -> WrongRevision.
 * Candidates without any serial (e.g. a zipped ROM the scan could not read a header from) are matched by SHA-1 alone, but only hashed when
 * their size is 16 MiB (or unknown). */
RomPick pick_rom(const std::vector<RomCandidate>& cands, const std::string& base_sha1, const std::function<std::string(const std::string&)>& hash = sha1_file);
std::string rom_message(const RomPick& p);
/* Machine-readable failure reasons (sent to the partner in np_kt_fail) and the fixed sentences shown for them (never raw error text / paths). */
std::string fail_sentence(const std::string& reason);   // unknown reason -> generic sentence
bool valid_fail_reason(const std::string& reason);
std::string reason_for(RomState s);                       // "no_rom" | "wrong_rom"   // user-facing text for a failed pick ("" when Ok)

/* ------------------------------ saves ------------------------------ */
/* Top-level *.sav files only, the newer file wins (the loser is kept as <dst>\.kt-backup\<name>.<n>). Returns the number of files written. */
int sync_saves(const std::string& src_dir, const std::string& dst_dir);
/* Per-role saves. The vault keeps KantoTogether.host.sav and KantoTogether.guest.sav (a host save and a guest save are different stories: the
 * guest's save carries the "synced" marker). The staged folder (what mGBA sees) always holds KantoTogether.sav + coop-trade-<role>.journal and is
 * scratch for ONE role: use a separate staged folder per role. A legacy vault KantoTogether.sav seeds the first role that is played and is then
 * renamed KantoTogether.sav.pre-roles (kept as a backup, ignored by everything). */
std::string role_name(bool host);                    // "host" | "guest"
std::string vault_sav_name(bool host);               // "KantoTogether.host.sav"
std::string journal_name(bool host);                 // "coop-trade-host.journal"
/* vault -> staged before a session. Journals: the newer copy wins (a journal left in the staged folder by a crash is real and must survive). */
int stage_saves(const std::string& vault, const std::string& staged, bool host);
/* staged -> vault after a clean exit. The journal is mirrored (a trade that completed deletes it in the vault too) and then removed from the
 * staged folder, so a leftover journal there always means a crashed session. */
int flush_saves(const std::string& staged, const std::string& vault, bool host);

/* ------------------------------ launcher command lines ------------------------------ */
struct LaunchOpts {
    bool host = true;
    std::string code;          // join: the host's 10-character code
    std::string dir;           // install dir
    std::string rom;           // <dir>\rom\KantoTogether.gba
    std::string saves;         // staged saves dir
    bool sound = true;
    int lua_port = 0;          // loopback port between launcher and mGBA (0 = pick a free one)
    std::string relay;         // "" = the package's own default; else COOP_RELAY
    std::string desktop;       // tests: name of a (hidden) desktop to start everything on
    bool preflight = true;     // run `coop-launch --dry-run` first (exit 2/3 -> clear message)
};
/* A message for a failed `coop-launch --dry-run` (exit 2: mGBA/ROM missing, 3: inside a temporary folder); "" for exit 0. */
std::string preflight_message(unsigned long exit_code);
/* Map a fatal "[launch] ..." line of the launcher to plain words ("" = not a failure: e.g. the verbose "[launch] <mGBA command line>"). */
std::string launch_failure_text(const std::string& line);
bool is_kanto_runtime_exe(const std::string& exe_path);   // runtime/mgba/mGBA.exe or runtime/node/node.exe under any package copy
bool is_non_ascii(const std::string& s);
std::string normalize_code(const std::string& s);   // upper-case [0-9A-F] only ("3f9a-0c7d 21" -> "3F9A0C7D21")
bool valid_code(const std::string& code);           // exactly 10 hex characters
std::string node_exe(const std::string& dir);
std::string mgba_exe(const std::string& dir);
std::string rom_path(const std::string& dir);
std::string launcher_cmdline(const LaunchOpts& o);  // full command line incl. quoted node.exe
std::string patch_cmdline(const std::string& dir, const std::string& base_rom);   // node apply-patch.mjs --manifest .. --out .. --rom ..
/* Console commands (each ends with "\n"). */
std::string cmd_trade(int slot);       // 1..6, anything else -> ""
std::string cmd_confirm();
std::string cmd_cancel();
std::string cmd_code(const std::string& code);   // guest: re-join with the host's new code ("" if invalid)
std::string cmd_quit();
std::string cmd_go();                  // skip a start-up wait (barrier) by hand

/* ------------------------------ launcher output -> state ------------------------------ */
enum class Phase { Idle, Starting, WaitingForPartner, Connected, Reconnecting, PartnerAway, Failed, Ended };
const char* phase_name(Phase p);
std::string phase_text(Phase p, bool host);

enum class TradePhase { None, Offered, Done, Cancelled };
struct Trade {
    TradePhase phase = TradePhase::None;
    std::string mine, theirs;     // "CHARMANDER lvl 5"
    int my_slot = 0;
    bool i_confirmed = false, peer_confirmed = false;
    std::string note;             // last trade message in plain words
};
struct State {
    Phase phase = Phase::Idle;
    bool host = true;
    std::string code;             // host: current room code
    int code_gen = 0;             // +1 on every new code (initial, restarted host, idle refresh): the hub forwards it to the guest
    std::string detail;           // short sentence for the status panel
    std::string last_error;
    Trade trade;
    int trades_done = 0;
    bool mgba_exited = false;
    bool synced_story = false;    // guest: the bridge applied the host's story snapshot (not a reset; needs the launcher's --verbose output)
    bool join_failed = false;     // guest: "Could not join" seen; the bridge keeps retrying by itself, so this is NOT terminal (cleared on connect)
    bool ever_connected = false;  // the partner was connected at least once this session
    /* start-up barriers (title screen to the first overworld step; package with "sync barriers") */
    bool barrier_waiting = false; // the game is paused waiting for the partner right now
    std::string barrier_text;     // plain words for the status panel while waiting ("" otherwise)
    bool barriers_off = false;    // the script switched them off (other ROM build) or the partner runs an older package
    bool legacy_peer = false;     // the partner's game has no barriers (older package): "both should update"
    bool together = false;        // "continuing together" / "both of you are in the game world" seen
};
/* Guest: may RetroHub push a new host code (`code NEW`) into the running launcher by itself? Never once the pair has connected: the bridge
 * then reconnects/resumes by itself, and `code` throws away its campaign (a fresh join re-syncs the guest = the game appears reset). */
inline bool guest_auto_rejoin_ok(const State& s) { return !s.ever_connected && s.phase != Phase::Connected; }
/* Diagnostics: appends "HH:MM:SS.mmm tag text" to <dir>\kanto-YYYY-MM-DD.log (keeps the newest 10 files). IPv4 addresses are masked. */
std::string redact_log_text(const std::string& s);
void write_log(const std::string& dir, const std::string& line);
/* Interpret one console line (with or without the "[bridge] " prefix). Returns true when the state changed. */
bool feed_line(State& s, const std::string& line);

/* ------------------------------ texts ------------------------------ */
std::string first_session_text(bool host);
std::string howto_text();             // the "How to play" sheet (plain text, \n separated, "# " heading lines)
const char* credit_text();

#ifdef _WIN32
/* ------------------------------ a running launcher ------------------------------ */
class Session {
  public:
    ~Session() { stop(true); drain_stops(); }
    bool start(const LaunchOpts& o, std::string* err);
    void update();                              // read the launcher's output; call every frame
    bool running() const;                       // node.exe still alive
    void send(const std::string& cmd);          // raw console line (with \n)
    void trade(int slot) { send(cmd_trade(slot)); }
    void confirm() { send(cmd_confirm()); }
    void cancel() { send(cmd_cancel()); }
    void go() { send(cmd_go()); }                // skip a start-up wait
    void rejoin(const std::string& code) { send(cmd_code(code)); }
    /* Ends the session: WM_CLOSE to mGBA's window (so the save flushes), wait for it, "quit", then the job object is closed (kills leftovers).
     * Runs on a worker thread so the UI never blocks; wait=true joins it (shutdown). */
    void stop(bool wait = false);
    static int stopping();                      // number of stop workers still running
    static void drain_stops(unsigned timeout_ms = 15000);
    void set_line_sink(std::function<void(const std::string&)> f) { sink_ = std::move(f); }   // every launcher output line (diagnostics)
    const State& state() const { return st_; }
    State& mutable_state() { return st_; }
    const std::deque<std::string>& log() const { return log_; }
    HANDLE dup_handle() const;                  // a duplicate of the node process handle (caller closes)
    DWORD pid() const { return pid_; }
    bool active() const { return started_; }
  private:
    HANDLE proc_ = nullptr, in_w_ = nullptr, out_r_ = nullptr, job_ = nullptr;
    DWORD pid_ = 0;
    bool started_ = false;
    std::string desktop_;
    std::string buf_;
    State st_;
    std::deque<std::string> log_;
    std::function<void(const std::string&)> sink_;
};

/* ------------------------------ install + build the game (worker thread) ------------------------------ */
class Prep {
  public:
    enum class Stage { Idle, Installing, FindingRom, Patching, Done, Failed };
    ~Prep() { join(); }
    void start(const Package& pkg, const std::string& data_dir, const std::vector<RomCandidate>& cands, const std::string& desktop = "");
    Stage stage() const { return stage_.load(); }
    bool busy() const { Stage s = stage_.load(); return s == Stage::Installing || s == Stage::FindingRom || s == Stage::Patching; }
    bool ok() const { return stage_.load() == Stage::Done; }
    bool failed() const { return stage_.load() == Stage::Failed; }
    float progress() const;                      // 0..1 while installing
    std::string message() const;                 // progress / failure text
    std::string reason() const;                  // failure reason code ("no_rom", "wrong_rom", "install", "patch", "cancelled", "package")
    RomState rom_state() const { return rom_state_.load(); }
    std::string dir() const { return dir_; }
    void join() { if (th_.joinable()) th_.join(); }
    void cancel() { cancel_ = true; }
    void reset() { join(); stage_ = Stage::Idle; rom_state_ = RomState::Ok; cancel_ = false; }
  private:
    void run(Package pkg, std::string data_dir, std::vector<RomCandidate> cands, std::string desktop);
    std::thread th_;
    std::atomic<Stage> stage_{Stage::Idle};
    std::atomic<RomState> rom_state_{RomState::Ok};
    std::atomic<uint64_t> got_{0}, total_{0};
    std::atomic<bool> cancel_{false};
    mutable std::mutex mu_;
    std::string msg_, dir_, reason_;
};
int free_loopback_port();
bool is_under_temp(const std::string& path);         // the launcher refuses rom/mGBA below %TEMP%
std::string ascii_safe_path(const std::string& dir);  // 8.3 short form when the path has non-ASCII characters (mGBA's Lua io.open is ANSI)
/* "" = free; else a sentence: an mGBA/Node of ANY Kanto Together package copy (e.g. the user started Host.cmd) is running, or its default port 8788 is taken. */
std::string running_instance_block();
/* Run a hidden process to completion (patcher) capturing its output. false if it could not start; *code = exit code. */
bool run_hidden(const std::string& cmdline, const std::string& cwd, const std::string& desktop, DWORD timeout_ms, std::string* output, DWORD* code,
                const std::atomic<bool>* cancel = nullptr);   // cancel set -> the process is killed, *code = 1
#endif

}  // namespace kanto
