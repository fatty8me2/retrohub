"""Unpacks a tar archive (.tar.zst or .tar.gz) for RetroHub's Hookshot build kit. RetroHub runs it with the kit's own Python:

    python -I unpack_tar.py <archive> <dest-folder> [--strip N] [--include PATTERN ...]
    python -I unpack_tar.py --selftest

Why a script and not Windows' tar.exe: tar.exe refuses LLVM's zstd stream (its window is bigger than bsdtar allows). The
zstandard package reads it with max_window_size=2**31.

Safety (the archive is checked by hash before this runs, but the names are checked anyway):
  - every member name must be relative, with no "..", no drive letter or ":", no backslash and no NUL, or the whole unpack stops;
  - every member goes through tarfile's filter="data" (no absolute paths, no links out of the folder, no devices);
  - symbolic links are skipped; a hard link becomes a copy of the file it points to.
--strip N drops N leading folders from every name (like tar --strip-components). --include keeps only the members whose name
(after --strip) matches one of the patterns (fnmatch; "*" also matches "/"); folders are made as files need them.

Output lines for RetroHub: "@@progress <0..1>" while it works, then "@@done <files> <bytes>" or "@@error <text>" (exit code 1).
"""
import fnmatch
import os
import shutil
import sys
import tarfile


class Unsafe(Exception):
    pass


def safe_name(name, strip=0):
    """The member's name after stripping N leading folders, as a relative posix path ('' = nothing left). Raises Unsafe."""
    if not name or "\0" in name or "\\" in name:
        raise Unsafe("bad name in the archive: %r" % name)
    if name.startswith("/") or ":" in name:
        raise Unsafe("absolute path in the archive: %s" % name)
    parts = [p for p in name.split("/") if p not in ("", ".")]
    if any(p == ".." for p in parts):
        raise Unsafe("path with .. in the archive: %s" % name)
    return "/".join(parts[strip:])


class Counting:
    """Reads through to a file and remembers how far it got (the progress is the compressed bytes read)."""

    def __init__(self, f):
        self.f = f
        self.pos = 0

    def read(self, n=-1):
        b = self.f.read(n)
        self.pos += len(b)
        return b

    def readable(self):
        return True

    def close(self):
        self.f.close()


def open_stream(path, raw):
    low = path.lower()
    if low.endswith(".zst") or low.endswith(".zstd"):
        import zstandard
        return zstandard.ZstdDecompressor(max_window_size=2 ** 31).stream_reader(raw)
    return raw   # .tar.gz / .tgz / .tar: tarfile's own "r|*" reads them


def unpack(archive, dest, strip=0, include=None, progress=None):
    """Unpacks into dest. Returns (files, bytes). Raises Unsafe or tarfile.TarError on a bad archive."""
    include = include or []
    dest = os.path.abspath(dest)
    os.makedirs(dest, exist_ok=True)
    total = max(1, os.path.getsize(archive))
    files = size = 0
    last = -1.0
    with open(archive, "rb") as f:
        raw = Counting(f)
        stream = open_stream(archive, raw)
        with tarfile.open(fileobj=stream, mode="r|*") as tf:
            for m in tf:
                name = safe_name(m.name, strip)
                if not name:
                    continue
                if m.isdir():
                    continue
                if include and not any(fnmatch.fnmatchcase(name, p) for p in include):
                    continue
                if m.issym():
                    print("skipped a symbolic link: %s" % name, flush=True)
                    continue
                target = os.path.join(dest, *name.split("/"))
                if m.islnk():   # a hard link: a copy of the file it names (extracted earlier, or skipped by --include)
                    src = safe_name(m.linkname, strip)
                    src_path = os.path.join(dest, *src.split("/"))
                    if not src or not os.path.isfile(src_path):
                        print("skipped a link to a file that was not unpacked: %s -> %s" % (name, m.linkname), flush=True)
                        continue
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    shutil.copyfile(src_path, target)
                    files += 1
                    size += os.path.getsize(target)
                    continue
                if not m.isreg():
                    raise Unsafe("not a plain file or folder in the archive: %s" % name)
                m = m.replace(name=name, deep=False)
                tf.extract(m, dest, filter="data")
                files += 1
                size += m.size
                if progress and raw.pos / total - last >= 0.01:
                    last = raw.pos / total
                    progress(min(0.999, last))
    if progress:
        progress(1.0)
    return files, size


def selftest():
    import io
    import tempfile
    base = tempfile.mkdtemp(prefix="unpack_tar_selftest_")
    try:
        def make(path, members):
            with tarfile.open(path, "w:gz") as tf:
                for n, kind, data in members:
                    ti = tarfile.TarInfo(n)
                    if kind == "file":
                        ti.size = len(data)
                        tf.addfile(ti, io.BytesIO(data))
                    elif kind == "sym":
                        ti.type = tarfile.SYMTYPE
                        ti.linkname = data
                        tf.addfile(ti)
                    elif kind == "hard":
                        ti.type = tarfile.LNKTYPE
                        ti.linkname = data
                        tf.addfile(ti)
                    elif kind == "dir":
                        ti.type = tarfile.DIRTYPE
                        tf.addfile(ti)

        # the names that must stop the unpack
        for i, evil in enumerate(["../evil.txt", "top/../../evil.txt", "/abs.txt", "C:/drive.txt", "top/c:evil.txt", "top\\back.txt", "a/b/../../../x"]):
            a = os.path.join(base, "evil%d.tar.gz" % i)
            out = os.path.join(base, "out%d" % i)
            make(a, [("top/ok.txt", "file", b"ok"), (evil, "file", b"bad")])
            try:
                unpack(a, out, strip=1)
            except Unsafe:
                pass
            else:
                raise SystemExit("selftest failed: %r was not refused" % evil)
        if os.path.exists(os.path.join(base, "evil.txt")) or os.path.exists(os.path.join(os.path.dirname(base), "evil.txt")):
            raise SystemExit("selftest failed: a file escaped the folder")
        # good archive: strip, include, links
        a = os.path.join(base, "good.tar.gz")
        make(a, [("top/", "dir", b""), ("top/bin/tool.exe", "file", b"MZ"), ("top/bin/tool-link.exe", "hard", "top/bin/tool.exe"),
                 ("top/bin/sym.exe", "sym", "../../outside"), ("top/lib/a/b.txt", "file", b"b"), ("top/doc/skip.txt", "file", b"no")])
        out = os.path.join(base, "good")
        files, _ = unpack(a, out, strip=1, include=["bin/*", "lib/*"])
        want = {"bin/tool.exe": b"MZ", "bin/tool-link.exe": b"MZ", "lib/a/b.txt": b"b"}
        for n, data in want.items():
            p = os.path.join(out, *n.split("/"))
            if not os.path.isfile(p) or open(p, "rb").read() != data:
                raise SystemExit("selftest failed: %s was not unpacked" % n)
        if os.path.exists(os.path.join(out, "doc")) or os.path.exists(os.path.join(out, "bin", "sym.exe")) or files != 3:
            raise SystemExit("selftest failed: --include or the link rules")
        # a symlink member named outside the folder must not be created either
        print("selftest ok", flush=True)
    finally:
        shutil.rmtree(base, ignore_errors=True)


def main(argv):
    if argv[:1] == ["--selftest"]:
        selftest()
        return 0
    args = list(argv)
    strip = 0
    include = []
    pos = []
    while args:
        a = args.pop(0)
        if a == "--strip":
            strip = int(args.pop(0))
        elif a == "--include":
            while args and not args[0].startswith("--"):
                include.append(args.pop(0))
        else:
            pos.append(a)
    if len(pos) != 2:
        print("@@error usage: unpack_tar.py <archive> <dest> [--strip N] [--include PATTERN ...]", flush=True)
        return 2
    try:
        files, size = unpack(pos[0], pos[1], strip, include, lambda f: print("@@progress %.3f" % f, flush=True))
    except Unsafe as e:
        print("@@error %s" % e, flush=True)
        return 1
    except Exception as e:   # a damaged archive, a full disk, a missing package: said in one line
        print("@@error could not unpack %s: %s: %s" % (os.path.basename(pos[0]), type(e).__name__, e), flush=True)
        return 1
    print("@@done %d %d" % (files, size), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
