# ROM patches: the patch engine

`src/common/patcher.h` / `patcher.cpp` / `patcher_vcdiff.cpp` apply ROM hack and translation patches: **IPS, UPS, BPS and VCDIFF (xdelta3 output)**. It is part of `hubcommon`, has no dependency on the hub, and is tested by `patcher_test`. The API lives in the namespace `hub::patch` and is meant to stay stable: later code (the hack library) is written against it.

```cpp
hub::patch::Info info; std::string err;
if (!hub::patch::inspect_file(patch_path, &info, &err)) { /* err is plain words */ }
// BPS / UPS: info.source_crc names the game this patch was made for (info.has_source_crc)
auto r = hub::patch::apply_file(game_path, patch_path, out_path, /*max_out*/ 256ull << 20,
                                [](double f) { /* 0..1 */ return true; /* false = cancel */ });
if (!r.ok()) show(r.message);   // "This patch is for a different version of the game."
```

## What it reads

| Format | Detected by | Checks | Notes |
|---|---|---|---|
| IPS | `PATCH` | none (the format has none) | Records, RLE records, the `EOF` marker, the optional 3-byte truncation size. A record at offset `0x454F46` (which spells EOF) is a record when more than a truncation size follows. Offsets are 24-bit, so records reach 16 MB; the game can be larger (it is copied first). |
| UPS | `UPS1` | source / target / patch CRC32, sizes | Wrong game gives `WrongSource`. Not applied in reverse (the hub only needs forward); a game that already has the patch gets its own message. |
| BPS | `BPS1` | source / target / patch CRC32, sizes | SourceRead, TargetRead, SourceCopy, TargetCopy, metadata. `inspect` reads the footer, so the base game can be found from `source_crc` without applying anything. |
| VCDIFF | `D6 C3 C4 00` | Adler-32 of every window (when the encoder wrote it), structure | RFC 3284 with the default code table, `VCD_SOURCE` and `VCD_TARGET` windows, `VCD_ADLER32`, the xdelta3 application header, secondary compression with **DJW**, **FGK** and **LZMA** (an xz stream of LZMA2 chunks per section type, as `xdelta3 -S lzma` writes it). No whole-file checksum exists in VCDIFF, so the caller checks the result (the hub keys a hack by the CRC32 of the patched output). |
| PPF | `PPF1x`..`PPF3x` | | Not supported yet: `Error::Unsupported`, "PPF patches aren't supported yet." (PS1 images are hundreds of MB.) |

VCDIFF things that are refused with `Error::Unsupported` and a plain message: a custom code table (`VCD_CODETABLE`; xdelta3 3.2 removed it itself), a window larger than 256 MB (xdelta3 stops at 64 MB), an xz dictionary larger than 256 MB.

`format_from_ext` knows `.ips .ups .bps .xdelta .vcdiff .delta`; `detect` goes by the first bytes and is what `apply` trusts.

## Errors

Every failure has an `Error` and a `message` written for the player; `detail` has the technical version (CRCs, offsets) for the log.

| Error | When | message |
|---|---|---|
| `BadPatch` | Truncated, damaged, or not a patch; the patch's own CRC is wrong; a count or offset points outside the patch | "This patch file is damaged or incomplete." / "This file isn't a patch the hub can read." |
| `WrongSource` | UPS / BPS source size or CRC differs; a VCDIFF window that copied from the game fails its Adler-32 (by far the likeliest cause is another version of the game); the patch copies from past the end of the game | "This patch is for a different version of the game." / "This game already has this patch applied." |
| `BadOutput` | The patch and the game were fine but the result's CRC differs from the patch's (UPS / BPS), or a window that never touched the game fails its checksum. The result is thrown away. | "The patched game didn't pass its safety check, so it was thrown away." |
| `TooBig` | The result would be larger than `max_out` (0 = no limit); or the machine could not allocate | "The patched game would be larger than the hub allows." / "There isn't enough memory to apply this patch." |
| `Unsupported` | PPF, and the VCDIFF features above | plain sentence for each |
| `Io` | A file could not be opened / read / written, the disk is full, `out_path` is the game or the patch, the final rename failed | "The game file couldn't be read." ... "There isn't enough free disk space to save the patched game." |
| `Cancelled` | The progress callback returned false | "Patching was cancelled." |

The messages do not name versions ("USA 1.0 vs 1.1"): the patch only knows a CRC. Compare `Info::source_crc` with your catalog to say which version the patch wants.

## Memory and files

`apply_file` never loads the game or the result:

* the game is read on demand (an 8 x 256 KB block cache for small reads, direct reads for big ones), so a 1.4 GB GameCube image costs a few MB of RAM;
* the result is streamed to `<out_path>.tmp` in 1 MB pieces, flushed, and renamed over `out_path` only when every check passed. A failure or a cancel deletes the temp file and leaves an existing `out_path` untouched. The folder of `out_path` is created when missing;
* `out_path` may not be the game or the patch (checked by file identity, so another spelling of the same path is caught): `Error::Io`;
* UTF-8 paths with spaces, non-ASCII characters and `\\?\` long paths work;
* a VCDIFF patch costs about three times one window (xdelta3's default window is 8 MB), not the size of the game. Windows are built lazily, so a window that claims 200 MB and delivers nothing costs nothing. A 35 MB patch of a 64 MB ROM peaked at 27 MB;
* UPS / BPS read the patch and the game once to check their CRCs before writing anything, so a wrong game fails in seconds with nothing written. The progress bar covers all of it.

`apply` (memory to memory) is for cartridges: it takes the game as a vector and the patch as a pointer and gives a vector back (never the same vector as a half-written result: `out` may even be the source).

## Hostile and damaged input

Every read of a patch goes through a bounds-checked cursor; every count is checked against what is left before anything is allocated; offsets and lengths use 64-bit arithmetic with overflow checks. `patcher_test` truncates every fixture at every length and flips bits / bytes in them thousands of times (exact-size heap buffers, so an over-read faults under AddressSanitizer or the debug CRT); it was run clean under both. VCDIFF section sizes are capped relative to the window, and the output cap `max_out` is checked before each window and each action.

## Testing

`patcher_test` (ctest, offline, about 20 s):

* round trips of random games and targets through small IPS / UPS / BPS / VCDIFF encoders written in the test;
* hand-built VCDIFF windows from RFC 3284: its section 3 example, every address mode and cache slot, the double opcodes, `VCD_TARGET`, bad checksums, bad headers; the default code table against the xdelta3 table;
* real xdelta3 v3.2.2 output (`src/common/testdata/patcher`, see its README): plain, DJW, DJW9, several windows, no checksum, no application header, no source; and an LZMA fixture;
* golden CRCs, wrong-source and already-patched messages, the IPS truncation / RLE / EOF-offset cases, BPS metadata, `max_out` on every format;
* `apply_file`: UTF-8 paths with spaces, atomic replace, cancel, the same-file guard, long paths, missing files;
* the fuzz pass (`--fuzz N`, default 2500 mutations per fixture, also through files).

`patcher_test --big` additionally patches a 1.5 GB sparse file and checks that the peak memory stays small (it writes 1.5 GB to the temp folder).

Checked locally, not in the repo: the Kanto Together BPS (a real Flips file, 655 KB, FireRed) gives the exact SHA-1 in `patch.json`; xdelta3 patches of a vanilla Smash 64 ROM to the 64 MB Smash Remix ROM (35 MB plain, 33 MB DJW) decode to the right CRC in 1.3 s and 2.2 s.

## Licences

LZMA SDK (public domain) and a DJW decoder derived from xdelta3 v3.2.2 (Apache-2.0) are vendored under `third_party/`; see `THIRD_PARTY.md`. xdelta3 **v3.1.0 is GPL-2.0-or-later**, not Apache: Apache-2.0 starts at v3.2.0. Both are compatible with RetroHub's GPL-3.0-or-later.

## Not done

PPF; IPS32 and APS; applying UPS in reverse; custom VCDIFF code tables; checking the BLAKE3 whole-file hashes that xdelta3 3.2's "armor" mode writes into the application header (the header text is shown in `Info::metadata`, but not verified).
