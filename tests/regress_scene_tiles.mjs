// tests/regress_scene_tiles.mjs
//
// Pixel-regression check for the scene.tiles -> SceneRenderer pipeline.
//
//   1. decode the tile + special index atlases (Node zlib, no deps)
//   2. parse the scene.tiles layout.json
//   3. render with web/src/scene-render.js (the same code the browser uses)
//   4. decode the scene.build native PNG for the same scene
//   5. compare pixel-by-pixel and report the mismatch rate
//
// Usage: node tests/regress_scene_tiles.mjs <sceneId> [outPng]
//        (auto-discovers the newest artifact set under out/scenes)
//
// Expectation: the two images are produced by the same algorithm, so the
// mismatch rate should be ~0% (a few pixels only if an alpha/edge edge case
// differs).

import fs from 'fs';
import zlib from 'zlib';
import path from 'path';
import { fileURLToPath } from 'url';
import { renderScene } from '../web/src/scene-render.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '..');
const SCENES = path.join(ROOT, 'out', 'scenes');

// ---- minimal PNG reader/writer (truecolor+alpha, 8-bit) ----------------------
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
  let prev = Buffer.alloc(stride);
  let q = 0;
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
        const pa = Math.abs(bb - c), pb = Math.abs(a - c),
              pc = Math.abs(a + bb - 2 * c);
        const pr = pa <= pb && pa <= pc ? a : pb <= pc ? bb : c;
        v = (v + pr) & 255;
      }
      cur[i] = v;
    }
    cur.copy(out, y * stride); prev = cur;
  }
  return { width: w, height: h, ch, data: out };
}

function writePNG(p, img) {
  const w = img.width, h = img.height, data = img.data, ch = img.ch || 4;
  const ct = ch === 4 ? 6 : ch === 3 ? 2 : 0;
  const stride = w * ch;
  const raw = Buffer.alloc(h * (stride + 1));
  const src = Buffer.from(data.buffer, data.byteOffset, data.byteLength);
  for (let y = 0; y < h; ++y) {
    raw[y * (stride + 1)] = 0; // filter: none
    src.copy(raw, y * (stride + 1) + 1, y * stride, y * stride + stride);
  }
  const idat = zlib.deflateSync(raw);
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = ct; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  const chunk = (type, body) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(body.length, 0);
    const t = Buffer.from(type, 'ascii');
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(Buffer.concat([t, body])) >>> 0, 0);
    return Buffer.concat([len, t, body, crc]);
  };
  const png = Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk('IHDR', ihdr),
    chunk('IDAT', idat),
    chunk('IEND', Buffer.alloc(0)),
  ]);
  fs.writeFileSync(p, png);
}
function crc32(buf) {
  let c = ~0;
  for (let i = 0; i < buf.length; ++i) {
    c ^= buf[i];
    for (let k = 0; k < 8; ++k) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1));
  }
  return ~c;
}

// ---- discover newest artifact set for a scene id ----------------------------
function newest(re) {
  let best = null, bt = 0;
  for (const f of fs.readdirSync(SCENES)) {
    const m = f.match(re);
    if (!m) continue;
    const t = fs.statSync(path.join(SCENES, f)).mtimeMs;
    if (t > bt) { bt = t; best = f; }
  }
  return best ? path.join(SCENES, best) : null;
}

const sceneId = process.argv[2] || 'l1_rnd2';
const outPng = process.argv[3] || path.join(SCENES, sceneId + '__renderer.png');

const tilesResult = newest(new RegExp(`^${sceneId}_.*\\.tiles\\.result\\.json$`));
const tilesPng = newest(new RegExp(`^${sceneId}_.*_tiles\\.png$`));
const specialPng = newest(new RegExp(`^${sceneId}_.*_special\\.png$`));
// scene.build native PNG: <id>_<hash>.png but NOT the preview/overlay variants.
const buildPng = newest(new RegExp(`^${sceneId}_[0-9a-f]+\\.png$`));

console.log('tiles result :', tilesResult);
console.log('tiles png    :', tilesPng);
console.log('special png  :', specialPng || '(none)');
console.log('build png    :', buildPng);
if (!tilesResult || !tilesPng || !buildPng)
  throw new Error('could not locate artifacts for scene ' + sceneId);

const parsed = JSON.parse(fs.readFileSync(tilesResult, 'utf8'));
const layout = parsed.result ? parsed.result.layout : parsed.layout;
const tileAtlas = readPNG(tilesPng);
const specialAtlas = specialPng ? readPNG(specialPng) : null;

console.log(`tile atlas   : ${tileAtlas.width}x${tileAtlas.height} ch${tileAtlas.ch}`);
console.log(`special atlas: ${specialAtlas ? specialAtlas.width + 'x' + specialAtlas.height : 'none'}`);
console.log(`grid         : ${layout.grid_width}x${layout.grid_height}, micro_len ${layout.micro_len}`);

const rendered = renderScene(layout, tileAtlas, specialAtlas);
writePNG(outPng, rendered);
console.log('renderer out :', outPng, `${rendered.width}x${rendered.height}`);

const ref = readPNG(buildPng);
if (ref.width !== rendered.width || ref.height !== rendered.height) {
  console.error(`DIM MISMATCH: renderer ${rendered.width}x${rendered.height} vs build ${ref.width}x${ref.height}`);
  process.exit(2);
}

let diff = 0, total = 0;
const n = ref.width * ref.height * 4;
for (let i = 0; i < n; ++i) {
  if (ref.data[i] !== rendered.data[i]) diff++;
}
total = n;
const pct = (100 * (total - diff) / total).toFixed(4);
console.log(`PIXEL MATCH  : ${(total - diff)}/${total} (${pct}%)  mismatched bytes=${diff}`);
// Also report per-channel mismatch to distinguish "tiny shade drift" from "broken".
let cd = [0, 0, 0, 0];
for (let i = 0; i < n; i += 4)
  for (let c = 0; c < 4; ++c) if (ref.data[i + c] !== rendered.data[i + c]) cd[c]++;
console.log(`  channel mismatches R/G/B/A = ${cd[0]}/${cd[1]}/${cd[2]}/${cd[3]}`);

if (pct < 99.0) {
  console.error('REGRESSION: mismatch rate too high');
  process.exit(1);
}
console.log('OK: SceneRenderer reproduces scene.build output.');
