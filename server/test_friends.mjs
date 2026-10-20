// Friends server test. Usage: node test_friends.mjs [ws://127.0.0.1:8799]
import { randomBytes } from "node:crypto";

const BASE = process.argv[2] || "ws://127.0.0.1:8799";
const B32 = "abcdefghijklmnopqrstuvwxyz234567";
function b32(buf) {
  let bits = 0, val = 0, out = "";
  for (const b of buf) {
    val = (val << 8) | b; bits += 8;
    while (bits >= 5) { out += B32[(val >>> (bits - 5)) & 31]; bits -= 5; }
  }
  if (bits > 0) out += B32[(val << (5 - bits)) & 31];
  return out;
}
const makeId = () => ({ uid: b32(randomBytes(16)), secret: b32(randomBytes(32)) });

let pass = 0, fail = 0;
function check(name, cond, extra) {
  if (cond) { pass++; console.log("  ok   " + name); }
  else { fail++; console.log("  FAIL " + name + (extra ? "  " + JSON.stringify(extra) : "")); }
}
const ADMIN_UID = process.env.ADMIN_UID || "4ooju7u7o3zy67l2qqaotuj5n4";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

class Client {
  constructor(id, name, secret) {
    this.id = id; this.name = name; this.secret = secret ?? id.secret;
    this.buf = []; this.waiters = []; this.closed = null;
  }
  open() {
    return new Promise((resolve, reject) => {
      const ws = (this.ws = new WebSocket(`${BASE}/friends?uid=${this.id.uid}`));
      ws.onerror = () => reject(new Error("ws error"));
      ws.onclose = (e) => {
        this.closed = { code: e.code, reason: e.reason };
        for (const w of this.waiters) if (w.close) w.resolve(this.closed);
      };
      ws.onmessage = (e) => {
        const m = JSON.parse(e.data);
        const i = this.waiters.findIndex((w) => !w.close && w.pred(m));
        if (i >= 0) { const [w] = this.waiters.splice(i, 1); w.resolve(m); } else this.buf.push(m);
      };
      ws.onopen = () => {
        this.send({ type: "hello", secret: this.secret, name: this.name, version: "test" });
        resolve();
      };
    });
  }
  async welcome() {
    await this.open();
    const w = await this.wait((m) => m.type === "welcome", 4000);
    // The server auto-friends every new account with the admin (ADMIN_UID). Remove that link at once
    // (it sticks server-side) and hide the admin from the lists the checks look at.
    if (w && w.friends && w.friends.some((f) => f.uid === ADMIN_UID)) {
      this.send({ type: "remove_friend", uid: ADMIN_UID });
      const r = await this.wait(T("friend_removed", { uid: ADMIN_UID }), 4000);
      console.log(`  admin auto-link removed for ${this.name}: ${r ? "friend_removed" : "TIMEOUT"}`);
      w.friends = w.friends.filter((f) => f.uid !== ADMIN_UID);
    }
    return w;
  }
  send(o) { this.ws.send(JSON.stringify(o)); }
  wait(pred, ms = 3000) {
    const i = this.buf.findIndex(pred);
    if (i >= 0) return Promise.resolve(this.buf.splice(i, 1)[0]);
    return new Promise((resolve) => {
      const w = { pred, resolve };
      this.waiters.push(w);
      setTimeout(() => { const k = this.waiters.indexOf(w); if (k >= 0) { this.waiters.splice(k, 1); resolve(null); } }, ms);
    });
  }
  waitClose(ms = 3000) {
    if (this.closed) return Promise.resolve(this.closed);
    return new Promise((resolve) => {
      const w = { close: true, resolve };
      this.waiters.push(w);
      setTimeout(() => resolve(this.closed), ms);
    });
  }
  async none(pred, ms = 600) { await sleep(ms); return !this.buf.some(pred); }
  close() { try { this.ws.close(1000); } catch {} }
}

const T = (type, extra = {}) => (m) => m.type === type && Object.entries(extra).every(([k, v]) => m[k] === v);

async function befriend(a, b) {
  a.send({ type: "add_friend", code: b.code });
  await b.wait(T("request"));
  b.send({ type: "accept", uid: a.id.uid });
  await a.wait(T("friend_added"));
  await b.wait(T("friend_added"));
}

async function main() {
  const A = new Client(makeId(), "qa-alice");
  const B = new Client(makeId(), "qa-bob");
  const D = new Client(makeId(), "qa-dave");

  console.log("register / welcome");
  const wa = await A.welcome();
  const expectCode = (() => { const p = A.id.uid.slice(0, 8).toUpperCase(); return `RH-${p.slice(0, 4)}-${p.slice(4)}`; })();
  check("welcome has code", wa && wa.code === expectCode, wa);
  check("welcome empty lists", wa && wa.friends.length === 0 && wa.requests.incoming.length === 0 && wa.inbox.length === 0);
  A.code = wa.code;
  B.code = (await B.welcome()).code;
  D.code = (await D.welcome()).code;

  console.log("bad secret");
  const bad = new Client(A.id, "qa-mallory", "x".repeat(40));
  await bad.open();
  const c = await bad.waitClose();
  check("bad secret closed 4003", c && c.code === 4003, c);

  console.log("errors");
  A.send({ type: "add_friend", code: "RH-AAAA-AAAA" });
  const e1 = await A.wait(T("error"));
  check("unknown code", e1 && e1.reason === "unknown-code", e1);
  A.send({ type: "add_friend", code: A.code });
  const e2 = await A.wait(T("error"));
  check("self add", e2 && e2.reason === "self", e2);

  console.log("add -> request -> accept");
  A.send({ type: "add_friend", code: B.code.toLowerCase() });
  const out = await A.wait(T("request"));
  const inc = await B.wait(T("request"));
  check("A sees outgoing", out && out.dir === "out" && out.req.uid === B.id.uid, out);
  check("B sees incoming", inc && inc.req.uid === A.id.uid && inc.req.name === "qa-alice", inc);
  B.send({ type: "accept", uid: A.id.uid });
  const fa = await A.wait(T("friend_added"));
  const fb = await B.wait(T("friend_added"));
  check("friend_added A", fa && fa.friend.uid === B.id.uid && fa.friend.name === "qa-bob", fa);
  check("friend_added B", fb && fb.friend.uid === A.id.uid, fb);
  A.send({ type: "add_friend", code: B.code });
  const e3 = await A.wait(T("error"));
  check("already friends", e3 && e3.reason === "already-friends", e3);

  console.log("presence");
  B.send({ type: "status", status: { state: "in_lobby", game_id: "smash64", game_title: "Smash", system: "n64", lobby_code: "ABC234", joinable: true, players: 1, max_players: 4 } });
  const p1 = await A.wait((m) => m.type === "presence" && m.status.state === "in_lobby");
  check("friend sees in_lobby + lobby_code", p1 && p1.uid === B.id.uid && p1.status.lobby_code === "ABC234" && p1.status.joinable === true, p1);
  check("non-friend gets nothing", await D.none(T("presence")));
  B.send({ type: "status", status: { state: "offline" } });
  const p2 = await A.wait((m) => m.type === "presence" && m.status.state === "offline");
  check("invisible shows offline", p2 && p2.status.lobby_code === undefined, p2);
  B.send({ type: "status", status: { state: "in_game", game_id: "g" } });
  await A.wait((m) => m.type === "presence" && m.status.state === "in_game");

  console.log("multi-device");
  const B2 = new Client(B.id, "qa-bob");
  await B2.welcome();
  check("second device keeps in_game (no fan-out)", await A.none(T("presence")));
  B.close();
  const p3 = await A.wait((m) => m.type === "presence" && m.status.state === "online");
  check("closing best device -> online", !!p3, p3);
  B2.close();
  const p4 = await A.wait((m) => m.type === "presence" && m.status.state === "offline");
  check("last socket closed -> offline", !!p4, p4);

  console.log("offline message -> inbox -> ack");
  await sleep(200);
  A.send({ type: "message", to: B.id.uid, text: "hello offline" });
  await sleep(400);
  let B3 = new Client(B.id, "qa-bob");
  let w = await B3.welcome();
  check("inbox has message", w.inbox.length === 1 && w.inbox[0].text === "hello offline" && w.inbox[0].from === A.id.uid && w.inbox[0].kind === "text", w);
  const msgId = w.inbox[0]?.id;
  B3.close();
  await sleep(300);
  B3 = new Client(B.id, "qa-bob");
  w = await B3.welcome();
  check("inbox redelivered until ack", w.inbox.length === 1);
  B3.send({ type: "ack", ids: [msgId] });
  await sleep(300);
  B3.close();
  await sleep(300);
  B3 = new Client(B.id, "qa-bob");
  w = await B3.welcome();
  check("ack deletes", w.inbox.length === 0, w.inbox);
  A.send({ type: "message", to: B.id.uid, text: "live msg" });
  const lm = await B3.wait(T("msg"));
  check("online push", lm && lm.msg.text === "live msg", lm);
  A.send({ type: "history", with: B.id.uid });
  const h = await A.wait(T("history"));
  check("history", h && h.msgs.length === 2 && h.msgs[1].text === "live msg", h);
  A.send({ type: "message", to: B.id.uid, text: "x".repeat(501) });
  const e4 = await A.wait(T("error"));
  check("message > 500 rejected", e4 && e4.reason === "bad-request", e4);
  A.send({ type: "message", to: D.id.uid, text: "hi" });
  const e5 = await A.wait(T("error"));
  check("message to non-friend", e5 && e5.reason === "not-friends", e5);
  await B3.wait(T("presence"), 100); // drain nothing

  console.log("invites");
  A.send({ type: "invite", to: [B.id.uid], invite: { lobby_code: "ABC234", game_id: "smash64", game_title: "Smash 64", system: "n64" } });
  const iv = await B3.wait((m) => m.type === "msg" && m.msg.kind === "invite");
  check("invite delivered", iv && iv.msg.invite.lobby_code === "ABC234" && iv.msg.invite.expires_at > Date.now() && iv.msg.from_name === "qa-alice", iv);
  A.send({ type: "invite", to: [D.id.uid], invite: { lobby_code: "ABC234", game_id: "g", game_title: "G", system: "n64" } });
  const e6 = await A.wait(T("error"));
  check("invite to non-friend rejected", e6 && e6.reason === "not-friends", e6);
  A.send({ type: "invite", to: [B.id.uid], invite: { lobby_code: "bad!", game_id: "g" } });
  const e7 = await A.wait(T("error"));
  check("invite bad lobby code", e7 && e7.reason === "bad-request", e7);
  for (let i = 0; i < 25; i++) A.send({ type: "invite", to: [B.id.uid], invite: { lobby_code: "ABC234", game_id: "g", game_title: "G", system: "n64" } });
  const rl = await A.wait((m) => m.type === "error" && m.reason === "rate-limited", 4000);
  check("invite rate limited (20/min)", !!rl, rl);
  await sleep(500);
  B3.buf.length = 0;

  console.log("remove friend");
  A.send({ type: "remove_friend", uid: B.id.uid });
  const ra = await A.wait(T("friend_removed"));
  const rb = await B3.wait(T("friend_removed"));
  check("friend_removed both sides", ra && rb && rb.uid === A.id.uid, { ra, rb });
  B3.send({ type: "message", to: A.id.uid, text: "still there?" });
  const e8 = await B3.wait(T("error"));
  check("message after removal fails", e8 && e8.reason === "not-friends", e8);

  console.log("block");
  B3.code = B.code;
  await befriend(A, B3);
  B3.send({ type: "status", status: { state: "in_lobby", lobby_code: "ZZZ234", joinable: true } });
  await A.wait((m) => m.type === "presence" && m.status.state === "in_lobby");
  A.send({ type: "block", uid: B.id.uid });
  const bf = await B3.wait(T("friend_removed"));
  check("block removes friendship", !!bf, bf);
  A.buf.length = 0;
  B3.send({ type: "status", status: { state: "in_game", game_id: "x" } });
  check("blocked user presence hidden", await A.none(T("presence")));
  B3.send({ type: "message", to: A.id.uid, text: "let me in" });
  const e9 = await B3.wait(T("error"));
  check("blocked user cannot message", e9 && e9.reason === "not-friends", e9);
  B3.send({ type: "add_friend", code: A.code });
  const sent = await B3.wait(T("request"));
  check("blocked add_friend looks pending to sender", sent && sent.dir === "out", sent);
  check("blocker receives nothing (request dropped)", await A.none(T("request")));
  A.send({ type: "add_friend", code: B.code });
  const e10 = await A.wait((m) => m.type === "error" && m.ref === "add_friend");
  check("blocker cannot add blocked", e10 && e10.reason === "blocked", e10);
  A.send({ type: "unblock", uid: B.id.uid });
  await A.wait(T("block_update"));

  console.log("name sanitize");
  const E = new Client(makeId(), "qa-\u0007  Evil\u0000   Na​me " + "x".repeat(40));
  E.code = (await E.welcome()).code;
  await befriend(A, E);
  A.send({ type: "status", status: { state: "online" } });
  const fl = new Client(A.id, "qa-alice");
  const wl = await fl.welcome();
  const ef = wl.friends.find((f) => f.uid === E.id.uid);
  check("name sanitized (ctrl chars stripped, <=24)", ef && ef.name.length <= 24 && !/[\u0000-\u001f​]/.test(ef.name) && ef.name.startsWith("qa- Evil Name"), ef);
  E.send({ type: "rename", name: "  qa-New\tName  " });

  const rp = await A.wait((m) => m.type === "presence" && m.uid === E.id.uid && m.name === "qa-New Name");
  check("rename fans out sanitized name", !!rp, rp);

  console.log("limits");
  E.ws.send(JSON.stringify({ type: "message", to: A.id.uid, text: "y".repeat(9000) }));
  const e11 = await E.wait(T("error"));
  check("oversize JSON rejected", e11 && e11.reason === "bad-request", e11);

  console.log("internal routes not public");
  const r = await fetch(BASE.replace("ws", "http") + "/i/presence", { method: "POST", body: "{}" });
  check("/i/presence is 404 publicly", r.status === 404, r.status);

  for (const c of [A, B3, D, E, fl]) c.close();
  console.log(`\n${pass} passed, ${fail} failed`);
  process.exit(fail ? 1 : 0);
}

main().catch((e) => { console.error(e); process.exit(2); });
