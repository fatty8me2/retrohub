// Node 24 test for the /hub endpoint. Usage: node test_hub.mjs [ws://127.0.0.1:8799]
const BASE = process.argv[2] || "ws://127.0.0.1:8799";
let fails = 0;
const check = (c, m) => { if (!c) { fails++; console.log("FAIL:", m); } else console.log("ok:  ", m); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function open(room, role, name, slot, caps) {
  let u = `${BASE}/hub?room=${room}&role=${role}&name=${name}` + (slot !== undefined ? `&slot=${slot}` : "") + (caps ? `&caps=${caps}` : "");
  const ws = new WebSocket(u);
  ws.binaryType = "arraybuffer";
  const c = { ws, text: [], bin: [], closed: null };
  ws.onmessage = (e) => {
    if (typeof e.data === "string") c.text.push(JSON.parse(e.data));
    else c.bin.push(new Uint8Array(e.data));
  };
  ws.onclose = (e) => { c.closed = { code: e.code, reason: e.reason }; };
  c.send = (o) => ws.send(JSON.stringify(o));
  c.last = (type) => [...c.text].reverse().find((m) => m.type === type);
  c.wait = async (fn, ms = 3000) => {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) { if (fn()) return true; await sleep(20); }
    return false;
  };
  return c;
}

const code = () => {
  const a = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  let s = ""; for (let i = 0; i < 6; i++) s += a[Math.floor(Math.random() * a.length)];
  return s;
};

const R = code();
const no = open(R, "join", "ghost");
await no.wait(() => no.closed);
check(no.last("error")?.reason === "no-such-room" && no.closed.code === 4000, "join without host -> no-such-room");

const h = open(R, "host", "alice");
await h.wait(() => h.last("welcome"));
check(h.last("welcome")?.slot === 0 && h.last("welcome").room === R, "host welcome slot 0");

const dup = open(R, "host", "evil");
await dup.wait(() => dup.closed);
check(dup.last("error")?.reason === "room-in-use", "second host -> room-in-use");

const b = open(R, "join", "bob");
await b.wait(() => b.last("welcome"));
check(b.last("welcome")?.slot === 1, "first joiner slot 1");
const c = open(R, "join", "carol");
await c.wait(() => c.last("welcome"));
check(c.last("welcome")?.slot === 2, "second joiner slot 2");
const d = open(R, "join", "dave\u0001x".repeat(10));
await d.wait(() => d.last("welcome"));
check(d.last("welcome")?.slot === 3, "third joiner slot 3");
const e = open(R, "join", "eve");
await e.wait(() => e.closed);
check(e.last("error")?.reason === "room-full", "fifth member -> room-full");

await sleep(200);
const mem = h.last("members").members;
check(mem.length === 4 && mem[0].name === "alice" && mem[1].name === "bob", "members broadcast has 4");
check(mem[3].name.length <= 24 && !/[\x00-\x1f]/.test(mem[3].name), "name sanitized/trimmed: " + JSON.stringify(mem[3].name));

// msg
b.send({ type: "msg", body: { kind: "chat", text: "hi" } });
await h.wait(() => h.last("msg"));
check(h.last("msg")?.from === 1 && h.last("msg").body.text === "hi", "msg reaches host with from=1");
check(c.last("msg")?.from === 1, "msg reaches other client");
check(!b.last("msg"), "msg not echoed to sender");

// peers
b.send({ type: "endpoint", public: "1.2.3.4:5555", local: "192.168.1.5:5555" });
await h.wait(() => h.last("peers"));
let p = h.last("peers").peers;
check(p.length === 1 && p[0].slot === 1 && p[0].public === "1.2.3.4:5555", "peers broadcast");
c.send({ type: "endpoint", public: "<script>", local: "-" });
await h.wait(() => h.last("peers")?.peers.length === 2);
p = d.last("peers").peers;
check(p.length === 2 && p[1].public === "-", "invalid endpoint sanitized to '-'");

// binary relay
b.ws.send(new Uint8Array([2, 9, 8, 7]));
await c.wait(() => c.bin.length);
check(c.bin[0] && c.bin[0][0] === 1 && c.bin[0][1] === 9 && c.bin[0].length === 4, "binary to slot 2 rewritten to sender 1");
check(h.bin.length === 0 && d.bin.length === 0, "binary only to dest");
h.ws.send(new Uint8Array([0xff, 5]));
await d.wait(() => d.bin.length);
check(b.bin.length === 1 && c.bin.length === 2 && d.bin.length === 1 && d.bin[0][0] === 0, "binary broadcast 0xFF");
h.ws.send(new Uint8Array(1300));
await sleep(200);
check(b.bin.length === 1, "oversized binary dropped");

// keepalive
const n = b.text.length;
b.send({ type: "keepalive" });
await b.wait(() => b.text.length > n);
check(b.last("keepalive_ack"), "keepalive ack");

// client leave
d.ws.close();
await h.wait(() => h.last("members")?.members.length === 3);
check(h.last("members").members.length === 3, "members updated on leave");
// slot reuse via explicit slot
const d2 = open(R, "join", "dave2", 3);
await d2.wait(() => d2.last("welcome"));
check(d2.last("welcome")?.slot === 3, "slot=3 request honored");
const d3 = open(R, "join", "dupslot", 3);
await d3.wait(() => d3.closed);
check(d3.last("error")?.reason === "room-full", "taken slot -> room-full");

// host leaves
h.ws.close();
await b.wait(() => b.closed, 15000);
await c.wait(() => c.closed, 15000);
await d2.wait(() => d2.closed, 10000); console.log(b.closed, c.closed, d2.closed, d2.ws.readyState, d2.text.map(m=>m.type+JSON.stringify(m.members||"")));
check(b.closed?.code === 4001 && c.closed?.code === 4001 && d2.closed?.code === 4001, "host-left closes everyone with 4001");


// ---- host migration (clients that send caps=hostmig) ----
const M = code();
const m0 = open(M, "host", "host0", undefined, "hostmig");
await m0.wait(() => m0.last("welcome"));
const m1 = open(M, "join", "one", undefined, "hostmig");
const m2 = open(M, "join", "two", undefined, "hostmig");
const m3 = open(M, "join", "three", undefined, "hostmig");
await m3.wait(() => m3.last("welcome"));
await sleep(150);
m2.send({ type: "endpoint", public: "1.1.1.1:1", local: "-" });
m0.send({ type: "msg", body: { kind: "select", game: "mk64" } });
await m1.wait(() => m1.last("msg"));
m0.ws.close();                       // host leaves
await m1.wait(() => m1.last("host_changed"));
await m2.wait(() => m2.last("host_changed"));
check(!m1.closed && !m2.closed && !m3.closed, "migration: members stay connected");
let hc = m1.last("host_changed");
check(hc && hc.from_slot === 1 && hc.slot === 0 && hc.name === "one", "migration: host_changed names lowest slot (1 -> 0)");
check(m3.last("host_changed")?.from_slot === 1, "migration: every member gets host_changed");
await sleep(150);
let ml = m3.last("members").members;
check(ml.length === 3 && ml[0].slot === 0 && ml[0].name === "one" && ml[1].slot === 2 && ml[2].slot === 3, "migration: members = new host at slot 0, others keep slots");
const dupH = open(M, "host", "usurp", undefined, "hostmig");
await dupH.wait(() => dupH.closed);
check(dupH.last("error")?.reason === "room-in-use", "migration: code still has exactly one host (role=host -> room-in-use)");
m1.send({ type: "msg", body: { kind: "select", game: "mk64" } });   // new host acts as slot 0
await m2.wait(() => m2.text.filter((t) => t.type === "msg").length >= 1);
check(m2.last("msg")?.from === 0 && m2.last("msg").body.game === "mk64", "migration: new host's msgs arrive from slot 0");
m2.ws.send(new Uint8Array([0, 7]));
await m1.wait(() => m1.bin.length);
check(m1.bin[0]?.[0] === 2, "migration: binary to slot 0 reaches the new host");
const n1 = open(M, "join", "late", undefined, "hostmig");
await n1.wait(() => n1.last("welcome"));
check(n1.last("welcome")?.slot === 1, "migration: new joiner takes the free slot 1");
// double leave: new host and next-lowest leave in quick succession
m1.ws.close(); n1.ws.close();
await sleep(800);
const hcs = m3.text.filter((t) => t.type === "host_changed");
check((hcs.length === 2 || hcs.length === 3) && hcs.at(-1).from_slot === 2 && hcs.at(-1).name === "two" && !m2.closed && !m3.closed, "migration: double leave (either order) ends with slot 2 promoted, nobody dropped");
ml = m3.last("members").members;
check(ml.length === 2 && ml[0].slot === 0 && ml[0].name === "two" && ml[1].slot === 3, "migration: members after double leave");
// race: both remaining leave at once -> room closes, code reusable
m2.ws.close(); m3.ws.close();
let re;
for (let i = 0; i < 40; i++) {   // local workerd delivers client closes slowly; retry until the room has emptied
  await sleep(500);
  re = open(M, "host", "fresh", undefined, "hostmig");
  await re.wait(() => re.last("welcome") || re.closed);
  if (re.last("welcome")) break;
}
check(re.last("welcome")?.slot === 0 && re.last("members")?.members.length === 1, "migration: empty room closes; code can be hosted again fresh");
const ghost = open(M, "join", "ghost2", undefined, "hostmig");
await ghost.wait(() => ghost.last("welcome"));
check(ghost.last("welcome")?.slot === 1, "migration: fresh room works after previous one emptied");
re.ws.close(); ghost.ws.close();
await sleep(200);

// mixed room: old client (no caps) is closed 4001, capable members migrate
const X = code();
const x0 = open(X, "host", "xh", undefined, "hostmig");
await x0.wait(() => x0.last("welcome"));
const xold = open(X, "join", "oldie");            // slot 1, no caps
await xold.wait(() => xold.last("welcome"));
const xnew = open(X, "join", "newbie", undefined, "hostmig");
await xnew.wait(() => xnew.last("welcome"));
x0.ws.close();
await xold.wait(() => xold.closed, 20000);
await xnew.wait(() => xnew.last("host_changed"));
check(xold.closed?.code === 4001 && !xold.last("host_changed"), "mixed: old client closed with 4001, never sees host_changed");
check(!xnew.closed && xnew.last("host_changed")?.from_slot === 2, "mixed: capable member is promoted to host");
await sleep(150);
check(xnew.last("members").members.length === 1 && xnew.last("members").members[0].slot === 0, "mixed: members excludes the closed old client");
xnew.ws.close();

// all-old room keeps the original behaviour (covered above: "host-left closes everyone with 4001")

// game rooms (suffix) never migrate even for capable clients
const GM = code() + "-3";
const gm0 = open(GM, "host", "n64run", undefined, "hostmig");
await gm0.wait(() => gm0.last("welcome"));
const gm1 = open(GM, "join", "n64run", 1, "hostmig");
await gm1.wait(() => gm1.last("welcome"));
gm0.ws.close();
await gm1.wait(() => gm1.closed, 20000);
check(gm1.closed?.code === 4001 && !gm1.last("host_changed"), "game room: host leaving still closes members (no migration)");

// game room
const G = R + "-7";
const gh = open(G, "host", "n64run");
await gh.wait(() => gh.last("welcome"));
const gj = open(G, "join", "n64run", 2);
await gj.wait(() => gj.last("welcome"));
check(gh.last("welcome")?.slot === 0 && gj.last("welcome")?.slot === 2, "game room: host 0, joiner slot=2");
check(gh.last("welcome").room === G, "game room code echoed");
const bad = open("abc", "host", "x");
await bad.wait(() => bad.closed);
check(bad.closed !== null, "bad room code rejected");
gh.ws.close(); gj.ws.close();
await sleep(200);

console.log(fails ? `${fails} FAILURES` : "ALL PASSED");
process.exit(fails ? 1 : 0);
