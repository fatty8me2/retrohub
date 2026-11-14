# Hookshot source shipped with RetroHub

`hookshot-src-8ef8461.zip` is `git archive --format=zip` of https://github.com/NotRexed/Hookshot at commit
`8ef84611049f4fc31835a0b12793cf074ed49c3c` (release v2.1.0, 2026-11-10), unmodified.

- sha256 `05ab70bf02640f35ef22fd670200d3f3eb0187eef31efe1f84778a0a12d283d5` (the archive is reproducible: the same
  command gives the same bytes). RetroHub checks this hash before it unpacks the archive.
- License: GPL-3.0-or-later (`LICENSE` inside the archive). Parts under other GPL-compatible or permissive licenses keep their own
  notices in their folders (Opus, zstd/zbic, SDL, Sirit, VMA, a64-kit, Dear ImGui, AMD FidelityFX, Libertinus, Kenney).
- Audited by RetroHub on 2026-11-12 as a diff from the previously audited 137c423 (2026-11-07): verdict CLEAN WITH NOTES. No malware
  indicators; vendored Dear ImGui 1.92.9b, zstd 1.5.7, FidelityFX SDK 1.1.4 / API 2.4.0 and Libertinus 7.051 match upstream. Notes (see
  `docs/HOOKSHOT.md`): v2.1.0 adds a start-up update check with a top-most box, a Discord status and a microphone input; RetroHub turns the
  first two off (HOOKSHOT_NO_UPDATE_CHECK=1, HOS_DISCORD=0) and the NVIDIA DLSS / FSR downloads are never requested.
- The previously audited 137c423 stays accepted for a build a player already made (the card offers a rebuild, never forces one).

The archive holds no game data, keys or executables. RetroHub builds the game from it on the player's own PC, from the
player's own copy of the game, and never shares what it builds.

## Longshot

`longshot-src-5c1f00d.zip` is `git archive --format=zip` of https://github.com/madebyjordan/Longshot at commit
`5c1f00dd444e735bf27b73b2f91c0617f930ddac` (tag v1.0.0), unmodified. Longshot is a fork of Hookshot v2.0.5 (`232eecbd`).

- sha256 `ec0aaa4590c1e28de932d429074d8171c340447ef227a9686aceb0e6ccc072ee`. RetroHub checks this hash before it unpacks the archive.
- License: GPL-3.0-or-later (`LICENSE` inside the archive), with the same bundled third-party notices as Hookshot.
- Audited by RetroHub on 2026-11-14 in two parts. Hookshot 137c423 to v2.0.5 (`232eecbd`): CLEAN WITH NOTES. v2.0.5 to Longshot v1.0.0: CLEAN
  WITH NOTES. Notes: the only network code in the game is the start-up update check, now pointed at `madebyjordan/longshot`, which RetroHub
  keeps off (`HOOKSHOT_NO_UPDATE_CHECK=1`, and no `hookshot_version.txt` is ever written); the shipped `lists/pipeline_states.bin` is data.
- Built into its own folders (`<kit>\src-longshot`, `<kit>\work-longshot`, `<data>\emulators\longshot`). See `docs/HOOKSHOT.md`, "Longshot".

Same as above: the archive holds no game data, keys or executables.
