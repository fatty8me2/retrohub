# Hookshot source shipped with RetroHub

`hookshot-src-137c423.zip` is `git archive --format=zip` of https://github.com/NotRexed/Hookshot at commit
`137c423589f178bb4274e339b17b4dfd466435e9` (2026-11-07), unmodified.

- sha256 `afd262616752aca42317fc2f53dbb1de3c9a12a1207b4d3b98c548ace0f228ba` (the archive is reproducible: the same
  command gives the same bytes). RetroHub checks this hash before it unpacks the archive.
- License: GPL-3.0-or-later (`LICENSE` inside the archive). Parts under other GPL-compatible licenses keep their own
  notices in their folders (Opus, zstd/zbic, SDL, Sirit, VMA, a64-kit).
- Audited by RetroHub on 2026-11-07 (all 912 files): no network code, no telemetry, no process launching beyond the
  compiler and linker during the build, no registry, startup or security changes, no obfuscated or hidden code; the
  vendored Opus 1.5.2 and zstd 1.5.7 match upstream. See `docs/HOOKSHOT.md`.

The archive holds no game data, keys or executables. RetroHub builds the game from it on the player's own PC, from the
player's own copy of the game, and never shares what it builds.
