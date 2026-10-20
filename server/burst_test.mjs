// Relay burst test: node burst_test.mjs [ws://127.0.0.1:8799] [count] [gapMs]
const BASE = process.argv[2] || "ws://127.0.0.1:8799";
const N = +(process.argv[3] || 1000);
const GAP = +(process.argv[4] || 0);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const room = "BURST" + Math.floor(Math.random() * 8 + 2);
const mk = (role, slot) => {
  const ws = new WebSocket(`${BASE}/hub?room=${room}&role=${role}&name=t${slot ?? ""}` + (slot ? `&slot=${slot}` : ""));
  ws.binaryType = "arraybuffer";
  const c = { ws, n: 0, open: new Promise((r) => (ws.onopen = r)) };
  ws.onmessage = (e) => { if (typeof e.data !== "string") c.n++; };
  return c;
};
const h = mk("host"); await h.open; await sleep(100);
const j = mk("join", 1); await j.open; await sleep(200);
for (let i = 0; i < N; i++) {
  j.ws.send(new Uint8Array([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]));
  if (GAP && i % 4 === 3) await sleep(GAP);
}
await sleep(1500);
console.log(`client->host burst: sent ${N}, host received ${h.n}`);
process.exit(0);
