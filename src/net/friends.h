#pragma once
/* hubnet friends client (see docs/FRIENDS.md). Single UI thread: call update() every frame.
 * Non-blocking; reconnects automatically (backoff 1s..30s). */

#include <cstdint>
#include <memory>
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
                FriendAdded, FriendRemoved, MessageReceived, InviteReceived, Error };
    Kind kind = Error;
    std::string uid;          // the friend / requester / sender (empty for some Errors)
    std::string name;
    std::string text;         // RequestResolved: "accepted"|"declined"|"cancelled"; Error: reason
    Message msg;              // MessageReceived / InviteReceived
    Invite invite;            // InviteReceived
};

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
    void rename(const std::string& name);
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

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hubnet
