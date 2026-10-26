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
 * UI contract: poll the getters each frame; when installer_started() turns
 * true, quit the app immediately (the installer is waiting for the exit). */

#include <functional>
#include <map>
#include <string>

namespace hubupdate {

void check_async(bool force);      // force=false honours launch_check_disabled(); once per run
bool checking();
bool update_available();
bool check_failed();
std::string latest_version();      // release tag, e.g. "v1.8.0"
std::string release_notes();       // plain text, markdown stripped
std::string current_version();     // HUB_VERSION_STR
std::string status();              // human-readable status / error line

void start_update();               // download + verify + launch installer (background thread)
bool downloading();                // true from start_update() until failure or handoff
float download_progress();         // 0..1, or -1 when unknown
bool installer_started();          // true => quit the app now
std::string manual_download_url(); // release page to download by hand (shown when verification fails)

void skip_latest();                // remember "skip this version"
bool latest_skipped();
bool launch_check_disabled();      // env RETROHUB_NO_UPDATE_CHECK / SSB64_NO_UPDATE_CHECK or --no-update-check

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
}

} // namespace hubupdate
