// Node 24 test for the /hub endpoint. Usage: node test_hub.mjs [ws://127.0.0.1:8799]
const BASE = process.argv[2] || "ws://127.0.0.1:8799";
let fails = 0;
const check = (c, m) => { if (!c) { fails++; console.log("FAIL:", m); } else console.log("ok:  ", m); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function open(room, role, name, slot) {
  let u = `${BASE}/hub?room=${room}&role=${role}&name=${name}` + (slot !== undefined ? `&slot=${slot}` : "");
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
await b.wait(() => b.closed);
await c.wait(() => c.closed);
await d2.wait(() => d2.closed, 10000); console.log(b.closed, c.closed, d2.closed, d2.ws.readyState, d2.text.map(m=>m.type+JSON.stringify(m.members||"")));
check(b.closed?.code === 4001 && c.closed?.code === 4001 && d2.closed?.code === 4001, "host-left closes everyone with 4001");

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
