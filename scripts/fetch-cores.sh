#!/bin/bash
# macOS twin of scripts/fetch-cores.ps1: downloads the official libretro macOS arm64 cores from the libretro nightly buildbot,
# plus their frontend .info files, and stages them into <build>/bin/cores/ (and <build>/bin/cores/info/).
# Usage: scripts/fetch-cores.sh [build-dir] [--force] [--strict]     (build-dir default: build-mac)
#   --force   re-download and overwrite cores that are already staged
#   --strict  refuse (exit 1, nothing installed for that core) when a core's SHA-256 differs from the pinned build
# Policy matches fetch-cores.ps1: the buildbot 'latest' folder moves, so by default a different hash only warns (the newer build is
# installed anyway). Re-run qa/gb_gba_synctest.sh (and the NES/SNES synctest) when a pin changes, then update the table below.
# Downloads are untrusted data: each zip is extracted into its own fresh temp dir, and only the named .dylib is moved out.
# bash 3.2 compatible (macOS /bin/bash).
set -euo pipefail

BUILD=build-mac; FORCE=0; STRICT=0
for a in "$@"; do
    case "$a" in
        --force) FORCE=1 ;;
        --strict) STRICT=1 ;;
        -h|--help) sed -n 2,9p "$0"; exit 0 ;;
        -*) echo "unknown option: $a" >&2; exit 2 ;;
        *) BUILD="$a" ;;
    esac
done

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac
CORES="$BUILD/bin/cores"
INFO="$CORES/info"
CORE_BASE='https://buildbot.libretro.com/nightly/apple/osx/arm64/latest/'
INFO_ZIP_URL='https://buildbot.libretro.com/assets/frontend/info.zip'

# name | size of the .dylib | SHA-256 of the .dylib inside the buildbot zip (fetched 2026-11-27)
TABLE='mgba_libretro|2829600|54ae92e80b012013f6d777fb3158b9f925be22c97896049b8549614d7514f505
gambatte_libretro|3916184|5314745e75a0041347b91328edb97684e9744b2642a90edbf125b317af407925
sameboy_libretro|324296|bd66272935c44fc234430f763f2abc97f3d03f235521f31e2757f53374101e40
melondsds_libretro|6069656|81ac8a8d0dd97dcea99839c0587b18a2cf42a593e4cb1a59f2b0b7ae7af71852
nestopia_libretro|6073464|c54566c7c2311c5c115589e578c231a3f053c3c6f420e9e53beee43f70ede8fd
snes9x_libretro|3006440|e911f924314819f29cf5c4c69147818c089dd54fbe02aaf525847f610f55120e'

mkdir -p "$CORES" "$INFO"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/fetch-cores.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
fsize() { stat -f%z "$1"; }

fail=0
while IFS='|' read -r core size want; do
    name="$core.dylib"
    if [ -f "$CORES/$name" ] && [ "$FORCE" = 0 ]; then echo "Skip (exists): $name"; continue; fi
    d="$WORK/$core"; mkdir -p "$d/x"
    echo "Downloading $CORE_BASE$name.zip"
    curl -fsSL --retry 3 -o "$d/$name.zip" "$CORE_BASE$name.zip"
    unzip -q -o "$d/$name.zip" -d "$d/x"
    src="$(find "$d/x" -type f -name "$name" | head -n 1)"
    if [ -z "$src" ]; then echo "ERROR: $name missing in its zip" >&2; fail=1; continue; fi
    got="$(sha256 "$src")"
    if [ "$got" != "$want" ]; then
        if [ "$STRICT" = 1 ]; then
            echo "ERROR: $name SHA-256 $got differs from the verified build $want (--strict): not installed" >&2
            fail=1; continue
        fi
        echo "WARNING: $name SHA-256 $got differs from the verified build $want; run qa/gb_gba_synctest.sh" >&2
    fi
    mv -f "$src" "$CORES/$name.part" && mv -f "$CORES/$name.part" "$CORES/$name"
    echo "Installed: $name"
done <<EOF2
$TABLE
EOF2

# Info files (only the ones for the cores above).
need=""
while IFS='|' read -r core size want; do
    if [ ! -f "$INFO/$core.info" ] || [ "$FORCE" = 1 ]; then need="$need $core.info"; fi
done <<EOF2
$TABLE
EOF2
if [ -n "$need" ]; then
    d="$WORK/info"; mkdir -p "$d/x"
    echo "Downloading $INFO_ZIP_URL"
    curl -fsSL --retry 3 -o "$d/info.zip" "$INFO_ZIP_URL"
    unzip -q -o "$d/info.zip" -d "$d/x"
    for n in $need; do
        src="$(find "$d/x" -type f -name "$n" | head -n 1)"
        if [ -z "$src" ]; then echo "ERROR: $n missing in info.zip" >&2; fail=1; continue; fi
        mv -f "$src" "$INFO/$n"; echo "Installed: info/$n"
    done
else
    echo "Skip (exists): info files"
fi

# Summary: core, size, display_version.
echo
printf '%-22s %10s  %s\n' Core Size Version
while IFS='|' read -r core size want; do
    f="$CORES/$core.dylib"; sz=missing; ver=unknown
    [ -f "$f" ] && sz="$(fsize "$f")"
    if [ -f "$INFO/$core.info" ]; then
        v="$(sed -n 's/^[[:space:]]*display_version[[:space:]]*=[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$INFO/$core.info" | head -n 1)"
        [ -n "$v" ] && ver="$v"
    fi
    printf '%-22s %10s  %s\n' "$core" "$sz" "$ver"
done <<EOF2
$TABLE
EOF2
exit "$fail"
