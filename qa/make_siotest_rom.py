#!/usr/bin/env python3
"""Builds a tiny GBA ROM (hand-assembled ARM, no toolchain needed) that exercises multiplayer-mode SIO.
Both linked consoles run it: switch SIOCNT to multi-player mode, wait, put 0x001F in SIOMLT_SEND, start (master only),
wait, then show a colour on the backdrop: SIOMULTI1 | (own SIOCNT id bits << 1).
  linked correctly : console 1 (id 0) -> 0x001F (red), console 2 (id 1) -> 0x003F (red+bit)   [never white]
  not linked       : SIOMULTI1 stays 0xFFFF -> white
usage: make_siotest_rom.py out.gba"""
import struct, sys

def mov_imm(rd, imm8, rot=0): return 0xE3A00000 | rd << 12 | rot << 8 | imm8
def orr_imm(rd, rn, imm8, rot=0): return 0xE3800000 | rn << 16 | rd << 12 | rot << 8 | imm8
def and_imm(rd, rn, imm8, rot=0): return 0xE2000000 | 0x01000000 * 0 | rn << 16 | rd << 12 | rot << 8 | imm8  # AND imm: 0xE20
def ldr_lit(rd, off): return 0xE59F0000 | rd << 12 | off          # ldr rd,[pc,#off]
def strh(rd, rn, off): return 0xE1C000B0 | rn << 16 | rd << 12 | (off >> 4) << 8 | (off & 15)
def ldrh(rd, rn, off): return 0xE1D000B0 | rn << 16 | rd << 12 | (off >> 4) << 8 | (off & 15)
def subs_imm(rd, rn, imm8): return 0xE2500000 | rn << 16 | rd << 12 | imm8
def lsl_reg(rd, rm, sh): return 0xE1A00000 | rd << 12 | sh << 7 | rm
def or_reg(rd, rn, rm): return 0xE1800000 | rn << 16 | rd << 12 | rm
def bne(from_idx, to_idx): return 0x1A000000 | ((to_idx - from_idx - 2) & 0xFFFFFF)
def b(from_idx, to_idx): return 0xEA000000 | ((to_idx - from_idx - 2) & 0xFFFFFF)

code = []
def emit(w): code.append(w); return len(code) - 1
def delay(reg):           # ldr reg,=0x300000 ; loop: subs reg,reg,#1 ; bne loop
    emit(('lit', reg))
    i = emit(subs_imm(reg, reg, 1)); emit(bne(i + 1, i))

emit(mov_imm(0, 1, 3))                 # r0 = 0x04000000
emit(mov_imm(5, 5, 3))                 # r5 = 0x05000000
emit(0xE2806C01)                       # add r6,r0,#0x100  (strh offsets are 8 bit)
emit(mov_imm(1, 0)); emit(strh(1, 6, 0x34)); emit(strh(1, 0, 0))   # RCNT = 0, DISPCNT = 0 (backdrop only)
emit(mov_imm(1, 0x20, 12)); emit(orr_imm(1, 1, 3)); emit(strh(1, 6, 0x28))   # SIOCNT = 0x2003 (multi, 115200)
delay(3)
emit(mov_imm(1, 0x1F)); emit(strh(1, 6, 0x2A))   # SIOMLT_SEND = 0x001F
emit(mov_imm(1, 0x20, 12)); emit(orr_imm(1, 1, 3)); emit(orr_imm(1, 1, 0x80)); emit(strh(1, 6, 0x28))   # start (master)
delay(3)
emit(ldrh(1, 6, 0x22))                # SIOMULTI1
emit(ldrh(2, 6, 0x28))                # SIOCNT
emit(and_imm(2, 2, 0x30)); emit(lsl_reg(2, 2, 1)) if False else None
emit(and_imm(2, 2, 0x30)) if False else None
emit(0xE2022030)                       # and r2,r2,#0x30
emit(0xE1A02082)                       # mov r2,r2,lsl #1
emit(or_reg(1, 1, 2)) if False else emit(0xE1811002)   # orr r1,r1,r2
emit(strh(1, 5, 0))                    # PALETTE[0]
i = emit(0); code[i] = b(i, i)         # spin

# literal pool for delay loops: resolve 'lit' placeholders (ldr rd,[pc,#off] -> pool after code)
pool_idx = len(code)
code.append(0x00300000)
for idx, w in enumerate(list(code)):
    if isinstance(w, tuple):
        off = (pool_idx - idx) * 4 - 8
        code[idx] = ldr_lit(w[1], off)

rom = bytearray(0x200)
struct.pack_into('<I', rom, 0, 0xEA00002E)         # b 0xC0
rom[0xA0:0xAC] = b'SIOTEST\0\0\0\0\0'
rom[0xAC:0xB0] = b'ATST'
rom[0xB0:0xB2] = b'01'
rom[0xB2] = 0x96
chk = 0
for x in rom[0xA0:0xBD]: chk = (chk - x) & 0xFF
rom[0xBD] = (chk - 0x19) & 0xFF
for k, w in enumerate(code): struct.pack_into('<I', rom, 0xC0 + 4 * k, w)
rom += b'\0' * (0x2000 - len(rom))
open(sys.argv[1], 'wb').write(rom)
