#!/usr/bin/env python3
"""Builds a tiny, license-free PlayStation test disc (homebrew): ISO 9660 + SYSTEM.CNF + a PS-X EXE that resets the GPU, shows a
320x240 screen and fills a red background with a blue rectangle, then loops. Used by qa\\ps1_qa.ps1 and the library tests.

  python qa\\make_ps1_testdisc.py <out dir> [--serial SCUS-94570] [--name TestDisc] [--chd]

Writes <name>.bin (2048-byte sectors) + <name>.cue (MODE1/2048) and <name>.iso (same bytes). The boot serial only goes into
SYSTEM.CNF / the EXE file name (so identification tests can use a real catalog serial without any copyrighted data).
"""
import argparse
import os
import struct

SECTOR = 2048


def both32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)


def both16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)


def dir_record(name_bytes, extent, size, flags):
    nl = len(name_bytes)
    rl = 33 + nl + (1 if nl % 2 == 0 else 0)
    r = bytearray(rl)
    r[0] = rl
    r[2:10] = both32(extent)
    r[10:18] = both32(size)
    r[18:25] = bytes([126, 1, 1, 0, 0, 0, 0])
    r[25] = flags
    r[28:32] = both16(1)
    r[32] = nl
    r[33:33 + nl] = name_bytes
    return bytes(r)


def mips_program():
    code = []

    def lui(rt, imm): code.append(0x3C000000 | (rt << 16) | (imm & 0xFFFF))
    def ori(rt, rs, imm): code.append(0x34000000 | (rs << 21) | (rt << 16) | (imm & 0xFFFF))
    def sw(rt, base, off): code.append(0xAC000000 | (base << 21) | (rt << 16) | (off & 0xFFFF))

    T0, T1 = 8, 9
    lui(T0, 0x1F80)

    def put(off, val):
        lui(T1, val >> 16)
        ori(T1, T1, val & 0xFFFF)
        sw(T1, T0, off)

    for v in (0x00000000, 0x03000000, 0x08000001, 0x05000000, 0x06C60260, 0x07040010):   # GP1: reset, display on, 320x240, area, h/v range
        put(0x1814, v)
    for v in (0x020000FF, 0x00000000, 0x00F00140):   # GP0: fill the screen red
        put(0x1810, v)
    for v in (0x02FF0000, 0x00500050, 0x00800100):   # GP0: blue rectangle
        put(0x1810, v)
    loop_index = len(code)
    code.append(0)  # j loop (patched below)
    code.append(0)  # nop
    base = 0x80010000
    code[loop_index] = 0x08000000 | (((base + loop_index * 4) >> 2) & 0x03FFFFFF)
    return b''.join(struct.pack('<I', c) for c in code), base


def build(serial, name):
    cnf_serial = serial.replace('-', '_')
    exe_name = cnf_serial[:4] + '_' + serial[5:8] + '.' + serial[8:10]    # SCUS_945.70
    code, base = mips_program()
    tsize = (len(code) + 2047) // 2048 * 2048
    hdr = bytearray(2048)
    hdr[0:8] = b'PS-X EXE'
    struct.pack_into('<IIIIII', hdr, 0x10, base, 0, base, tsize, 0, 0)
    struct.pack_into('<II', hdr, 0x30, 0x801FFF00, 0)
    hdr[0x4C:0x4C + 55] = b'Sony Computer Entertainment Inc. for North America area'
    exe = bytes(hdr) + code.ljust(tsize, b'\0')
    cnf = ('BOOT = cdrom:\\%s;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFF00\r\n' % exe_name).encode('ascii')

    root_lba, cnf_lba, exe_lba = 20, 21, 22
    exe_sectors = len(exe) // SECTOR
    total = exe_lba + exe_sectors
    img = bytearray(total * SECTOR)

    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b'CD001'
    pvd[6] = 1
    pvd[8:40] = b'PLAYSTATION'.ljust(32)
    pvd[40:72] = name.upper()[:32].encode('ascii').ljust(32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(SECTOR)
    pvd[132:140] = both32(10)
    pvd[140:144] = struct.pack('<I', 18)
    pvd[148:152] = struct.pack('>I', 19)
    root_rec = dir_record(b'\0', root_lba, SECTOR, 2)
    pvd[156:156 + len(root_rec)] = root_rec
    for off in (813, 830, 847, 864):
        pvd[off:off + 16] = b'0' * 16
    pvd[881] = 1
    img[16 * SECTOR:17 * SECTOR] = pvd
    term = bytearray(SECTOR)
    term[0] = 255
    term[1:6] = b'CD001'
    term[6] = 1
    img[17 * SECTOR:18 * SECTOR] = term

    ptl = struct.pack('<BBIH', 1, 0, root_lba, 1) + b'\0\0'
    ptm = struct.pack('>BBIH', 1, 0, root_lba, 1) + b'\0\0'
    img[18 * SECTOR:18 * SECTOR + len(ptl)] = ptl
    img[19 * SECTOR:19 * SECTOR + len(ptm)] = ptm

    rootdir = dir_record(b'\0', root_lba, SECTOR, 2) + dir_record(b'\1', root_lba, SECTOR, 2)
    rootdir += dir_record(b'SYSTEM.CNF;1', cnf_lba, len(cnf), 0)
    rootdir += dir_record((exe_name + ';1').encode('ascii'), exe_lba, len(exe), 0)
    img[root_lba * SECTOR:root_lba * SECTOR + len(rootdir)] = rootdir
    img[cnf_lba * SECTOR:cnf_lba * SECTOR + len(cnf)] = cnf
    img[exe_lba * SECTOR:exe_lba * SECTOR + len(exe)] = exe
    return bytes(img)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--serial', default='SCUS-94570')
    ap.add_argument('--name', default='TestDisc')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    data = build(a.serial, a.name)
    with open(os.path.join(a.out, a.name + '.bin'), 'wb') as f:
        f.write(data)
    with open(os.path.join(a.out, a.name + '.iso'), 'wb') as f:
        f.write(data)
    with open(os.path.join(a.out, a.name + '.cue'), 'w') as f:
        f.write('FILE "%s.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n' % a.name)
    print('wrote', os.path.join(a.out, a.name + '.cue'), len(data), 'bytes')


if __name__ == '__main__':
    main()
