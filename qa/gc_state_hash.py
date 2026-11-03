#!/usr/bin/env python3
"""Frame-aligned game-state hashes of a running Dolphin (QA: proves whether two NetPlay instances really diverge).

Dolphin must run with its GDB stub (-C Dolphin.General.GDBPort=<port>); it waits for this client at boot. Near every sample point
the script arms a WRITE watchpoint on a per-frame counter in game RAM (e.g. the VI-rate counter from <work>\\<game>.vi.txt), so the
emulated CPU stops at the SAME instruction on every instance (Dolphin stops before the store). When the counter equals the sample
frame (a multiple of --every) it reads all of MEM1 (24 MiB) with ReadProcessMemory while the CPU is halted, records its SHA-1 and
removes the watchpoint. Two instances in lockstep with identical state give identical hashes for every common sample frame; real
game-state divergence gives different hashes (--dump keeps the snapshots for a word diff with --diff).

Default (sparse): the game runs free; the counter is polled with ReadProcessMemory and the watchpoint is armed only --margin
frames before a sample. --dense arms it from the first instruction (a stop every frame: very slow).
Whether arming/disarming at different wall times perturbs emulation is checked by the run itself: Dolphin's own desync detector
(timebase compare every 60 frames) stays silent and the hashes match in a run without a desync cause.

usage: gc_state_hash.py --port <gdb port> --pid <dolphin pid> --game G4QE01 --addr 80374114 --every 600 --secs 300 --out <file>
                        [--dump <dir>] [--margin 6] [--dense] [--first 300] [--start 7200]
       gc_state_hash.py --diff <a.bin> <b.bin>        (word diff of two dumps)
Output file: one line per sample "frame <counter> sha1 <hex> t <seconds since connect> stops <n>".
"""
import argparse, ctypes, ctypes.wintypes as wt, hashlib, os, socket, sys, time, zlib

MEM1 = 0x1800000


def pkt(data):
    return b"$" + data + b"#" + ("%02x" % (sum(data) & 0xFF)).encode()


class Gdb:
    def __init__(self, s):
        self.s = s
        self.buf = b""

    def _byte(self):
        if not self.buf:
            d = self.s.recv(65536)
            if not d:
                raise IOError("gdb stub closed")
            self.buf = d
        c, self.buf = self.buf[:1], self.buf[1:]
        return c

    def read(self, timeout):
        self.s.settimeout(timeout)
        while self._byte() != b"$":
            pass
        out = b""
        while True:
            c = self._byte()
            if c == b"#":
                self._byte(); self._byte()
                self.s.sendall(b"+")
                return out
            out += c

    def cmd(self, data, timeout=60):
        self.s.sendall(pkt(data))
        return self.read(timeout)


k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p), ("AllocationProtect", wt.DWORD),
                ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD), ("Protect", wt.DWORD), ("Type", wt.DWORD)]


k32.OpenProcess.restype = wt.HANDLE
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


def rpm(h, addr, n):
    b = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), b, n, ctypes.byref(got)) or got.value != n:
        return None
    return b.raw


def find_mem1(h, game):
    a, m = 0, MBI()
    while k32.VirtualQueryEx(h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m)) == ctypes.sizeof(m):
        base = m.BaseAddress or 0
        if m.State == 0x1000 and m.Type == 0x40000 and m.RegionSize >= MEM1:   # MEM_COMMIT, MEM_MAPPED
            if rpm(h, base, 6) == game.encode():
                return base
        a = base + m.RegionSize
        if a >= 1 << 47:
            break
    return 0


def diff(a, b):
    x, y = zlib.decompress(open(a, "rb").read()), zlib.decompress(open(b, "rb").read())
    n, first, spans = 0, [], []
    for i in range(0, MEM1, 4):
        if x[i:i + 4] != y[i:i + 4]:
            n += 1
            if len(first) < 40:
                first.append("%08X %s %s" % (0x80000000 + i, x[i:i + 4].hex(), y[i:i + 4].hex()))
            if spans and spans[-1][1] >= i - 64:
                spans[-1][1] = i
            else:
                spans.append([i, i])
    print("differing words: %d in %d spans" % (n, len(spans)))
    for s in spans[:60]:
        print("  span %08X-%08X (%d bytes)" % (0x80000000 + s[0], 0x80000000 + s[1] + 3, s[1] + 4 - s[0]))
    print("\n".join(first))


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--diff":
        diff(sys.argv[2], sys.argv[3])
        return 0
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--addr", required=True)
    ap.add_argument("--every", type=int, default=600)
    ap.add_argument("--secs", type=float, default=300)
    ap.add_argument("--out", required=True)
    ap.add_argument("--dump", default="")
    ap.add_argument("--margin", type=int, default=6)
    ap.add_argument("--dense", action="store_true")
    ap.add_argument("--idle", action="store_true")   # connect, continue, do nothing (overhead check)
    ap.add_argument("--first", type=int, default=0)
    ap.add_argument("--start", type=int, default=0)
    o = ap.parse_args()
    o.idle = o.idle or os.environ.get("GCHASH_IDLE") == "1"
    addr = int(o.addr, 16) | 0x80000000
    h = k32.OpenProcess(0x0400 | 0x0010, False, o.pid)   # QUERY_INFORMATION | VM_READ
    if not h:
        print("OpenProcess failed"); return 1
    s = None
    for _ in range(400):
        try:
            s = socket.create_connection(("127.0.0.1", o.port), timeout=2)
            break
        except OSError:
            time.sleep(1)
    if s is None:
        print("no GDB stub on port %d" % o.port); return 1
    g = Gdb(s)
    out = open(o.out, "w")
    t0 = time.time()
    g.s.sendall(b"+")
    g.cmd(b"?", 120)                                        # initial stop (Dolphin waits for us at boot)
    st = {"base": 0, "stops": 0}

    def take(v):
        snap = rpm(h, st["base"], MEM1) if st["base"] else None
        if not snap:
            out.write("# frame %d: no snapshot\n" % v); out.flush()
            return
        out.write("frame %d sha1 %s t %.1f stops %d\n" % (v, hashlib.sha1(snap).hexdigest(), time.time() - t0, st["stops"])); out.flush()
        if o.dump:
            os.makedirs(o.dump, exist_ok=True)
            open(os.path.join(o.dump, "f%07d.bin" % v), "wb").write(zlib.compress(snap, 1))

    nxt, armed = None, False

    def after(v):   # next sample frame after counter value v: --first once, then multiples of --every from --start on
        if o.first and v < o.first:
            return o.first
        n = (v // o.every + 1) * o.every
        return max(n, ((o.start + o.every - 1) // o.every) * o.every)
    if o.dense:
        out.write("# Z2 %08X -> %s\n" % (addr, g.cmd(b"Z2,%08x,4" % addr).decode(errors="replace"))); out.flush()
        armed = True
    g.s.sendall(pkt(b"c"))
    if o.idle:
        time.sleep(o.secs)
    try:
        while not o.idle and time.time() - t0 < o.secs:
            if not st["base"]:
                st["base"] = find_mem1(h, o.game)
            if not armed:                                       # sparse: run free, poll the counter, arm just before a sample
                if not st["base"]:
                    time.sleep(0.2); continue
                raw = rpm(h, st["base"] + (addr & 0x1FFFFFF), 4)
                v = int.from_bytes(raw, "big") if raw else 0
                if nxt is None or v >= nxt:
                    nxt = after(v)
                if nxt - v > o.margin:
                    time.sleep(0.002); continue
                g.s.sendall(b"\x03")
                g.read(30)
                g.cmd(b"Z2,%08x,4" % addr)
                armed = True
                g.s.sendall(pkt(b"c"))
            try:
                g.read(60)                                      # stop reply (watchpoint hit: before the store)
            except socket.timeout:
                out.write("# no stop for 60 s\n"); out.flush(); continue
            st["stops"] += 1
            try:
                v = int(g.cmd(b"m%08x,4" % addr), 16)
            except ValueError:
                v = -1
            if nxt is None and v >= 0:
                nxt = after(v)
            if v >= 0 and nxt is not None and v >= nxt:
                if v == nxt:                                    # only exact sample frames are comparable across instances
                    take(v)
                nxt = after(v)
                if not o.dense:
                    g.cmd(b"z2,%08x,4" % addr)
                    armed = False
            g.s.sendall(pkt(b"c"))
    except Exception as ex:   # never leave the emulated CPU halted at a watchpoint: the whole NetPlay room would stall
        out.write("# ERROR %r (continuing so Dolphin runs on)\n" % (ex,)); out.flush()
        try:
            g.cmd(b"z2,%08x,4" % addr, 5)
        except Exception:
            pass
        try:
            g.s.sendall(pkt(b"c"))   # Dolphin 2509 has no detach command: continue, then close
        except Exception:
            pass
        out.close()
        return 1
    try:
        g.s.sendall(b"\x03")
        g.read(10)
        g.cmd(b"z2,%08x,4" % addr)
        g.s.sendall(pkt(b"c"))   # leave the CPU running (no detach command in Dolphin 2509)
    except Exception:
        pass
    out.write("# end stops %d\n" % st["stops"]); out.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:   # leave the reason next to the results (the harness runs us without a console)
        out = None
        for i, a in enumerate(sys.argv):
            if a == "--out" and i + 1 < len(sys.argv):
                out = sys.argv[i + 1]
        if out:
            with open(out, "a") as f:
                f.write("# ERROR %r\n" % (e,))
        raise
