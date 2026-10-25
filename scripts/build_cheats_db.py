#!/usr/bin/env python3
"""Convert libretro-database cht/ into the compact RetroHub cheat bundle data/cheats_db.bin.

Source: https://github.com/libretro/libretro-database (cht/), pinned commit below. License: CC BY-SA 4.0
(see THIRD_PARTY.md). Usage:
    python scripts/build_cheats_db.py [path-to-libretro-database-checkout-or-extracted-tarball] [out]
With no source argument the pinned commit tarball is downloaded.

GameCube (sys "gc") comes from Dolphin's own bundled per-game settings (Data/Sys/GameSettings/<GAMEID>[rN].ini, tag 2509,
GPL-2.0-or-later, same release RetroHub downloads): the [Gecko] and [ActionReplay] code sections. libretro-database has no
GameCube cht files. Sections are keyed by the 4..6 char game ID (revision suffix kept in the name). Extra flags:
    --dolphin <dir>   use an extracted Sys/GameSettings folder instead of downloading the 2509 source tarball
    --gc-only         keep every non-gc section of the existing output bundle and only rebuild the gc sections

Bundle = zlib-compressed UTF-8 text:
    RHCHT1\n
    @<sys>\t<format>\t<game file stem>\n      one section per .cht file (sys: n64 gb gbc gba ps1 gc)
    <desc>\t<code>[+<code>...]\n               one line per cheat (default format of the section)
    !<format>\t<desc>\t<code>\n               same, with a format that differs from the section
"""
import io, os, re, sys, tarfile, urllib.request, zlib

COMMIT = "12c21cf79f1e20eff6ac68b80875816c717d0a7a"
DIRS = {
    "Nintendo - Nintendo 64": "n64",
    "Nintendo - Nintendo 64 (Aleck64)": "n64",
    "Nintendo - Nintendo 64 (Unreleased)": "n64",
    "Nintendo - Nintendo 64 (iQue)": "n64",
    "Nintendo - Game Boy": "gb",
    "Nintendo - Game Boy Color": "gbc",
    "Nintendo - Game Boy Advance": "gba",
    "Sony - PlayStation": "ps1",
}
KV = re.compile(r'^cheat(\d+)_(desc|code)\s*=\s*"(.*)"\s*$')
HEX = re.compile(r'^[0-9A-Fa-f]+$')


def file_format(sys_, stem):
    s = stem.lower()
    if "game genie" in s: return "gamegenie"
    if "code breaker" in s or "codebreaker" in s: return "codebreaker"
    if "action replay" in s: return "ar"
    if "gameshark" in s: return "gameshark"
    return {"n64": "gameshark", "gb": "gameshark", "gbc": "gameshark", "gba": "", "ps1": "gameshark"}[sys_]


def code_format(sys_, fmt, code):
    """Per-cheat format; GBA files without a device tag are classified from the code shape."""
    first = code.split("+")[0].strip()
    if sys_ in ("gb", "gbc") and re.match(r'^[0-9A-Fa-f]{3}-[0-9A-Fa-f]{3}', first):
        return "gamegenie"
    if sys_ == "gba" and not fmt:
        if re.match(r'^[0-9A-Fa-f]{8} [0-9A-Fa-f]{8}$', first): return "ar"
        if re.match(r'^[0-9A-Fa-f]{8} [0-9A-Fa-f]{4}$', first): return "codebreaker"
        return "raw"
    return fmt or "raw"


def parse(text):
    d = {}
    for line in text.splitlines():
        m = KV.match(line.strip())
        if m:
            d.setdefault(int(m.group(1)), {})[m.group(2)] = m.group(3)
    return [d[k] for k in sorted(d) if "desc" in d[k] and "code" in d[k]]


DOLPHIN_TAG = "2509"
GC_8x8 = re.compile(r'^[0-9A-Fa-f]{8}[ \t]+[0-9A-Fa-f]{8}$')
GC_ENC = re.compile(r'^[0-9A-Za-z]{4}-[0-9A-Za-z]{4}-[0-9A-Za-z]{5}$')


def parse_dolphin_ini(text):
    """-> {"gecko": [(name, [codes])], "ar_gc": [...]} from the [Gecko] / [ActionReplay] sections only."""
    out = {"gecko": [], "ar_gc": []}
    sec = None
    cur = None
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("[") and line.endswith("]"):
            name = line[1:-1]
            sec = {"Gecko": "gecko", "ActionReplay": "ar_gc"}.get(name)
            cur = None
            continue
        if sec is None or not line or line[0] in "*#;":
            continue
        if line[0] == "$":
            cur = (line[1:].strip(), [])
            out[sec].append(cur)
        elif cur is not None:
            ok = GC_8x8.match(line) or GC_ENC.match(line)
            if ok:
                cur[1].append(re.sub(r'[ \t]+', ' ', line).upper())
            else:
                cur[1].append(None)   # a bad line poisons the whole code
    return out


def gc_sources(dolphin_dir):
    if dolphin_dir:
        for f in sorted(os.listdir(dolphin_dir)):
            if f.endswith(".ini"):
                yield f[:-4], open(os.path.join(dolphin_dir, f), encoding="utf-8", errors="replace").read()
        return
    url = "https://github.com/dolphin-emu/dolphin/archive/refs/tags/%s.tar.gz" % DOLPHIN_TAG
    data = urllib.request.urlopen(url).read()
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tf:
        for m in tf:
            if m.isfile() and "/Data/Sys/GameSettings/" in m.name and m.name.endswith(".ini"):
                yield os.path.basename(m.name)[:-4], tf.extractfile(m).read().decode("utf-8", "replace")


def gc_sections(dolphin_dir):
    games = cheats = 0
    buf = []
    for stem, text in gc_sources(dolphin_dir):
        if not re.match(r'^[0-9A-Za-z]{3,6}(r\d+)?$', stem):
            continue
        parsed = parse_dolphin_ini(text)
        for fmt in ("gecko", "ar_gc"):
            lines, seen = [], set()
            for name, codes in parsed[fmt]:
                if not name or not codes or any(c is None for c in codes):
                    continue
                desc = re.sub(r'[\t\r\n+]+', ' ', name).strip()
                code = "+".join(codes)
                if (desc, code) in seen: continue
                seen.add((desc, code))
                lines.append("%s\t%s\n" % (desc, code))
            if lines:
                games += 1
                cheats += len(lines)
                buf.append("@gc\t%s\t%s\n" % (fmt, stem))
                buf.extend(lines)
    return buf, games, cheats


def sources(src):
    if src and os.path.isdir(src):
        root = src
        for sub in os.listdir(src):
            if os.path.isdir(os.path.join(src, sub, "cht")):
                root = os.path.join(src, sub)
        cht = os.path.join(root, "cht")
        for dname, sys_ in DIRS.items():
            p = os.path.join(cht, dname)
            if os.path.isdir(p):
                for f in sorted(os.listdir(p)):
                    if f.endswith(".cht"):
                        yield sys_, f[:-4], open(os.path.join(p, f), encoding="utf-8", errors="replace").read()
        return
    url = "https://github.com/libretro/libretro-database/archive/%s.tar.gz" % COMMIT
    data = urllib.request.urlopen(url).read()
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tf:
        for m in tf:
            if not (m.isfile() and m.name.endswith(".cht")): continue
            parts = m.name.split("/")
            if len(parts) == 4 and parts[1] == "cht" and parts[2] in DIRS:
                yield DIRS[parts[2]], parts[3][:-4], tf.extractfile(m).read().decode("utf-8", "replace")


def main():
    args = sys.argv[1:]
    dolphin = None
    gc_only = "--gc-only" in args
    if gc_only:
        args.remove("--gc-only")
    if "--dolphin" in args:
        i = args.index("--dolphin")
        dolphin = args[i + 1]
        del args[i:i + 2]
    src = args[0] if len(args) > 0 else None
    out = args[1] if len(args) > 1 else os.path.join(os.path.dirname(__file__), "..", "data", "cheats_db.bin")
    buf = ["RHCHT1\n"]
    games = cheats = 0
    seen_blocks = {}
    if gc_only:   # keep the existing non-gc sections
        old = zlib.decompress(open(out, "rb").read()).decode("utf-8")[len("RHCHT1\n"):]
        skipping = False
        for ln in old.splitlines(True):
            if ln.startswith("@"):
                skipping = ln.startswith("@gc\t")
                if not skipping:
                    games += 1
            elif not skipping and ln.strip():
                cheats += 1
            if not skipping:
                buf.append(ln)
    for sys_, stem, text in ([] if gc_only else sources(src)):
        fmt = file_format(sys_, stem)
        lines, seen = [], set()
        for c in parse(text):
            desc = re.sub(r'[\t\r\n]+', ' ', c["desc"]).strip()
            code = c["code"].strip()
            if not desc or not code or code.upper() in ("N/A", "NONE"): continue
            f = code_format(sys_, fmt, code)
            if (desc, code) in seen: continue
            seen.add((desc, code))
            lines.append(("!%s\t" % f if f != fmt else "") + "%s\t%s\n" % (desc, code))
        if not lines: continue
        games += 1
        cheats += len(lines)
        buf.append("@%s\t%s\t%s\n" % (sys_, fmt, stem))
        buf.extend(lines)
    gbuf, gg, gcnt = gc_sections(dolphin)
    buf.extend(gbuf)
    print("gc games(sections)=%d gc cheats=%d" % (gg, gcnt))
    games += gg
    cheats += gcnt
    raw = "".join(buf).encode("utf-8")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(zlib.compress(raw, 9))
    print("games=%d cheats=%d raw=%d packed=%d -> %s" % (games, cheats, len(raw), os.path.getsize(out), out))


if __name__ == "__main__":
    main()
