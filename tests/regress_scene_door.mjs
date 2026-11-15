// tests/regress_scene_door.mjs
//
// Validates the front-end door local-toggle path (Phase 3) WITHOUT a browser:
//   1. render the scene base with no override (closed doors)
//   2. override a door's cell to its OPEN micro -> image must DIFFER (door opens)
//   3. override a door's cell to its CLOSED micro (== layout) -> image identical
//   4. when the door carries an open-state dSpecial frame (open_special), the
//      specialOverride must change the image too (l1s 7/8 cathedral, l2s 5/6)
//   5. when the door carries a DoorSet jamb patch (`neighbor`), applying it must
//      change the image (cathedral only)
//
// Usage: node tests/regress_scene_door.mjs <sceneId>
//        sceneId may be a fixed preset (l2_blind), a random Catacombs scene
//        (random_l5/l6/l7), or a random Cathedral scene (random_l1, produced by
//        scene.random -> <id>_default.layout.json). Picks the newest matching
//        layout and its same-stem atlas under out/scenes.

import fs from 'fs';
import zlib from 'zlib';
import path from 'path';
import { fileURLToPath } from 'url';
import { renderScene, renderSceneOverlay } from '../web/src/scene-render.js';

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
// scene.tiles writes <id>_<hash>.tiles.result.json; scene.random writes the
// fixed stem <id>_default.layout.json. Accept both, then prefer the atlas that
// shares the layout's stem so stale artifacts from earlier runs cannot mix.
const layoutFile = newest(new RegExp(
  `^${sceneId}_.*(?:\\.tiles\\.result\\.json|\\.layout\\.json)$`));
if (!layoutFile) {
  // Random scenes live only until scene.clearCache; regenerate them via
  // tests/regress_scene_random.mjs first, or just run this on a preset.
  console.log(`SKIP: no layout result for ${sceneId}`);
  process.exit(0);
}
const parsed = JSON.parse(fs.readFileSync(layoutFile, 'utf8'));
const L = parsed.result ? parsed.result.layout : parsed.layout;
if (!L.doors || !L.doors.length) {
  console.log(`scene ${sceneId} has no doors — nothing to test (OK, skip)`);
  process.exit(0);
}
const stem = path.basename(layoutFile)
  .replace(/(?:\.tiles\.result\.json|\.layout\.json)$/, '');
const preferSameStem = (suffix) => {
  const exact = path.join(SCENES, stem + suffix);
  if (fs.existsSync(exact)) return exact;
  return newest(new RegExp(`^${sceneId}_.*${suffix.replace('.', '\\.')}$`));
};
const tilesPng = preferSameStem('_tiles.png');
const layout = L;
const tileAtlas = readPNG(tilesPng);
const specialPng = preferSameStem('_special.png');
const specialAtlas = specialPng ? readPNG(specialPng) : null;

console.log(`scene=${sceneId} doors=${L.doors.length} tiles=${tileAtlas.width}x${tileAtlas.height}`);

const renderClosed = renderScene(layout, tileAtlas, specialAtlas); // no override
const door = L.doors[0];
const idx = door.y * layout.grid_width + door.x;
// Mirror the frontend toggleDoor: swap the leaf micro, draw the open-state
// dSpecial frame, and apply the DoorSet jamb patch when present.
const overrideOpen = { [idx]: door.open };     // swap to open doorway micro
const overrideClosed = { [idx]: door.closed }; // == layout micro (no-op expected)
const specialOpen = door.open_special ? { [idx]: door.open_special } : null;
if (door.neighbor && Number.isInteger(door.neighbor.x)) {
  const nIdx = door.neighbor.y * layout.grid_width + door.neighbor.x;
  overrideOpen[nIdx] = door.neighbor.open;
  overrideClosed[nIdx] = door.neighbor.closed;
}

const renderOpen = renderScene(layout, tileAtlas, specialAtlas, {
  microOverride: overrideOpen,
  specialOverride: specialOpen,
});
const renderOpenNoSpecial = renderScene(layout, tileAtlas, specialAtlas, {
  microOverride: overrideOpen,
});
const renderClosedAgain = renderScene(layout, tileAtlas, specialAtlas, {
  microOverride: overrideClosed,
  specialOverride: door.open_special ? { [idx]: door.closed_special || 0 } : null,
});

// The override path must draw EXACTLY what a layout that already carries the
// open micro + its SOL flags would draw. A closed Catacombs leaf (537/539) is
// SOL Solid|BlockLight|BlockMissile while the open doorway (12/16) is a floor
// piece, so without re-deriving the flags from `micro_flags` the floor pass
// skipped the cell and the opened door was not drawn at all (a hole instead).
const layoutOpen = JSON.parse(JSON.stringify(layout));
layoutOpen.micro_grid[idx] = door.open;
if (door.open_special) layoutOpen.special[idx] = door.open_special;
if (Array.isArray(layout.micro_flags)) {
  const f = layout.micro_flags[door.open];
  layoutOpen.solid[idx] = (f & 1) ? 1 : 0;
  layoutOpen.block_light[idx] = (f & 2) ? 1 : 0;
  layoutOpen.block_missile[idx] = (f & 4) ? 1 : 0;
  if (door.neighbor && Number.isInteger(door.neighbor.x)) {
    const nIdx = door.neighbor.y * layout.grid_width + door.neighbor.x;
    const nf = layout.micro_flags[door.neighbor.open];
    layoutOpen.micro_grid[nIdx] = door.neighbor.open;
    layoutOpen.solid[nIdx] = (nf & 1) ? 1 : 0;
    layoutOpen.block_light[nIdx] = (nf & 2) ? 1 : 0;
    layoutOpen.block_missile[nIdx] = (nf & 4) ? 1 : 0;
  }
}
const renderOpenLayout = renderScene(layoutOpen, tileAtlas, specialAtlas);

let fail = 0;
if (bytesEqual(renderClosed, renderOpen)) {
  console.error('FAIL: overriding door to OPEN produced an identical image (door did not change)');
  fail++;
} else {
  console.log('PASS: door OPEN override changes the rendered image');
}
if (!Array.isArray(layout.micro_flags)) {
  console.error('FAIL: layout lacks micro_flags (regenerate the scene: front-end door open/close needs it)');
  fail++;
} else if (!bytesEqual(renderOpen, renderOpenLayout)) {
  console.error('FAIL: door OPEN override != layout with the open micro (+SOL flags) — the override does not re-derive SOL flags');
  fail++;
} else {
  console.log('PASS: door OPEN override == open micro + re-derived SOL flags (doorway floor drawn)');
}
if (!bytesEqual(renderClosed, renderClosedAgain)) {
  console.error('FAIL: overriding door to its CLOSED micro (== layout) changed the image (no-op expected)');
  fail++;
} else {
  console.log('PASS: door CLOSED override == no-override image (no accidental shift)');
}
// The SOL tint overlay must follow the override too (open cell is no longer
// Solid, so its red tint has to disappear).
{
  const ovClosed = renderSceneOverlay(layout);
  const ovOpen = renderSceneOverlay(layout, { microOverride: overrideOpen });
  const ovOpenLayout = renderSceneOverlay(layoutOpen);
  const px = (door.x - door.y) * 32 - layout.projection.minX + 32;
  // Overlay diamond centre: slot (baseY-31) + 16, identical to renderScene's
  // slotTop(colTop(i,j),0) + 16.
  const py = (door.x + door.y) * 16 - layout.projection.minY - 15;
  const at = (img) => Array.from(img.data.subarray((py * img.width + px) * 4, (py * img.width + px) * 4 + 4));
  if (bytesEqual(ovClosed, ovOpen)) {
    console.error('FAIL: overlay did not change when the door opened (SOL tint ignored the micro override)');
    fail++;
  } else if (!bytesEqual(ovOpen, ovOpenLayout)) {
    console.error('FAIL: overlay override != layout-open overlay (SOL tint not re-derived from micro_flags)');
    fail++;
  } else {
    console.log(`PASS: overlay SOL tint follows the override (closed=${at(ovClosed)} open=${at(ovOpen)})`);
  }
}
if (door.open_special) {
  if (bytesEqual(renderOpen, renderOpenNoSpecial)) {
    console.error(`FAIL: open door dSpecial frame ${door.open_special} drew nothing (specialOverride ignored)`);
    fail++;
  } else {
    console.log(`PASS: open door draws its dSpecial frame ${door.open_special}`);
  }
} else {
  console.log('INFO: door has no open-state dSpecial frame (pure micro swap)');
}
if (door.neighbor && Number.isInteger(door.neighbor.x)) {
  // Same open state WITHOUT the jamb patch — isolates the neighbour rewrite.
  const renderNoJamb = renderScene(layout, tileAtlas, specialAtlas, {
    microOverride: { [idx]: door.open },
    specialOverride: specialOpen,
  });
  if (bytesEqual(renderOpen, renderNoJamb)) {
    console.error('FAIL: DoorSet jamb patch changed nothing (neighbor override ignored)');
    fail++;
  } else {
    console.log(`PASS: DoorSet jamb patch rewrites neighbour (${door.neighbor.closed} -> ${door.neighbor.open})`);
  }
} else {
  console.log('INFO: door has no jamb patch (Catacombs doors are single-cell swaps)');
}

console.log(fail ? 'DOOR REGRESSION FAILED' : 'OK: door local-toggle override path works');
process.exit(fail ? 1 : 0);
