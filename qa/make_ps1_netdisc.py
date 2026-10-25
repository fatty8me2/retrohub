#!/usr/bin/env python3
"""Builds a license-free PlayStation homebrew disc for netplay QA ("NetDisc"): every frame it polls the DualShock pads on both
controller ports through the real SIO0 protocol (analog mode switched on first), folds buttons + both sticks of both pads and a
hardware timer into a 32-bit LCG, and draws one rectangle per pad (position = left stick, colour = buttons), two small squares
(right sticks), one from the LCG and a background tinted by the LCG. Any input or timing divergence between two PCs therefore
shows in the picture and never heals.

  python qa\\make_ps1_netdisc.py <out dir> [--name NetDisc]

Writes <name>.cue/.bin (MODE2/2352 with EDC/ECC: the core's disc layer rejects unchecked 2048-byte sectors) and <name>.exe.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_ps1_testdisc as base_disc  # noqa: E402

R = dict(zero=0, v0=2, v1=3, a0=4, a1=5, a2=6, a3=7, t0=8, t1=9, t2=10, t3=11, t4=12, t5=13, t6=14, t7=15,
         s0=16, s1=17, s2=18, s3=19, s4=20, s5=21, s6=22, s7=23, t8=24, t9=25, sp=29, ra=31)
BASE = 0x80010000


class Asm:
    def __init__(self):
        self.w = []
        self.labels = {}
        self.fix = []   # (index, label, kind)
        self.n = 0

    def here(self): return len(self.w)
    def label(self, name): self.labels[name] = self.here()
    def uniq(self, p): self.n += 1; return '%s_%d' % (p, self.n)
    def emit(self, v): self.w.append(v & 0xFFFFFFFF)
    def r(self, rs, rt, rd, sh, fn): self.emit((R[rs] << 21) | (R[rt] << 16) | (R[rd] << 11) | (sh << 6) | fn)
    def i(self, op, rs, rt, imm): self.emit((op << 26) | (R[rs] << 21) | (R[rt] << 16) | (imm & 0xFFFF))
    def nop(self): self.emit(0)
    def addu(self, d, s, t): self.r(s, t, d, 0, 0x21)
    def and_(self, d, s, t): self.r(s, t, d, 0, 0x24)
    def or_(self, d, s, t): self.r(s, t, d, 0, 0x25)
    def xor(self, d, s, t): self.r(s, t, d, 0, 0x26)
    def sll(self, d, t, sh): self.r('zero', t, d, sh, 0x00)
    def srl(self, d, t, sh): self.r('zero', t, d, sh, 0x02)
    def mult(self, s, t): self.r(s, t, 'zero', 0, 0x18)
    def mflo(self, d): self.r('zero', 'zero', d, 0, 0x12)
    def move(self, d, s): self.addu(d, s, 'zero')
    def addiu(self, t, s, imm): self.i(0x09, s, t, imm)
    def andi(self, t, s, imm): self.i(0x0C, s, t, imm)
    def ori(self, t, s, imm): self.i(0x0D, s, t, imm)
    def xori(self, t, s, imm): self.i(0x0E, s, t, imm)
    def lui(self, t, imm): self.i(0x0F, 'zero', t, imm)

    def li(self, t, v):
        v &= 0xFFFFFFFF
        if v < 0x10000:
            self.ori(t, 'zero', v)
        else:
            self.lui(t, v >> 16)
            if v & 0xFFFF:
                self.ori(t, t, v & 0xFFFF)

    def la(self, t, name):
        self.fix.append((self.here(), name, 'hi')); self.lui(t, 0)
        self.fix.append((self.here(), name, 'lo')); self.ori(t, t, 0)

    def lw(self, t, off, b): self.i(0x23, b, t, off); self.nop()      # load delay slot
    def lbu(self, t, off, b): self.i(0x24, b, t, off); self.nop()
    def sw(self, t, off, b): self.i(0x2B, b, t, off)
    def sh(self, t, off, b): self.i(0x29, b, t, off)
    def sb(self, t, off, b): self.i(0x28, b, t, off)
    def br(self, op, s, t, name): self.fix.append((self.here(), name, 'br')); self.i(op, s, t, 0); self.nop()
    def beq(self, s, t, name): self.br(4, s, t, name)
    def bne(self, s, t, name): self.br(5, s, t, name)
    def j(self, name): self.fix.append((self.here(), name, 'j')); self.emit(0x08000000); self.nop()
    def jal(self, name): self.fix.append((self.here(), name, 'j')); self.emit(0x0C000000); self.nop()
    def jr(self, s): self.r(s, 'zero', 'zero', 0, 0x08); self.nop()

    def data(self, name, bs):
        self.label(name)
        bs = bytes(bs) + b'\0' * (-len(bs) % 4)
        for k in range(0, len(bs), 4):
            self.emit(struct.unpack('<I', bs[k:k + 4])[0])

    def link(self):
        for idx, name, kind in self.fix:
            tgt = self.labels[name]
            if kind == 'br': self.w[idx] = (self.w[idx] & 0xFFFF0000) | ((tgt - (idx + 1)) & 0xFFFF)
            elif kind == 'j': self.w[idx] |= ((BASE + tgt * 4) >> 2) & 0x03FFFFFF
            elif kind == 'hi': self.w[idx] |= ((BASE + tgt * 4) >> 16) & 0xFFFF
            elif kind == 'lo': self.w[idx] |= (BASE + tgt * 4) & 0xFFFF
        return b''.join(struct.pack('<I', c) for c in self.w)


def program(interlace=False):
    a = Asm()
    io = 's7'

    def gp0(valreg):
        lb = a.uniq('gw')
        a.label(lb)
        a.lw('t0', 0x1814, io); a.srl('t0', 't0', 28); a.andi('t0', 't0', 1); a.beq('t0', 'zero', lb)
        a.sw(valreg, 0x1810, io)

    def gp0i(v):
        a.li('t1', v); gp0('t1')

    a.emit(0x40806000); a.nop()   # mtc0 zero, SR: no interrupts (the BIOS handler would steal the VBlank flag)
    a.lui(io, 0x1F80)
    def mark(n):
        a.la('t8', 'dbg'); a.li('t9', n); a.sw('t9', 16, 't8')
    mark(1)
    for v in (0x00000000, 0x03000000, (0x08000025 if interlace else 0x08000001), 0x05000000, 0x06C60260, 0x07040010):
        a.li('t1', v); a.sw('t1', 0x1814, io)
    gp0i(0xE1000408); gp0i(0xE3000000); gp0i(0xE407BD3F if interlace else 0xE403BD3F); gp0i(0xE5000000)
    a.li('t0', 0x000D); a.sh('t0', 0x1048, io); a.li('t0', 0x0088); a.sh('t0', 0x104E, io)
    # Pad sources: port 1 addressed 1..4 (a multitap there answers every address; a plain pad only 1) and port 2 address 1.
    SRC = [(0x1003, 1), (0x1003, 2), (0x1003, 3), (0x1003, 4), (0x3003, 1)]
    # put every pad in analog mode: enter config, set analog+lock, exit config (the address byte is patched into the sequences)
    for k, (ctrl, addr) in enumerate(SRC):
        for seq in ('cfg_on', 'cfg_mode', 'cfg_off'):
            a.li('a0', ctrl); a.la('a1', seq + str(addr)); a.li('a2', 9); a.la('a3', 'rx%d' % k); a.jal('sendseq')
    mark(2)
    a.li('s0', 0x12345678)
    a.li('s6', 0)
    a.label('frame')
    a.label('wv')
    a.lw('t0', 0x1070, io); a.andi('t0', 't0', 1); a.beq('t0', 'zero', 'wv')
    mark(3)
    a.li('t0', 0xFFFFFFFE); a.sw('t0', 0x1070, io)
    for k, (ctrl, addr) in enumerate(SRC):
        a.li('a0', ctrl); a.la('a1', 'seq_poll%d' % addr); a.li('a2', 9); a.la('a3', 'rx%d' % k); a.jal('sendseq')
        # decode: t1 = buttons (active high), t2 = lx | ly<<8 | rx<<16 | ry<<24
        a.la('t0', 'rx%d' % k)
        a.lbu('t1', 3, 't0'); a.lbu('t2', 4, 't0'); a.sll('t2', 't2', 8); a.or_('t1', 't1', 't2'); a.xori('t1', 't1', 0xFFFF)
        a.lbu('t2', 7, 't0'); a.lbu('t3', 8, 't0'); a.sll('t3', 't3', 8); a.or_('t2', 't2', 't3')
        a.lbu('t3', 5, 't0'); a.lbu('t4', 6, 't0'); a.sll('t3', 't3', 16); a.sll('t4', 't4', 24); a.or_('t2', 't2', 't3'); a.or_('t2', 't2', 't4')
        # fold into the LCG, keep for drawing
        a.li('t5', 1664525); a.mult('s0', 't5'); a.mflo('s0'); a.li('t5', 1013904223); a.addu('s0', 's0', 't5')
        a.xor('s0', 's0', 't1'); a.addu('s0', 's0', 't2')
        a.la('t5', 'pd'); a.sw('t1', 8 * k, 't5'); a.sw('t2', 8 * k + 4, 't5')
    mark(4)
    # fold the timer-2 counter in too
    a.lw('t1', 0x1120, io); a.andi('t1', 't1', 0xFFFF); a.addu('s0', 's0', 't1')
    mark(6)
    # background
    a.srl('t1', 's0', 8); a.li('t2', 0x1F1F1F); a.and_('t1', 't1', 't2'); a.lui('t2', 0x0200); a.or_('t1', 't1', 't2'); gp0('t1')
    gp0i(0); gp0i(0x01E00140 if interlace else 0x00F00140)

    def rect(btn, stk, xoff, xor_col):
        a.andi('t1', btn, 0xFF); a.sll('t1', 't1', 16); a.srl('t2', btn, 8); a.sll('t2', 't2', 8); a.or_('t1', 't1', 't2'); a.ori('t1', 't1', 0x40)
        a.li('t2', xor_col); a.xor('t1', 't1', 't2'); a.lui('t2', 0x6000); a.or_('t1', 't1', 't2'); gp0('t1')
        a.andi('t1', stk, 0xFF); a.addiu('t1', 't1', xoff); a.srl('t2', stk, 8); a.andi('t2', 't2', 0xFF); a.sll('t2', 't2', 16); a.or_('t1', 't1', 't2'); gp0('t1')
        gp0i(0x00180018)

    mark(7)
    for k in range(5):
        a.la('t5', 'pd'); a.lw('s1', 8 * k, 't5'); a.lw('s2', 8 * k + 4, 't5')
        rect('s1', 's2', 30 * k, (0x00FFFF, 0xFF00FF, 0xFFFF00, 0x40FF40, 0)[k])
        # right stick as a small square
        a.srl('t3', 's2', 16)
        gp0i((0x6000FF00, 0x600000FF, 0x60FF0000, 0x60FFFFFF, 0x6000FFFF)[k])
        a.andi('t1', 't3', 0xFF); a.srl('t2', 't3', 8); a.andi('t2', 't2', 0xFF); a.sll('t2', 't2', 16); a.or_('t1', 't1', 't2'); gp0('t1')
        gp0i(0x00080008)
    mark(9)
    # LCG rectangle
    a.srl('t1', 's0', 4); a.lui('t2', 0x00FF); a.ori('t2', 't2', 0xFFFF); a.and_('t1', 't1', 't2'); a.lui('t2', 0x6000); a.or_('t1', 't1', 't2'); gp0('t1')
    a.srl('t1', 's0', 16); a.andi('t1', 't1', 0xFF); a.srl('t2', 's0', 8); a.andi('t2', 't2', 0x7F); a.sll('t2', 't2', 16); a.or_('t1', 't1', 't2'); gp0('t1')
    gp0i(0x00200020)
    a.addiu('s6', 's6', 1)
    a.la('t1', 'dbg'); a.sw('s6', 0, 't1'); a.sw('s0', 4, 't1')   # QA: state peek in RAM
    a.j('frame')

    # sendseq(a0 = SIO0 ctrl value, a1 = tx bytes, a2 = count, a3 = rx buffer)
    a.label('sendseq')
    a.move('t0', 'zero')
    a.label('sq_clr')
    a.addu('t1', 'a3', 't0'); a.li('t2', 0xFF); a.sb('t2', 0, 't1'); a.addiu('t0', 't0', 1); a.bne('t0', 'a2', 'sq_clr')
    a.lbu('t0', 0x1040, io)   # dummy RX read: arms the receiver (the core only receives after an RX read)
    a.sh('a0', 0x104A, io)
    a.li('t0', 100)
    a.label('sq_d1'); a.addiu('t0', 't0', -1); a.bne('t0', 'zero', 'sq_d1')
    a.move('t0', 'zero')
    a.label('sq_byte')
    a.addu('t1', 'a1', 't0'); a.lbu('t2', 0, 't1'); a.sw('t2', 0x1040, io)
    a.li('t3', 4000)
    a.label('sq_w1')
    a.lw('t4', 0x1044, io); a.andi('t4', 't4', 2); a.bne('t4', 'zero', 'sq_got')
    a.addiu('t3', 't3', -1); a.bne('t3', 'zero', 'sq_w1')
    a.j('sq_done')
    a.label('sq_got')
    a.lbu('t4', 0x1040, io)
    a.addu('t1', 'a3', 't0'); a.sb('t4', 0, 't1')
    a.addiu('t5', 't0', 1); a.beq('t5', 'a2', 'sq_done')
    a.li('t3', 4000)
    a.label('sq_w2')
    a.lw('t4', 0x1044, io); a.andi('t4', 't4', 0x200); a.bne('t4', 'zero', 'sq_ack')
    a.addiu('t3', 't3', -1); a.bne('t3', 'zero', 'sq_w2')
    a.j('sq_done')
    a.label('sq_ack')
    a.ori('t4', 'a0', 0x10); a.sh('t4', 0x104A, io)
    a.move('t0', 't5'); a.j('sq_byte')
    a.label('sq_done')
    a.sh('zero', 0x104A, io)
    a.li('t0', 300)
    a.label('sq_d2'); a.addiu('t0', 't0', -1); a.bne('t0', 'zero', 'sq_d2')
    a.jr('ra')

    for addr in (1, 2, 3, 4):
        a.data('cfg_on%d' % addr, [addr, 0x43, 0x00, 0x01, 0, 0, 0, 0, 0])
        a.data('cfg_mode%d' % addr, [addr, 0x44, 0x00, 0x01, 0x03, 0, 0, 0, 0])
        a.data('cfg_off%d' % addr, [addr, 0x43, 0x00, 0x00, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A])
        a.data('seq_poll%d' % addr, [addr, 0x42, 0x00, 0, 0, 0, 0, 0, 0])
    for k in range(5):
        a.data('rx%d' % k, [0] * 16)
    a.data('pd', [0] * 48)
    a.data('dbg', [0] * 32)
    return a.link(), BASE


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--name', default='NetDisc')
    ap.add_argument('--serial', default='SCUS-94570')
    ap.add_argument('--interlace', action='store_true', help='320x480 interlaced display (exercises the deinterlacer)')
    o = ap.parse_args()
    os.makedirs(o.out, exist_ok=True)
    code, base = program(o.interlace)
    data = base_disc.build(o.serial, o.name, (code, base))
    raw = base_disc.raw_mode2(data)
    with open(os.path.join(o.out, o.name + '.bin'), 'wb') as f:
        f.write(raw)
    with open(os.path.join(o.out, o.name + '.cue'), 'w') as f:
        f.write('FILE "%s.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % o.name)
    tsize = (len(code) + 2047) // 2048 * 2048
    hdr = bytearray(2048)
    hdr[0:8] = b'PS-X EXE'
    struct.pack_into('<IIIIII', hdr, 0x10, base, 0, base, tsize, 0, 0)
    struct.pack_into('<II', hdr, 0x30, 0x801FFF00, 0)
    hdr[0x4C:0x4C + 55] = b'Sony Computer Entertainment Inc. for North America area'
    with open(os.path.join(o.out, o.name + '.exe'), 'wb') as f:
        f.write(bytes(hdr) + code.ljust(tsize, bytes(1)))
    print('wrote', os.path.join(o.out, o.name + '.cue'), len(raw), 'bytes')


if __name__ == '__main__':
    main()
