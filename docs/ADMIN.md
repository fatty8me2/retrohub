# Owner dashboard, blacklist and approval mode

The owner account (the Worker's `ADMIN_UID`) gets an **Admin** page in RetroHub. Everyone else never sees it, and the Worker refuses every `admin_*` message from any other account, so hiding the page is a convenience, not the security.

## Who is the admin (server side)
- An admin message is honoured only on a socket that passed the normal friends hello **for the `ADMIN_UID` account with its stored secret** (`SHA-256(secret)` match, else close 4003). The `uid` in the URL and any `uid` / `admin` fields in a message body are never trusted: the hub answers from its own stored profile.
- A non-admin sending any `admin_*` type gets exactly what an unknown message type gets: no reply, no error, no log line. The welcome carries `admin:true` for the owner only.
- Admin calls are limited to 60 per minute (in memory) on top of the normal per-account message budget.
- The registry (Durable Object `AdminRegistry`, binding `ADMIN_REG`) has no public route; it only answers requests with the internal header, and its list/detail/ban/settings/seed routes also need the admin uid, which only the admin's own hub sends.

## What is stored (and what is not)
One row per account, hard capped: uid, display name, username, created, first/last seen, PC estimate (most simultaneous sockets seen), status (`ok | pending | banned | denied`) and who set it. **No IP address, message, file name or anything else new.** Test accounts (`qa-` names, `test:true`) are never stored or listed.

## Client -> server
| Message | Effect |
|---|---|
| `{type:"admin_list", q?, sort:"last"\|"first", order:"asc"\|"desc", status:"all"\|"pending"\|"banned"\|"ok", offset, limit<=50}` | page of accounts; the first call seeds the registry from the admin's own friend list (300 per step) |
| `{type:"admin_detail", uid}` | how they got onto your list (`auto`/`request`/`room`), friend names (<=30), first/last seen |
| `{type:"admin_ban"\|"admin_unban"\|"admin_approve"\|"admin_deny", uid}` | status change; replies `admin_result` |
| `{type:"admin_settings", set?:{approval?, vouch?, autofriend?}}` | read or change the switches |

Server -> client: `admin_list`, `admin_detail`, `admin_result`, `admin_settings`, `admin_pending` (push when a new account starts waiting).

## Ban / deny
The registry marks the uid and pushes the verdict to the account's hub, which closes its sockets (4010) and, over alarm steps of 25 (Workers Free allows 50 subrequests per invocation), removes its friends and cancels/declines its requests everywhere. The admin's own friend rows and requests for it go at once. The banned hub then answers every internal call with "no such account", its `link_auth` / `saves_auth` refuse (links, cloud saves), and the room server closes its seat at `ident` (4010). Hello gets `{error: "account-unavailable"}` and the socket is left open, so shipped clients do not retry in a loop. An unban pushes `ok` and closes the waiting socket (4011) so the client reconnects at once. A ban recorded for an uid that never registered sticks for when it does.

## Approval mode (default OFF)
Turning it on records a cutoff. An account whose own `created` time is before the cutoff is grandfathered and untouched. A newer account is `pending`: it can run RetroHub and play offline; friends, rooms, saves and links answer `pending-approval` ("Waiting for the owner to approve this account"; room seats close 4012). Approve/deny from the Pending view; approval wakes the waiting client (4011). Turning the mode off lets waiting accounts in on their next check (at most two minutes).
- **Vouch** (separate toggle, default OFF, needs approval mode): a waiting account that an approved account adds **by friend code** is approved; at most 5 per voucher.
- A spoofed test account is not a way around the gate: with approval on, test accounts are refused too (and invisible).

## Auto-friend toggle (default ON)
Off: new accounts are not added to the owner's friends (an account that connects while it is off is skipped for good). Existing friends are never touched.

## Safety rules (no existing account is ever locked out)
- An **error** never locks anybody out. If the registry cannot be reached at hello: an account with an `ok` verdict is welcomed; an account with no verdict that is older than 24 h is welcomed (nothing is cached, so it registers at its next hello); a brand-new account is held **only while approval mode is known to be on** (otherwise it is welcomed too, and the admin auto-link is retried later, never marked done). Only an explicit ban/deny, or a brand-new account under approval mode, is ever refused. An unreadable verdict counts as `ok`.
- **Seeding** never makes anybody pending: accounts seeded from the owner's friend list are `ok`, `by=seed`, creation time unknown until their own hello reports it. Seeding restarts when the owner's friend list grows or after 24 h.
- **Rollback.** Every stored verdict is ignored when the `ADMIN_REG` binding is absent (`gateLocal()` returns `ok`, `acctGate` skips the registry), so redeploying the previous code, or this code without the binding, lets everyone back in at once. Keep the `AdminRegistry` class exported and the `v8` migration in place: removing the class from the exports while the migration history still lists it makes `wrangler deploy` fail, and a `deleted_classes` migration would destroy the registry's stored data. Do not add a `deleted_classes` migration unless you mean to drop the bans and the account list.
- A **failed push** (ban, unban, approve, deny) is not lost: the row is kept `unapplied` (the dashboard shows "Not applied yet - retrying"), the registry retries from an alarm (1 min, then every 5 min), a banned hub re-checks the registry at least every 10 minutes (so a missed unban cannot stick), and every hello of an approved account checks in at least every 6 hours (so a missed ban lands, closing the account's other authenticated sockets).
- **Test accounts.** The client's `test:true` flag and `qa-` names are honoured **only** where `ALLOW_TEST_FLAG=1` is set (local wrangler and the test suite; production never sets it). Without it they are ordinary accounts: auto-linked, listed, subject to approval. With it, test accounts are stored (flagged), count toward the caps, are hidden unless "Show test accounts" is on, and are still bound by bans and approval mode. Local online runs: start wrangler with `--var ALLOW_TEST_FLAG:1`.
- **Banned usernames stay reserved** for the banned account (nobody else can take a banned person's name or impersonate it).
- A vouched account cannot vouch (no chains); `admin_peek` returns at most 30 friend names.

## Caps (free tier)
20 000 account rows (past it new `ok` accounts are simply not tracked, never refused), 500 queued pending (past it still refused, not queued), 3 000 bans, 5 vouches per voucher, 50 rows per page, 4 x 300 seed rows per list call, 60 admin calls a minute. An approved account re-checks in with the registry at most every 6 hours (1-2 rows written), so the registry adds roughly one write per active account per 6 hours to the rows budget.

## Client
`src/net/admin_logic.h` (pure; `hubadmin_test`), `FriendsClient::is_admin()/admin_*()/access()`, `src/hub/page_admin.cpp`. Screenshot aids: `HUB_SHOT_ADMIN=1|pending|banned|detail|confirm|settings`, `HUB_SHOT_ACCESS=pending|banned` (Play Online notice).
