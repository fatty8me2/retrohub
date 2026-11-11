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

**Your account is not in the zip or the sync folder.** The account secret (what signs you in to friends and
cloud saves) stays on the PC that holds it: exports and sync folders never contain it, and importing a zip can
never switch you to another account. To use your account on another PC, link it there with a link code or your
account code (Settings > Account). Zips and sync folders made by older versions may still hold the file; it is
ignored, so you can delete the `_identity` folder from them. The sync log says it: "Your account isn't included. Use Settings > Account > Add this account to another PC to sign in elsewhere."

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

Vault layout: `<vault>\<system>\<game key>\{battery\, states\, meta.json}`; path components are sanitized. Read-only code (lists, thumbnails, counts, play time, resume facts) must use `game_dir_path()` / `battery_dir_path()` / `states_dir_path()`, which never create folders; `game_dir()` / `battery_dir()` / `states_dir()` create them and are for launching, saving, importing and syncing in, so merely looking at a game leaves nothing in the vault.
Other top-level entries: `.sync\` (manifests, never synced), `_conflicts\` (synced like any other file), `_identity\`
(this PC's account secret: never synced, exported or imported; the same goes for the old copies older versions kept in
`_conflicts\_identity\`). A game save that merely has a name like `identity.json` is a save and travels.

**`sync_folder(root, include_states)`**
- The sync folder mirrors the vault layout. Both trees are scanned; each file is identified by relative
  path (case-insensitive) and SHA-256. Hashes are reused when size+mtime match the manifest.
- Manifest from the previous sync: `<vault>\.sync\<root>-<hash>.json` (path -> size, mtime, sha256 of the
  agreed content). Per file: only one side exists -> copy; same hash -> nothing; one side differs from the
  manifest -> that side overwrites the other; both differ (or no manifest and they differ) -> conflict.
- Conflict: newer mtime wins (tie: higher hash, so both PCs agree). The loser is first copied to
  `_conflicts/<rel>.<device>.<yyyymmdd-hhmmss>` on **both** sides, then overwritten. `meta.json` conflicts
  just take the newer one with no conflict copy. A conflict also counts as one push or pull.
- **Older or empty incoming file (extra backup, same winner).** When only one side changed, it still wins on its hash alone.
  But if the file about to be overwritten is newer by mtime than the incoming one by more than 2 s (FAT granularity: a cloud
  client restored an old version, a stale USB stick) or the incoming file is empty while the existing one is not (half-written),
  the file being overwritten is first copied to `_conflicts/` on both sides (same naming as a conflict; logged as `backup ...`
  and counted in `conflicts`). A normal newer incoming save gets no extra copy. `meta.json` is exempt. If the copy cannot be
  made, the file is left untouched.
- **Incomplete scans refuse to sync, as narrowly as the layout allows.** Both trees are listed completely before anything is copied. Only
  "folder gone / empty" (`ERROR_NO_MORE_FILES`, `ERROR_FILE_NOT_FOUND`, `ERROR_PATH_NOT_FOUND`) counts as normal. Any other listing
  error (antivirus, sync-client lock, access denied) inside a game's tree (`<system>/<game>/...`; a "system" is any name in the canonical
  table `system_short()`/`console_id()` in `library.h`, so new systems are covered automatically, or any top-level folder the vault
  itself already has) leaves out just that game on both sides this run: it is logged (`skip game ...` with the Windows
  error code), reported in `SyncReport::warning` in plain words, and the rest syncs (`ok` stays true). An unreadable top-level folder
  that is not a system is left out with a `skip folder ...` log line. An error listing the root, a system folder itself, `_conflicts`
  or `_account` fails the whole sync (`ok=false`, plain-words `error`) and changes nothing.
  Also, if the `_conflicts` backup copy succeeds in the vault but fails in the sync folder, the vault-side copy this attempt just made
  is removed again (nothing older is ever touched), so retries do not pile up copies. In the "only one
  side has it" branches the destination is probed first: if it exists after all, the pair is compared and handled as a normal
  two-sided file (conflict copy if needed), never overwritten blindly. A USB stick root's `System Volume Information` and
  `$RECYCLE.BIN` are skipped. (`export_zip` does not use this refusal.) Test seam: `set_enum_error_hook()` (inert unless set).
- **Junk is ignored on both sides**: `desktop.ini`, `Thumbs.db`, `.DS_Store`, names starting `~$`, `.~lock.`, `.dropbox`,
  `.syncthing.`, the file `.sync`, and names ending `.crdownload` or `.!sync` (case-insensitive). Such files already in a vault
  or sync folder are left where they are, never deleted, and simply not synced. `*.tmp`, `*.partial` and `~`-suffixed names are
  deliberately NOT ignored: Wii U / 3DS / Switch games pick their own save file names inside the battery folder. Zip export
  still includes everything.
- Copies are atomic (`*.hubtmp` + `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`) and keep the mtime.
  Stale `*.hubtmp` files are ignored by scans.
- Robustness: `\\?\` long paths, UTF-8 API / UTF-16 internally, junctions and symlinks are skipped
  (never followed), unreadable files (e.g. OneDrive "online only" placeholders that cannot be fetched,
  locked files) are skipped with a `skip ...` log line and retried next sync; the sync as a whole stays `ok`.
- Rejects a sync folder equal to, inside, or containing the vault.

**`export_zip` / `import_zip`**: miniz (MIT, fetched with FetchContent, 3.0.2). Entries use `/` separators and
the UTF-8 name flag. Import rejects unsafe entry names (`..`, absolute, drive letters), skips `.sync/` and the
account files (`_identity/`, `_conflicts/_identity/`; logged as "never imported"), and
merges: new file -> restored; identical -> nothing; different -> newer mtime wins and the loser goes to
`_conflicts/` (zip timestamps have 2 s resolution, so the local file wins within 2 s).

**Limitations (v1)**: no deletion propagation; no locking, so do not run two syncs on one sync folder at
the same time; clock skew between PCs affects which side "wins" a conflict (the loser is still kept);
files are read into memory for zip export/import.

**Tests**: `hubsaves_test` (console) simulates two PCs sharing one sync folder: push/pull, edit propagation,
conflicts (both directions), states excluded/included, unicode and long paths, zip round trip, idempotency, and
that the account secret never travels (sync folder, export, hostile import zip). The cloud engine has its own
test, `hubsaves_engine_test`, against an in-process mock Worker (see `CLOUD_SAVES_V2.md`).

## Bring your saves (other emulators)

Saves page > "Bring your saves from other emulators" (`src/hub/save_import_ui.cpp`, logic `src/common/save_import.*`, test `save_import_test`).
It looks in the usual places (RetroArch `retroarch.cfg` save folder and `saves\`, Project64 `Save\`, Dolphin `GC\`, PCSX2 `memcards\`, mGBA `config.ini` / `Documents\mGBA`, VisualBoyAdvance(-M), melonDS), next to the library ROMs, and in any folder or file the player picks. RetroHub's own emulator folders are never offered.
Each save is matched to a library game by ROM file stem, then by normalised title (GameCube cards by game code, PS2 cards by the game's serial). The preview shows game, source, date and size of both sides and marks the newer one; nothing is ticked when the vault save is newer, when the match is ambiguous or when the file cannot be converted.
Written into the vault under exactly the name the runner reads:

| Source | Vault file |
|---|---|
| RetroArch `.srm`, mGBA / VBA / melonDS `.sav` (GB, GBC, GBA, NES, SNES, DS, PS1) | `battery\<ROM stem>.srm` (stem = `GameEntry::save_stem` or the ROM file's stem) |
| Project64 `.eep` `.sra` `.fla` `.mpk` | mupen64plus names `battery\<GoodName cut to 32>-<MD5[0:8]>.<ext>` (`SaveFilenameFormat 1`; GoodName from `mupen64plus.ini`, else the ROM header name; an existing old-style `<goodname>.<ext>` is kept). `.eep` is padded to 2 KB (the core always keeps 2 KB); a 32 KB Project64 pak (`_Cont_N.mpk`) goes into slot N of the core's single 128 KB `.mpk`; `.sra`/`.fla` are raw (both emulators keep them in the same word order) |
| RetroArch N64 `.srm` (mupen64plus-next, 296,960 bytes) | split into `.eep` / `.mpk` / `.sra` / `.fla`, only the parts that hold data |
| Dolphin `.gci` folder or `.raw` card | `battery\GCI\*.gci` for the matching game (a raw card is split into `.gci` files per game code) |
| PCSX2 8 MB `.ps2` card | `battery\memcards\RetroHub\<safe key>\Mcd001.ps2` (the whole card) |

An existing different file is copied to `_conflicts\<system>\<game>\battery\<file>.import.<yyyymmdd-hhmmss>` first (no backup, no overwrite); writes are atomic and stamp "now", so the import is the newest copy on every PC. After the import the game's `meta.json` is refreshed and `cloud_game_changed()` queues a cloud push. Other emulators' save states are never imported.
`cloud_game_changed()` is also called after a history restore/undo and after a folder pull or zip import (`cloud_sync_all_now`), so those changes no longer wait for the next play (audit saves-F13).

## Account prefs (`_account/prefs.json`)
`src/common/prefs.h` defines a typed file in the vault's `_account/` folder (class `settings` in the cloud, so it is always synced): `favorites`, `user_shelves`, `hidden`, `renames`, `covers` (game key -> image path), `theme`, `profile.avatar|fav_game|status`. Each field carries its own modified time (unix ms) in `ts`; `merge()` keeps the newer side per field, so two PCs editing different fields never overwrite each other. `favorites` and `hidden` go further: each key has its own stamp, and a removal leaves a tombstone (cleaned up after 90 days), so two PCs adding different favorites offline keep both. The cloud engine syncs the file as the hidden pseudo-game `_account/_`; on a both-sides conflict it calls `merge_text()` through `merge_into_file()` instead of keeping both copies, then applies favorites back into `library.json` (tested by `prefs_test` and `hubsaves_engine_test`). The hub arms `set_vault_root()` at startup, so `hub::set_favorite` mirrors into prefs, and the screenshot viewer's "Set as cover" writes `covers[key]` (`custom_set_cover` first copies the picture into the vault so it syncs). The library tiles (`tiles.cpp`, via `custom_cover`) show that picture as the game's cover.
Screenshots live in `<vault>/<system>/<game key>/shots/` and stay local unless "Back up screenshots" is on.

## Online save policy

"Use host's save" is a room option (host-controlled, default on) for lockstep/rollback modes that need every PC to boot from
byte-identical save data. In those modes the HOST's vault save is copied into a throwaway dir (never edited in place), sent
to every guest before frame 0 and loaded everywhere. Nobody writes back into a vault: the shared save is a *session* save, so
progress made online is discarded (the host's vault stays exactly as it was). With the option off, everyone starts empty.

| Mode | Save used | Vault written? |
|---|---|---|
| N64 lockstep (n64run) | host vault battery dir shared (`--share-host-save`, slot 0 = host) | no (temp dir) |
| GB / GBC / GBA lockstep + rollback cores | host vault save RAM + RTC shared (`--share-host-save`) | no (temp dir) |
| PlayStation 2 lockstep (retrorun, LRPS2) | host's PCSX2 memory card 1 (`Mcd001.ps2`, 8 MB, deflated) sent before the core boots (`--share-host-save`, slot 0 = host); slot 2 blank | no (temp dir `netplay\ps2`) |
| PlayStation rollback (retrorun, Beetle PSX) | host memory card 1 (128 KB save RAM) shared (`--share-host-save`, slot 0 = host) | no (temp dir) |
| GB / GBC / GBA link cable (Pokemon trade/battle) | each player's OWN vault save, exchanged via the room | own console only |
| DS wireless (`--ds-link`) | each player's own vault save | yes, own |
| 3DS, GameCube | each player's own emulator/vault save (3DS room sessions and the Wii U remote-play host use the same vault path as offline play) | yes, own |
| Kanto Together, Smash Classic | each player's own save | yes, own |

Guests therefore never touch their vault in shared-save modes; vault sync (Sync button) is unaffected.
Test: `qa\retrorun_hostsave.ps1 -Share` (guest save differs from host's; final state hashes must match).
