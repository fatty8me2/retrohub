# Saves

All games keep their save data in one portable folder, the **save vault**
(default: `userdata\saves\` next to the hub). You can sync it between PCs or back it up as a zip.

## For players

**Move saves to another PC / survive a Windows reinstall**

1. Pick a *sync folder* that both PCs can see:
   - **OneDrive / Dropbox / Google Drive**: choose a folder inside it, e.g. `OneDrive\RetroHub`.
     Use the same folder on every PC.
   - **USB stick / NAS**: choose a folder on the drive. Plug it in, press *Sync*, take it to the other PC.
2. Press *Sync*. New saves go to the folder, saves from other PCs come back. Do this after playing,
   and again before playing on another PC.

**Backup / restore**: *Export* writes one `.zip` (opens in Explorer). *Import* merges a zip back in.
On a fresh PC, importing into an empty vault restores everything exactly.

**What is synced**: battery saves (in-game saves, memory cards) and each game's `meta.json`.
Save states (`states\`) are **left out** unless you tick "include save states" (they are big).

**Conflicts**: if you played the same game on two PCs without syncing in between, both versions
changed. The newer file wins, and the other version is kept, never lost, in
`_conflicts\<system>\<game>\battery\<file>.<computer>.<date-time>` in both the vault and the sync folder.
Copy it back over the original if you prefer it.

Syncing never deletes anything. Deleting a save on one PC does not delete it elsewhere (it comes back
on the next sync).

## Developer notes (`src/saves`, library `hubsaves`)

API: `src/saves/savestore.h`. The hub calls `set_vault_dir()` at startup (setting is persisted by the hub),
passes `battery_dir()` / `states_dir()` to runners, and calls `touch_game()` after a session.

Vault layout: `<vault>\<system>\<game key>\{battery\, states\, meta.json}`; path components are sanitized.
Other top-level entries: `.sync\` (manifests, never synced), `_conflicts\` (synced like any other file).

**`sync_folder(root, include_states)`**
- The sync folder mirrors the vault layout. Both trees are scanned; each file is identified by relative
  path (case-insensitive) and SHA-256. Hashes are reused when size+mtime match the manifest.
- Manifest from the previous sync: `<vault>\.sync\<root>-<hash>.json` (path -> size, mtime, sha256 of the
  agreed content). Per file: only one side exists -> copy; same hash -> nothing; one side differs from the
  manifest -> that side overwrites the other; both differ (or no manifest and they differ) -> conflict.
- Conflict: newer mtime wins (tie: higher hash, so both PCs agree). The loser is first copied to
  `_conflicts/<rel>.<device>.<yyyymmdd-hhmmss>` on **both** sides, then overwritten. `meta.json` conflicts
  just take the newer one with no conflict copy. A conflict also counts as one push or pull.
- Copies are atomic (`*.hubtmp` + `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`) and keep the mtime.
  Stale `*.hubtmp` files are ignored by scans.
- Robustness: `\\?\` long paths, UTF-8 API / UTF-16 internally, junctions and symlinks are skipped
  (never followed), unreadable files (e.g. OneDrive "online only" placeholders that cannot be fetched,
  locked files) are skipped with a `skip ...` log line and retried next sync; the sync as a whole stays `ok`.
- Rejects a sync folder equal to, inside, or containing the vault.

**`export_zip` / `import_zip`**: miniz (MIT, fetched with FetchContent, 3.0.2). Entries use `/` separators and
the UTF-8 name flag. Import rejects unsafe entry names (`..`, absolute, drive letters), skips `.sync/`, and
merges: new file -> restored; identical -> nothing; different -> newer mtime wins and the loser goes to
`_conflicts/` (zip timestamps have 2 s resolution, so the local file wins within 2 s).

**Limitations (v1)**: no deletion propagation; no locking, so do not run two syncs on one sync folder at
the same time; clock skew between PCs affects which side "wins" a conflict (the loser is still kept);
files are read into memory for zip export/import.

**Tests**: `hubsaves_test` (console) simulates two PCs sharing one sync folder: push/pull, edit propagation,
conflicts (both directions), states excluded/included, unicode and long paths, zip round trip, idempotency.
