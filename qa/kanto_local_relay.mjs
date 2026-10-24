// Local stand-in for the Kanto Together relay (protocol read from the package's coop-bridge.mjs), for tests only.
// usage: node kanto_local_relay.mjs <path to installed KantoTogether dir> <port>      binds 127.0.0.1 only
import { pathToFileURL } from 'node:url';
import path from 'node:path';
import crypto from 'node:crypto';

const dir = process.argv[2];
const port = Number(process.argv[3] || 8799);
const { WebSocketServer } = await import(pathToFileURL(path.join(dir, 'node_modules', 'ws', 'wrapper.mjs')).href);
const rooms = new Map();   // code -> { host, campaign, guests: Map(peerId -> ws) }
const wss = new WebSocketServer({ host: '127.0.0.1', port, maxPayload: 65536 });
const send = (ws, o) => { if (ws.readyState === 1) ws.send(JSON.stringify(o)); };
wss.on('listening', () => console.log(`relay listening ${port}`));
wss.on('connection', (ws) => {
  let room = null, role = null, peerId = null;
  ws.on('message', (raw) => {
    let m; try { m = JSON.parse(raw.toString()); } catch { return; }
    if (m.type === 'register_host') {
      const code = crypto.randomBytes(5).toString('hex').toUpperCase();
      room = { host: ws, campaign: m.campaignId, guests: new Map(), code };
      rooms.set(code, room); role = 'host';
      send(ws, { type: 'registered', code });
    } else if (m.type === 'join' || m.type === 'resume') {
      let r = m.type === 'join' ? rooms.get(String(m.code || '').toUpperCase()) : [...rooms.values()].find((x) => x.campaign === m.campaignId);
      if (!r) { send(ws, { type: 'error', message: 'no such room' }); return; }
      room = r; role = 'guest'; peerId = crypto.randomUUID(); r.guests.set(peerId, ws);
      send(r.host, { type: 'guest_message', peerId, payload: m });
    } else if (role === 'host' && m.type === 'forward_to_guest') {
      const g = room.guests.get(m.peerId); if (g) send(g, m.payload);
    } else if (role === 'guest' && room) {
      send(room.host, { type: 'guest_message', peerId, payload: m });
    }
  });
  ws.on('close', () => {
    if (role === 'host' && room) { rooms.delete(room.code); for (const g of room.guests.values()) g.close(); }
    else if (role === 'guest' && room) { room.guests.delete(peerId); send(room.host, { type: 'guest_left', peerId }); }
  });
  ws.on('error', () => {});
});
