// Scripted lobby side of the drop-in QA (what the hub does, without the hub window): a host lobby seat sends the launch message, a drop-in seat
// (role=watch&dropin=1) receives the replayed launch and derives the joiner's runner arguments exactly like hubui::dropin_port_plan, and a second
// drop-in seat afterwards must be refused no-free-port once the runner has joined the game room (the server committed the port).
// usage: node dropin_lobby.mjs <ws://server> <CODE> <join_after_sec> <out.json> <hold_sec>
const [server, code, joinAfter, out, hold] = process.argv.slice(2);
import { writeFileSync, appendFileSync } from "node:fs";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (o) => appendFileSync(out, JSON.stringify(o) + "\n");
function open(role, extra = "") {
  const ws = new WebSocket(`${server}/hub?room=${code}&role=${role}&name=${role === "host" ? "Host" : "Kai"}&caps=reasons${extra}`);
  const c = { ws, msgs: [], closed: null };
  ws.onmessage = (e) => { if (typeof e.data === "string") c.msgs.push(JSON.parse(e.data)); };
  ws.onclose = (e) => { c.closed = e.code; };
  return c;
}
const until = async (f, ms) => { const t = Date.now(); while (Date.now() - t < ms) { const v = f(); if (v) return v; await sleep(50); } return null; };
writeFileSync(out, "");
const host = open("host");
if (!(await until(() => host.msgs.find((m) => m.type === "welcome"), 10000))) { log({ error: "host seat" }); process.exit(1); }
// the hub's launch message for a 2-player multi-port room with one free port (hubui::launch_json "dropin":N)
host.ws.send(JSON.stringify({ type: "msg", body: { kind: "launch", game: "qa-netplay", launch: 1, delay: 0, seed: 1, share_save: false, ports: [0, 1, -1, -1], players: 2, dropin: 1 } }));
await sleep(Number(joinAfter) * 1000);
const seat = open("watch", "&dropin=1");
const launchMsg = await until(() => seat.msgs.find((m) => m.type === "msg" && m.body && m.body.kind === "launch"), 10000);
const welcome = seat.msgs.find((m) => m.type === "welcome");
if (!launchMsg) { log({ error: "no launch replayed", closed: seat.closed, msgs: seat.msgs }); process.exit(1); }
const b = launchMsg.body;
const ports = b.ports.slice();
let slot = [1, 2, 3].find((s) => !ports.includes(s));
const free = ports.indexOf(-1);
ports[free] = slot;
log({ step: "plan", welcome, replayed: { watch: b.watch, dropin_join: b.dropin_join, dropin: b.dropin, launch: b.launch }, slot, ports: ports.join(","), players: new Set(b.ports.filter((p) => p >= 0)).size, launch: b.launch });
seat.ws.close();   // the hub closes its lobby seat as soon as the runner is started
await sleep(Number(hold) * 1000 > 15000 ? 15000 : 8000);
const again = open("watch", "&dropin=1");
await until(() => again.closed !== null || again.msgs.find((m) => m.type === "welcome"), 5000);
const err = again.msgs.find((m) => m.type === "error");
log({ step: "second", refused: err ? err.reason : null, welcomed: !!again.msgs.find((m) => m.type === "welcome") });
again.ws.close();
await sleep(Math.max(0, Number(hold) * 1000 - 15000));
host.ws.close();
process.exit(0);
