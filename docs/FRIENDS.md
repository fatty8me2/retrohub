# RetroHub friends: protocol and contract

This is the friends list, presence, direct messages and lobby invites across every game in RetroHub. The server is the existing Cloudflare Worker `smash-netplay`, which gets a new `/friends` endpoint and one Durable Object per user (class `UserHub`, SQLite storage). The client is `hubnet::FriendsClient` in `src/net/friends.h`, and the UI lives in RetroHub.

## Identity (no passwords)

- **First run:** the client generates a 16-byte random `user_id` and a 32-byte random `secret`. It stores them in `data_dir()/identity.json` as `{user_id, secret, created}`, both base32 lowercase.
  - The identity is also mirrored into this PC's save vault at `<vault>/_identity/identity.json` (for recovery on this PC). Since the saves fix, exports and sync folders never contain `_identity`, so folder sync does NOT carry the account identity to other PCs; another PC signs in with the account code instead.
  - On startup, if `data_dir` has no identity but the vault does, the client adopts the vault copy (same PC, e.g. after a wiped data folder; the vault copy never reaches another PC).
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
| `{type:"status", status}` | Sets my presence. It is fanned out to every friend's UserHub, which pushes `presence` to that friend's sockets. `lobby_code` is only shared with friends. A device that sends `state:"offline"` shows as offline to friends. |
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
| `{type:"error", reason, ref?, detail?}` | Reasons: `unknown-code`, `not-friends`, `blocked`, `rate-limited`, `self`, `already-friends`, `bad-request`. |
| `{type:"invite_result", results:[{uid, ok, reason?}]}` | Reply to an `invite`: one entry per recipient. A failed entry has `reason` (default `undeliverable`). |

**Error texts (v1.21.0, `logic.cpp`):** the client turns `error` into a toast. Reasons map to: `unknown-code` "No player has that friend code.", `self` "That is your own friend code.", `already-friends` "You are already friends.", `not-friends` "You can only do that with friends.", `rate-limited` "Slow down a little - too many requests.", `blocked` "That player is blocked.". The `detail` values `too-many-requests`, `already-requested`, `undeliverable`, `no-such-request`, `too-many-blocked` and `retired-elsewhere` each have their own sentence. Internal details (`retire-failed`, `bad-secret`, `text`, `too-large`) show nothing, because they are retried or not user-actionable. Anything else shows "Friends: <reason>".

**Invite result toast:** `invite_result_text` joins the names. Examples: "Invited A, B to room ABC234.", "Invited A to room ABC234. Could not invite C.", and when the reason is `rate-limited` the failure part reads "Slow down - could not invite C.". A send with no results shows nothing.

## Invites and joining (the main feature)

- **Sending:** the host is in a RetroHub lobby room (`/hub` endpoint, code `ABC234`) with a game selected. They click Invite on one or more friends, which sends `invite` with `lobby_code` set to the room code and `game_id`/`game_title`/`system` set to the selected game. `expires_at` is 30 minutes from sending.
- **Receiving:** the recipient's RetroHub shows a toast and a notification card (while a game is running nothing pops up: the invite waits in the bell): "<name> invited you to play <game> — Join / Dismiss". Clicking Join:
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
- Offline-device behaviour: a socket sending `state:"offline"` is treated as offline and does not count for the effective status; a second device that is online still shows the user online.
- Messages to an online user are pushed and not stored in the inbox; if they have no socket they go to the inbox. Inbox messages are re-sent on every connect until acked.
- A friends list holds 200 people (the admin account 5000). A presence event reaches at most 40 online friends (`PRESENCE_FANOUT_CAP`, one subrequest each: the Workers Free plan allows 50 per invocation). The friends it skipped come first on the next event, so everybody is reached in turn, and a friend always sees the right state when they connect. No paid plan is needed.

## One entry per person (dedupe)
- Lists are keyed by uid. The client runs `dedupe_friends` / `dedupe_requests` on welcome and before drawing: a person is never both a friend and a request, never both an incoming and outgoing request (incoming wins), and never in two list sections.
- Identity adoption (link-a-new-PC, vault adopt): the client keeps the replaced identity in `<data_dir>/pending-retire.json` and sends `{"type":"retire_old","uid","secret"}` on the new authed socket after welcome. The server verifies the old secret, moves the old uid's friends/requests/blocks to the new uid (peers see the old entry swapped for the new one), retires the old uid (it can no longer connect: close 4005) and answers `{"type":"retired","uid"}` (idempotent; the client then deletes the file).
- The server also cleans duplicate/crossed rows on every hello (idempotent).
- Screenshot aid: `HUB_SHOT_FAKE_FRIENDS=1 HUB_SHOT_FAKE_DUPES=1` (add `HUB_SHOT_FAKE_ROOM=1` to see the "In your room" status).

## Round 2: why duplicates still appeared
- The admin account (`ADMIN_UID`) is auto-linked with every identity ever created, so every reinstall or wiped data folder leaves one more offline-forever entry with the same display name. The old uid cannot be retired (its secret is gone).
- Retire only covered the last replaced identity (`pending-retire.json` is overwritten). The client now retires every older identity it can find (`older_identities`: pending file, `identity-backup*.json`, `<vault>/_identity/*.json`), tracking confirmed ones in `retired-ids.txt`.
- Server: completed retires write `old -> new` into the Directory (`alias` table). On every hello the user's friends/requests are swapped to the newest uid (`applyAliases`, chains followed, two-sided via `alias_link`). Outgoing requests older than 60 days are pruned.
- Client: `stale_same_name` finds offline entries that share a name with a fresher one. Only entries whose uid is **confirmed** (in `retired-ids.txt`) are hidden and offered a bulk "Merge" / "Keep both". Unconfirmed same-name pairs (the other entry was merely seen 7+ days later; being online is not evidence) stay visible and the panel asks per entry: "Remove the old 'Kai' (last seen ...)?" with Remove / Keep. Unconfirmed people are never bulk-removed, since they may be genuinely different friends.

## Alias trust and failure handling
- An alias `old -> new` is written only by the new account after it proved ownership of `old` (`retire_old` verifies the old secret). Peers and the Directory never accept an alias from anyone else. Cycles are refused (409) and chains are followed at most 8 hops.
- The alias is written **before** the `retire_done:<old>` marker. If the POST fails, the replayed `retire_old` takes the already-done path and retries it (idempotent), so a transient Directory error cannot lose the mapping.
- `applyAliases` never drops a friend for lack of room: if the friend list is full and the new identity is not already listed, the old row is kept (it is retried on the next hello once a slot is free).
- Aliases are server-side trust only: the client treats `retired-ids.txt` (written after the server's `retired` reply) as the sole signal that an identity is a confirmed duplicate.

## Automatic friends from rooms
The lobby client sends an ident frame (uid + secret) after the welcome; HubRoom verifies it with the account's hub. Joining a room links guest and host; the host's launch links everyone present. Settings > Friends > Automatically add people I play with as friends (default on, sent as auto_friend in the hello and live as prefs). The server skips blocked, removed, test and full-list pairs. Toast: You and NAME are now friends.

## Usernames (client side, `agent/usernames-c`; wire = the server's `usernames.js` / `friends.js`)
A username is the account: unique, one owner at a time, usable on several PCs at once (a PC joins with a link code, `identity_link`). Cloud saves, friends and room names follow the uid that owns it. The free-text display name is gone from the UI.
- **Rules** (`src/net/username.h`, unit tested; the server is authoritative and also folds look-alikes and holds a reserved list; full spec in `docs/ACCOUNTS.md`): 3-20 characters; printable ASCII (letters, digits, spaces, symbols) except `" \ < > ` /`, plus Latin-1 letters U+00C0-U+00FF except x and / ; spaces trimmed/collapsed; at least 3 letters/digits; never shaped like a friend code. Display keeps the typed case. Suggestions are made locally (`name2`, `name_64`, `name123`).
- **Client -> server:** `{type:"username_check", name}`, `{type:"set_username", name}` (claim or change; the old name is held for its owner `kUsernameHoldDays` = 7 days), `{type:"add_friend", username}` (next to `code`).
- **Server -> client:** `{type:"username_result", op:"check"|"set", ok, name|username, yours?, reason?, detail?}` (detail: `taken` | `held` | `reserved` | `profanity` | `looks-like-code` | `invalid-chars` | `rate-limited` ...); `{type:"username", username|null, status:"ok"|"none"|"needs_new_name", previous?}` pushed to every device of the account; `welcome` carries `username` and `username_status`. `unknown-code` with detail `unknown-username` when adding an unknown name. The server echoes the checked `name`, not a ref, so `FriendsClient` maps name -> ref itself.
- **Client UX:** wizard step "Pick your username" (debounced live check: `available` / `taken` / `not allowed`, suggestion chips; Next claims; offline = continue and claim later via a banner); "I already have a username on another PC" takes the Settings > Account > "Add this account to another PC" code (10 min, single use) and signs this PC in, turns cloud saves on and starts the first sync. Accounts flagged `needs_new_name` get a one-time prompt. Pure logic: `src/hub/username_logic.h`. Screenshot aids: `HUB_SHOT_USERNAME=empty|invalid|checking|available|taken|notallowed|offline|claiming`, `HUB_SHOT_USERNAME_TEXT`, `_PROMPT`, `_BANNER`, `_CHANGE`, `_CLAIMED=<name>`, `HUB_SHOT_WIZARD=name|returning`, `HUB_SHOT_PAGE=settings`; `qa\shot_usernames.ps1` runs them all.

## Game libraries (greyed-out friend games)

Metadata only: a list of what each player owns (never files or ROMs), so friends see games they lack greyed out. Logic: `server/cloudflare/src/gamelib.js` (BattleShip repo).

- **Publish:** `{type:"library", share:bool, h:string(<=24), games:[{s,k,c,t}]}`. `s` system short code, `k` stable game key (<=64), `c` catalog id (<=32, may be empty), `t` title (<=80). Invalid entries are dropped, duplicates (`s`+`k`) merged, at most 2000 kept. This frame may be up to 300000 chars; every other message keeps the 8 KB limit. Reply `{type:"library_ack", ver, share, h}`. `ver` 0 = nothing shared; otherwise a counter that only grows when the share flag or content changes (an identical re-publish writes nothing). `share:false` clears the stored list. Limit 12 per 10 minutes (`rate-limited`, ref `library`). `h` is a client-side content hash echoed back.
- **Versions:** presence (`presence` push, friends in `welcome`) carries the owner's `lib` version. `welcome.lib = {ver,h,share}` describes the account itself, so a reconnecting client re-sends only when its own `h` differs.
- **Fetch:** `{type:"friend_library", uid, have}` -> `{type:"friend_library", uid, ver, unchanged:true}` when `have == ver`, else `{..., ver, games:[...]}`. `ver:0, games:[]` when unavailable (not shared, blocked, or a test account asking about a real one or vice versa). A non-friend gets `not-friends`. Limit 60 per minute (`rate-limited`).
- **Privacy:** only friends, never blocked users; removing or blocking someone drops access at once. Deleting the account clears the list.
- **Ask to host:** `ask` accepts `host:true`, delivered as `ask.host:true`.

## Play time (friends' hours)
- Setting "Share my play time with friends" (default on). When a game session ends the client sends `{type:"playtime", share, games:[{k,t,m}]}`: its top 30 games by whole minutes (key, title, minutes). Turning the setting off sends `share:false`, which erases the stored totals.
- Server (UserHub): table `playtime` (max 30 rows), rate limited to 12 writes/hour; test accounts store nothing.
- `{type:"playtime_get"}` (30/hour; sent once per connection and at most every 10 minutes when a game's details open) returns `{type:"playtime", friends:{uid:[{k,t,m}]}}` from a per-friend cache in the `friends` table. Stale entries (older than 6 h) are refreshed with at most 12 friend lookups per request (`pt_pull`, friends only, never blocked users). Removed or blocked friends drop out with their row.
- UI: game details show "Friends: Mickey 14h . camer 3h" (top 3), and a friend's actions menu shows "Top games: ...". Screenshot aid: HUB_SHOT_FAKE_FRIENDS=1 with HUB_SHOT_FAKE_LIB and HUB_SELECT_GAME=fake:0.

## Friend came online (opt-in)
- Per-friend toggle "Notify me when they come online" in the friend's actions menu, off by default. The list is `watch_online` in settings.json (comma separated uids, at most 50).
- `{type:"watch_online", uids:[...]}` (client -> server; about 4 changes per 10 minutes per socket, in memory; refused at >= 90% of the daily budget; an unchanged list writes nothing) replaces the list: friends only, unique, at most 50. The server answers `{type:"watch_online", uids}` with what it kept. The client sends it when the list changes and after every welcome (when non-empty).
- The server checks in the WATCHER's own UserHub when a friend's `presence` arrives (so there is no extra fan-out and no extra subrequest): offline -> online for a watched friend pushes `{type:"friend_online", uid, name, at}` to the watcher's sockets only. Non-watchers get nothing new.
- Cost rules: the one new inbound message counts like any other in REQ_BUDGET (`meter(..., "ws")`, 1/20 unit); the push is outbound and free. Pings are skipped at >= 90% of the daily budget (same gate as the presence alarms) and limited to one per friend per 10 minutes (flapping offline/online).
- Client: `Event::WatchedOnline` -> a bell entry (`Kind::FriendOnline`, "NAME came online"), and an in-hub toast. With "Show a Windows notification for invites when no game is running" on, a Windows toast (`hubtoast::notify_online`) appears when no game is running; during a game it waits in the bell.

## DM attachments, cards, offline outbox and liveness (Plan 4)
- **Message fields:** `message` accepts `{to, text?, attach:{id}?, card?}`. `attach.id` is a file already uploaded to the Worker (`/saves/v2/attach`, 8 MB, 7 days, friends only); the server stamps the stored message with `attach:{id,name,size,sha,kind:"shot"|"state"|"patch",expires}` and ignores any other client fields. `card` is an opaque string of at most 1536 chars (`kCardMax`; challenge cards, `RHC1:...`). The stored message stays `kind:"text"`, so old clients just show `text`. Errors: `bad-request` with detail `attach-gone` (unknown/expired id) or `attach-not-yours` (not uploaded for that friend). Client: `hubnet::Attach`, `Message::has_attach/attach/card`, both persisted in `friends/<uid>.json`; `FriendsClient::send_message(uid, text, const Attach*, card)`.
- **Offline outbox (`src/net/outbox_logic.h`):** while disconnected, add friend / accept / decline / cancel / remove / block / unblock / message / invite are queued in `<data_dir>/outbox.json` (max 50, 24 h; invites 10 min) instead of being dropped, and sent in order right after the next welcome. Block then unblock of one person cancel out; a later accept/decline replaces an earlier one. `Event::Queued` tells the UI ("...will send when you're back online"), `Event::SendFailed` reports an item that was refused or expired. `outbox_size()`, `queued(msg_id)` and `cancel_queued(msg_id)` let the chat mark pending lines. "Ask to join" is never queued.
- **Liveness (`src/net/liveness_logic.h`):** the client pings every 20 s and treats any received frame (the `keepalive_ack`) as proof of life; after 60 s of silence it closes the socket and reconnects with the normal backoff (a PC waking from sleep recovers by itself). Server side, the lease alarm closes sockets whose auto-answered keepalives stopped for 150 s with code **4006** `stale`, so a sleeping friend stops showing Online. No extra polling: both ride the existing keepalive and alarm.

## Chat attachments, Send to a friend, Send a hack (UI)
- **Chat lines.** A DM with an attachment (`Message::has_attach`) is drawn as a card (`fr_attach_card`, friends_ui.cpp). Pictures download themselves the first time the card is drawn into `<data_dir>\attach_cache\<id>.png` and show a thumbnail plus an **Open** button (default image viewer). A hack patch shows **Add this hack**: it downloads, then calls `hack_add_patch(path)`. A challenge state shows "Challenge file" (the challenge card handles it). A download that fails shows the plain text from `hub::attach::attach_error_text` in red with **Retry**; an attachment past its 7 days shows "Expired (files last 7 days)". Lines with a `card` go to `hubui::challenge_draw_card` first and fall back to normal text.
- **Offline.** Nothing pretends to have been sent. While disconnected every action sits in the outbox (see "Outbox" above): the player gets "Saved - will send when you're back online." (`Event::Queued`) or the plain failure text (`Event::SendFailed`). A queued message line shows **Waiting to send** with **Cancel** (`cancel_queued`; the line then reads "Not sent"). Accepting a request or sending a friend request offline says "Offline - will send when connected." instead of a success message.
- **Send to a friend (screenshot viewer).** The button opens the shared friend picker (`hubui::FriendPicker`, friend_pick_ui.cpp: online friends first, tick up to 8, pad friendly). Pictures only (clips are refused), at most 8 MB (`attach_size_problem`). The upload runs on a worker thread (`hub::attach::upload`, "Sending... 40%" under the buttons); when it finishes each picked friend gets a DM "Sent a screenshot of <game>" carrying the attachment, and a toast "Sent to A, B." names who. Errors toast the plain text.
- **Send a hack.** On a hack's page (About > Versions) **Send this hack to a friend** uploads the stored patch file (`hack_patch`, kind `patch`, at most 8 MB) the same way; the receiver gets **Add this hack**.
- **Screenshot aids.** `HUB_SHOT_FAKE_FRIENDS=1 HUB_SHOT_FAKE_ATTACH=1 HUB_SHOT_FRIENDS=1 HUB_SHOT_CHAT=1` shows Mia's chat with a picture, a hack, an expired file, a failed download, a sent picture and a queued line. `HUB_SHOT_FRIEND_PICK=1` (with `HUB_SHOT_GALLERY=1 HUB_SHOT_VIEWER=1 HUB_SHOT_DETAILS=fake:0 HUB_SHOT_TAB=2`) opens the picker over the viewer. `qa\attach_ui_shots.ps1` runs both.

## Friend challenges (Plan 4 p4-social)
Code: `challenge_logic.h` (cards, leaderboard, rules), `challenge_flow.h` (pure state machines, clock), `challenge_ui.cpp` (engine + chat card), `quickmenu.cpp` (rows and the Challenge page). Offline only, built-in runners only (not Dolphin / Cemu / Citra / PCSX2 / the native port).
- **Sending.** Game menu > **Challenge a friend from here**. The runner saves a state (runctl `save_state` into the first EMPTY slot; if all ten are used, slot 10 is backed up to `<data>\challenge\` and put back afterwards, so a player's own saves are never lost). The state is copied to `<data>\challenge\ch-<id>-send.state`; the slot thumbnail is the picture. Over 8 MB (DS, PS1) the row says why and nothing is sent. Then pick the goal (**Fastest time**, **Just beat it**, **Score** typed with the on-screen keyboard), tick up to 8 friends, and **Send challenge**: the state (kind `state`) and the picture (kind `shot`) upload through `/saves/v2/attach`, then each friend gets a DM with the state attached and an `RHC1:` challenge card. Progress and plain errors stay inside the game menu; nothing pops up over the game.
- **Receiving.** The card shows in chat with the game, goal, picture and a small leaderboard. **Accept** first compares the card's ROM key with the local library entry for that game; if they differ or the game is missing it says "You need the same game (TITLE) to take this challenge." and offers nothing else. Otherwise the state downloads to `<data>\challenge\ch-<id>-recv.state` and the game starts from it (`A.resume_key` / `A.resume_state`, never a user slot). RetroAchievements are off for that launch (`achievements_launch_args()` returns "" once), because hardcore refuses state loads.
- **During the run.** The hub updates a top-right timer about once a second (runctl `osd_corner`); time the game menu is open does not count. The game menu shows **I did it** (Score goals ask for the number first) and **Give up**. Closing the game without answering counts as giving up. A result card DM goes back to the challenger; every card shows a leaderboard (`leaderboard()`), kept in `<data>\challenge_results.txt`.
- **Old clients** see the card's fallback text ("Challenge: Fastest time in Super Mario 64") and the state as a plain attachment.
- **Screenshot aids.** `HUB_SHOT_CHALLENGE=card|results|cant` (chat card previews), `HUB_SHOT_QM=main|challenge` (menu row / friend picker); `qa\shot_challenge.ps1` runs them.

## Owner dashboard, blacklist, approval mode
See `docs/ADMIN.md`: admin-only `admin_*` messages (owner secret checked server-side), account registry, ban / approve / deny, the approval and auto-friend switches, and the refusal reasons `pending-approval` / `account-unavailable` (close codes 4010 blocked, 4011 approved, 4012 waiting).
