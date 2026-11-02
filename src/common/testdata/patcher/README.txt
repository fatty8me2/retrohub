Fixtures for patcher_test
=========================

All of these are synthetic: no game data, nothing downloaded at test time. They are small on purpose.

  base.bin             56,000 bytes, crc32 54a8982a. A made-up "game": prose from a tiny vocabulary, noise, 16-byte records.
  x_plain.xdelta       made by the real xdelta3 v3.2.2: -S none (no secondary compression), application header, Adler-32
  x_djw.xdelta         -S djw (DJW static Huffman, three compressed sections)
  x_djw9.xdelta        -S djw9
  x_multi_plain.xdelta -S none -W 16384 -B 32768: five windows, each with its own source segment
  x_multi_djw9.xdelta  the same with -S djw9
  x_noadler.xdelta     -n (no checksums)
  x_noapp.xdelta       -A= (no application header)
  x_nosrc.xdelta       no source file at all (-S djw): a 30 KB patch that builds the target from scratch
  x_fgk.xdelta         -S fgk, no source: the first 9000 bytes of base.bin, FGK adaptive Huffman (xdelta3 builds it only on request)
  z_xz.xdelta          x_multi_plain.xdelta with every section of five windows re-wrapped as one xz stream of LZMA2 chunks per
                       section type (what xdelta3 -S lzma writes); the LZMA2 data was made by python's lzma module, and
                       make_fixtures.py checks each stream with liblzma before writing it

Every x_*.xdelta except x_nosrc and x_fgk decodes, against base.bin, to the same 72,104-byte target: crc32 b43de862.
x_nosrc decodes to that target too (with an empty source). x_fgk decodes to base.bin[:9000] (crc32 00687cdb).

Regenerating (only needed to add a case; the output is deterministic, so the files only change when this recipe does):
  1. Build xdelta3 from the upstream tag v3.2.2 (https://github.com/jmacd/xdelta, tarball SHA-256
     995319ccb7fe721a523fb3142376f5813093685b1cc302d07018ca97d2584af2) with DJW and FGK:
       cmake -S xdelta-3.2.2/xdelta3 -B build -DXD3_BUILD_TESTS=OFF -DXD3_BUILD_LIB=OFF -DXD3_ARMOR=OFF -DXD3_LZMA_MODE=off -DXD3_SECONDARY_FGK=ON
       cmake --build build --config Release --target xdelta3
  2. python make_fixtures.py <path to xdelta3.exe> <output folder>   (Python 3.8+ with the lzma module)
  3. copy base.bin, x_*.xdelta and z_xz.xdelta here, and update the golden CRC/size constants at the top of the fixtures
     section of src/common/test_patcher.cpp if base or target changed.
make_fixtures.py also writes target.bin (not kept in the repo: the test checks its size and CRC instead).

Not here on purpose: a patch that uses xdelta3's own LZMA code path needs liblzma to build, and a Smash Remix .xdelta (the
Remix project ships original.xdelta) is not in the repo. The decoder handles what such files need: Adler-32, the
application header, DJW and LZMA secondary compression. It was checked locally against 35 MB (-S none) and 33 MB (-S djw)
xdelta3 patches that turn a vanilla Smash 64 ROM into the 64 MB Remix ROM.
