#pragma once
/* Optional cloud saves: a second sync backend beside sync folders (see savestore.h sync_folder).
 *
 * Backend: the RetroHub Worker's /saves endpoints (Cloudflare R2), authenticated with the friends account secret
 * (HMAC-signed requests; the secret itself never leaves the PC). Design: the cloud is mirrored into a local
 * "mirror" folder (sibling of the vault, never inside it) and then sync_folder(mirror) does the vault<->mirror merge,
 * so conflict handling is exactly the folder-sync logic (newer wins, loser kept in _conflicts/ on both sides).
 * Per run:  list remote -> download changed objects into the mirror (resumable, sha-verified) -> sync_folder ->
 * upload changed mirror files (If-Match/If-None-Match; a lost race is merged on the next run, never overwritten).
 * Deletions are not propagated (same as folder sync). The _identity folder is never uploaded.
 * Blocking; call from the existing sync job thread. Safe to cancel by killing the process: every step is
 * idempotent and state is only recorded after a transfer is verified. */

#include "savestore.h"

#include <string>

namespace hub::saves {

struct CloudConfig {
    std::string base_url;      // e.g. "https://smash-netplay.xxx.workers.dev" (no trailing slash needed)
    std::string user_id;       // friends identity user_id (26 base32)
    std::string secret;        // friends identity secret
    bool include_states = false;
    std::string mirror_dir;    // empty = <parent of vault>\cloud_mirror\<user_id[0..8]>
    int max_retries = 4;       // per request, exponential backoff on network errors / 5xx / 429
};

struct CloudStatus {
    bool enabled = false;      // server answered /saves/list
    bool unavailable = false;  // server says cloud saves are off (503) or the account is not registered (401)
    long long used = 0, quota = 0;
    int files = 0;
    std::string error;
};

/* Cheap check for the UI status line ("Cloud saves: 12 files, 3.1 MB of 500 MB"). */
CloudStatus cloud_status(const CloudConfig& cfg);

/* Full two-way sync of the vault with the cloud. SyncReport.pushed/pulled count uploaded/downloaded objects;
 * conflicts are those sync_folder preserved (plus remote-vs-pending local edits kept in _conflicts/). */
SyncReport sync_cloud(const CloudConfig& cfg);

/* Pure helpers (exposed for tests). */
std::string cloud_sign(const std::string& secret, const std::string& method, const std::string& path_and_query,
                       long long ts_ms, const std::string& body_sha);
std::string sha256_hex_of(const std::string& data);

} // namespace hub::saves
