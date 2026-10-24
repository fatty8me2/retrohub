"""Write a Dolphin .dtm input movie for GameCube port 1 that mashes buttons from power-on (QA only).
usage: gc_make_dtm.py out.dtm GALE01 [--seconds 240] [--pattern amash|startmash]
The movie starts from a clean boot (not a savestate), memory card slot A present, config not saved.
Pad bytes: byte0 = Start,A,B,X,Y,Z,Up,Down ; byte1 = Left,Right,L,R,disc,reset,connected,get_origin ; then LT,RT,sx,sy,cx,cy."""
import struct, sys, argparse

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("game_id")
ap.add_argument("--seconds", type=int, default=240)
ap.add_argument("--pattern", default="amash")
ap.add_argument("--lead", type=int, default=1200, help="idle frames before the first press")
a = ap.parse_args()

frames = a.seconds * 60
body = bytearray()
for f in range(frames):
    b0 = 0
    if f >= a.lead:
        k = (f - a.lead) % 40
        if a.pattern == "cycle":   # diagnostic: A, Start, B, X, Y, Z held 20 frames each in turn
            blk = ((f - a.lead) // 60) % 6
            if (f - a.lead) % 60 < 20:
                b0 |= [0x02, 0x01, 0x04, 0x08, 0x10, 0x20][blk]
        elif a.pattern == "hold":
            b0 |= 0x02
        elif k < 4:
            b0 |= 0x02            # A
        if a.pattern == "startmash" and (f - a.lead) % 400 < 4:
            b0 |= 0x01            # Start now and then
    body += bytes([b0, 0x40, 0, 0, 128, 128, 128, 128])

h = bytearray(256)
h[0:4] = b"DTM\x1a"
h[4:10] = a.game_id.encode().ljust(6, b"\0")[:6]
h[0x0A] = 0                     # not Wii
h[0x0B] = 0x01                  # GC port 1
h[0x0C] = 0                     # from power-on
struct.pack_into("<Q", h, 0x0D, frames)
struct.pack_into("<Q", h, 0x15, frames)
h[0x31:0x31 + 8] = b"hc-qa\0\0\0"
h[0x51:0x51 + 8] = b"Null\0\0\0\0"
h[0x89] = 0                     # bSaveConfig
h[0x97] = 0x01                  # memory card in slot A
h[0x98] = 0                     # bClearSave
open(a.out, "wb").write(bytes(h) + bytes(body))
print("wrote", a.out, frames, "frames")
