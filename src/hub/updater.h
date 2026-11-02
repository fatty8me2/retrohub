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

#include <functional>
#include <map>
#include <string>
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
    bool has_asset = false;     // the platform asset was found and passed the trust/size checks
    std::string url, digest;
    std::string sums_url;       // SHA256SUMS.txt asset of the same release ("" when the release has none)
    long long size = 0;
};
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
                      const std::string* sums, const std::string& api_digest);
const char* verdict_message(Verdict v);
std::string sha256_file_hex(const std::string& path);   // "" on error

struct FetchSpec {
    std::string zip_url, sums_url, tag, digest;
    long long size = 0;
};
/* Downloads (curl) + verifies the zip into zip_path. Caller must have vetted the URLs. On failure the file
 * is deleted and *err says why. Used by start_update(); tests drive it against a 127.0.0.1 server. */
bool download_verified(const FetchSpec& f, const std::string& zip_path, std::string* err,
                       const std::function<void(const std::string&)>& on_progress_line);

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

/* ---- staged files kept for a retry ("updates\staged.json" beside "updates\staged\") ---- */
struct StagedInfo {
    std::string version;
    long long files = 0, bytes = 0;
};
StagedInfo scan_staged(const std::string& staged_dir);                       // counts the regular files under the dir
std::string staged_manifest_json(const StagedInfo& s);
bool parse_staged_manifest(const std::string& text, StagedInfo* out);
/* The manifest names `version`, matches what is on disk, and both required exes are there: the download can be skipped. */
bool staged_reusable(const std::string& updates_dir, const std::string& version);
bool write_staged_manifest(const std::string& updates_dir, const std::string& version);
}

} // namespace hubupdate
