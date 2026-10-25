#!/usr/bin/env python3
"""Read emulated GameCube RAM from a running Dolphin through its GDB stub (started with -C Dolphin.General.GDBPort=<port>; the
emulation waits for this client). Used by qa/gc_cheats_play.ps1 to PROVE that a cheat changed game memory.

usage: gdb_read_mem.py <port> <run-seconds> <addr:len> [<addr:len> ...]
Connects (retrying while Dolphin boots), continues the game for <run-seconds>, breaks in, prints "ADDR=HEX" per range, detaches.
Exit 0 on success, 1 on any protocol failure."""
import os, socket, sys, time


def pkt(data):
    return b"$" + data + b"#" + ("%02x" % (sum(data) & 0xFF)).encode()


def read_packet(s, timeout):
    s.settimeout(timeout)
    buf = b""
    while True:
        c = s.recv(1)
        if not c:
            raise IOError("closed")
        if c == b"$":
            buf = b""
            while True:
                c = s.recv(1)
                if c == b"#":
                    s.recv(2)
                    s.sendall(b"+")
                    return buf
                buf += c


def main():
    port, run_s = int(sys.argv[1]), float(sys.argv[2])
    ranges = [a.split(":") for a in sys.argv[3:]]
    s = None
    for _ in range(120):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=2)
            break
        except OSError:
            time.sleep(1)
    if s is None:
        print("no GDB stub on port %d" % port)
        return 1
    try:
        s.sendall(pkt(b"?"))
        s.settimeout(10)
        stop = b""
        t0 = time.time()
        while time.time() - t0 < 10 and b"$" not in stop:
            stop += s.recv(256)
        s.sendall(b"+")
        s.sendall(pkt(b"c"))                 # let the game run (codes are applied by the code handler / AR engine each frame)
        time.sleep(run_s)
        s.sendall(b"\x03")                   # break in
        for _ in range(10):
            try:
                p = read_packet(s, 10)
            except socket.timeout:
                continue
            if os.environ.get("GDB_DEBUG"): print("stop-wait packet:", p[:60])
            if p[:1] in (b"S", b"T"):
                break
        s.settimeout(1.5)                    # swallow stray packets (stop replies queued by the break) so replies stay in step with requests
        try:
            while True:
                read_packet(s, 1.5)
        except (socket.timeout, IOError):
            pass
        for a, n in ranges:
            s.sendall(pkt(("m%s,%s" % (a, n)).encode()))
            p = read_packet(s, 10)
            if os.environ.get("GDB_DEBUG"): print("mem reply:", p[:80])
            print("%s=%s" % (a.upper(), p.decode()))
        s.sendall(pkt(b"D"))
        return 0
    except Exception as e:  # noqa
        print("gdb error: %r" % (e,))
        return 1
    finally:
        try:
            s.close()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
