#!/usr/bin/env python3
"""Builds two tiny hand-assembled test ROMs for NES / SNES netplay QA (no toolchain needed).
Both read every controller each frame, fold all pads into a running hash and show it as the backdrop colour, so any
input difference between peers shows up in the emulation state (and in the video/audio output hash).
  netplay.nes : NROM; reads $4016/$4017 three times (pads 1-4 through a Four Score, then its signature bits)
  netplay.sfc : LoROM; reads pad 1 (D0), the port-2 multitap pads 2-5 by hand ($4201 bit 7 selects 2/3 or 4/5)
usage: make_nesnes_roms.py <out dir>"""
import os, struct, sys


class Asm:
    def __init__(self, org):
        self.org, self.b, self.labels, self.fix = org, bytearray(), {}, []

    def label(self, n): self.labels[n] = self.org + len(self.b)
    def raw(self, *xs): self.b += bytes(xs)
    def abs_(self, op, addr): self.b += bytes([op, addr & 255, addr >> 8])
    def rel(self, op, lab): self.b += bytes([op, 0]); self.fix.append((len(self.b) - 1, lab, 'rel'))
    def jmp(self, lab): self.b += bytes([0x4C, 0, 0]); self.fix.append((len(self.b) - 2, lab, 'abs'))

    def done(self):
        for pos, lab, kind in self.fix:
            t = self.labels[lab]
            if kind == 'rel':
                d = t - (self.org + pos + 1)
                assert -128 <= d <= 127
                self.b[pos] = d & 255
            else:
                self.b[pos], self.b[pos + 1] = t & 255, t >> 8
        return bytes(self.b)


def nes():
    a = Asm(0xC000)
    a.raw(0x78, 0xD8, 0xA2, 0xFF, 0x9A)                      # sei cld ldx #$ff txs
    a.raw(0xA9, 0x00); a.abs_(0x8D, 0x2000); a.raw(0xA9, 0x0A); a.abs_(0x8D, 0x2001)   # PPUCTRL = 0, PPUMASK = background on (a repainted frame, not a stale blank buffer)
    a.raw(0xA9, 0x00, 0x85, 0x10)                            # hash = 0
    a.label('vb'); a.abs_(0x2C, 0x2002); a.rel(0x10, 'vb')   # bit $2002 / bpl: wait for vblank
    a.raw(0xA9, 0x01); a.abs_(0x8D, 0x4016); a.raw(0xA9, 0x00); a.abs_(0x8D, 0x4016)   # strobe
    a.raw(0xA2, 0x08)                                         # ldx #8 (pads 1/2, then 3/4 below)
    for rnd, (z1, z2) in enumerate(((0x00, 0x01), (0x02, 0x03))):
        if rnd: a.raw(0xA2, 0x08)
        a.label('r%d' % rnd)
        a.abs_(0xAD, 0x4016); a.raw(0x4A, 0x26, z1)           # lda $4016 ; lsr ; rol z1
        a.abs_(0xAD, 0x4017); a.raw(0x4A, 0x26, z2)
        a.raw(0xCA); a.rel(0xD0, 'r%d' % rnd)                 # dex ; bne
    a.raw(0xA5, 0x10)                                         # lda hash
    for z in (0, 1, 2, 3):
        a.raw(0x45, z, 0x2A)                                  # eor z ; rol a
    a.raw(0x85, 0x10)
    a.raw(0xA9, 0x3F); a.abs_(0x8D, 0x2006); a.raw(0xA9, 0x00); a.abs_(0x8D, 0x2006)
    a.raw(0xA5, 0x10, 0x29, 0x3F); a.abs_(0x8D, 0x2007)       # backdrop colour = hash & $3f
    a.raw(0xA9, 0x00); a.abs_(0x8D, 0x2006); a.abs_(0x8D, 0x2006)   # reset the VRAM address (scroll) inside vblank
    a.jmp('vb')
    prg = a.done().ljust(16384 - 6, b'\xEA') + struct.pack('<HHH', 0xC000, 0xC000, 0xC000)   # nmi, reset, irq
    return b'NES\x1a\x01\x01\x00\x00' + bytes(8) + prg + bytes(8192)


def snes():
    a = Asm(0x8000)
    a.raw(0x78, 0x18, 0xFB)                                   # sei clc xce (emulation mode: 8-bit A)
    a.raw(0xA9, 0x8F); a.abs_(0x8D, 0x2100)                   # forced blank while setting up
    a.abs_(0x9C, 0x4200)                                      # no NMI / auto-joypad
    a.raw(0xA9, 0x0F); a.abs_(0x8D, 0x2100)                   # screen on (backdrop = CGRAM colour 0)
    a.raw(0xA9, 0x00, 0x85, 0x10)
    a.label('vb'); a.abs_(0xAD, 0x4210); a.rel(0x10, 'vb')    # wait for vblank (RDNMI bit 7)
    for rnd, (lo1, d0lo, d1lo) in enumerate(((0x00, 0x02, 0x04), (None, 0x06, 0x08))):
        a.raw(0xA9, 0x80 if rnd == 0 else 0x00); a.abs_(0x8D, 0x4201)   # multitap select: pads 2/3 then 4/5
        a.raw(0xA9, 0x01); a.abs_(0x8D, 0x4016); a.abs_(0x9C, 0x4016)   # strobe
        a.raw(0xA2, 0x10)
        a.label('r%d' % rnd)
        if lo1 is not None:
            a.abs_(0xAD, 0x4016); a.raw(0x4A, 0x26, lo1, 0x26, lo1 + 1)
        a.abs_(0xAD, 0x4017); a.raw(0x4A, 0x26, d0lo, 0x26, d0lo + 1, 0x4A, 0x26, d1lo, 0x26, d1lo + 1)
        a.raw(0xCA); a.rel(0xD0, 'r%d' % rnd)
    a.raw(0xA9, 0xFF); a.abs_(0x8D, 0x4201)
    a.raw(0xA5, 0x10)
    for z in range(10):
        a.raw(0x45, z, 0x2A)
    a.raw(0x85, 0x10)
    a.abs_(0x9C, 0x2121); a.abs_(0x8D, 0x2122); a.abs_(0x8D, 0x2122)   # CGRAM colour 0 = hash, hash
    a.jmp('vb')
    prg = bytearray(a.done().ljust(32768, b'\xFF'))
    h = 0x7FC0
    prg[h:h + 21] = b'RETROHUB NETPLAY TEST '[:21]
    prg[h + 21] = 0x20; prg[h + 22] = 0; prg[h + 23] = 5; prg[h + 24] = 0; prg[h + 25] = 1; prg[h + 26] = 0x33; prg[h + 27] = 0
    prg[0x7FFC:0x7FFE] = b'\x00\x80'
    prg[0x7FEA:0x7FEC] = b'\x00\x80'
    chk = sum(prg) & 0xFFFF
    # fix the checksum so snes9x's header scoring picks LoROM
    prg[h + 28:h + 32] = struct.pack('<HH', 0, 0)
    chk = sum(prg) & 0xFFFF
    prg[h + 28:h + 32] = struct.pack('<HH', chk ^ 0xFFFF, chk)
    return bytes(prg)


if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, 'netplay.nes'), 'wb').write(nes())
    open(os.path.join(out, 'netplay.sfc'), 'wb').write(snes())
    print('wrote netplay.nes, netplay.sfc to', out)
