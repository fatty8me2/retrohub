"""Builds a fake shared pack (no real ROMs): python qa/make_pack_fixture.py <outdir>
<outdir>/<Console>/... mixes loose .7z/.zip files and game folders, like the real Drive GamePack2 layout."""
import os, sys, struct, subprocess, shutil, tempfile
TAR = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "tar.exe")

def gc_iso(gid, title, size=600 * 1024):
    b = bytearray(size)
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

def main(out):
    shutil.rmtree(out, ignore_errors=True)
    for c in ("GameCube", "PS2", "PS1", "Wii U", "GBA"): os.makedirs(os.path.join(out, c))
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
    print("fixture ready:", out)

if __name__ == "__main__": main(sys.argv[1])
