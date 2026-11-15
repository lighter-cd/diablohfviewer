// tests/regress_scene_random.mjs
//
// End-to-end regression for Phase 5/P6 random dungeon maps (scene.random)
// WITHOUT a browser. Drives the mcp-server WS bridge + its /scenes/ HTTP endpoint:
//   1. scene.list exposes 11 randomizable levels, one per dlvl that carries a
//      vanilla fixed room (plus L1 baseline): random_l1=L1 Cathedral,
//      random_l2=L2 (Butcher), random_l3=L3 (Skeleton King), random_l4=L4
//      (Ogden banner), random_l5/6/7=L5/6/7 Catacombs (Blood/Chamber/Blind),
//      random_l10=L10 Caves (Anvil), random_l13/15/16=L13/15/16 Hell
//      (Warlord/Betrayer/Diablo); all 80x80 (Hell micro_len=12)
//   2. scene.random generate -> full scene.tiles-shaped result; atlas PNG valid;
//      renderScene(layout, atlas, special) draws a real map (non-bg pixels)
//   3. determinism (same seed -> identical micro_grid), different seed differs
//   4. default cache overwritten by regenerate; a saved snapshot survives it
//   5. save/list/switch load; scene.clearCache deletes all default + saved
//
// Usage: node tests/regress_scene_random.mjs [wsUrl] [mpqPath]
//   wsUrl   default ws://127.0.0.1:18765
//   mpqPath default: discovered from a small list of known fixture locations
//
// If the WS server (or an MPQ with the L1 Cathedral assets) is unavailable the
// test prints SKIP and exits 0 (it never fails an offline build).

import fs from 'fs';
import path from 'path';
import zlib from 'zlib';
import { fileURLToPath } from 'url';
import { renderScene, renderSceneOverlay } from '../web/src/scene-render.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '..');
const WS_URL = process.argv[2] || 'ws://127.0.0.1:18765';
const ORIGIN = WS_URL.replace(/^ws/, 'http').replace(/^wss/, 'https')
  .replace(/\/+$/, '');

const MPQ_CANDIDATES = [
  process.argv[3] || '',
  'D:/work/xt/diablo/DevilutionX-Hell Fire/DIABDAT.MPQ',
  'D:/fixtures/DIABDAT.MPQ',
  path.join(ROOT, 'out', 'DIABDAT.MPQ'),
].filter(Boolean);

function findMpq() {
  for (const p of MPQ_CANDIDATES) {
    try { if (fs.statSync(p).isFile()) return p; } catch { /* not there */ }
  }
  return null;
}

// ---- minimal PNG decoder (truecolor+alpha, 8-bit) ----------------------------
function readPNG(bytes) {
  const b = Buffer.from(bytes);
  if (b.readUInt32BE(0) !== 0x89504e47) throw new Error('not a PNG');
  const w = b.readUInt32BE(16), h = b.readUInt32BE(20), ct = b[25];
  let off = 8; const idat = [];
  while (off < b.length) {
    const len = b.readUInt32BE(off);
    const type = b.toString('ascii', off + 4, off + 8);
    if (type === 'IDAT') idat.push(b.subarray(off + 8, off + 8 + len));
    if (type === 'IEND') break;
    off += 12 + len;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const ch = ct === 6 ? 4 : ct === 2 ? 3 : 1;
  const stride = w * ch;
  const out = Buffer.alloc(h * stride);
  let prev = Buffer.alloc(stride), q = 0;
  for (let y = 0; y < h; ++y) {
    const f = raw[q++];
    const line = raw.subarray(q, q + stride); q += stride;
    const cur = Buffer.alloc(stride);
    for (let i = 0; i < stride; ++i) {
      const a = i >= ch ? cur[i - ch] : 0, bb = prev[i], c = i >= ch ? prev[i - ch] : 0;
      let v = line[i];
      if (f === 1) v = (v + a) & 255;
      else if (f === 2) v = (v + bb) & 255;
      else if (f === 3) v = (v + ((a + bb) >> 1)) & 255;
      else if (f === 4) {
        const pa = Math.abs(bb - c), pb = Math.abs(a - c), pc = Math.abs(a + bb - 2 * c);
        const pr = pa <= pb && pa <= pc ? a : pb <= pc ? bb : c;
        v = (v + pr) & 255;
      }
      cur[i] = v;
    }
    cur.copy(out, y * stride); prev = cur;
  }
  return { width: w, height: h, data: new Uint8ClampedArray(out.buffer, out.byteOffset, out.byteLength) };
}

let ws = null;
let nextId = 1;
function call(name, args) {
  return new Promise((resolve, reject) => {
    const id = nextId++;
    const timer = setTimeout(() => {
      ws.removeEventListener('message', onMsg);
      reject(new Error('timeout waiting for ' + name));
    }, 120000);
    function onMsg(ev) {
      let m; try { m = JSON.parse(ev.data); } catch { return; }
      if (m.id !== id) return;
      clearTimeout(timer);
      ws.removeEventListener('message', onMsg);
      if (m.error) reject(new Error(JSON.stringify(m.error)));
      else resolve(m.result);
    }
    ws.addEventListener('message', onMsg);
    ws.send(JSON.stringify({ jsonrpc: '2.0', id, method: 'tools/call',
      params: { name, arguments: args } }));
  });
}

async function fetchPNG(url) {
  const resp = await fetch(ORIGIN + url);
  if (!resp.ok) throw new Error('HTTP ' + resp.status + ' for ' + url);
  const buf = await resp.arrayBuffer();
  const png = readPNG(buf);
  if (png.width <= 0 || png.height <= 0) throw new Error('empty atlas PNG ' + url);
  return png;
}

function buffersEqual(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; ++i) if (a[i] !== b[i]) return false;
  return true;
}

let checks = 0;
function ok(cond, msg) {
  checks++;
  if (!cond) throw new Error('FAIL: ' + msg);
  console.log('  PASS:', msg);
}

async function main() {
  const mpq = findMpq();
  if (!mpq) {
    console.log('SKIP: no MPQ fixture found (pass mpqPath as 2nd arg).');
    return;
  }
  // Node 24 has a global WebSocket; skip if the server isn't running.
  if (typeof WebSocket === 'undefined') {
    console.log('SKIP: global WebSocket unavailable (need Node >= 22).');
    return;
  }
  try {
    ws = new WebSocket(WS_URL);
    await new Promise((res, rej) => {
      const t = setTimeout(() => rej(new Error('connect timeout')), 4000);
      ws.addEventListener('open', () => { clearTimeout(t); res(); }, { once: true });
      ws.addEventListener('error', () => { clearTimeout(t); rej(new Error('connect error')); }, { once: true });
    });
  } catch (e) {
    console.log('SKIP: WS server not reachable at ' + WS_URL + ' (' + e.message + ').');
    if (ws) { try { ws.close(); } catch {} }
    return;
  }

  try {
    const list = await call('scene.list', {});
    const rand = (list.scenes || []).filter((s) => s.randomizable);
    ok(rand.length === 11, 'scene.list exposes 11 randomizable dlvls');

    // id -> exact dlvl, generator/tileset, vanilla fixed-room i18n key.
    // Suffix is the game dlvl; fixed_room is surfaced by the UI hint.
    const expected = {
      random_l1:  { level: 1,  type: 'Cathedral', room: '' },
      random_l2:  { level: 2,  type: 'Cathedral', room: 'butcher' },
      random_l3:  { level: 3,  type: 'Cathedral', room: 'skelking' },
      random_l4:  { level: 4,  type: 'Cathedral', room: 'banner' },
      random_l5:  { level: 5,  type: 'Catacombs', room: 'blood' },
      random_l6:  { level: 6,  type: 'Catacombs', room: 'schamb' },
      random_l7:  { level: 7,  type: 'Catacombs', room: 'blind' },
      random_l10: { level: 10, type: 'Caves',     room: 'anvil' },
      random_l13: { level: 13, type: 'Hell',      room: 'warlord' },
      random_l15: { level: 15, type: 'Hell',      room: 'betrayer' },
      random_l16: { level: 16, type: 'Hell',      room: 'diablo' },
    };
    ok(rand.every((s) => {
      const e = expected[s.id];
      return e && s.random_level === e.level && s.fixed_room === e.room &&
             s.grid_width === 80 && s.grid_height === 80 &&
             s.micro_len === (e.type === 'Hell' ? 12 : 10);
    }), 'randomizable dlvls L1/2/3/4/5/6/7/10/13/15/16 carry correct fixed_room @ 80x80');

    // Generate + render every fixed-room dlvl; each must produce a real map
    // with the matching generator and tileset (regression against all-Cathedral).
    const catacombIds = ['random_l5', 'random_l6', 'random_l7'];
    let overrideChecked = false;
    for (const id of Object.keys(expected)) {
      const e = expected[id];
      const g = await call('scene.random',
        { action: 'generate', id, seed: 424242, archive: mpq });
      ok(g.random === true && g.random_level === e.level &&
         g.fixed_room === e.room && g.dungeon_type === e.type,
         id + ' generates via ' + e.type + ' (L' + e.level +
         ', room=' + (e.room || 'none') + ')');
      ok(g.grid_width === 80 && g.grid_height === 80 &&
         g.layout.micro_grid.length === 6400 && g.frame_count > 0,
         id + ' emits a full 80x80 micro_grid + atlas');
      const atlas = await fetchPNG(g.tiles_url);
      const special = g.special_url ? await fetchPNG(g.special_url) : null;
      const rendered = renderScene(g.layout, atlas, special);
      const bg = g.layout.background || [0, 0, 0, 255];
      let drawn = 0;
      for (let i = 0; i < rendered.data.length; i += 4) {
        if (rendered.data[i] !== bg[0] || rendered.data[i + 1] !== bg[1] ||
            rendered.data[i + 2] !== bg[2]) drawn++;
      }
      ok(drawn > 10000, id + ' renders substantial non-bg art (' + drawn + ' px)');
      // AddL2Objs -> SetDoorStateClosed on Catacombs (L5/L6/L7) leaves closed
      // leaf micros (537/539). random_l2 is now Cathedral/Butcher, so the door
      // checks belong to the Catacombs ids, not random_l2.
      if (catacombIds.includes(id)) {
        const doors = g.layout.doors || [];
        ok(doors.length > 0 &&
           doors.every((d) => d.closed === 537 || d.closed === 539),
           id + ' marks L2 doorways with closed leaves (537/539)');
        // Front-end local toggle (same path as regress_scene_door.mjs); one
        // Catacombs scene is enough to cover the shared microOverride path.
        if (!overrideChecked && doors.length > 0) {
          const d = doors[0];
          const idx = d.y * g.layout.grid_width + d.x;
          const opened = renderScene(g.layout, atlas, special,
            { microOverride: { [idx]: d.open } });
          const closedAgain = renderScene(g.layout, atlas, special,
            { microOverride: { [idx]: d.closed } });
          ok(!buffersEqual(rendered.data, opened.data),
             id + ' OPEN door override changes the rendered image');
          ok(buffersEqual(rendered.data, closedAgain.data),
             id + ' CLOSED door override == no-op');
          overrideChecked = true;
        }
      }
    }

    const gen = await call('scene.random',
      { action: 'generate', id: 'random_l1', seed: 20261114, archive: mpq });
    ok(gen.random === true && gen.random_level === 1 && gen.seed === 20261114,
       'generate echoes random / level / seed');
    ok(gen.result_id === 'random_l1_default' && gen.is_default === true &&
       !!gen.layout_url,
       'generate writes the default cache + layout_url');
    ok(gen.grid_width === 80 && gen.grid_height === 80 &&
       gen.layout.micro_grid.length === 6400,
       'generate produces a full 80x80 micro_grid (6400 cells)');
    ok(!!gen.tiles_url && gen.frame_count > 0, 'generate emits an index atlas');

    // Atlas PNG is valid and the front-end SceneRenderer draws a real map.
    const tileAtlas = await fetchPNG(gen.tiles_url);
    const specialAtlas = gen.special_url ? await fetchPNG(gen.special_url) : null;
    const out = renderScene(gen.layout, tileAtlas, specialAtlas);
    const ov = renderSceneOverlay(gen.layout);
    ok(out.width > 0 && out.height > 0 &&
       ov.width === out.width && ov.height === out.height,
       'renderScene + overlay produce aligned native-res buffers');
    const bg = gen.layout.background || [0, 0, 0, 255];
    let drawn = 0;
    for (let i = 0; i < out.data.length; i += 4) {
      if (out.data[i] !== bg[0] || out.data[i + 1] !== bg[1] || out.data[i + 2] !== bg[2]) drawn++;
    }
    ok(drawn > 10000, 'rendered random dungeon has substantial non-bg art (' + drawn + ' px)');

    // Determinism + different seeds.
    const gen2 = await call('scene.random',
      { action: 'generate', id: 'random_l1', seed: 20261114, archive: mpq });
    ok(JSON.stringify(gen.layout.micro_grid) === JSON.stringify(gen2.layout.micro_grid),
       'same seed -> identical micro_grid (deterministic)');
    const gen3 = await call('scene.random',
      { action: 'generate', id: 'random_l1', seed: 777, archive: mpq });
    ok(JSON.stringify(gen2.layout.micro_grid) !== JSON.stringify(gen3.layout.micro_grid),
       'different seed -> different micro_grid');

    // The persisted layout.json is the same object the front-end consumes.
    const lj = await (await fetch(ORIGIN + gen3.layout_url)).json();
    ok(lj.seed === 777 && lj.result_id === 'random_l1_default' &&
       JSON.stringify(lj.layout.micro_grid) === JSON.stringify(gen3.layout.micro_grid),
       'default .layout.json matches the inline result');

    // save -> named snapshot; regenerate overwrites default but keeps snapshot.
    const sv = await call('scene.random', { action: 'save', id: 'random_l1' });
    ok(!!sv.saved && sv.count >= 2, 'save creates a named snapshot');
    const savedEntry = (sv.results || []).find((r) => r.result_id === sv.saved);
    ok(savedEntry && savedEntry.is_default === false && !!savedEntry.layout_url,
       'saved snapshot is non-default and loadable');
    const sj = await (await fetch(ORIGIN + savedEntry.layout_url)).json();
    ok(sj.seed === 777 && sj.result_id === sv.saved &&
       !!sj.tiles_url,
       'saved .layout.json is a self-contained scene.tiles result');

    await call('scene.random',
      { action: 'generate', id: 'random_l1', seed: 999, archive: mpq });
    const lst = await call('scene.random', { action: 'list', id: 'random_l1' });
    ok(lst.default === 'random_l1_default' &&
       (lst.results || []).some((r) => r.result_id === sv.saved && !r.is_default),
       'regenerate overwrites default but keeps the saved snapshot');

    // Switching to the snapshot still renders (atlas copied, URLs rewritten).
    const savedAtlas = await fetchPNG(sj.tiles_url);
    const savedSpecial = sj.special_url ? await fetchPNG(sj.special_url) : null;
    const savedRender = renderScene(sj.layout, savedAtlas, savedSpecial);
    ok(savedRender.width > 0 && savedRender.data.length > 0,
       'saved snapshot re-renders via the front-end SceneRenderer');

    // clearCache deletes all default + saved results. NOTE: scene.clearCache
    // wipes the whole out/scenes dir, which also holds the FIXED-scene tiles /
    // overlay fixtures that the offline regress_scene_tiles/door/overlay.mjs
    // tests read. Re-warm those afterwards so the suite stays order-independent.
    await call('scene.clearCache', { out: 'out/scenes' });
    const empty = await call('scene.random', { action: 'list', id: 'random_l1' });
    ok(empty.count === 0 && !empty.default,
       'clearCache deletes all default + saved results');
    const fixed = ['tristram', 'l1_rnd2', 'l1_skngdo', 'l2_blind', 'l3_anvil', 'l4_diab1'];
    for (const id of fixed) {
      const base = { id, archive: mpq, preview_max_width: 1000 };
      await call('scene.tiles', { ...base, lighting: false });
      await call('scene.build', { ...base, lighting: false });
    }

    console.log(`OK: scene.random end-to-end passed (${checks} checks) [fixed caches re-warmed]`);
  } finally {
    try { ws.close(); } catch {}
  }
}

main().catch((e) => { console.error('REGRESSION:', e.message || e); process.exit(1); });
