# RetroHub friends: protocol and contract

This is the friends list, presence, direct messages and lobby invites across every game in RetroHub. The server is the existing Cloudflare Worker `smash-netplay`, which gets a new `/friends` endpoint and one Durable Object per user (class `UserHub`, SQLite storage). The client is `hubnet::FriendsClient` in `src/net/friends.h`, and the UI lives in RetroHub.

## Identity (no passwords)

- **First run:** the client generates a 16-byte random `user_id` and a 32-byte random `secret`. It stores them in `data_dir()/identity.json` as `{user_id, secret, created}`, both base32 lowercase.
  - The identity is also mirrored into the save vault at `<vault>/_identity/identity.json`. Folder sync then carries it to the player's other PCs.
  - On startup, if `data_dir` has no identity but the vault does, the client adopts the vault's copy.
- **Friend code:** `RH-` followed by the first 8 base32 characters of `user_id`, uppercase, grouped 4-4 (for example `RH-7KQ2-M9XD`). The server resolves a code to a user with a `Directory` lookup: a single DO named `"directory"` maps the 8-character prefix to the full `user_id`. On a prefix collision at registration, the client generates a new identity.
- **Auth:** the first connection registers the user. The UserHub DO stores `SHA-256(secret)`. Every later connection must present the same secret. A mismatch closes the socket with code 4003 `bad-secret`.

## Connection

`wss://smash-netplay.jumpcircuit.workers.dev/friends?uid=<user_id>`

- **First message from the client:** `{type:"hello", secret, name, version}`. `name` is 1–24 printable characters.
- **Server reply:** `{type:"welcome", code:"RH-XXXX-XXXX", friends:[Friend...], requests:{incoming:[Req...], outgoing:[Req...]}, blocked:[uid...], inbox:[Msg...]}`. The inbox holds messages that arrived while offline. They are delivered once, then deleted server-side once acked (see below).
- **Keepalive:** `{type:"keepalive"}` every 20 s, answered automatically with `keepalive_ack`.
- **Devices:** multiple sockets per user are allowed, one per PC. Presence is the "most active" one: in_game beats in_lobby beats online.

## Data shapes

```
Friend  = {uid, code, name, status, since, note?}
status  = {state:"offline"|"online"|"in_lobby"|"in_game", game_id?, game_title?, system?,
           lobby_code?, joinable?:bool, players?:int, max_players?:int, updated}
Req     = {uid, code, name, at}
Msg     = {id, from, from_name, to, kind:"text"|"invite", text?, invite?:Invite, at}
Invite  = {lobby_code, game_id, game_title, system, expires_at}
```

## Client → server

| Message | Effect |
|---|---|
| `{type:"status", status}` | Sets my presence. It is fanned out to every friend's UserHub, which pushes `presence` to that friend's sockets. `lobby_code` is only shared with friends. With `invisible` (`state:"offline"` sent while connected), friends see offline. |
| `{type:"add_friend", code}` | Resolves the code via the directory. Creates an outgoing request for me and an incoming one for them, and pushes `request` to them. If they already requested me, it becomes a friendship right away. |
| `{type:"accept", uid}` / `{type:"decline", uid}` / `{type:"cancel", uid}` | Accepts, declines or cancels a friend request. |
| `{type:"remove_friend", uid}` | Removes the friendship on both sides and pushes `friend_removed`. |
| `{type:"block", uid}` / `{type:"unblock", uid}` | A block also removes the friendship and silently drops their messages, requests and invites. |
| `{type:"message", to, text}` | Sends a direct message (friends only; at most 500 characters). The recipient is pushed `msg` if online, or it is stored in their inbox (capped at 200 messages, kept 30 days). |
| `{type:"invite", to:[uid...], invite}` | Sends an invite to friends only. Each recipient gets a `msg` with `kind:"invite"`. Rate limited to 20 per minute. |
| `{type:"ack", ids:[...]}` | The client has shown these inbox messages, so the server deletes them. |
| `{type:"rename", name}` | Changes my display name; friends get `presence` with the new name. |
| `{type:"history", with: uid, before?: at}` | Optional: the last 50 messages with a friend. The server keeps a per-pair history of 200 messages in the sender's and recipient's DOs. |

## Server → client

| Message | Meaning |
|---|---|
| `{type:"presence", uid, name, status}` | A friend's presence changed. |
| `{type:"request", req}` | A new incoming friend request. |
| `{type:"request_update", uid, result:"accepted"\|"declined"\|"cancelled"}` | A pending request was resolved. |
| `{type:"friend_added", friend}` | A new friend. |
| `{type:"friend_removed", uid}` | A friend was removed. |
| `{type:"msg", msg}` | A direct message or an invite. |
| `{type:"error", reason, ref?}` | Reasons: `unknown-code`, `not-friends`, `blocked`, `rate-limited`, `self`, `already-friends`, `bad-request`. |

## Invites and joining (the main feature)

- **Sending:** the host is in a RetroHub lobby room (`/hub` endpoint, code `ABC234`) with a game selected. They click Invite on one or more friends, which sends `invite` with `lobby_code` set to the room code and `game_id`/`game_title`/`system` set to the selected game. `expires_at` is 30 minutes from sending.
- **Receiving:** the recipient's RetroHub shows a toast and a notification card: "<name> invited you to play <game> — Join / Dismiss". Clicking Join:
  - leaves any current room;
  - checks that the game is owned (catalog id or game key). If it isn't, it still joins the room but warns "You don't have <game> yet";
  - joins the lobby with the code, using the existing LobbyClient join flow.
- **Join from presence:** a friend whose status is `in_lobby` with `joinable:true` shows a **Join** button in the friends list. It does the same as accepting an invite.
- **Joinability:** the host's RetroHub sets `joinable` to true while the room has a free slot and the game hasn't launched.
- **Host leaves:** the room survives (see ARCHITECTURE "Room lifetime"). The next member becomes host (slot 0) with the same room code, keeps the game selection, rules and picks, re-broadcasts them, and shows "<name> is now the host". Because presence is keyed by room code, friends and unexpired invites keep pointing at the same room and the new host's presence continues to advertise it. A game running when the host left ends as before; the remaining players are back in the room under the new host.

## Limits and abuse

| Limit | Value |
|---|---|
| JSON message size | 8 KB |
| Friends per user | 200 |
| Pending outgoing requests | 50 |
| Add-friend attempts | 30 per hour |
| Messages | 60 per minute |
| Invites | 20 per minute |

- **Names:** names are sanitized: printable characters only, trimmed, and 1–24 characters long.
- **Blocking and privacy:** blocked users can't see presence. Lobby codes are only ever sent to friends.

## Server implementation notes (additions and clarifications)

- All timestamps (`at`, `since`, `updated`, `expires_at`) are milliseconds since the Unix epoch. The server sets `expires_at` itself (now + 30 min) and ignores the client's value.
- `uid` is 26 chars of base32 lowercase (`[a-z2-7]`); `secret` is a string of 16-128 chars.
- Additional close codes: `4002` bad/missing hello, `4004` `code-collision` (prefix already used by another uid; generate a new identity).
- `request` is pushed with `dir:"in"` for incoming and `dir:"out"` for the outgoing request the user just created (so every device sees it).
- `{type:"block_update", uid, blocked:bool}` is pushed to all of the user's sockets after block/unblock.
- `history` replies with `{type:"history", with, msgs:[Msg...]}` (oldest first, text messages only).
- `error` may carry an extra `detail` string (for example `friends-full`, `too-many-requests`, `already-requested`, `no-such-request`, `target-full`, `text`, `too-large`) and `ref` is the request type, or the target uid for message/invite errors.
- Blocking: the blocker's pending requests are resolved to the other side as `declined` (incoming) / `cancelled` (outgoing); a blocked user's `add_friend` to the blocker looks successful (pending) but is silently dropped. The blocked user does see `friend_removed` if they were friends.
- Offline-device behaviour: a socket sending `state:"offline"` is "invisible" and does not count for the effective status; a second device that is online still shows the user online.
- Messages to an online user are pushed and not stored in the inbox; if they have no socket they go to the inbox. Inbox messages are re-sent on every connect until acked.
- Cross-user fan-out is one subrequest per friend (200 max); this exceeds the Workers Free subrequest limit (50) for users with many friends, so use a paid plan.

## One entry per person (dedupe)
- Lists are keyed by uid. The client runs `dedupe_friends` / `dedupe_requests` on welcome and before drawing: a person is never both a friend and a request, never both an incoming and outgoing request (incoming wins), and never in two list sections.
- Identity adoption (link-a-new-PC, vault adopt): the client keeps the replaced identity in `<data_dir>/pending-retire.json` and sends `{"type":"retire_old","uid","secret"}` on the new authed socket after welcome. The server verifies the old secret, moves the old uid's friends/requests/blocks to the new uid (peers see the old entry swapped for the new one), retires the old uid (it can no longer connect: close 4005) and answers `{"type":"retired","uid"}` (idempotent; the client then deletes the file).
- The server also cleans duplicate/crossed rows on every hello (idempotent).
- Screenshot aid: `HUB_SHOT_FAKE_FRIENDS=1 HUB_SHOT_FAKE_DUPES=1` (add `HUB_SHOT_FAKE_ROOM=1` to see the "In your room" status).
