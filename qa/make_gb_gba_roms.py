#!/usr/bin/env python3
"""Writes tiny original homebrew test ROMs (hand-assembled here, public domain) for the Game Boy and GBA cores, so the rollback
SyncTest can run in CI without anybody's commercial game (audit netcore-M4).

  gb_test.gb   32 KB, MBC3 + TIMER + RAM + BATTERY (cart type 0x10, 8 KB RAM). Main loop: counter in WRAM, copied to battery RAM $A000
               (-> the .srm) and to the background palette register every pass; the MBC3 clock is latched and a seconds register read each
               pass, so the real-time-clock path (the .rtc file, sameboy_rtc=accurate under SyncTest) is exercised too.
               The 48-byte Nintendo logo at $104 is required by the hardware boot check (it is the same bytes in every Game Boy program).
  gba_test.gba the SIO multiplayer-mode ROM from make_siotest_rom.py, run on its own (nothing linked): timers, IO registers, palette writes.

Usage: make_gb_gba_roms.py <out_dir>   (default deps/testroms)
"""
import os
import subprocess
import sys

LOGO = bytes([0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
              0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
              0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E])


def make_gb(rtc=True):
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])      # nop ; jp $0150
    rom[0x104:0x134] = LOGO
    rom[0x134:0x13B] = b"RHTEST\0"
    rom[0x143] = 0x00                                       # DMG only
    rom[0x147] = 0x10 if rtc else 0x03                      # MBC3 + TIMER + RAM + BATTERY, or MBC1 + RAM + BATTERY
    rom[0x148] = 0x00                                       # 32 KB
    rom[0x149] = 0x02                                       # 8 KB RAM
    code = bytearray()
    code += bytes([0xF3, 0x31, 0xFE, 0xFF])                 # di ; ld sp,$FFFE
    code += bytes([0x3E, 0x0A, 0xEA, 0x00, 0x00])           # ld a,$0A ; ld ($0000),a   (RAM + RTC enable)
    loop = len(code)
    code += bytes([0x21, 0x00, 0xC0, 0x34, 0x7E])           # ld hl,$C000 ; inc (hl) ; ld a,(hl)
    code += bytes([0xEA, 0x00, 0xA0, 0xE0, 0x47])           # ld ($A000),a ; ldh ($47),a
    if rtc:
        code += bytes([0xAF, 0xEA, 0x00, 0x60, 0x3C, 0xEA, 0x00, 0x60])   # latch the clock: write 0 then 1 to $6000
        code += bytes([0x3E, 0x08, 0xEA, 0x00, 0x40])       # select RTC seconds
        code += bytes([0xFA, 0x00, 0xA0, 0xEA, 0x01, 0xC0]) # ld a,($A000) ; ld ($C001),a
        code += bytes([0xAF, 0xEA, 0x00, 0x40])             # back to RAM bank 0
    at = len(code)
    code += bytes([0x18, (loop - (at + 2)) & 0xFF])         # jr loop
    rom[0x150:0x150 + len(code)] = code
    x = 0
    for i in range(0x134, 0x14D):
        x = (x - rom[i] - 1) & 0xFF
    rom[0x14D] = x
    return bytes(rom)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join("deps", "testroms")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "gb_test.gb"), "wb") as f:
        f.write(make_gb())
    with open(os.path.join(out, "gb_test_nortc.gb"), "wb") as f:   # same program on MBC1: no clock
        f.write(make_gb(False))
    subprocess.check_call([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_siotest_rom.py"),
                           os.path.join(out, "gba_test.gba")])
    print("wrote", out)
