#pragma once
/* RetroHub auto-updater (ported from Smash Classic's verified Updater.cpp).
 *
 * Checks the latest GitHub release of fatty8me2/retrohub (asset
 * BattleShip-windows.zip), accepts only a strictly newer semver, downloads
 * with curl, verifies size + SHA-256, unpacks with tar.exe, then runs a TEMP copy of
 * RetroHub.exe as `--apply-update` (see update_apply.h): it waits for the old
 * processes, backs up, overlays the files onto exe_dir() (never deleting anything),
 * rolls back on failure and relaunches RetroHub.exe.
 *
 * NEVER DURING A GAME. start_update() may download and unpack while a game runs, but the hand-off to the installer
 * (RetroHub quits, files are replaced, RetroHub restarts) waits until no hub session runs and nothing else from the
 * install folder is running (update_deferred() is true meanwhile: the bell says "Update ready: installs when you
 * finish playing"). The installer itself never terminates a game (update_apply.h). Callers need no checks.
 *
 * UI contract: call tick() every frame (UI thread) and poll the getters; when installer_started() turns
 * true, quit the app (the installer is waiting for the exit). */

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hubupdate {

void check_async(bool force);      // force=false honours launch_check_disabled(); once per run
bool checking();
bool update_available();
bool check_failed();
std::string latest_version();      // release tag, e.g. "v1.8.0"
std::string release_notes();       // plain text, markdown stripped
std::string current_version();     // HUB_VERSION_STR
std::string status();              // human-readable status / error line

void set_game_probe(bool (*fn)());     // RetroHub.exe: "is a game active?" (hub::game_active, incl. a full-screen app); passed to the applier as --game-active
void set_session_probe(bool (*fn)());  // RetroHub.exe: "is a hub game session running or starting?" (hub::session_active or the start animation); the install waits while true
void start_update();             // download + verify + unpack in the background; hands over to the installer once no game runs. Idempotent while busy.
void tick();                     // every frame, UI thread: starts the deferred hand-off when nothing is running any more
bool downloading();                // true from start_update() until failure or handoff (including while the install waits for a game)
bool update_deferred();            // downloaded and verified, waiting for the game(s) to close
float download_progress();         // 0..1, or -1 when unknown
bool installer_started();          // true => quit the app now
void shutdown();                   // the app is leaving: stop waiting, drop the unused installer copy, join the startup pass
std::string manual_download_url(); // release page to download by hand (shown when verification fails)

void skip_latest();                // remember "skip this version"
bool latest_skipped();
bool launch_check_disabled();      // env RETROHUB_NO_UPDATE_CHECK / SSB64_NO_UPDATE_CHECK or --no-update-check

/* Startup pass (once per run, background): repairs an interrupted update from its backup (apply::recover_interrupted) and reads
 * the failure note an earlier installer left (userdata\update_status.json), so the bell can show it ONCE. Poll
 * take_startup_notes() each frame: true once, when the pass is done (out may be empty). Never shows anything itself. */
void startup_pass_async();
bool take_startup_notes(std::vector<std::string>* out);

extern const char* const kReadyText;       // "Update ready: installs when you finish playing" (About page / update popup status)
extern const char* const kReadyBellText;   // "Update ready: installs after your game" (the bell cuts a headline off at about 42 characters)

/* Pure helpers, exposed for hub_updater_test. */
namespace detail {
int compare_versions(const std::string& a, const std::string& b);   // -1/0/1
bool is_trusted_download_url(const std::string& url);
std::string strip_markdown(const std::string& md);
struct Release {
    bool ok = false;
    std::string tag, body;
    bool has_asset = false;     // the platform asset (BattleShip-windows.zip) was found and passed the trust/size checks
    std::string url, digest;
    std::string sums_url;       // SHA256SUMS.txt asset of the same release ("" when the release has none)
    long long size = 0;
    std::string sig_url;        // SHA256SUMS.txt.sig of the same release ("" when the release has none: it is refused)
    bool has_hub_asset = false; // RetroHub-windows.zip (RetroHub's files only) was found and passed the same checks
    std::string hub_url, hub_digest;
    long long hub_size = 0;
};
extern const char* const kFullAssetName;   // "BattleShip-windows.zip": the whole install (every release ships it; old clients only know it)
extern const char* const kHubAssetName;    // "RetroHub-windows.zip": RetroHub's part only
extern const char* const kHubOnlyMarker;   // "# retrohub-only": a comment line in SHA256SUMS.txt (which is signed)
/* The marker line is `# retrohub-only smash-sha256 <sha256 of BattleShip.exe> smash-since vX.Y.Z`: this release's RetroHub files assume
 * exactly that Smash Classic build. valid is false for a missing, repeated or malformed marker (a bare "# retrohub-only" is malformed). */
struct HubOnlyMarker { bool valid = false; std::string smash_sha256, smash_since; };
HubOnlyMarker parse_hub_only_marker(const std::string& sums_text);
/* Which payload to install. The small RetroHub-only zip is used ONLY when the release has it AND its marker names the BattleShip.exe that is
 * installed right now (installed_smash_sha256, "" when it could not be read => never a match). Anyone else (an older Smash Classic, a
 * missing or unreadable BattleShip.exe, a bad marker) gets the full BattleShip-windows.zip, exactly as before; a release that has only the
 * hub zip and no match has no usable payload (None). */
enum class Payload { None, Full, HubOnly };
Payload choose_payload(const Release& r, const std::string& sums_text, const std::string& installed_smash_sha256);
/* ---- release signing (shell-M2) ----
 * Every update must carry SHA256SUMS.txt.sig: an ECDSA P-256 / SHA-256 signature (DER) over the exact bytes of SHA256SUMS.txt, made with the
 * publisher's private key (kept off-line; the packager signs, see package-windows.ps1). Only a list that verifies is believed, and only then
 * is the zip checked against it. Windows CNG does the maths; the public key is built in. */
extern const char* const kSigAssetName;       // "SHA256SUMS.txt.sig"
extern const char* const kReleasePubKeyHex;   // the publisher's public key, raw X||Y (64 bytes) as 128 hex digits
extern const char* const kUnsignedMessage;    // "This update isn't signed by RetroHub's publisher, so it wasn't installed."
/* Strict DER SEQUENCE{INTEGER r, INTEGER s} -> 64-byte r||s (each left-padded to 32). false for anything malformed: truncated, trailing
 * bytes, wrong or overlong or non-minimal lengths, negative or zero r/s, r/s wider than 32 bytes. */
bool der_to_raw_rs(const void* der, size_t len, unsigned char out[64]);
/* ECDSA P-256 verify: pub = raw X||Y (64 bytes), SHA-256 of msg, sig = DER. */
bool verify_ecdsa_p256(const unsigned char pub[64], const void* msg, size_t msg_len, const void* sig_der, size_t sig_len);
#ifdef RETROHUB_UPDATER_TESTING   // defined only on the hub_updater_test target: the shipping RetroHub.exe has no way to change the trusted key
/* Tests only: replace the built-in public key (nullptr restores it). */
void set_pubkey_for_test(const unsigned char pub[64]);
#endif
/* Is this SHA256SUMS.txt authentic? (the ONE gate: no signature, a malformed one or a wrong one => false and *why_not = kUnsignedMessage) */
bool sums_are_trusted(const std::string& tag, const std::string& sums_text, const std::string& sig_bytes, std::string* why_not);
Release parse_release(const std::string& json);

/* ---- integrity verification (SHA256SUMS.txt + GitHub's asset digest) ---- */
/* First release that ships SHA256SUMS.txt. Releases >= this REQUIRE it; older ones are legacy. */
extern const char* const kFirstChecksumRelease;
bool checksums_required(const std::string& tag);
/* "<64 hex>  name" / "<64 hex> *name" lines -> name -> lowercase hex. A name listed twice with different
 * hashes maps to "" (poisoned => verification fails). Malformed lines are ignored. */
std::map<std::string, std::string> parse_sha256sums(const std::string& text);
enum class Verdict { Ok, OkLegacy, NoSums, BadSums, ZipMismatch, DigestMismatch };
/* sums == nullptr means the release has no (or an unreadable) SHA256SUMS.txt. api_digest is GitHub's
 * "sha256:..." (may be empty). actual_hex is the SHA-256 of the downloaded zip. */
Verdict verify_hashes(const std::string& tag, const std::string& actual_hex,
                      const std::string* sums, const std::string& api_digest, const std::string& asset_name = "BattleShip-windows.zip");
const char* verdict_message(Verdict v);
std::string sha256_file_hex(const std::string& path);   // "" on error

struct FetchSpec {
    std::string zip_url, sums_url, tag, digest;
    long long size = 0;
    std::string asset_name;    // the name SHA256SUMS.txt lists the zip under ("" => BattleShip-windows.zip)
    std::string sums_text;     // when set, the sums were already fetched (and trusted): not fetched again
    std::string sig_url;       // SHA256SUMS.txt.sig
    std::string sig_bytes;     // when sums_text is set: its signature (already fetched)
};
/* Fetches SHA256SUMS.txt from sums_url into *text (curl, size-capped). false + *err when it cannot be had. */
bool fetch_sums(const std::string& sums_url, const std::string& scratch_path, std::string* text, std::string* err);
/* The signature file (tiny, size-capped). false + *err = kUnsignedMessage when it cannot be had. */
bool fetch_sig(const std::string& sig_url, const std::string& scratch_path, std::string* bytes, std::string* err);
/* ---- a slow connection must be able to finish: no hard time cap, a stall detector, retries, and resume ---- */
constexpr int kStallBytesPerSec = 1024;   // curl gives up when the speed stays below this ...
constexpr int kStallSeconds = 60;         // ... for this long (a stalled link), however slow the rest of the download is
constexpr int kRetryCount = 3, kRetryDelaySec = 2;
/* curl's switches for the zip download (everything except the output file and the url). resume => "-C -" (continue the partial file). */
std::string download_flags(bool resume);
/* What a partial download is recorded as, in "<zip>.resume" beside it. Two releases never share a record: tag + asset name + exact size +
 * the SHA-256 the signed SHA256SUMS.txt lists for that asset must all match. */
struct ResumeKey { std::string tag, asset, sha256; long long size = 0; };
std::string resume_record_json(const ResumeKey& k);
/* May the partial file (partial_size bytes) be continued? Only when its record names exactly `want`, and the file is smaller than the
 * expected size (a full-size file that is still here failed verification once: never resumed again). */
bool resume_eligible(const std::string& record_text, const ResumeKey& want, long long partial_size);

/* Downloads (curl) + verifies the zip into zip_path. Caller must have vetted the URLs. A partial file of the SAME release (see ResumeKey)
 * is continued; one that fails the size or SHA-256 check is deleted for good (with its record). A failed transfer keeps its partial file
 * for the next try. *err says why. Used by start_update(); tests drive it against a 127.0.0.1 server. */
bool download_verified(const FetchSpec& f, const std::string& zip_path, std::string* err,
                       const std::function<void(const std::string&)>& on_progress_line);

/* ---- system tools (curl.exe, tar.exe) are always run by absolute path ----
 * A bare "curl" makes Windows look in the application's own folder first, so a planted curl.exe beside RetroHub.exe would run.
 * The updater only ever runs <Windows system folder>\curl.exe and tar.exe. */
std::string system_dir();   // GetSystemDirectoryW as UTF-8, no trailing slash ("" when unknown)
/* dir + "\\" + exe_name when that file exists; otherwise false and *err says (in plain words) which file is missing. */
bool resolve_system_tool(const std::string& dir, const char* exe_name, std::string* path, std::string* err);
bool find_system_tool(const char* exe_name, std::string* path, std::string* err);   // resolve_system_tool(system_dir(), ...)

/* ---- worker threads are owned, never detached ----
 * The check, the download and the hand-off each run on an OwnedThread that shutdown() joins (with a bounded wait), so nothing touches
 * this file's statics after the app has started to exit. */
class OwnedThread {
public:
    OwnedThread() = default;
    OwnedThread(const OwnedThread&) = delete;
    OwnedThread& operator=(const OwnedThread&) = delete;
    ~OwnedThread();                                       // a thread still running here is detached (shutdown() should have ended it)
    /* Runs fn on a new thread. A previous run is joined first (it is waited for up to wait_prev_ms); false when that one is still going. */
    bool start(std::function<void()> fn, unsigned wait_prev_ms = 3000);
    bool running() const;                                 // started and fn has not returned yet
    bool join_for(unsigned ms);                           // true when no thread is left (joined, or never started); false when still running after ms
    void detach();                                        // last resort after a failed join_for
private:
    bool join_locked(unsigned ms);
    mutable std::mutex mu_;
    std::thread th_;
    std::shared_ptr<std::atomic<bool>> done_;
};
/* The one place a child process (curl, tar) is run: output lines go to onLine, timeout_ms ends it by its HANDLE. Every child is registered,
 * so shutdown() can end a running one at once (its exit code is then non-zero); no child is started once shutdown() began. */
int run_command(const std::string& cmd, const std::function<void(const std::string&)>& onLine, unsigned long timeout_ms = 0xFFFFFFFFul);
int live_child_count();   // children currently registered (tests)
#ifdef RETROHUB_UPDATER_TESTING
void reset_quit_for_test();   // tests only: undo shutdown()'s "leaving" state
#endif

/* ---- the deferred hand-off (pure state machine; time is injected) ---- */
constexpr unsigned long long kSettleMs = 2500;   // nothing running for this long before the installer starts (a game's exit is followed by save work)
struct Gate {
    bool idle = false;                   // nothing was running at the last tick
    unsigned long long idle_since = 0;   // when that began
    bool fired = false;                  // Go was returned: the hand-off is under way
};
enum class GateState {
    NotReady,    // no downloaded update to hand over
    Held,        // downloaded, but a session or a leftover game is running: waits (bell: kReadyBellText, status: kReadyText)
    Settling,    // nothing running, waiting out kSettleMs
    Go,          // start the installer now (returned exactly once)
    Done,        // already handed over
};
GateState gate_step(Gate& g, bool ready, bool session_busy, bool install_busy, unsigned long long now_ms, unsigned long long settle_ms = kSettleMs);

/* ---- retry spacing and the unpack timeout (pure) ---- */
constexpr unsigned long long kCheckRetryMs = 5ull * 60 * 1000;   // after a FAILED check, the next automatic check waits this long (a dead network is not hammered)
constexpr unsigned long kTarTimeoutMs = 10ul * 60 * 1000;        // a stuck tar.exe is ended after this long and the update fails cleanly
/* May an automatic (not user-forced) check run now? checked = an earlier check in this session finished (success or a final answer);
 * last_fail_ms = tick of the last failed check (0 = none). A failed check does not count as "checked", but waits kCheckRetryMs. */
inline bool check_retry_due(bool checked, unsigned long long last_fail_ms, unsigned long long now_ms) {
    if (checked) return false;
    return last_fail_ms == 0 || now_ms < last_fail_ms || now_ms - last_fail_ms >= kCheckRetryMs;
}

/* ---- staged files kept for a retry ("updates\staged.json" beside "updates\staged\") ---- */
struct StagedFile { std::string path; long long size = 0; std::string sha256; };   // path: relative to the staged dir, '/' separators
struct StagedInfo {
    std::string version;
    long long files = 0, bytes = 0;
    bool hub_only = false;   // unpacked from RetroHub-windows.zip (no Smash Classic files); manifests without "payload" are full
    std::string smash_sha256; // hub-only: the BattleShip.exe hash the (signed) marker named; the applier re-checks the installed exe against it
    std::vector<StagedFile> list;   // every unpacked file with its SHA-256, taken right after the unpack ("list" in staged.json). A manifest without it
                                    // (written by RetroHub 1.41.0 or older) is not trusted for reuse: the folder is discarded and downloaded again.
};
/* Every regular file under staged_dir with its size and SHA-256, sorted by path. false when a file cannot be read or *cancel became true. */
bool hash_staged(const std::string& staged_dir, std::vector<StagedFile>* out, const std::atomic<bool>* cancel = nullptr);
/* The folder holds EXACTLY these files (no extra, none missing) and every size and SHA-256 matches. An empty list never matches. */
bool verify_staged_list(const std::string& staged_dir, const std::vector<StagedFile>& list, const std::atomic<bool>* cancel = nullptr);
StagedInfo scan_staged(const std::string& staged_dir);                       // counts the regular files under the dir
std::string staged_manifest_json(const StagedInfo& s);
bool parse_staged_manifest(const std::string& text, StagedInfo* out);
/* The manifest names `version`, its per-file SHA-256 list matches what is on disk exactly, and the required exes are there (RetroHub.exe, plus BattleShip.exe for a
 * full payload): the download can be skipped. *hub_only (optional) says which kind is staged. */
bool staged_reusable(const std::string& updates_dir, const std::string& version, bool* hub_only = nullptr, std::string* smash_sha256 = nullptr,
                    const std::atomic<bool>* cancel = nullptr);
/* A hub-only manifest without a valid smash_sha256 is not reusable (the hand-off could not re-check the installed Smash Classic). */
bool write_staged_manifest(const std::string& updates_dir, const std::string& version, bool hub_only = false, const std::string& smash_sha256 = std::string(),
                           const std::atomic<bool>* cancel = nullptr);   // hashes every file: the (large) cost of making a staged folder trustworthy
}

} // namespace hubupdate
