// tests/regress_scene_light.mjs
//
// Regression for the front-end DoLighting port (web/src/scene-light.js).
//
// DevilutionX's DoLighting only works correctly when the light sits >=15 cells
// from every grid edge (its dPiece grid has a 16-cell border, so the min/max
// quadrant-reuse branches are effectively dead code there). The viewer renders
// cropped fixed rooms (12x12..22x22) and 80x80 random maps where the torch can
// sit right at an edge, so each rotated quadrant must be clipped by the *real*
// distance to the edge it extends toward. The old reuse clipped the wrong
// quadrant and left wedge-shaped pitch-black holes inside the light radius.
//
// Two assertions per grid size / light position:
//   1. no level-15 (pitch-black) cell within Euclidean distance 12 of the light
//   2. bit-exact match against a padded interior light cropped to the grid
//      (the ground truth: a light with >=15 cells of margin on every side)
//
// Usage: node tests/regress_scene_light.mjs   (no MPQ / no server needed)

import { buildLightMap } from '../web/src/scene-light.js';

let passed = 0, failed = 0;
function ok(cond, msg) {
  if (cond) { passed++; }
  else { failed++; console.error('FAIL: ' + msg); }
}

// Ground truth: run the same function on a padded grid with the light well
// inside, then crop back to the target grid. With >=15 cells of margin the
// quadrant clipping cannot bite, so this is the un-clipped reference cone.
function interiorTruth(gw, gh, px, py, ox, oy, soft) {
  const PAD = 20;
  const bw = gw + PAD * 2, bh = gh + PAD * 2;
  const big = buildLightMap(bw, bh, 15,
    [{ x: PAD + px, y: PAD + py, radius: 15, offset_x: ox, offset_y: oy }], soft);
  const out = new Uint8Array(gw * gh);
  for (let y = 0; y < gh; ++y)
    for (let x = 0; x < gw; ++x)
      out[y * gw + x] = big[(y + PAD) * bw + (x + PAD)];
  return out;
}

const GRIDS = [[12, 12], [18, 18], [22, 22], [40, 40], [80, 80]];
for (const [gw, gh] of GRIDS) {
  const positions = [
    [gw / 2 | 0, gh / 2 | 0, 0, 0],                    // centre, no offset
    [gw / 4 | 0, gh / 4 | 0, 0, 0],                    // upper-left quarter
    [gw / 4 | 0, gh / 4 | 0, 5, 5],                    // quarter + subcell offset
    [gw - 1 - (gw / 4 | 0), gh - 1 - (gh / 4 | 0), 3, 2], // lower-right quarter
    [1, 1, 0, 0],                                       // top-left corner
    [gw - 2, gh - 2, 7, 6],                             // bottom-right corner
  ];
  for (const [px, py, ox, oy] of positions) {
    const m = buildLightMap(gw, gh, 15,
      [{ x: px, y: py, radius: 15, offset_x: ox, offset_y: oy }], false);

    // 1. no pitch-black holes within r=12 of the source
    let wrongBlack = 0;
    for (let y = 0; y < gh; ++y)
      for (let x = 0; x < gw; ++x)
        if (m[y * gw + x] >= 15 && Math.hypot(x - px, y - py) <= 12) wrongBlack++;
    ok(wrongBlack === 0,
      `${gw}x${gh} light(${px},${py}+${ox},${oy}): ${wrongBlack} black cells within r12`);

    // 2. exact match with the padded interior reference
    const truth = interiorTruth(gw, gh, px, py, ox, oy, false);
    let diff = 0;
    for (let k = 0; k < m.length; ++k) if (m[k] !== truth[k]) diff++;
    ok(diff === 0,
      `${gw}x${gh} light(${px},${py}+${ox},${oy}): ${diff} cells differ from interior truth`);
  }
}

// Keep the pre-existing invariants too (ambient fill, centre lit, falloff).
const amb = buildLightMap(5, 4, 15, [], false);
ok(amb.length === 20 && amb.every((v) => v === 15), 'ambient-only grid is all level 15');
const lit = buildLightMap(5, 5, 15, [{ x: 2, y: 2, radius: 15, offset_x: 0, offset_y: 0 }], false);
ok(lit[2 * 5 + 2] === 0, 'light centre is level 0');
ok(lit[0] > lit[2 * 5 + 1], 'light falls off with distance');

console.log(`scene.light: ${failed ? 'FAIL' : 'PASS'} (${passed} passed, ${failed} failed)`);
process.exit(failed ? 1 : 0);
