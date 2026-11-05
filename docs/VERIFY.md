# Verify my games (Plan 4 Wave 3, L3)

Compares the player's cartridge ROMs with the known good dumps from the No-Intro lists. Offline, read-only, never fetches a ROM.

## Data: `data/known_dumps.bin`
- Built by `tools/gen_known_dumps.py` (fetches the .dat files, or `--from-dir` for already downloaded ones; `--selftest` checks the parser). The generated file is committed.
- Source: the No-Intro .dat files mirrored by [libretro-database](https://github.com/libretro/libretro-database) (`metadat/no-intro`), data credited to the [No-Intro project](https://no-intro.org).
- **License:** libretro-database is CC-BY-SA-4.0. The generated file is a derived work under the same license (attribution + share-alike). The license and source are stated in the file's first line and in its first text lines after inflating.
- Only CRC32, size and the canonical title of each entry are kept (27,030 entries, about 460 KB). Systems: NES, SNES, N64, GB, GBC, GBA, NDS.
- Layout: one ASCII header line (`RHKD1 CC-BY-SA-4.0 ...`), then a zlib stream of `@<system>` sections with `CRC32<TAB>size<TAB>title` rows.
- Disc systems (PS1, PS2, GameCube, Wii U, 3DS, PSP) are out of scope: they cannot be hashed cheaply. The UI says "not checked" for them.

## Hashing (`src/common/verify.*`)
Same normalization No-Intro used: N64 is converted to z64 byte order (.v64 and .n64 dumps), the 16-byte iNES header is skipped (NES), a 512-byte copier header is skipped (SNES). Everything else is hashed as is. GB and GBC look in each other's list (dual-mode carts). Zipped cartridges are hashed from their unpacked copy in the rom cache.

Hashing runs on a background thread, only when the player presses "Verify my games" (never during a scan). Results are cached by path + size + mtime in `userdata\verify_cache.tsv`, so a second run reads nothing. After the first scan a quiet cache-only pass restores earlier verdicts without reading any ROM.

## UI (`src/hub/verify_ui.cpp`)
- **Library health card** (All Games, above the grid). Only appears when there is something to say, plus a one-line "Library health" entry until the first check. Collapsed: "Library health: 2 notes". Expanded: games that match no known dump ("might be a bad or modified copy; hacks are fine"), missing PlayStation / PlayStation 2 BIOS, offline or unreadable folders, games with several copies. Optional DS / GBA BIOS (for the real startup screen) is listed but not counted. "Verify my games" starts the check.
- **Game page** (Info tab): "Verified good dump", "Not a known dump", or nothing when the game has not been checked (and always nothing for discs and ROM hacks).
- Screenshot aid: `HUB_SHOT_VERIFY=card|collapsed` with `HUB_SHOT_FAKE_LIB=n`.
