#!/usr/bin/env node
/* Builds data/gameinfo.tsv (RetroHub's offline game metadata) from libretro-database metadat/.
 *
 *   node scripts/gameinfo_build.js [out.tsv] [cache-dir]
 *
 * Source: https://github.com/libretro/libretro-database  (CC BY-SA 4.0), pinned to PIN below. Downloads are cached in cache-dir
 * (default: <os tmp>/gameinfo_cache) so a rebuild is reproducible and offline-friendly. Needs Node 18+ (global fetch).
 *
 * Output: one tab-separated line per game ("base title"), regions merged (USA > Europe > World > Japan > other), betas/protos/demos dropped:
 *   sys  title  serials(,)  developer  publisher  genre  year  month  players  esrb  famitsu  edge
 * Consumed by src/common/gameinfo.cpp. Keep the format in sync with gameinfo::load_db(). */
'use strict';
const fs = require('fs');
const path = require('path');
const os = require('os');

const PIN = 'd5e2e4d2453e3726f6c6a569b0504653eca436ff';   // libretro-database master on 2026-10-25
const RAW = `https://raw.githubusercontent.com/libretro/libretro-database/${PIN}/metadat/`;
const out = process.argv[2] || path.join(__dirname, '..', 'data', 'gameinfo.tsv');
const cache = process.argv[3] || path.join(os.tmpdir(), 'gameinfo_cache');
fs.mkdirSync(cache, { recursive: true });

/* system short -> libretro system name and the metadat folders that hold data (crc-keyed systems) */
const CRC_SYSTEMS = {
  n64: { name: 'Nintendo - Nintendo 64', esrb: 'Nintendo - Nintendo 64 (BigEndian)', mags: true },
  gba: { name: 'Nintendo - Game Boy Advance' },
  gb:  { name: 'Nintendo - Game Boy' },
  gbc: { name: 'Nintendo - Game Boy Color' },
  nds: { name: 'Nintendo - Nintendo DS' },
  nes: { name: 'Nintendo - Nintendo Entertainment System' },
  snes: { name: 'Nintendo - Super Nintendo Entertainment System' },
};
const SERIAL_SYSTEMS = { ps1: 'Sony - PlayStation', ps2: 'Sony - PlayStation 2' };

async function get(rel) {
  const f = path.join(cache, rel.replace(/[\\/:*?"<>|]/g, '_'));
  if (fs.existsSync(f)) return fs.readFileSync(f, 'utf8');
  const res = await fetch(RAW + rel.split('/').map(encodeURIComponent).join('/'));
  if (res.status === 404) { fs.writeFileSync(f, ''); return ''; }
  if (!res.ok) throw new Error(rel + ': HTTP ' + res.status);
  const t = await res.text();
  fs.writeFileSync(f, t);
  return t;
}

/* clrmamepro / libretro dat -> [{field: value|[values], rom: [{...}]}] for every top-level "game (" block */
function parseDat(text) {
  const tok = [];
  const re = /"((?:[^"\\]|\\.)*)"|([()])|([^\s()"]+)/g;
  let m;
  while ((m = re.exec(text))) tok.push(m[1] !== undefined ? { s: m[1] } : { w: m[2] || m[3] });
  const games = [];
  let i = 0;
  function block() {   // after '('
    const o = { rom: [] };
    while (i < tok.length) {
      const t = tok[i++];
      if (t.w === ')') return o;
      const key = t.w;
      if (!key) continue;
      if (tok[i] && tok[i].w === '(') {
        i++;
        const sub = block();
        if (key === 'rom') o.rom.push(sub); else o[key] = sub;
      } else if (tok[i]) {
        const v = tok[i++];
        o[key] = v.s !== undefined ? v.s : v.w;
      }
    }
    return o;
  }
  while (i < tok.length) {
    const t = tok[i++];
    if (t.w === 'game' && tok[i] && tok[i].w === '(') { i++; games.push(block()); }
    else if (t.w === 'clrmamepro' && tok[i] && tok[i].w === '(') { i++; block(); }
  }
  return games;
}

function region(name) {
  const m = /\(([^)]*)\)/g;
  let r = 9, x;
  while ((x = m.exec(name))) {
    const s = x[1];
    if (/USA|^US$/.test(s)) r = Math.min(r, 0);
    else if (/Europe|^EU$|^UK$|Australia/.test(s)) r = Math.min(r, 1);
    else if (/World/.test(s)) r = Math.min(r, 2);
    else if (/Japan|^JP$/.test(s)) r = Math.min(r, 3);
  }
  return r;
}
const JUNK = /\((?:[^)]*\b)?(?:Beta|Proto|Prototype|Demo|Sample|Unl|Kiosk|Promo|Possible Proto|Debug|Pirate|DAT|Undub|Not for Resale|Preview|Alpha|Test Program|Aftermarket|Bung|Hack|Translated?)\b[^)]*\)/i;

/* "Legend of Zelda, The - Ocarina of Time (USA) (Rev B)" -> "The Legend of Zelda: Ocarina of Time" */
function baseTitle(name) {
  let t = name.replace(/\s*\([^)]*\)/g, '').replace(/\s*\[[^\]]*\]/g, '').replace(/\.(cue|bin|iso|chd)$/i, '').trim();
  t = t.replace(/^(.*?), (The|A|An)( - |: |$)/, (a, b, art, sep) => art + ' ' + b + (sep.trim() ? ': ' : ''));
  t = t.replace(/ - /g, ': ');
  return t.trim();
}
function normKey(t) {
  return t.toLowerCase().normalize('NFKD').replace(/[̀-ͯ]/g, '').replace(/&/g, 'and').replace(/\bversion\b/g, '').replace(/[^a-z0-9]/g, '').replace(/^the/, '');
}

const FIELDS = ['dev', 'pub', 'genre', 'year', 'month', 'players', 'esrb', 'famitsu', 'edge'];
function emptyRec() { const r = {}; for (const f of FIELDS) r[f] = ''; r.serials = new Set(); return r; }

function clean(s) { return String(s == null ? '' : s).replace(/[\t\r\n]+/g, ' ').trim(); }

async function buildCrcSystem(sys, cfg) {
  const byCrc = new Map();   // crc -> {name, serial, f: {field: value}}
  const touch = (crc, name) => {
    crc = crc.toUpperCase();
    let r = byCrc.get(crc);
    if (!r) { r = { name: '', serials: new Set(), f: {} }; byCrc.set(crc, r); }
    if (name && !r.name) r.name = name;
    return r;
  };
  async function layer(folder, sysName, field, key, transform) {
    const games = parseDat(await get(`${folder}/${sysName}.dat`));
    for (const g of games) {
      const v = g[key];
      if (v === undefined) continue;
      for (const rom of g.rom) {
        if (!rom.crc) continue;
        const r = touch(rom.crc, g.comment || g.name);
        if (field === '_serial') { const mm = /^[A-Z0-9]{3}-([A-Z0-9]{4})(?:-|$)/.exec(v); if (mm) r.serials.add(mm[1]); }
        else r.f[field] = transform ? transform(v) : v;
      }
    }
  }
  await layer('serial', cfg.name, '_serial', 'serial');
  await layer('developer', cfg.name, 'dev', 'developer');
  await layer('publisher', cfg.name, 'pub', 'publisher');
  await layer('genre', cfg.name, 'genre', 'genre');
  await layer('releaseyear', cfg.name, 'year', 'releaseyear');
  await layer('releasemonth', cfg.name, 'month', 'releasemonth');
  await layer('maxusers', cfg.name, 'players', 'users');
  await layer('esrb', cfg.esrb || cfg.name, 'esrb', 'esrb_rating');
  if (cfg.mags) {
    await layer('magazine/famitsu', cfg.name, 'famitsu', 'famitsu_rating');
    await layer('magazine/edge', cfg.name, 'edge', 'edge_rating');
  }
  const groups = new Map();
  for (const r of byCrc.values()) {
    if (!r.name || JUNK.test(r.name)) continue;
    const bt = baseTitle(r.name);
    if (!bt) continue;
    const k = normKey(bt);
    let g = groups.get(k);
    if (!g) { g = { title: bt, rank: 99, rec: emptyRec() }; groups.set(k, g); }
    const rk = region(r.name);
    if (rk < g.rank) { g.rank = rk; g.title = bt; }
    // best region first: a field from a better region overwrites; otherwise only fills gaps
    for (const f of FIELDS) {
      const v = clean(r.f[f]);
      if (!v) continue;
      if (!g.rec[f] || rk <= (g['rk_' + f] ?? 99)) { g.rec[f] = v; g['rk_' + f] = rk; }
    }
    for (const s of r.serials) g.rec.serials.add(s);
  }
  return [...groups.values()].map(g => ({ sys, title: g.title, ...g.rec }));
}

async function buildSerialSystem(sys, sysName) {
  const games = parseDat(await get(`developer/${sysName}.dat`));
  let esrb = new Map(), edge = new Map(), fam = new Map();
  const serialLayer = async (folder, key, map) => {
    for (const g of parseDat(await get(`${folder}/${sysName}.dat`))) {
      const v = g[key];
      if (v === undefined) continue;
      for (const rom of g.rom) if (rom.serial) map.set(rom.serial.toUpperCase(), v);
    }
  };
  if (sys === 'ps1') {
    await serialLayer('esrb', 'esrb_rating', esrb);
    await serialLayer('magazine/edge', 'edge_rating', edge);
    await serialLayer('magazine/famitsu', 'famitsu_rating', fam);
  }
  const groups = new Map();
  for (const g of games) {
    const name = g.name || '';
    if (!name || JUNK.test(name)) continue;
    const serials = new Set();
    for (const s of [g.serial, ...g.rom.map(r => r.serial)]) if (s) for (const p of String(s).split(/[,\s]+/)) if (p) serials.add(p.toUpperCase());
    for (const x of [...serials]) if (!/^S[LC][UE][SD]-/.test(x)) serials.delete(x);   // US/EU discs only (keeps the bundle small)
    if (!serials.size) continue;
    if (!g.developer && !g.publisher) continue;
    const bt = baseTitle(name);
    const k = normKey(bt);
    let gr = groups.get(k);
    if (!gr) { gr = { title: bt, rank: 99, rec: emptyRec() }; groups.set(k, gr); }
    const rk = region(name);
    if (rk < gr.rank) { gr.rank = rk; gr.title = bt; }
    const f = { dev: g.developer, pub: g.publisher, genre: g.genre, year: g.releaseyear, month: g.releasemonth, players: g.users };
    for (const s of serials) { if (esrb.has(s)) f.esrb = esrb.get(s); if (edge.has(s)) f.edge = edge.get(s); if (fam.has(s)) f.famitsu = fam.get(s); }
    for (const x of FIELDS) {
      const v = clean(f[x]);
      if (!v) continue;
      if (!gr.rec[x] || rk <= (gr['rk_' + x] ?? 99)) { gr.rec[x] = v; gr['rk_' + x] = rk; }
    }
    for (const s of serials) gr.rec.serials.add(s);
  }
  return [...groups.values()].map(g => ({ sys, title: g.title, ...g.rec }));
}

(async () => {
  const rows = [];
  for (const [sys, cfg] of Object.entries(CRC_SYSTEMS)) rows.push(...await buildCrcSystem(sys, cfg));
  for (const [sys, nm] of Object.entries(SERIAL_SYSTEMS)) rows.push(...await buildSerialSystem(sys, nm));
  rows.sort((a, b) => a.sys.localeCompare(b.sys) || a.title.localeCompare(b.title));
  const lines = ['#gameinfo v1 libretro-database ' + PIN + ' (CC BY-SA 4.0)',
                 '#sys\ttitle\tserials\tdeveloper\tpublisher\tgenre\tyear\tmonth\tplayers\tesrb\tfamitsu\tedge'];
  for (const r of rows) {
    if (!r.dev && !r.pub && !r.genre && !r.year && !r.esrb && !r.famitsu && !r.edge) continue;
    lines.push([r.sys, clean(r.title), [...r.serials].sort().join(','), r.dev, r.pub, r.genre, r.year, r.month, r.players, r.esrb, r.famitsu, r.edge].join('\t'));
  }
  fs.writeFileSync(out, lines.join('\n') + '\n');
  const per = {};
  for (const l of lines.slice(2)) per[l.split('\t')[0]] = (per[l.split('\t')[0]] || 0) + 1;
  console.log('wrote', out, fs.statSync(out).size, 'bytes', JSON.stringify(per));
})().catch(e => { console.error(e); process.exit(1); });
