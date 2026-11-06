// Strict rooms end to end with the REAL runners (retrorun headless peers) against a LOCAL worker. Muted, hidden, loopback only.
//
//   Start the worker (lockout-bs branch of BattleShip) from its server\cloudflare folder, e.g.
//     node node_modules\wrangler\bin\wrangler.js dev --ip 127.0.0.1 --port 8771 --persist-to <fresh dir> --var ADMIN_UID:aaaaaaaaaaaaaaaaaaaaaaaaaa
//        REG_APPROVAL_DEFAULT:0 REG_TOUCH_MS:0 STRICT_CACHE_MS:0 STRICT_IDENT_MS:4000 TEST_REG:1 ALLOW_TEST_FLAG:1
//   then: node qa\strict_rooms_e2e.mjs ws://127.0.0.1:8771
//
// What it proves: with strict_rooms ON, retrorun peers that get the account through the inherited RETROHUB_IDENT environment variable
// (the hub sets it around CreateProcess; never on a command line) keep their seats in a game room: lockstep players, rollback players and a
// spectator are never closed 4013; the same peers WITHOUT the variable, or with it bound to another server, are refused; and the secret
// never appears in a log. Needs deps\cores and deps\testroms (a junction to RetroHub\deps works).
import { spawn } from "node:child_process";
import { mkdirSync, readFileSync, existsSync, rmSync } from "node:fs";
import { join, resolve } from "node:path";

const base = process.argv[2] || "ws://127.0.0.1:8771";
if (!/^ws:\/\/(127\.0\.0\.1|localhost|\[::1\])(:\d+)?$/.test(base)) { console.log("refusing a non-loopback server:", base); process.exit(2); }
const root = resolve(import.meta.dirname, "..");
const out = join(root, "qa", "out", "strict_rooms");
rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });
const exe = join(root, "build", "bin", "Release", "retrorun.exe");
const core = join(root, "deps", "cores", "mgba_libretro.dll");
const rom = join(root, "deps", "testroms", "sram.gba");
for (const f of [exe, core, rom]) if (!existsSync(f)) { console.log("missing", f); process.exit(2); }

let fails = 0;
const check = (c, m) => { if (!c) { fails++; console.log("FAIL:", m); } else console.log("ok:  ", m); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const B32 = "abcdefghijklmnopqrstuvwxyz234567";
const rnd = (n) => Array.from({ length: n }, () => B32[Math.floor(Math.random() * 32)]).join("");
const ADMIN = "a".repeat(26), ADMIN_SECRET = "admin-fixed-secret-for-local-tests";

function friends(uid, secret, name, extra = {}) {   // a friends socket: registers the account (approval is off in this worker)
  return new Promise((res) => {
    const u = { uid, secret, msgs: [] };
    const ws = new WebSocket(`${base}/friends?uid=${uid}`);
    u.ws = ws;
    const t = setTimeout(() => res(u), 5000);
    ws.onopen = () => ws.send(JSON.stringify({ type: "hello", secret, name, test: true, ...extra }));
    ws.onmessage = (e) => { const m = JSON.parse(e.data); u.msgs.push(m); if (m.type === "welcome") { clearTimeout(t); res(u); } };
  });
}
const mkAccount = async (name) => { const a = { uid: rnd(26), secret: rnd(52) }; const c = await friends(a.uid, a.secret, name); a.ok = c.msgs.some((m) => m.type === "welcome"); c.ws.close(); return a; };

async function setStrict(on) {
  const adm = await friends(ADMIN, ADMIN_SECRET, "qa-admin");
  adm.ws.send(JSON.stringify({ type: "admin_settings", set: { strict_rooms: on } }));
  await sleep(600);
  const m = [...adm.msgs].reverse().find((x) => x.type === "admin_settings" || x.settings);
  adm.ws.close();
  return m;
}

const room = () => Array.from({ length: 6 }, () => "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"[Math.floor(Math.random() * 32)]).join("") + "-1";

/* Runs one retrorun peer. ident: account to prove (null = none), bindServer: the server the variable is bound to. */
function peer(tag, rm, { slot, ident, bindServer = base, spectate = false, rollback = false, frames = 700 }) {
  const dir = join(out, tag); mkdirSync(join(dir, "sv"), { recursive: true }); mkdirSync(join(dir, "st"), { recursive: true });
  const log = join(dir, "run.log");
  const a = ["--core", core, "--rom", rom, "--system", "gba", "--save-dir", join(dir, "sv"), "--state-dir", join(dir, "st"), "--headless", "--frames", String(frames),
    "--net", base, "--room", rm, "--ports", "0,1,-1,-1", "--log", log];
  if (spectate) a.push("--spectate", "--players", "2");
  else a.push("--slot", String(slot), "--players", "2", "--delay", "auto", "--fuzz-input");
  if (rollback) a.push("--rollback", "--realtime");   // real time: a spectator can join while the match is still running
  const env = { ...process.env, RETRORUN_MUTE: "1", SSB64_MUTE: "1", N64RUN_MUTE: "1" };
  delete env.RETROHUB_IDENT;
  if (ident) env.RETROHUB_IDENT = `${bindServer}|${ident.uid}|${ident.secret}`;
  const p = { tag, log, code: null, done: null };
  const cp = spawn(exe, a, { env, windowsHide: true, stdio: "ignore" });   // PID is ours; killed by PID only
  p.pid = cp.pid;
  p.done = new Promise((res) => { cp.on("exit", (c) => { p.code = c; res(c); }); });
  p.kill = () => { try { process.kill(cp.pid); } catch { /* gone */ } };
  p.text = () => (existsSync(log) ? readFileSync(log, "utf8") : "");
  return p;
}
async function finish(ps, ms = 90000) {
  const t = setTimeout(() => ps.forEach((p) => p.kill()), ms);   // our own PIDs only
  await Promise.all(ps.map((p) => p.done));
  clearTimeout(t);
}

const A = await mkAccount("qa-strict-a"), B = await mkAccount("qa-strict-b"), S = await mkAccount("qa-strict-s");
check(A.ok && B.ok && S.ok, "three test accounts registered on the local worker");
const set = await setStrict(true);
check(!!set, "strict_rooms switched ON by the owner (admin_settings reply seen)");

// 1. lockstep players with proof keep their seats
{
  const rm = room();
  const p0 = peer("lock-p0", rm, { slot: 0, ident: A }), p1 = peer("lock-p1", rm, { slot: 1, ident: B });
  await finish([p0, p1]);
  check(p0.code === 0 && p1.code === 0, `strict ON, lockstep, both players prove their account: exit ${p0.code}/${p1.code}`);
  check(!/4013|ident-required/.test(p0.text() + p1.text()), "no seat was closed 4013");
}
// 2. rollback players + a spectator that proves its account
{
  const rm = room();
  const p0 = peer("rb-p0", rm, { slot: 0, ident: A, rollback: true, frames: 1500 }), p1 = peer("rb-p1", rm, { slot: 1, ident: B, rollback: true, frames: 1500 });
  await sleep(4000);
  const sp = peer("rb-spec", rm, { spectate: true, ident: S, rollback: true, frames: 1500 });
  await finish([p0, p1, sp]);
  check(p0.code === 0 && p1.code === 0, `strict ON, rollback players keep their seats: exit ${p0.code}/${p1.code}`);
  check(!/4013|ident-required/.test(p0.text() + p1.text() + sp.text()), "rollback room + spectator: nobody closed 4013");
  check(sp.code === 0, `the spectator finished cleanly: exit ${sp.code}`);
}
// 3. no proof at all -> refused (control: the switch really bites)
{
  const rm = room();
  const p0 = peer("anon-p0", rm, { slot: 0, ident: null, frames: 400 });
  await finish([p0], 40000);
  check(p0.code !== 0 && /RetroHub account|ident-required|4013/.test(p0.text()), `strict ON, no RETROHUB_IDENT: refused (exit ${p0.code}) with the account message`);
}
// 4. a variable bound to ANOTHER server sends nothing (the secret never leaves for a foreign URL)
{
  const rm = room();
  const p0 = peer("wrong-bind", rm, { slot: 0, ident: A, bindServer: "ws://127.0.0.1:1", frames: 400 });
  await finish([p0], 40000);
  check(p0.code !== 0 && /RetroHub account|ident-required|4013/.test(p0.text()), "RETROHUB_IDENT bound to another server: no proof is sent (the seat is refused like an anonymous one)");
}
// 5. the secret is never logged
{
  const all = ["lock-p0", "lock-p1", "rb-p0", "rb-p1", "rb-spec", "anon-p0", "wrong-bind"].map((t) => { const f = join(out, t, "run.log"); return existsSync(f) ? readFileSync(f, "utf8") : ""; }).join("\n");
  check(!all.includes(A.secret) && !all.includes(B.secret) && !all.includes(S.secret), "no account secret appears in any runner log");
}
await setStrict(false);
console.log(fails ? `RESULT: FAIL (${fails})` : "RESULT: PASS");
process.exit(fails ? 1 : 0);
