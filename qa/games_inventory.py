"""Read-only inventory of game files (ROMs / discs / Wii U) for catalog work.

Usage: python -I qa/games_inventory.py [ROOT ...]
Never writes into the scanned folders. Archives are listed with zipfile and
only the header bytes of the inner ROM are read (nothing is extracted).
Output: one tab-separated line per game: source, system, size, identity.
"""
import hashlib
import os
import sys
import zipfile

ROM_EXT = {".z64", ".v64", ".n64", ".gba", ".gb", ".gbc", ".iso", ".gcm", ".rvz",
           ".wbfs", ".ciso", ".nkit", ".gcz", ".wia"}
WIIU_EXT = {".wud", ".wux", ".rpx", ".wua"}
OTHER_EXT = {".nds", ".3ds", ".cia", ".xci", ".nsp", ".cue", ".bin", ".chd", ".pbp", ".ps1", ".iso"}
ARCHIVE_EXT = {".zip", ".7z"}

DEFAULT_ROOTS = [
    r"C:\Users\fatty\Desktop\Everything\Games",
    r"C:\Users\fatty\Desktop\Everything\N64Games",
    r"C:\Users\fatty\Desktop\Everything\Cemu_2.6",
    r"C:\Users\fatty\Desktop\Everything\project64k",
    r"C:\Users\fatty\Desktop\Everything\simple64",
]


def ext_of(name):
    return os.path.splitext(name)[1].lower()


def n64_normalize(img):
    m = img[:4]
    if m == b"\x80\x37\x12\x40":
        return img
    if m == b"\x37\x80\x40\x12":  # v64: byteswapped pairs
        b = bytearray(img)
        b[0::2], b[1::2] = img[1::2], img[0::2]
        return bytes(b)
    if m == b"\x40\x12\x37\x80":  # n64: little-endian words
        b = bytearray(len(img))
        for i in range(0, len(img) - 3, 4):
            b[i:i + 4] = img[i:i + 4][::-1]
        return bytes(b)
    return None


def ident_n64(full):
    z = n64_normalize(full)
    if z is None:
        return "n64?: unknown byte order"
    name = z[0x20:0x34].decode("latin-1").rstrip(" \x00")
    gid = z[0x3B:0x3F].decode("latin-1")
    region = chr(z[0x3E]) if len(z) > 0x3E else "?"
    md5 = hashlib.md5(z).hexdigest().upper()
    return f"title='{name}' id={gid} region={region} md5={md5}"


def ident_gba(head):
    if len(head) < 0xC0:
        return "gba?: short"
    if head[0xB2] != 0x96:
        return "gba?: bad fixed byte 0xB2"
    title = head[0xA0:0xAC].decode("latin-1").rstrip("\x00 ")
    code = head[0xAC:0xB0].decode("latin-1")
    maker = head[0xB0:0xB2].decode("latin-1")
    return f"title='{title}' code={code} maker={maker}"


def ident_gb(head):
    if len(head) < 0x150:
        return "gb?: short"
    raw = head[0x134:0x144]
    title = raw.split(b"\x00")[0].decode("latin-1", "replace")
    cgb = head[0x143]
    kind = "GBC" if cgb in (0x80, 0xC0) else "GB"
    gsum = (head[0x14E] << 8) | head[0x14F]
    hsum_ok = (sum(head[0x134:0x14D]) + 0x19) & 0xFF
    hdr_ok = "ok" if ((-sum(head[0x134:0x14D]) - 0x19) & 0xFF) == head[0x14D] else "BAD"
    return f"title='{title}' kind={kind} global=0x{gsum:04x} hdr-checksum={hdr_ok}"


def ident_gc(head):
    if len(head) < 0x20 or head[0x1C:0x20] != b"\xC2\x33\x9F\x3D":
        return "gc?: no GC magic"
    disc_id = head[0:6].decode("latin-1")
    disc = head[6]
    title = head[0x20:0x60].split(b"\x00")[0].decode("latin-1", "replace")
    return f"disc_id={disc_id} disc={disc} title='{title}'"


def identify(name_for_ext, head, full=None):
    e = ext_of(name_for_ext)
    if e in (".z64", ".v64", ".n64"):
        return "n64", ident_n64(full if full is not None else head)
    if e == ".gba":
        return "gba", ident_gba(head)
    if e in (".gb", ".gbc"):
        return ("gbc" if e == ".gbc" else "gb"), ident_gb(head)
    if e in (".iso", ".gcm", ".rvz", ".wbfs", ".ciso", ".nkit", ".gcz", ".wia"):
        return "gc?", ident_gc(head)
    return "?", "unidentified"


def report(src, system, size, ident):
    print(f"{src}\t{system}\t{size}\t{ident}")


def scan_zip(path):
    try:
        zf = zipfile.ZipFile(path)
    except Exception as ex:  # corrupt / partial archive
        report(path, "archive-error", os.path.getsize(path), str(ex))
        return
    with zf:
        entries = [i for i in zf.infolist() if not i.is_dir()]
        if not entries:
            report(path, "archive-empty", os.path.getsize(path), "")
        for info in entries:
            e = ext_of(info.filename)
            src = f"{path} :: {info.filename}"
            if e in ROM_EXT or e in WIIU_EXT or e in OTHER_EXT:
                if e in WIIU_EXT:
                    report(src, "wiiu", info.file_size, "not supported")
                    continue
                if e in OTHER_EXT and e not in (".iso",):
                    report(src, "other", info.file_size, "not supported")
                    continue
                want_full = e in (".z64", ".v64", ".n64")
                with zf.open(info) as f:
                    data = f.read() if want_full else f.read(0x200)
                head = data[:0x200] if not want_full else data
                system, ident = identify(info.filename, head, data if want_full else None)
                report(src, system, info.file_size, ident)
            elif e in ARCHIVE_EXT:
                report(src, "nested-archive", info.file_size, "not listed")
            else:
                # Non-game member (readme, nfo, save...): skipped silently
                pass


def scan_file(path):
    e = ext_of(path)
    size = os.path.getsize(path)
    if e in ARCHIVE_EXT:
        if e == ".zip":
            scan_zip(path)
        else:
            report(path, "7z", size, "not listed (7z not opened)")
        return
    if e in WIIU_EXT:
        report(path, "wiiu", size, "not supported")
        return
    if e in ROM_EXT or e in OTHER_EXT:
        if e in OTHER_EXT and e != ".iso":
            report(path, "other", size, "not supported")
            return
        with open(path, "rb") as f:
            head = f.read(0x200)
        if e in (".z64", ".v64", ".n64"):
            with open(path, "rb") as f:
                full = f.read()
            system, ident = identify(path, head, full)
        else:
            system, ident = identify(path, head)
        report(path, system, size, ident)


def main(roots):
    for root in roots:
        if not os.path.isdir(root):
            print(f"# missing folder: {root}", file=sys.stderr)
            continue
        for dirpath, _dirs, files in os.walk(root):
            for name in sorted(files):
                full = os.path.join(dirpath, name)
                e = ext_of(name)
                if e in ROM_EXT | WIIU_EXT | OTHER_EXT | ARCHIVE_EXT:
                    if name.lower().endswith(".crdownload"):
                        continue
                    try:
                        scan_file(full)
                    except Exception as ex:
                        report(full, "error", 0, str(ex))
                elif name.lower().endswith(".crdownload"):
                    report(full, "partial-download", os.path.getsize(full), "skipped")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    main(sys.argv[1:] or DEFAULT_ROOTS)
