# Cloud saves (optional, Cloudflare R2)

A second sync backend next to sync folders (docs/SAVES.md). Code: `src/saves/cloud.*`; server: `BattleShip/server/cloudflare/src/saves.js`.

- Auth: requests are HMAC-signed with `sha256hex(identity secret)`; the secret never leaves the PC. Same account as friends.
- Objects: `saves/<user_id>/<vault-relative path>`, max 16 MB each, 200 MB / 1500 files per account, rate limited (120 writes, 600 reads per minute). `_identity/` is never uploaded.
- Sync = download changes into a local mirror (`<data>\cloud_mirror\<uid8>`), run the normal `sync_folder` merge vault<->mirror (so conflicts behave exactly like folder sync: newer wins, loser kept in `_conflicts/`), upload changes with `If-Match`/`If-None-Match`. A lost race is merged on the next run. Downloads resume with `Range` and are sha256-verified; retries use backoff.
- Off by default and off on the server until the R2 binding exists (`/saves/*` answer 503).

## Enable R2 (one time, by the account owner)
1. Cloudflare dashboard > R2 Object Storage > enable R2 (needs a payment method; the free tier is 10 GB).
2. `npx wrangler r2 bucket create retrohub-saves`
3. In `BattleShip/server/cloudflare/wrangler.jsonc` uncomment the `r2_buckets` line.
4. `npx wrangler deploy`
5. Check: `GET https://<worker>/saves/list` returns 401 (not 503).

## Tests
- `node test_saves.mjs` in the Worker dir (starts its own `wrangler dev`; `--nor2` checks the disabled mode).
- `powershell -File qa\saves_cloud.ps1` (two simulated PCs through a local Worker).

## Free-tier budget guard (never costs money)
A global Durable Object (`R2Budget`, SQLite/free plan, `server/cloudflare/src/r2budget.js`) reserves budget before every R2 call and keeps the file index (list/quota/delete need no R2 call). Hard caps (~70% of R2's free tier): 7 GB stored, 700k Class A ops/month, 7M Class B ops/month (UTC calendar month). Over a cap: 507 `cloud-saves-full` / 429 `cloud-saves-paused` (client shows "Cloud saves are full / paused this month", no retries). Deletes always work. If the guard DO errors, everything is refused (fail closed). Test: `node test_r2budget.mjs`.
