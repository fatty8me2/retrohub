#!/usr/bin/env python3
"""Writes two tiny original homebrew test ROMs (CC0 / public domain, hand-assembled here) for the NES and SNES cores.

  nes_test.nes  NROM-128, battery. NMI handler: counter at $00 changes the backdrop colour every frame, writes the
                counter to battery RAM $6000 (-> the .srm) and a running sum to $6001.
  snes_test.sfc LoROM 32 KB, 2 KB battery SRAM. NMI handler: counter changes backdrop colour 0 every frame and is
                stored to SRAM $70:0000 / $70:0001 (-> the .srm).

Usage: make_nes_snes_roms.py <out_dir>   (default deps/testroms)
"""
import os
import sys


def make_nes():
    code = bytearray()
    # reset at $C000 (offsets used below: v1 at 20, v2 at 25, forever at 40)
    code += bytes([0x78, 0xD8, 0xA2, 0x40, 0x8E, 0x17, 0x40, 0xA2, 0xFF, 0x9A, 0xE8,
                   0x8E, 0x00, 0x20, 0x8E, 0x01, 0x20, 0x8E, 0x10, 0x40])
    assert len(code) == 20
    code += bytes([0x2C, 0x02, 0x20, 0x10, 0xFB])          # v1: BIT $2002 / BPL v1
    code += bytes([0x2C, 0x02, 0x20, 0x10, 0xFB])          # v2
    code += bytes([0xA9, 0x80, 0x8D, 0x00, 0x20])          # NMI on
    code += bytes([0xA9, 0x1E, 0x8D, 0x01, 0x20])          # rendering on
    assert len(code) == 40
    code += bytes([0x4C, 0x28, 0xC0])                      # forever: JMP $C028
    code += bytes([0xEA] * (0x40 - len(code)))
    nmi = bytes([0xE6, 0x00,                               # INC $00
                 0xA9, 0x3F, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20,   # PPU addr $3F00
                 0xA5, 0x00, 0x29, 0x3F, 0x8D, 0x07, 0x20,                     # backdrop = counter & $3F
                 0xA9, 0x00, 0x8D, 0x05, 0x20, 0x8D, 0x05, 0x20,               # scroll 0,0
                 0xA5, 0x00, 0x8D, 0x00, 0x60,                                 # STA $6000
                 0x18, 0xA5, 0x01, 0x65, 0x00, 0x85, 0x01, 0x8D, 0x01, 0x60,   # sum -> $01, $6001
                 0x40])
    code += nmi
    prg = bytearray(code) + bytearray([0xFF] * (16384 - len(code)))
    prg[0x3FFA:0x3FFC] = bytes([0x40, 0xC0])               # NMI   $C040
    prg[0x3FFC:0x3FFE] = bytes([0x00, 0xC0])               # RESET $C000
    prg[0x3FFE:0x4000] = bytes([0x00, 0xC0])               # IRQ
    hdr = bytes([ord('N'), ord('E'), ord('S'), 0x1A, 1, 1, 0x02, 0x00]) + bytes(8)   # NROM, battery
    chr_rom = bytes(8192)
    return hdr + bytes(prg) + chr_rom


def make_snes():
    rom = bytearray([0xFF] * 0x8000)
    reset = bytes([0x78, 0x18, 0xFB,                   # SEI, CLC, XCE (native)
                   0xC2, 0x30, 0xA2, 0xFF, 0x1F, 0x9A,  # REP #$30, LDX #$1FFF, TXS
                   0xE2, 0x20,                         # SEP #$20
                   0xA9, 0x8F, 0x8D, 0x00, 0x21,       # force blank
                   0x9C, 0x00, 0x42,                   # STZ $4200
                   0x9C, 0x21, 0x21,                   # CGRAM addr 0
                   0xA9, 0x1F, 0x8D, 0x22, 0x21, 0xA9, 0x00, 0x8D, 0x22, 0x21,
                   0xA9, 0x0F, 0x8D, 0x00, 0x21,       # screen on
                   0xA9, 0x80, 0x8D, 0x00, 0x42,       # NMI on
                   0x80, 0xFE])                        # BRA self
    rom[0:len(reset)] = reset
    nmi = bytes([0xE6, 0x00,                           # INC $00
                 0xAD, 0x10, 0x42,                     # LDA $4210 (ack)
                 0x9C, 0x21, 0x21,                     # CGRAM addr 0
                 0xA5, 0x00, 0x8D, 0x22, 0x21,         # low byte = counter
                 0xA9, 0x03, 0x8D, 0x22, 0x21,         # high byte
                 0xA5, 0x00, 0x8F, 0x00, 0x00, 0x70,   # STA $700000
                 0x18, 0xA5, 0x01, 0x65, 0x00, 0x85, 0x01, 0x8F, 0x01, 0x00, 0x70,   # sum -> $700001
                 0x40])
    rom[0x40:0x40 + len(nmi)] = nmi
    rom[0x60] = 0x40                                   # RTI for the unused vectors
    h = 0x7FC0
    rom[h:h + 21] = b"RETROHUB SNES TEST".ljust(21)
    rom[h + 0x15] = 0x20                               # LoROM, slow
    rom[h + 0x16] = 0x02                               # ROM + RAM + battery
    rom[h + 0x17] = 0x05                               # 32 KB
    rom[h + 0x18] = 0x01                               # 2 KB SRAM
    rom[h + 0x19] = 0x01                               # USA/NTSC
    rom[h + 0x1A] = 0x33
    rom[h + 0x1C:h + 0x20] = bytes([0xFF, 0xFF, 0x00, 0x00])
    vec = {0x7FE4: 0x8060, 0x7FE6: 0x8060, 0x7FE8: 0x8060, 0x7FEA: 0x8040, 0x7FEE: 0x8060,
           0x7FF4: 0x8060, 0x7FF8: 0x8060, 0x7FFA: 0x8040, 0x7FFC: 0x8000, 0x7FFE: 0x8060}
    for off, v in vec.items():
        rom[off] = v & 0xFF
        rom[off + 1] = v >> 8
    s = sum(rom) & 0xFFFF
    c = s ^ 0xFFFF
    rom[h + 0x1C:h + 0x20] = bytes([c & 0xFF, c >> 8, s & 0xFF, s >> 8])
    return bytes(rom)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join("deps", "testroms")
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "nes_test.nes"), "wb").write(make_nes())
    open(os.path.join(out, "snes_test.sfc"), "wb").write(make_snes())
    print("wrote", out)
