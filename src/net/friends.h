#pragma once
/* hubnet friends client (see docs/FRIENDS.md). Single UI thread: call update() every frame.
 * Non-blocking; reconnects automatically (backoff 1s..30s). */

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace hubnet {

/* ---- identity ---- */
struct Identity {
    std::string user_id;    // 26 chars base32 lowercase (16 bytes)
    std::string secret;     // 52 chars base32 lowercase (32 bytes)
    int64_t created = 0;    // unix seconds
    bool valid() const { return !user_id.empty() && !secret.empty(); }
};
/* Loads <data_dir>/identity.json; if missing adopts <vault_dir>/_identity/identity.json;
 * if neither exists generates a new one. Always mirrors to the vault (when vault_dir != "").
 * Dirs are paths with or without trailing separator. */
Identity identity_load_or_create(const std::string& data_dir, const std::string& vault_dir);
Identity identity_generate();
/* After a vault sync/import: if <vault_dir>/_identity/identity.json is a valid, different identity with an
 * OLDER "created" than <data_dir>/identity.json, back the current one up to
 * <data_dir>/identity-backup-<timestamp>.json and switch identity.json to the vault's. Returns true (and
 * fills *adopted) when it switched. Pure file logic: does not touch any running client. */
bool identity_adopt_vault_if_older(const std::string& data_dir, const std::string& vault_dir, Identity* adopted);
/* Reads an identity.json-format file (validates both fields). */
bool identity_read_file(const std::string& path, Identity* id);
/* Unconditionally makes `v` the current identity: backs up a different existing identity.json to
 * identity-backup-<timestamp>.json, then writes identity.json and the vault mirror. Pure file logic. */
bool identity_adopt(const std::string& data_dir, const std::string& vault_dir, const Identity& v);
std::string friend_code(const std::string& user_id);   // "RH-7KQ2-M9XD"

/* ---- data ---- */
enum class PresenceState { Offline, Online, InLobby, InGame };

struct Status {
    PresenceState state = PresenceState::Offline;   // set Offline while connected = invisible
    std::string game_id, game_title, system;
    std::string lobby_code;       // only visible to friends
    bool joinable = false;
    int players = 0, max_players = 0;
    int64_t updated = 0;
};

struct Friend {
    std::string uid, code, name;
    Status status;
    int64_t since = 0;
    std::string note;
    bool online() const { return status.state != PresenceState::Offline; }
};

struct Request { std::string uid, code, name; int64_t at = 0; };

struct Invite {
    std::string lobby_code, game_id, game_title, system;
    int64_t expires_at = 0;   // same time base as server "at" (ms since epoch)
};

struct Message {
    std::string id, from, from_name, to;
    bool invite = false;      // kind == "invite"
    std::string text;
    Invite inv;
    int64_t at = 0;
    bool mine = false;        // from == my uid
};

struct Event {
    enum Kind { FriendOnline, FriendOffline, PresenceChanged, RequestReceived, RequestResolved,
                FriendAdded, FriendRemoved, MessageReceived, InviteReceived, Error,
                IdentityAdopted,
                InviteResult,     // IdentityAdopted: text = new friend code ("RH-XXXX-XXXX"), uid = new uid
                UsernameChecked,  // answer to check_username(): ref, name, ok (= available), yours, text = reason when not (taken|held|reserved|profanity|...)
                UsernameResult,   // answer to claim_username(): ref, name, ok, text = reason when not
                UsernameChanged };// this account's username is now `name` (welcome / claimed on another PC); ok=false + empty name = needs a new one
    Kind kind = Error;
    std::string uid;          // the friend / requester / sender (empty for some Errors)
    std::string name;
    std::string text;         // RequestResolved: "accepted"|"declined"|"cancelled"; Error: reason
    Message msg;              // MessageReceived / InviteReceived
    Invite invite;            // InviteReceived
    bool automatic = false;   // FriendAdded: made by the server because you joined a room / played together
    std::string detail;       // Error: the server's "detail" (e.g. "friends-full"); empty if none
    std::vector<std::string> ok_uids, failed_uids;   // InviteResult: per-target delivery outcome
    std::vector<std::string> failed_reasons;         // InviteResult: parallel to failed_uids
    int ref = 0;                                     // Username*: the id check_username()/claim_username() returned
    bool ok = false;                                 // Username*: available / claimed
    std::vector<std::string> suggestions;            // Username*: alternatives when taken / not allowed
    bool yours = false;                              // UsernameChecked: the name is already this account's
};

/* ---- dedupe (pure; the list is keyed by uid) ----
 * dedupe_friends: one entry per uid (empty uids and `self_uid` dropped). Duplicates merge into the first
 * occurrence: the more "present" status wins (InGame > InLobby > Online > Offline, then newer `updated`),
 * the earliest `since` and a non-empty name/code/note are kept.
 * dedupe_requests: also one entry per uid; a person who is already a friend (or yourself) has no request;
 * a person with both an incoming and an outgoing request keeps only the incoming one (you can accept). */
/* True when a server "error" reply to retire_old means it can never succeed (the saved old secret is wrong), so the
 * client must drop pending-retire.json instead of resending it forever. */
bool retire_error_is_terminal(const std::string& ref, const std::string& detail);
void dedupe_friends(std::vector<Friend>& friends, const std::string& self_uid = "");
/* Different uids, same display name. Returns the OFFLINE entries that look like a person's older identity.
 *  confirmed=true : the uid is in `retired` (the server's alias map / <data_dir>/retired-ids.txt says it was replaced). Safe to merge in bulk.
 *  confirmed=false: heuristic only (the fresher same-name entry was seen >= `gap_ms` later). Could be a genuinely different friend,
 *                   so the UI must ask per entry and never bulk-remove these. A merely-online same-name entry is NOT evidence.
 * Names compare case-insensitively, trimmed. */
struct StaleEntry { std::string uid; bool confirmed = false; };
std::set<std::string> read_retired_ids(const std::string& data_dir);
std::vector<StaleEntry> stale_same_name(const std::vector<Friend>& friends, const std::set<std::string>& retired = {},
                                        int64_t gap_ms = 7 * 86400 * 1000LL);
/* Every identity older than `current_uid` that this PC still knows and the server has not confirmed retired: pending-retire.json,
 * <data_dir>/identity-backup*.json and <vault>/_identity/*.json (other than identity.json). Skips uids listed in <data_dir>/retired-ids.txt. */
std::vector<Identity> older_identities(const std::string& data_dir, const std::string& vault_dir, const std::string& current_uid);
void dedupe_requests(const std::vector<Friend>& friends, std::vector<Request>& incoming, std::vector<Request>& outgoing,
                     const std::string& self_uid = "");

class FriendsClient {
  public:
    FriendsClient();
    ~FriendsClient();
    FriendsClient(const FriendsClient&) = delete;
    FriendsClient& operator=(const FriendsClient&) = delete;

    /* Optional, before start(): where identity/conversations live. The hub passes
     * hub::data_dir() and saves::vault_dir(). Tests pass temp dirs. If never called, uses
     * ".\\userdata\\" and no vault mirror. */
    void set_paths(const std::string& data_dir, const std::string& vault_dir);

    /* server "" = default_server(). name = display name (sanitized to 1..24 chars). */
    bool start(const std::string& server, const std::string& name);
    void stop();
    void update();

    bool connected() const;               // welcome received
    std::string status_text() const;      // "Connecting...", "Online", "Reconnecting in 4s", ...
    std::string my_code() const;          // "RH-XXXX-XXXX" (known immediately after start)
    std::string my_uid() const;
    std::string my_secret() const;        // the account secret (only for authenticating the lobby seat)
    /* "Automatically add people I play with as friends": sent in every hello and live via "prefs". Default on. */
    void set_auto_friend(bool on);
    std::set<std::string> retired_ids() const;   // uids the server confirmed replaced (retired-ids.txt)

    /* actions (ignored/queued-as-dropped while disconnected, except set_status/rename which
     * are remembered and re-sent on reconnect) */
    void set_status(const Status& s);
    void add_friend(const std::string& code);
    void accept(const std::string& uid);
    void decline(const std::string& uid);
    void cancel(const std::string& uid);
    void remove_friend(const std::string& uid);
    void block(const std::string& uid);
    void unblock(const std::string& uid);
    void send_message(const std::string& uid, const std::string& text);
    void send_invite(const std::vector<std::string>& uids, const Invite& inv);
    void ack(const std::vector<std::string>& ids);
    void rename(const std::string& name);   // legacy free-text display name (old servers); with usernames use claim_username()

    /* ---- usernames (docs/FRIENDS.md "Usernames"). All need a welcomed connection; they return the request ref, or 0 when
     * offline (the caller then offers "claim it later"). Answers arrive as Username* events. */
    int check_username(const std::string& name);    // is it free (or already mine)?
    int claim_username(const std::string& name);    // claim, or change to it (the old name is held for kUsernameHoldDays)
    void add_friend_username(const std::string& username);
    std::string username() const;                   // "" until claimed
    bool needs_new_name() const;                    // the server flagged this older account: pick a username
    bool username_known() const;                    // a welcome has told us (username() / needs_new_name() are meaningful)
    void request_history(const std::string& uid);

    /* state */
    const std::vector<Friend>& friends() const;
    const std::vector<Request>& incoming_requests() const;
    const std::vector<Request>& outgoing_requests() const;
    const std::vector<std::string>& blocked() const;
    const Friend* find_friend(const std::string& uid) const;

    /* conversations: oldest first, last 200, persisted to <data_dir>/friends/<uid>.json */
    const std::vector<Message>& conversation(const std::string& uid);
    int unread(const std::string& uid) const;
    int total_unread() const;
    void mark_read(const std::string& uid);

    bool poll_event(Event* out);

    /* Call after a sync/import finished. If the vault holds an older identity, adopts it (backup made),
     * reconnects with it if the client was started, queues Event::IdentityAdopted and returns true. */
    bool adopt_vault_identity_if_older();

    /* Replaces this PC's identity with `v` (linked from another PC or imported from a file): backs up
     * the current identity, reconnects with `v` if started, queues Event::IdentityAdopted. */
    bool adopt_identity(const Identity& v);

  private:
    void apply_adopted(const Identity& v);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hubnet
