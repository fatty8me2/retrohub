"""Builds a fake shared pack (no real ROMs): python qa/make_pack_fixture.py <outdir>
<outdir>/<Console>/... mixes loose .7z/.zip files and game folders, like the real Drive GamePack2 layout."""
import os, sys, struct, subprocess, shutil, tempfile
TAR = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "tar.exe")

NOISE_MB = float(os.environ.get("PACK_FIXTURE_NOISE_MB", "0"))   # >0: incompressible payload so the archives are really that big (progress-bar screenshots)

def gc_iso(gid, title, size=600 * 1024):
    b = bytearray(os.urandom(int(NOISE_MB * 1048576)) if NOISE_MB else size)
    if len(b) < size: b.extend(bytes(size - len(b)))
    b[0:0x440] = bytes(0x440)
    b[0:6] = gid.encode()
    struct.pack_into(">I", b, 0x1C, 0xC2339F3D)
    b[0x20:0x20 + len(title)] = title.encode()
    return bytes(b)

def pack(dest, name, data, fmt):
    d = tempfile.mkdtemp(prefix="pf_")
    try:
        open(os.path.join(d, name), "wb").write(data)
        r = subprocess.run([TAR, "-a", "-cf", dest, "-C", d, name], capture_output=True)
        if r.returncode: raise SystemExit("tar failed: %s" % r.stderr)
    finally:
        shutil.rmtree(d, ignore_errors=True)

def pack_multi(dest, members):
    """members: [(relative/path, bytes)] in archive order (a macOS __MACOSX fork first, sub folders, readme...)."""
    d = tempfile.mkdtemp(prefix="pf_")
    try:
        names = []
        for rel, data in members:
            p = os.path.join(d, *rel.split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "wb").write(data)
            names.append(rel)
        r = subprocess.run([TAR, "-a", "-cf", dest, "-C", d] + names, capture_output=True)
        if r.returncode: raise SystemExit("tar failed: %s" % r.stderr)
    finally:
        shutil.rmtree(d, ignore_errors=True)

def gcz_raw(gid, title, blocks=40000):
    """A GCZ whose block table is as long as a real disc's (~0.5 MB), so block 0 lies far from the file start; blocks stored raw."""
    h = bytearray(0x440); h[0:6] = gid.encode(); struct.pack_into(">I", h, 0x1C, 0xC2339F3D); h[0x20:0x20 + len(title)] = title.encode()
    hdr = struct.pack("<IIQQII", 0xB10BC001, 0, 0x440, 0x100000, 0x8000, blocks)
    ptrs = b"".join(struct.pack("<Q", (1 << 63) | (0 if i == 0 else 0x440 + i)) for i in range(blocks))
    return hdr + ptrs + bytes(4 * blocks) + bytes(h) + bytes(0x2000)

def ncsd_3ds(tid, product):
    b = bytearray(0x6000)
    b[0x100:0x104] = b"NCSD"; struct.pack_into("<I", b, 0x120, 0x20); struct.pack_into("<I", b, 0x124, 0x1000)
    n = 0x4000
    b[n + 0x100:n + 0x104] = b"NCCH"; struct.pack_into("<Q", b, n + 0x108, tid); struct.pack_into("<Q", b, n + 0x118, tid)
    b[n + 0x150:n + 0x150 + len(product)] = product.encode(); b[n + 0x188 + 7] = 0x04
    return bytes(b)

def ps2_cd_raw(serial, key="BOOT2", drive="cdrom0"):
    """A tiny CD-ROM XA image (MODE2/2352, form 1 user data at +24) holding an ISO9660 volume whose SYSTEM.CNF says BOOT2 = cdrom0:\\<serial>;1 (key="BOOT" for a PlayStation 1 disc)."""
    sync = b"\x00" + b"\xff" * 10 + b"\x00"
    def sector(lba, data):
        m = lba + 150
        hdr = bytes([m // 4500 // 1, (m // 75) % 60, m % 75, 2])
        hdr = bytes([(m // 75 // 60) % 100, (m // 75) % 60, m % 75, 2])
        bcd = bytes(((v // 10) << 4) | (v % 10) for v in hdr[:3]) + bytes([2])
        sub = b"\x00\x00\x08\x00" * 2
        body = (data + bytes(2048))[:2048]
        return sync + bcd + sub + body + bytes(280)
    cnf = ("%s = %s:\\%s;1\nVER = 1.00\nVMODE = NTSC\n" % (key, drive, serial)).encode()
    def rec(name, extent, size, flags):
        n = name
        r = bytearray(33 + len(n)); r[0] = len(r) + (len(r) & 1); r = r + bytes(r[0] - len(r))
        r[2:6] = extent.to_bytes(4, "little"); r[6:10] = extent.to_bytes(4, "big")
        r[10:14] = size.to_bytes(4, "little"); r[14:18] = size.to_bytes(4, "big")
        r[25] = flags; r[28:30] = (1).to_bytes(2, "little"); r[30:32] = (1).to_bytes(2, "big"); r[32] = len(n); r[33:33 + len(n)] = n
        return bytes(r)
    root = rec(b"\x00", 18, 2048, 2) + rec(b"\x01", 18, 2048, 2) + rec(b"SYSTEM.CNF;1", 19, len(cnf), 0)
    pvd = bytearray(2048); pvd[0] = 1; pvd[1:6] = b"CD001"; pvd[6] = 1
    pvd[80:84] = (20).to_bytes(4, "little"); pvd[84:88] = (20).to_bytes(4, "big")
    pvd[128:130] = (2048).to_bytes(2, "little"); pvd[130:132] = (2048).to_bytes(2, "big")
    pvd[156:156 + 34] = rec(b"\x00", 18, 2048, 2)[:34]
    term = bytearray(2048); term[0] = 255; term[1:6] = b"CD001"; term[6] = 1
    out = bytearray()
    for lba in range(0, 16): out += sector(lba, b"")
    out += sector(16, bytes(pvd)); out += sector(17, bytes(term)); out += sector(18, root); out += sector(19, cnf)
    for lba in range(20, 40): out += sector(lba, b"")
    return bytes(out)

def main(out):
    shutil.rmtree(out, ignore_errors=True)
    for c in ("GameCube", "PS2", "PS1", "Wii U", "GBA", "N64", "NDS"): os.makedirs(os.path.join(out, c))
    gc = os.path.join(out, "GameCube")
    # loose archives, the real-world case (title carried by the file name, header title differs)
    pack(os.path.join(gc, "Gauntlet - Dark Legacy (USA).7z"), "Gauntlet - Dark Legacy (USA).iso", gc_iso("GUNE5D", "Gauntlet Dark Legacy"), "7z")
    pack(os.path.join(gc, "Fakeland Zip (USA).zip"), "Fakeland Zip (USA).iso", gc_iso("GFZE01", "Fakeland Zip"), "zip")
    pack(os.path.join(gc, "Pikmin.zip"), "Pikmin.iso", gc_iso("GPIE01", "Pikmin"), "zip")
    pack(os.path.join(gc, "Generic Inner Game (USA).7z"), "game.iso", gc_iso("GGIE01", "Whatever"), "7z")   # inner name unrelated to the pack title
    # a game FOLDER holding a .7z (named differently from the folder)
    os.makedirs(os.path.join(gc, "Folder Style Game"))
    pack(os.path.join(gc, "Folder Style Game", "disc1.7z"), "disc1.iso", gc_iso("GFSE01", "Folder Style Game"), "7z")
    # PS2 / PS1 loose archives holding an .iso / .bin
    iso = bytearray(700 * 1024); iso[0x8001:0x8006] = b"CD001"
    pack(os.path.join(out, "PS2", "Gauntlet - Dark Legacy (USA).7z"), "Gauntlet - Dark Legacy (USA).iso", bytes(iso), "7z")
    pack(os.path.join(out, "PS2", "Fakeland PS2 (USA).zip"), "Fakeland PS2 (USA).iso", bytes(iso), "zip")
    pack(os.path.join(out, "PS1", "Crash Bandicoot (USA).7z"), "Crash Bandicoot (USA).bin", bytes(iso), "7z")
    # Wii U .wua in a loose 7z
    pack(os.path.join(out, "Wii U", "Fakeland WiiU.7z"), "Fakeland WiiU.wua", b"WUA0" + bytes(64 * 1024), "7z")
    # control: GBA folder-style with a plain .gba
    g = os.path.join(out, "GBA", "Control GBA Game"); os.makedirs(g)
    rom = bytearray(131072); rom[0xA0:0xAC] = b"CONTROLGBA\0\0"
    rom[0xAC:0xB0] = b"CTRL"; rom[0xB2] = 0x96; rom[0xBD] = (-(sum(rom[0xA0:0xBD]) + 0x19)) & 0xFF
    open(os.path.join(g, "control.gba"), "wb").write(bytes(rom))
    # PS2 CDs filed under GameCube in the pack (the real Gauntlet - Dark Legacy 7z is one): cue + bin + a readme, like Vimm's Lair
    def cdset(arc, stem, bin_data, readme=True):
        d = tempfile.mkdtemp(prefix="pf_")
        try:
            open(os.path.join(d, stem + ".bin"), "wb").write(bin_data)
            open(os.path.join(d, stem + ".cue"), "w").write('FILE "%s.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % stem)
            names = [stem + ".bin", stem + ".cue"]
            if readme:
                open(os.path.join(d, "Vimm's Lair.txt"), "w").write("downloaded from the vault\n"); names.append("Vimm's Lair.txt")
            r = subprocess.run([TAR, "-a", "-cf", arc, "-C", d] + names, capture_output=True)
            if r.returncode: raise SystemExit("tar failed: %s" % r.stderr)
        finally:
            shutil.rmtree(d, ignore_errors=True)
    raw = lambda n: os.urandom(n)
    # 1) named like a PS2 game (name database)
    cdset(os.path.join(gc, "Gauntlet - Seven Sorrows (USA).7z"), "Gauntlet - Seven Sorrows (USA)", raw(900 * 1024))
    # 2) not a known name: SYSTEM.CNF (BOOT2) inside a raw 2352-byte-sector ISO9660 track decides
    cdset(os.path.join(gc, "Fakeland CD.7z"), "Fakeland CD", ps2_cd_raw("SLUS_999.99"))
    # 3) nothing identifiable at all (must fail cleanly)
    cdset(os.path.join(gc, "Unplayable Thing.7z"), "Unplayable Thing", raw(300 * 1024))
    # N64: a loose big-endian image in a game folder, and a zipped one (the zip is unpacked into rom-cache by the scan)
    def n64(tag):
        b = bytearray(1024 * 1024 + 4096); b[0:4] = bytes([0x80, 0x37, 0x12, 0x40]); b[0x20:0x34] = tag.ljust(20).encode()[:20]; b[0x3B:0x3F] = b"NFK0"; b[0x1000:0x1010] = tag.encode()[:16].ljust(16)
        return bytes(b)
    g = os.path.join(out, "N64", "Fakeland 64"); os.makedirs(g)
    open(os.path.join(g, "fakeland64.z64"), "wb").write(n64("FAKELAND 64"))
    pack(os.path.join(out, "N64", "Zipland Cart.zip"), "Zipland Cart.z64", n64("FAKELAND ZIP"), "zip")
    # DS: loose .nds with a valid header (logo crc 0xCF56, header crc16)
    def nds(tag, code):
        h = bytearray(0x4000); h[0:12] = tag.ljust(12).encode()[:12]; h[0x0C:0x10] = code.encode(); h[0x15C:0x15E] = (0xCF56).to_bytes(2, "little")
        crc = 0xFFFF
        for x in h[:0x15E]:
            crc ^= x
            for _ in range(8): crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
        h[0x15E:0x160] = crc.to_bytes(2, "little"); return bytes(h)
    g = os.path.join(out, "NDS", "Fakeland DS"); os.makedirs(g)
    open(os.path.join(g, "fakeland.nds"), "wb").write(nds("FAKELANDDS", "AFKE"))
    # ---- formats-matrix cases: awkward containers / names a real shared pack has (see src/common/test_formats_matrix.cpp for the full matrix) ----
    # byte-swapped N64 (.v64) in a 7z: a sub folder, a macOS fork (same extension) listed FIRST, a readme
    v64 = bytearray(n64("FAKELAND V64"));
    for i in range(0, len(v64) - 1, 2): v64[i], v64[i + 1] = v64[i + 1], v64[i]
    pack_multi(os.path.join(out, "N64", "Junky V64 Cart (USA).7z"),
               [("__MACOSX/._Junky V64 Cart (USA).v64", os.urandom(4096)), ("Some Folder/Junky V64 Cart (USA).v64", bytes(v64)), ("readme.txt", b"hello\r\n")])
    # a disc image called "disc.iso" in a zip next to a .DS_Store: the title is the pack's, never "disc"
    pack_multi(os.path.join(gc, "Generic Disc Game.zip"), [(".DS_Store", os.urandom(6148)), ("disc.iso", gc_iso("GGDE01", "Header Title"))])
    # a .gcz whose first block sits behind a full-size block table
    pack(os.path.join(gc, "Compressed GCZ (USA).7z"), "Compressed GCZ (USA).gcz", gcz_raw("GCZE01", "Compressed GCZ"), "7z")
    # a PlayStation 1 CD (cue + bin + readme) with a name no database knows: SYSTEM.CNF decides
    cdset(os.path.join(out, "PS1", "Fakeland CD One.7z"), "Fakeland CD One", ps2_cd_raw("SLUS_999.31", "BOOT", "cdrom"))
    # a 3DS image in a 7z
    os.makedirs(os.path.join(out, "3DS"))
    pack(os.path.join(out, "3DS", "Fakeland Three D (USA).7z"), "game.3ds", ncsd_3ds(0x0004000000FA2100, "CTR-P-FKCA"), "7z")
    # a Wii U .wua called game.wua in a zip
    pack(os.path.join(out, "Wii U", "Generic Wua Game.zip"), "game.wua", b"WUA0" + bytes(64 * 1024), "zip")
    print("fixture ready:", out)

if __name__ == "__main__": main(sys.argv[1])
