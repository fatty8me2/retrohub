"""Fake GitHub + Google Drive for GamePack QA. Loopback only, tiny dummy data.
Usage: python qa/gamepack_fake_server.py <port>
  Control:  GET /__mode?packs=ok|404|malformed|empty|slow|huge|array   (what the community list answers)
            GET /__log  (requests seen, one per line)
Drive layout mimics embeddedfolderview HTML + /download?id=...
"""
import sys, os, time, json, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

COMM, USER = "COMMROOT0000000001", "USERROOT0000000001"
MODE = {"packs": "ok"}
LOG = []

def folder(i, name): return (i, name, True)
def file(i, name): return (i, name, False)

TREE = {
    COMM: [folder("COMMGBA0000000001", "gba"), folder("COMMN640000000001", "n64")],
    "COMMGBA0000000001": [folder("GAMEFAKE000000001", "Fake Game"), folder("GAMEDUPE000000001", "Dupe Game"),
                          folder("GAMEEVIL000000001", "Evil Game"), folder("GAMEBIG0000000001", "Big Game"),
                          folder("GAMEBAD0000000001", "Bad Game"), folder("GAMEHTML000000001", "Html Game"),
                          folder("GAMETRUNC00000001", "Trunc Game"), folder("GAMEFRIEND0000001", "Friend Game 1002")],
    "COMMN640000000001": [],
    "GAMEFRIEND0000001": [file("FILEFRIEND0000001", "friend.gba")],
    USER: [folder("USERGBA0000000001", "gba")],
    "USERGBA0000000001": [folder("USERDUPE000000001", "Dupe Game"), folder("USERONLY000000001", "Local Only")],
    "GAMEFAKE000000001": [file("FILEFAKE000000001", "fake.gba"), file("FILEFAKE000000002", "fake.sav")],
    "GAMEDUPE000000001": [file("FILEDUPE000000001", "dupe_COMMUNITY.gba")],
    "USERDUPE000000001": [file("FILEDUPE000000002", "dupe_USER.gba")],
    "USERONLY000000001": [file("FILEUSER000000001", "localonly.gba")],
    "GAMEEVIL000000001": [file("FILEEVIL000000001", "../../evil1.txt"), file("FILEEVIL000000002", "..\\..\\evil2.txt"),
                          file("FILEEVIL000000003", "C:\\evil3.txt"), file("FILEEVIL000000004", "gamepack.done"),
                          file("FILEEVIL000000005", "..")],
    "GAMEBIG0000000001": [file("FILEBIG0000000001", "big.gba")],
    "GAMEBAD0000000001": [file("FILEBAD0000000001", "ok.gba"), file("FILEBAD000000404", "missing.gba")],
    "GAMEHTML000000001": [file("FILEHTML000000001", "quota.gba")],
    "GAMETRUNC00000001": [file("FILETRUNC00000001", "trunc.gba")],
}
BUILTIN = {"19_2xjH7Ps7epabxUrr9sfxz69kuJGp3W", "17AvjquyVOjSs0ua1LnLZGRN3B2bUYQar", "10zAwtXq1STlpfdIpCDppwXwpKhK3gyRP", "1idefowNapO3KpwE4nRv8zqrpCeEZFgpc", "1FVsjeQMBAcxw-_gRU4MSSRLQpIOD3ygu", "19H5x-ZY6-8HHUl0g3XI0NEsW2e6ROWzr"}
BIG_SIZE = 300 * 1024 * 1024
FILES = {}   # file id -> local path, for --packdir

def load_packdir(root):
    """--packdir <dir>: <dir>/<Console>/<Game folder or loose file>... becomes the community pack root (replaces the built-in fake tree)."""
    n = [0]
    def nid(k): n[0] += 1; return "PD%s%014d" % (k, n[0])
    def walk(path, fid):
        ents = []
        for name in sorted(os.listdir(path)):
            full = os.path.join(path, name)
            if os.path.isdir(full):
                i = nid("D"); ents.append(folder(i, name)); walk(full, i)
            else:
                i = nid("F"); ents.append(file(i, name)); FILES[i] = full
        TREE[fid] = ents
    TREE.clear()
    TREE[COMM] = []
    walk(root, COMM)

def listing(fid):
    ents = TREE.get(fid)
    if ents is None and fid in BUILTIN: ents = [folder("GAMEBUILT00000001", "Builtin Game")]   # the real built-in pack ids, redirected here
    if ents is None: return None
    h = ['<html><body class="flip-embedded"><div class="flip-entries">']
    for i, name, isf in ents:
        name = name.replace("&", "&amp;").replace("<", "&lt;")
        link = ("https://drive.google.com/drive/folders/" + i) if isf else ("https://drive.google.com/file/d/" + i + "/view?usp=drive_web")
        h.append('<div class="flip-entry" id="entry-%s" tabindex="0"><div class="flip-entry-info"><a href="%s"><div class="flip-entry-title">%s</div></a></div></div>' % (i, link, name))
    h.append("</div></body></html>")
    return "".join(h).encode()

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def send(self, code, body, ctype="application/octet-stream"):
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query); LOG.append(self.path)
        if u.path == "/__mode":
            MODE.update({k: v[0] for k, v in q.items()}); return self.send(200, b"ok")
        if u.path == "/__log": return self.send(200, "\n".join(LOG).encode(), "text/plain")
        if u.path == "/fatty8me2/retrohub/main/data/packs.json":
            m = MODE["packs"]
            if m == "404": return self.send(404, b"404: Not Found", "text/plain")
            if m == "malformed": return self.send(200, b'{"packs": [ {"id": "COMMROOT0000000001"', "application/json")
            if m == "html": return self.send(200, b"<html>captive portal</html>", "text/html")
            if m == "empty": return self.send(200, b'{"packs":[]}', "application/json")
            if m == "array": return self.send(200, json.dumps([COMM, USER, "../bad id", COMM]).encode(), "application/json")
            if m == "slow": time.sleep(12)
            if m == "huge":
                self.send_response(200); self.send_header("Content-Length", str(200*1024*1024)); self.end_headers()
                try:
                    for _ in range(200*16): self.wfile.write(b"[" + b" " * (64*1024-1))
                except Exception: pass
                return
            return self.send(200, json.dumps({"packs": [{"id": COMM, "name": "Community"}, {"id": USER, "name": "dup of user"}]}).encode(), "application/json")
        if u.path == "/embeddedfolderview":
            b = listing(q.get("id", [""])[0])
            return self.send(200, b, "text/html") if b else self.send(404, b"nope", "text/html")
        if u.path == "/download":
            fid = q.get("id", [""])[0]
            if fid == "FILEBAD000000404": return self.send(404, b"gone", "text/plain")
            if fid == "FILEHTML000000001": return self.send(200, b"<html>quota</html>", "text/html")
            if fid == "FILETRUNC00000001":
                self.send_response(200); self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", "100000"); self.end_headers(); self.wfile.write(b"x" * 1000)
                self.close_connection = True; return
            if fid == "FILEBIG0000000001":
                self.send_response(200); self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(BIG_SIZE)); self.end_headers()
                try:
                    for _ in range(BIG_SIZE // 65536):
                        self.wfile.write(b"\0" * 65536); time.sleep(0.01)
                except Exception: pass
                return
            if fid in FILES: return self.send(200, open(FILES[fid], "rb").read())
            return self.send(200, ("DUMMY-" + fid).encode() * 10)
        self.send(404, b"nf", "text/plain")

if __name__ == "__main__":
    if len(sys.argv) > 3 and sys.argv[2] == "--packdir": load_packdir(sys.argv[3])
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
