# Cloud saves

The current design (automatic, per game, capped, with versions and a recovery code) is in **docs/CLOUD_SAVES_V2.md**.
This file used to describe the opt-in v1 (`src/saves/cloud.*`, `/saves/list|obj`). v1 still exists for older clients and
keeps working on the server until the sunset described in CLOUD_SAVES_V2.md section 7; nothing new is built on it.

Quick facts that did not change:
- Auth is `Authorization: RHSIG uid,ts,sig` with the HMAC key `sha256hex(identity secret)`; the secret never leaves the PC.
- `_identity/` is never uploaded. The Worker is `BattleShip/server/cloudflare` (`src/saves.js`, `src/saves_v2.js`,
  `src/r2budget.js`, `src/r2budget_v2.js`, `src/saves_pool.js`).
- Everything stays inside Cloudflare's free tier; the global `R2Budget` and `REQ_BUDGET` guards sit above every route
  and fail closed.

## Enable R2 (one time, by the account owner)
1. Cloudflare dashboard > R2 Object Storage > enable R2 (needs a payment method; the free tier is 10 GB).
2. `npx wrangler r2 bucket create retrohub-saves`
3. Keep the `r2_buckets` binding in `wrangler.jsonc`, then `npx wrangler deploy`.

Without the binding every `/saves/*` route answers 503 `cloud-saves-disabled` and the rest of the Worker is unaffected.
