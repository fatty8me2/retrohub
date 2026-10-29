"""GamePack mechanics against the local fake (qa/gamepack_fake_server.py). Usage: python qa/gamepack_qa.py"""
import subprocess, os, sys, time, shutil, urllib.request
R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(R, "build", "bin", "Release")
PORT = 18765
env = dict(os.environ, RETROHUB_QA="1", RETROHUB_GP_BASE="127.0.0.1:%d" % PORT, SSB64_MUTE="1")
srv = subprocess.Popen([sys.executable, os.path.join(R, "qa", "gamepack_fake_server.py"), str(PORT)])
time.sleep(1)
def mode(m): urllib.request.urlopen("http://127.0.0.1:%d/__mode?packs=%s" % (PORT, m)).read()
def run(label, *a, timeout=120):
    t = time.time()
    p = subprocess.run([os.path.join(BIN, "qa_gamepack.exe"), *a], env=env, capture_output=True, text=True, timeout=timeout)
    print("== %s (%.1fs)\n%s%s" % (label, time.time() - t, p.stdout, p.stderr))
def clean(): shutil.rmtree(os.path.join(BIN, "userdata", "gamepack"), ignore_errors=True)
COMM, USER = "COMMROOT0000000001", "USERROOT0000000001"
try:
    for m in ["ok", "404", "malformed", "html", "empty", "array", "slow", "huge"]:
        mode(m); run("index mode=%s no user links" % m, "index", "-", "gba", "Fake Game", "Local Only", "Dupe Game", "Builtin Game")
    mode("ok")
    run("index community + user link", "index", USER, "gba", "Fake Game", "Local Only", "Builtin Game")
    run("index user only (404 list)", "index", USER, "gba", "Fake Game", "Local Only", "Builtin Game") if mode("404") is None else None
    mode("ok"); clean()
    run("install community game", "install", "-", "gba", "Fake Game")
    run("install dupe (precedence: community first)", "install", USER, "gba", "Dupe Game")
    print(open(os.path.join(BIN, "userdata", "gamepack", "gba", "Dupe Game", "dupe_COMMUNITY.gba")).read()[:30] if os.path.exists(os.path.join(BIN, "userdata", "gamepack", "gba", "Dupe Game", "dupe_COMMUNITY.gba")) else "dupe file NOT community")
    run("install user-only game", "install", USER, "gba", "Local Only")
    run("install user-only game w/o link", "install", "-", "gba", "Local Only")
    run("install evil names", "install", "-", "gba", "Evil Game")
    run("bad: 404 file", "install", "-", "gba", "Bad Game")
    run("bad: html quota", "install", "-", "gba", "Html Game")
    run("bad: truncated", "install", "-", "gba", "Trunc Game")
    run("cancel midway big", "install", "-", "gba", "Big Game", str(20*1024*1024))
    run("bad console", "install", "-", "n64", "Nothing")
    mode("404"); run("install while list 404 (community game)", "install", "-", "gba", "Fake Game")
    print("evil files outside pack dir?", [f for f in ["evil1.txt","evil2.txt","evil3.txt"] if any(f in fs for _, _, fs in os.walk(os.path.join(BIN, "userdata")))])
    print(os.listdir(os.path.join(BIN, "userdata")))
finally:
    srv.kill()
