// tests/regress_scene_door.mjs
//
// Validates the front-end door local-toggle path (Phase 3) WITHOUT a browser:
//   1. render the scene base with no override (closed doors)
//   2. override a door's cell to its OPEN micro -> image must DIFFER (door opens)
//   3. override a door's cell to its CLOSED micro (== layout) -> image identical
//
// Usage: node tests/regress_scene_door.mjs <sceneId>
//        (auto-discovers the newest tile artifact set under out/scenes)

import fs from 'fs';
import zlib from 'zlib';
import path from 'path';
import { fileURLToPath } from 'url';
import { renderScene } from '../web/src/scene-render.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '..');
const SCENES = path.join(ROOT, 'out', 'scenes');

function readPNG(p) {
  const b = fs.readFileSync(p);
  if (b.readUInt32BE(0) !== 0x89504e47) throw new Error('not a PNG: ' + p);
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
  let prev = Buffer.alloc(stride); let q = 0;
  for (let y = 0; y < h; ++y) {
    const f = raw[q++];
    const line = raw.subarray(q, q + stride); q += stride;
    const cur = Buffer.alloc(stride);
    for (let i = 0; i < stride; ++i) {
      const a = i >= ch ? cur[i - ch] : 0;
      const bb = prev[i];
      const c = i >= ch ? prev[i - ch] : 0;
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
  return { width: w, height: h, ch, data: out };
}

function newest(re) {
  let best = null, bt = 0;
  for (const f of fs.readdirSync(SCENES)) {
    if (!re.test(f)) continue;
    const t = fs.statSync(path.join(SCENES, f)).mtimeMs;
    if (t > bt) { bt = t; best = f; }
  }
  return best ? path.join(SCENES, best) : null;
}

function bytesEqual(a, b) {
  if (a.data.length !== b.data.length) return false;
  for (let i = 0; i < a.data.length; ++i) if (a.data[i] !== b.data[i]) return false;
  return true;
}

const sceneId = process.argv[2] || 'l2_blind';
const tilesResult = newest(new RegExp(`^${sceneId}_.*\\.tiles\\.result\\.json$`));
if (!tilesResult) throw new Error('no tiles result for ' + sceneId);
const parsed = JSON.parse(fs.readFileSync(tilesResult, 'utf8'));
const L = parsed.result ? parsed.result.layout : parsed.layout;
if (!L.doors || !L.doors.length) {
  console.log(`scene ${sceneId} has no doors — nothing to test (OK, skip)`);
  process.exit(0);
}
const tilesPng = newest(new RegExp(`^${sceneId}_.*_tiles\\.png$`));
const layout = L;
const tileAtlas = readPNG(tilesPng);
const specialPng = newest(new RegExp(`^${sceneId}_.*_special\\.png$`));
const specialAtlas = specialPng ? readPNG(specialPng) : null;

console.log(`scene=${sceneId} doors=${L.doors.length} tiles=${tileAtlas.width}x${tileAtlas.height}`);

const renderClosed = renderScene(layout, tileAtlas, specialAtlas); // no override
const door = L.doors[0];
const idx = door.y * layout.grid_width + door.x;
const overrideOpen = { [idx]: door.open };     // swap to open doorway micro
const overrideClosed = { [idx]: door.closed }; // == layout micro (no-op expected)

const renderOpen = renderScene(layout, tileAtlas, specialAtlas, { microOverride: overrideOpen });
const renderClosedAgain = renderScene(layout, tileAtlas, specialAtlas, { microOverride: overrideClosed });

let fail = 0;
if (bytesEqual(renderClosed, renderOpen)) {
  console.error('FAIL: overriding door to OPEN produced an identical image (door did not change)');
  fail++;
} else {
  console.log('PASS: door OPEN override changes the rendered image');
}
if (!bytesEqual(renderClosed, renderClosedAgain)) {
  console.error('FAIL: overriding door to its CLOSED micro (== layout) changed the image (no-op expected)');
  fail++;
} else {
  console.log('PASS: door CLOSED override == no-override image (no accidental shift)');
}

console.log(fail ? 'DOOR REGRESSION FAILED' : 'OK: door local-toggle override path works');
process.exit(fail ? 1 : 0);
