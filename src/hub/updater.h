#pragma once
/* RetroHub auto-updater (ported from Smash Classic's verified Updater.cpp).
 *
 * Checks the latest GitHub release of fatty8me2/smash-classic (asset
 * BattleShip-windows.zip), accepts only a strictly newer semver, downloads
 * with curl, verifies size + SHA-256, then launches a hidden PowerShell
 * installer that waits for this process to exit, backs up, overlays the files
 * in the zip onto exe_dir() (never deleting anything), rolls back on failure
 * and relaunches RetroHub.exe.
 *
 * UI contract: poll the getters each frame; when installer_started() turns
 * true, quit the app immediately (the installer is waiting for the exit). */

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
    long long size = 0;
};
Release parse_release(const std::string& json);
}

} // namespace hubupdate
