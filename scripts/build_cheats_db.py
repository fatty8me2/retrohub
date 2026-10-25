#!/usr/bin/env python3
"""Convert libretro-database cht/ into the compact RetroHub cheat bundle data/cheats_db.bin.

Source: https://github.com/libretro/libretro-database (cht/), pinned commit below. License: CC BY-SA 4.0
(see THIRD_PARTY.md). Usage:
    python scripts/build_cheats_db.py [path-to-libretro-database-checkout-or-extracted-tarball] [out]
With no source argument the pinned commit tarball is downloaded.

Bundle = zlib-compressed UTF-8 text:
    RHCHT1\n
    @<sys>\t<format>\t<game file stem>\n      one section per .cht file (sys: n64 gb gbc gba ps1)
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
    src = sys.argv[1] if len(sys.argv) > 1 else None
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "data", "cheats_db.bin")
    buf = ["RHCHT1\n"]
    games = cheats = 0
    seen_blocks = {}
    for sys_, stem, text in sources(src):
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
    raw = "".join(buf).encode("utf-8")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(zlib.compress(raw, 9))
    print("games=%d cheats=%d raw=%d packed=%d -> %s" % (games, cheats, len(raw), os.path.getsize(out), out))


if __name__ == "__main__":
    main()
