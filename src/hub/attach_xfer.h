#pragma once
/* DM attachments: upload a file (screenshot, challenge save state, hack patch) for friends and download one that a friend sent.
 * The Worker side is /saves/v2/attach (8 MB per file, 7-day expiry, inside the sender's 50 MB cloud pool; see
 * BattleShip server/cloudflare/src/r2budget_attach.js). The bytes never ride the chat socket: the DM only carries a hubnet::Attach reference.
 *
 * Every transfer runs on its own worker thread (WinHTTP via saves::CloudHttp). The UI thread starts a job and polls it each frame; nothing here
 * blocks the UI and nothing opens a window. Errors are plain English (attach_error_text). */
#include "friends.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hub::attach {

constexpr int64_t kMaxBytes = 8 * 1024 * 1024;   // per file; the Worker enforces the same

/* Who we are and where the Worker is. base_url is http(s)://host[:port] (not ws://). The hub installs a provider that reads the friends
 * identity and the lobby server; tests install one that points at the local wrangler. */
struct Account {
    std::string base_url, user_id, secret, device_id;
    bool valid() const { return !base_url.empty() && !user_id.empty() && !secret.empty(); }
};
void set_account_provider(std::function<Account()> fn);
/* ws://host -> http://host, wss:// -> https:// (anything else unchanged, trailing slashes dropped). */
std::string http_base_from_server(const std::string& server_url);

/* ---- pure helpers (unit tested) ---- */
/* Plain-English text for a failed call. status 0 = no connection. code = the Worker's JSON "error" (may be empty). Never empty. */
std::string attach_error_text(int http_status, const std::string& code);
/* Client-side check before any bytes are read. Returns "" when the size is fine, else the message to show (names the size and what to do). */
std::string attach_size_problem(int64_t bytes, const std::string& kind);
/* A safe display/file name: no path parts or reserved characters, collapsed spaces, <= 64 chars, never empty ("file"). */
std::string sanitize_name(const std::string& raw);
/* Percent-encoding for a query value. */
std::string url_encode(const std::string& s);
/* "shot" | "state" | "patch". */
bool valid_kind(const std::string& kind);
/* "2.4 MB" / "310 KB" style size text. */
std::string size_text(int64_t bytes);

/* ---- jobs ---- */
struct Result {
    int job = 0;
    bool upload = false;          // else download
    bool done = false;            // finished (ok or not)
    bool ok = false;
    int percent = 0;              // coarse: 0 queued, 10 reading, 40 sending/receiving, 90 verifying, 100 done
    int http_status = 0;
    std::string code;             // Worker error code when it failed ("" otherwise)
    std::string error;            // plain English, set when done && !ok
    hubnet::Attach ref;           // upload: what to put in the DM (send it with FriendsClient::send_message); download: the reference downloaded
    std::string path;             // upload: the source file; download: the finished file
};

/* Starts an upload of `path` as `kind` for these friends (uids, 1..8; they must be your friends). display_name is shown to them
 * (sanitised). Returns the job id (always > 0; a job that fails at once is already done with its error). Identical content already sent by
 * this account and still live is reused server side (no new upload). */
int upload(const std::string& path, const std::string& kind, const std::string& display_name, const std::vector<std::string>& recipients);
/* Starts the download of a friend's attachment into `dest_path` (atomic: written as dest.part, sha256-checked, then renamed; the parent
 * folder is created). Only the recipient can download; an expired reference fails at once without a request. */
int download(const hubnet::Attach& att, const std::string& dest_path);
/* Latest state of a job (false = unknown id or already collected by forget()). */
bool poll(int job, Result* out);
/* Drops a finished job's bookkeeping. */
void forget(int job);
/* Blocks until the job is done (tests and tools only; never call this from the UI thread). */
bool wait(int job, Result* out, int timeout_ms = 60000);

} // namespace hub::attach
