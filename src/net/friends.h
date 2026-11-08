#pragma once
/* hubnet friends client (see docs/FRIENDS.md). Single UI thread: call update() every frame.
 * Non-blocking; reconnects automatically (backoff 1s..30s). */

#include "admin_logic.h"
#include "invite_logic.h"
#include "library_share.h"

#include <cstdint>
#include <map>
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
/* Does a valid identity already exist (data dir or vault mirror)? Pure file check, creates nothing: call BEFORE start() to tell an existing account from a brand-new install. */
bool identity_exists(const std::string& data_dir, const std::string& vault_dir);
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
/* Per-Windows-user identity backup: %LOCALAPPDATA%\RetroHub\identity-backup.json (env RETROHUB_IDENTITY_BACKUP overrides the path, for tests).
 * Path is "" (feature off) in QA / autotest runs. Written (atomically) only for a server-confirmed account; read only by a fresh install. */
std::string identity_backup_path();
bool identity_backup_allowed(const std::string& server, const std::string& name);   // false for qa- names, non-production servers, QA runs
bool identity_backup_save(const Identity& id);
/* Fresh install only (no <data_dir>/identity.json and no vault mirror): copies a valid backup to identity.json. Never overwrites an identity. */
bool identity_backup_adopt_if_fresh(const std::string& data_dir, const std::string& vault_dir, Identity* adopted);
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
    std::string prof_avatar, prof_game, prof_status;   // tiny public profile (see common/profile.h); empty = unset
};

struct Friend {
    std::string uid, code, name;
    Status status;
    int64_t since = 0;
    std::string note;
    int64_t lib_ver = 0;   // version of the games list this friend shares (0 = none); see library_share.h
    bool online() const { return status.state != PresenceState::Offline; }
};

struct Request { std::string uid, code, name; int64_t at = 0; };

struct Invite {
    std::string lobby_code, game_id, game_title, system;
    int64_t expires_at = 0;   // same time base as server "at" (ms since epoch)
};

/* A file that rides a DM (Plan 4): the bytes live in R2 (Worker /saves/v2/attach, 8 MB, 7 days), the DM carries only this reference.
 * kind: "shot" | "state" | "patch". expires = ms since epoch (same base as Message::at). Metadata is stamped by the server. */
struct Attach {
    std::string id, name, sha, kind;
    int64_t size = 0, expires = 0;
};
constexpr size_t kCardMax = 1536;   // server limit for Message::card

struct Message {
    std::string id, from, from_name, to;
    bool has_attach = false; Attach attach;   // kind "text" with an attached file (text may just be the file name)
    std::string card;         // opaque card payload (<= kCardMax), e.g. "RHC1:..." challenge cards (src/hub/challenge_logic.h); empty = none
    bool invite = false;      // kind == "invite"
    bool ask_host = false;    // kind == "ask" with host:true: "will you host this game?" (they do not have it)
    bool ask = false;         // kind == "ask": "may I join your game?" (game in inv.game_id/game_title/system; never stored in the conversation)
    std::string text;
    Invite inv;
    int64_t at = 0;
    bool mine = false;        // from == my uid
    bool react = false;       // kind == "react": a reaction to message `target` (never stored as a line)
    std::string target, emoji; bool on = true;
};

struct Event {
    enum Kind { FriendOnline, FriendOffline, PresenceChanged, RequestReceived, RequestResolved,
                FriendAdded, FriendRemoved, MessageReceived, InviteReceived, Error,
                IdentityAdopted,
                InviteResult,     // IdentityAdopted: text = new friend code ("RH-XXXX-XXXX"), uid = new uid
                UsernameChecked,  // answer to check_username(): ref, name, ok (= available), yours, text = reason when not (taken|held|reserved|profanity|...)
                UsernameResult,   // answer to claim_username(): ref, name, ok, text = reason when not
                UsernameChanged,
                AskReceived,      // a friend asks to join our game: uid, name, invite = the game they are in
                WatchedOnline,    // a friend you opted in to ("Notify me when they come online") went offline -> online: uid, name
                ReactionReceived, // a friend reacted to a DM line: uid, msg.target / msg.emoji / msg.on
                AskResult,
                Queued,           // an action made while offline was saved in the outbox: uid, name = kind (message|invite|block|...), text = plain sentence, msg.id = the pending conversation line id (messages)
                SendFailed,
                AdminResult, AdminJoinWaiting, AdminInviteUsed, InviteRedeemed };      // owner dashboard: answer to a ban/unban/approve/deny: uid, name (op), ok, text = plain sentence, detail = reason     // a queued or refused action will not happen: same fields, text = plain sentence ("Your message could not be sent (...)")      // answer to ask_to_join(): uid, ok, text = reason when not ("rate-limited"|"not-friends"|"undeliverable")// this account's username is now `name` (welcome / claimed on another PC); ok=false + empty name = needs a new one
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

/* ---- friends' play time (docs/FRIENDS.md "Play time") ----
 * One game's coarse total (whole minutes). The client sends its top kPlaytimeMaxGames games when a play session ends (only with
 * "Share my play time with friends" on) and fetches friends' totals with request_playtime(). */
struct PlayGame { std::string key, title; int minutes = 0; };
constexpr size_t kPlaytimeMaxGames = 30;
/* {"type":"playtime","share":bool,"games":[{k,t,m}...]}: games sorted by minutes (desc), minutes < 1 dropped, at most kPlaytimeMaxGames, empty when !share. */
std::string playtime_message_json(std::vector<PlayGame> games, bool share);
/* Parses the server's {"type":"playtime","friends":{uid:[{k,t,m}]}} reply (false when not that message). */
bool parse_playtime_reply(const std::string& text, std::map<std::string, std::vector<PlayGame>>* out);
/* ---- "Notify me when they come online" ----
 * The watched friends (per-friend opt-in, off by default). The client sends {"type":"watch_online","uids":[...]} when the list changes
 * and after every welcome; the server only pushes {"type":"friend_online"} for those friends (at most kWatchMax). */
constexpr size_t kWatchMax = 50;
/* Unique, non-empty uids, at most kWatchMax, in order. */
std::vector<std::string> watch_clean(const std::vector<std::string>& uids);
std::string watch_message_json(const std::vector<std::string>& uids);
/* Seconds -> whole minutes, rounded down (coarse on purpose). */
inline int playtime_minutes(unsigned long long seconds) { return (int)(seconds / 60 > 600000 ? 600000 : seconds / 60); }

/* ---- dedupe (pure; the list is keyed by uid) ----
 * dedupe_friends: one entry per uid (empty uids and `self_uid` dropped). Duplicates merge into the first
 * occurrence: the more "present" status wins (InGame > InLobby > Online > Offline, then newer `updated`),
 * the earliest `since` and a non-empty name/code/note are kept.
 * dedupe_requests: also one entry per uid; a person who is already a friend (or yourself) has no request;
 * a person with both an incoming and an outgoing request keeps only the incoming one (you can accept). */
/* True when a server "error" reply to retire_old means it can never succeed (the saved old secret is wrong), so the
 * client must drop pending-retire.json instead of resending it forever. */
bool retire_error_is_terminal(const std::string& ref, const std::string& detail);
/* A plain next step for a status text the client has stopped retrying on (the sign-in was rejected or replaced), or "" when the
 * status needs nothing from the person. `needs_account` is set when the fix is entering the account code in Settings > Account. */
std::string status_next_step(const std::string& status_text, bool* needs_account = nullptr);
/* True when `server` (a line the server stored, id = uuid) is the same line as `local` (this PC's own echo, id "local-..."): same
 * recipient, same text / card / file, sent within 10 minutes of each other. Used so a restart's history reply does not show a
 * line you sent twice. */
bool message_echo_matches(const Message& local, const Message& server);
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

/* ---- the PCs signed in to this account (per-PC credentials, docs/ACCOUNTS.md) ----
 * Every PC that connects with an up-to-date client gets its own secret from the server; the server keeps a small list. Locking a PC out
 * deletes its entry, so its next connect is refused. The first connect after an update swaps this PC's identity.json secret for its own
 * (same file format: an older RetroHub still reads it). */
struct DeviceInfo {
    std::string id, pc, cdev, name;   // id: the server's id; pc: the PC's own id; cdev: its cloud-sync device id (links "Stop syncing" to it); name: its computer name
    int64_t created = 0, seen = 0;    // ms since epoch
    bool current = false;             // this PC
};
struct DeviceList {
    std::vector<DeviceInfo> devices;
    int cap = 8;
    bool legacy = false;              // a PC that has not updated may still use the shared secret
    bool loaded = false;
};
/* The answer to the last lock-out / sign-out / delete request. seq bumps on every answer, so the UI can tell a new one. */
struct AccountResult {
    int seq = 0;
    std::string op;       // revoke_device | revoke_others | sign_out | delete_account
    bool ok = false;
    std::string reason;   // not-found | self | needs-device | confirm-mismatch | rate-limited | ...
    int count = 0;        // revoke_others: how many PCs were locked out
};
/* Pure rules (tested): the plain sentence for a lock-out / delete answer. */
std::string account_result_text(const AccountResult& r);
/* True when `typed` confirms deleting the account: the username or the friend code, any case, spaces at the ends ignored. */
bool delete_confirmation_ok(const std::string& typed, const std::string& username, const std::string& friend_code);
/* Plain-English status for a close the client will not retry (4003). */
extern const char* const kSignedOutStatus;

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
    /* Play time sharing: send_playtime() after a session ends or the setting changes (remembered and re-sent after a reconnect, never more
     * than the server's rate limit needs); request_playtime() asks for friends' totals (also done once per connection). */
    void send_playtime(const std::vector<PlayGame>& games, bool share);
    void request_playtime();
    /* Opt-in "came online" pings for these friends (replaces the list; remembered and re-sent after every reconnect). Events: WatchedOnline. */
    void set_watch_online(const std::vector<std::string>& uids);
    const std::map<std::string, std::vector<PlayGame>>& friend_playtime() const;   // uid -> that friend's games (top first); blocked/removed friends are never listed
    int playtime_version() const;                                                   // bumps whenever friend_playtime() changes
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
    /* With an attached file (already uploaded; only att->id goes on the wire, the rest is shown in your own chat line) and/or a card
     * (<= kCardMax chars, e.g. a challenge). text may be empty when att/card is given. Offline: queued in the outbox (Event::Queued). */
    void send_message(const std::string& uid, const std::string& text, const Attach* att, const std::string& card);
    /* Outbox (actions made while disconnected are kept in <data_dir>/outbox.json and sent in order after the next welcome). */
    int outbox_size() const;
    bool queued(const std::string& msg_id) const;      // this conversation line (Message::id of a mine line) is still waiting to be sent
    bool cancel_queued(const std::string& msg_id);     // drop a waiting message (the line stays; the UI may delete it)
    void send_reaction(const std::string& uid, const std::string& target_id, const std::string& emoji, bool on);
    void send_invite(const std::vector<std::string>& uids, const Invite& inv);
    void ack(const std::vector<std::string>& ids);
    /* "Ask to join": tells friend `uid` we would like an invite to the game they are in. The Worker rate limits it (1 per target per minute). */
    void ask_to_join(const std::string& uid, const std::string& game_id, const std::string& game_title, const std::string& system, bool host = false);

    /* ---- game libraries (metadata only). publish_library: the games this PC owns (share=false withdraws them). Remembered and sent
     * once welcomed, only when its hash differs from what the server holds. Friends' lists are fetched only when their version
     * changed (one at a time, paced) and cached in <data_dir>/friend_libs.json. */
    void publish_library(bool share, std::vector<LibGame> games);
    const std::vector<LibGame>* friend_library(const std::string& uid) const;   // null = none
    uint32_t library_epoch() const;                                              // bumps whenever any cached friend list changed
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

    /* ---- account access + owner dashboard (docs/ADMIN.md). access(): why the server refused this account's hello (Ok once welcomed).
     * is_admin(): the welcome said this account is the owner. The server independently refuses admin_* from anyone else. */
    Access access() const;
    /* "Check again": drop the wait (an access refusal slows reconnects to a minute) and connect right now. No-op while welcomed. */
    void reconnect_now();
    bool is_admin() const;
    void admin_request_list(const AdminQuery& q);
    void admin_request_detail(const std::string& uid);
    void admin_act(const std::string& op, const std::string& uid);   // ban | unban | approve | deny
    void admin_request_settings();
    void admin_set_settings(int approval, int vouch, int autofriend, int strict_rooms = -1);   // -1 = unchanged
    const AdminList& admin_list() const;
    const AdminDetail& admin_detail() const;
    const AdminSettings& admin_settings() const;
    int admin_pending() const;     // accounts waiting for approval (0 until the server said)
    int admin_version() const;     // bumps whenever any admin data changed
    /* Invite codes (docs/ADMIN.md). redeem_invite(): a WAITING account sends the code it was given on its waiting socket (queued and sent after a reconnect if the server has closed that socket); the answer is Event::InviteRedeemed
     * and, on success, the server approves it and wakes the socket (the usual unlock). False when there is no open socket to send on. */
    bool redeem_invite(const std::string& code);
    void admin_invite_create(int uses, int hours, const std::string& note);
    void admin_invite_list();
    void admin_invite_revoke(const std::string& code);
    void admin_approve_code(const std::string& friend_code);   // "RH-XXXX-XXXX" (see friend_code_normalize)
    const AdminInvites& admin_invites() const;
    void admin_request_pending();                 // asks for the waiting accounts (the Pending queue); the answer is admin_pending_list()
    const AdminList& admin_pending_list() const;  // the waiting accounts (complete list, newest activity first); empty when nobody waits

    /* ---- account lifecycle (all need a welcomed connection; the answer lands in account_result() / devices()) ---- */
    void request_devices();                                        // ask the server for the PCs on this account
    const DeviceList& devices() const;
    int devices_version() const;                                   // bumps whenever devices() changed
    void revoke_device(const std::string& id, const std::string& cdev = std::string());   // lock one PC out (by server id, or by its cloud device id)
    void revoke_other_pcs();                                       // lock out every other PC; the shared secret and every old account code stop working
    void sign_out_this_pc();                                       // the server drops this PC's credential; answer "sign_out" in account_result()
    void release_username();                                       // give the username up (held for you 7 days)
    void delete_account(const std::string& typed);                 // irreversible; `typed` must pass delete_confirmation_ok on the server too
    const AccountResult& account_result() const;
    /* After a "sign_out" answer: forgets the identity on this PC (identity.json, the vault copy and the per-user backup; the account code
     * reminder is the caller's job) and restarts as a brand-new anonymous account. Saves and games are not touched. */
    bool forget_identity_and_restart();
    std::string my_pc_id() const;                                  // this install's stable id (made once, kept in friends-pc.txt)
    bool server_has_devices() const;                               // the connected server offers per-PC sign-ins (an older server does not: nothing changes for this PC and the PC tools stay hidden)

    /* state */
    const std::vector<Friend>& friends() const;
    const std::vector<Request>& incoming_requests() const;
    const std::vector<Request>& outgoing_requests() const;
    const std::vector<std::string>& blocked() const;
    std::string blocked_name(const std::string& uid) const;   // the name they had when you blocked them; empty if unknown
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
