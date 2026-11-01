#!/usr/bin/env python3
"""Generates the synthetic fixtures for patcher_test (see README.txt in this folder).

 base.bin / target.bin   deterministic synthetic game and its edited result (see make_base / make_target)
 x_*.xdelta              made by the real xdelta3 v3.2.2 (built from the upstream tag) with the options named in FIXTURES
 z_xz.xdelta             x_multi_plain.xdelta with its sections re-wrapped as one xz/LZMA2 stream per section type
                         (xdelta3 -S lzma output: needs liblzma, which a plain xdelta3 build does not have)

usage: make_fixtures.py <xdelta3 executable> <output folder>
"""
import binascii
import hashlib
import lzma
import os
import random
import struct
import subprocess
import sys

WORDS = ("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma tau upsilon phi chi psi omega "
         "north south east west lake river stone forest tower castle bridge harbor market garden temple island valley desert canyon "
         "red green blue amber violet silver golden copper iron bronze crystal shadow ember frost thunder breeze ocean meadow "
         "quest sword shield potion scroll dragon knight wizard rogue ranger cleric bard monk druid paladin warlock sorcerer").split()


def text(rng, n):
    out = bytearray()
    while len(out) < n:
        out += rng.choice(WORDS).encode() + (b"\n" if rng.random() < 0.08 else b" ")
    return bytes(out[:n])


def make_base():
    rng = random.Random(0x5EED64)
    a = text(rng, 20000)
    b = bytes(rng.getrandbits(8) for _ in range(12000))
    c = b"".join(struct.pack("<IHH8s", i, (i * 37) & 0xFFFF, i % 7, bytes([i & 255]) * 8) for i in range(500))      # 16-byte records
    d = text(rng, 16000)
    return a, b, c, d


def make_target(base):
    a, b, c, d = base
    rng = random.Random(0xFACADE)
    a2 = bytearray(a)
    for _ in range(40):                                   # scattered small edits
        i = rng.randrange(len(a2))
        a2[i] = rng.randrange(256)
    new_text = text(rng, 3000)                            # a new block of prose
    a2[8000:8000] = new_text
    d2 = d[:5000] + d[7000:]                              # delete 2000 bytes
    pattern = b"abcabc" * 1000                            # self-overlapping copy material
    zeros = bytes(4096)                                   # a run
    chunk = bytes(a[2000:7000])                           # duplicated from far away
    return bytes(a2) + c + b + pattern + d2 + zeros + chunk + b"THE END\n"


FIXTURES = [
    # name,              xdelta3 options
    ("x_plain",          ["-S", "none"]),
    ("x_djw",            ["-S", "djw"]),
    ("x_djw9",           ["-S", "djw9"]),
    ("x_multi_plain",    ["-S", "none", "-W", "16384", "-B", "32768"]),
    ("x_multi_djw9",     ["-S", "djw9", "-W", "16384", "-B", "32768"]),
    ("x_noadler",        ["-S", "none", "-n"]),
    ("x_noapp",          ["-S", "none", "-A="]),
]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout.decode(errors="replace") + r.stderr.decode(errors="replace"))
        raise SystemExit("failed: " + " ".join(cmd))


# ---- minimal VCDIFF reading/writing for the xz re-wrap ----
def rd_varint(buf, pos):
    v = 0
    while True:
        b = buf[pos]
        pos += 1
        v = (v << 7) | (b & 0x7F)
        if not (b & 0x80):
            return v, pos


def wr_varint(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


def parse_vcdiff(buf):
    assert buf[:4] == b"\xd6\xc3\xc4\x00"
    pos = 4
    hdr_ind = buf[pos]; pos += 1
    assert hdr_ind & ~4 == 0, "only the application header is expected"
    app = b""
    if hdr_ind & 4:
        n, pos = rd_varint(buf, pos)
        app = buf[pos:pos + n]; pos += n
    wins = []
    while pos < len(buf):
        w = {}
        w["win_ind"] = buf[pos]; pos += 1
        w["src"] = None
        if w["win_ind"] & 3:
            l, pos = rd_varint(buf, pos)
            o, pos = rd_varint(buf, pos)
            w["src"] = (l, o)
        enc, pos = rd_varint(buf, pos)
        end = pos + enc
        w["tgtlen"], pos = rd_varint(buf, pos)
        w["del_ind"] = buf[pos]; pos += 1
        assert w["del_ind"] == 0
        dl, pos = rd_varint(buf, pos)
        il, pos = rd_varint(buf, pos)
        al, pos = rd_varint(buf, pos)
        w["adler"] = None
        if w["win_ind"] & 4:
            w["adler"] = buf[pos:pos + 4]; pos += 4
        w["data"] = buf[pos:pos + dl]; pos += dl
        w["inst"] = buf[pos:pos + il]; pos += il
        w["addr"] = buf[pos:pos + al]; pos += al
        assert pos == end
        wins.append(w)
    return hdr_ind, app, wins


def xz_headers():
    flags = b"\x00\x00"                                   # no integrity check
    stream_header = b"\xfd7zXZ\x00" + flags + struct.pack("<I", binascii.crc32(flags))
    prop = bytes([22])                                    # LZMA2 dictionary property: 8 MiB (the preset-6 value)
    body = bytes([0x00]) + b"\x00" + b"\x21\x01" + prop   # size byte placeholder, flags (1 filter), filter id 0x21, 1 prop byte
    body = body + b"\x00" * ((-(len(body) + 4)) % 4)      # pad so that header + crc is a multiple of 4
    hdr = bytes([(len(body) + 4) // 4 - 1]) + body[1:]
    block_header = hdr + struct.pack("<I", binascii.crc32(hdr))
    return stream_header + block_header


def lzma2_chunks(data):
    """The LZMA2 chunk sequence for `data` as liblzma writes it, without the 0x00 end marker."""
    filters = [{"id": lzma.FILTER_LZMA2, "preset": 6}]
    raw = lzma.compress(data, format=lzma.FORMAT_RAW, filters=filters)
    assert raw[-1] == 0
    return raw[:-1]


def to_xz(buf, parts_per_window=True):
    hdr_ind, app, wins = parse_vcdiff(buf)
    out = bytearray(b"\xd6\xc3\xc4\x00")
    out.append(hdr_ind | 1)
    out.append(2)                                         # VCD_LZMA_ID
    if hdr_ind & 4:
        out += wr_varint(len(app)) + app
    started = [False, False, False]
    streams = [bytearray(), bytearray(), bytearray()]     # what liblzma sees per section type, in window order
    plains = [bytearray(), bytearray(), bytearray()]
    for w in wins:
        sections = [w["data"], w["inst"], w["addr"]]
        coded, del_ind = [], 0
        for k, s in enumerate(sections):
            if len(s) < 10:
                coded.append(s)
                continue
            piece = lzma2_chunks(s)
            if not started[k]:
                piece = xz_headers() + piece
                started[k] = True
            coded.append(wr_varint(len(s)) + piece)
            streams[k] += piece
            plains[k] += s
            del_ind |= 1 << k
        body = wr_varint(w["tgtlen"]) + bytes([del_ind]) + wr_varint(len(coded[0])) + wr_varint(len(coded[1])) + wr_varint(len(coded[2]))
        if w["adler"] is not None:
            body += w["adler"]
        body += coded[0] + coded[1] + coded[2]
        out.append(w["win_ind"])
        if w["src"]:
            out += wr_varint(w["src"][0]) + wr_varint(w["src"][1])
        out += wr_varint(len(body)) + body
    for k in range(3):                                    # liblzma (python's lzma module) must read each stream and give the plain bytes back
        if streams[k]:
            got = lzma.LZMADecompressor(format=lzma.FORMAT_XZ).decompress(bytes(streams[k]))
            assert got == bytes(plains[k]), "xz stream %d does not decode to its section data" % k
    return bytes(out)


def main():
    xd3, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    base = make_base()
    base_bytes = b"".join(base)
    target = make_target(base)
    open(os.path.join(outdir, "base.bin"), "wb").write(base_bytes)
    open(os.path.join(outdir, "target.bin"), "wb").write(target)
    print("base.bin   %6d bytes  crc32 %08x" % (len(base_bytes), binascii.crc32(base_bytes)))
    print("target.bin %6d bytes  crc32 %08x" % (len(target), binascii.crc32(target)))
    for name, opts in FIXTURES:
        out = os.path.join(outdir, name + ".xdelta")
        if os.path.exists(out):
            os.remove(out)
        run([xd3, "-e", "-f", "-q"] + opts + ["-s", os.path.join(outdir, "base.bin"), os.path.join(outdir, "target.bin"), out])
        print("%-14s %6d bytes" % (name + ".xdelta", os.path.getsize(out)))
    # no source at all: the whole target is ADD / RUN / self-COPY
    out = os.path.join(outdir, "x_nosrc.xdelta")
    if os.path.exists(out):
        os.remove(out)
    run([xd3, "-e", "-f", "-q", "-S", "djw", os.path.join(outdir, "target.bin"), out])
    print("%-14s %6d bytes" % ("x_nosrc.xdelta", os.path.getsize(out)))
    # FGK adaptive Huffman, which xdelta3 only builds on request: the first 9000 bytes of base.bin, no source
    fin = os.path.join(outdir, "fgk_in.bin")
    open(fin, "wb").write(base_bytes[:9000])
    out = os.path.join(outdir, "x_fgk.xdelta")
    if os.path.exists(out):
        os.remove(out)
    run([xd3, "-e", "-f", "-q", "-S", "fgk", fin, out])
    os.remove(fin)
    print("%-14s %6d bytes  (decodes to base.bin[:9000], crc32 %08x)" % ("x_fgk.xdelta", os.path.getsize(out), binascii.crc32(base_bytes[:9000])))
    # The adaptive model carries over between windows: four windows of 16 KiB, no source. xdelta3 itself cannot read its own
    # FGK output when some window skips a section (the encoder's model has moved on, the decoder's has not), so this input
    # is prose that compresses in every window (checked with xdelta3 -d when the seed was picked).
    fin = os.path.join(outdir, "fgk_multi_in.bin")
    prose = text(random.Random(1), 52000)
    open(fin, "wb").write(prose)
    out = os.path.join(outdir, "x_fgk_multi.xdelta")
    if os.path.exists(out):
        os.remove(out)
    run([xd3, "-e", "-f", "-q", "-S", "fgk", "-W", "16384", fin, out])
    os.remove(fin)
    print("%-14s %6d bytes  (decodes to prose, 52000 bytes, crc32 %08x)" % ("x_fgk_multi.xdelta", os.path.getsize(out), binascii.crc32(prose)))
    z = to_xz(open(os.path.join(outdir, "x_multi_plain.xdelta"), "rb").read())
    open(os.path.join(outdir, "z_xz.xdelta"), "wb").write(z)
    print("%-14s %6d bytes" % ("z_xz.xdelta", len(z)))


if __name__ == "__main__":
    main()
