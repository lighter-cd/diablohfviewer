// web/src/scene-light.js
//
// Front-end port of decoders::buildLightMap (src/decoders/level_light.cpp), the
// pure-math DevilutionX DoLighting clone. It computes the per-cell dLight level
// (0 = bright .. 15 = pitch black) for a grid given an ambient floor and a set
// of point lights (torches). Ported verbatim so the front-end's lit render is
// byte-identical to the backend's scene.build / scene.tiles lit output — this is
// what makes the ambient slider and draggable torch update with ZERO server
// round-trips.
//
// Coordinate system: lights and the result grid use MICRO-cell coordinates, i.e.
// the same (i, j) as renderScene's micro_grid / projection. A light's
// offsetX/offsetY are sub-cell offsets in [0..7] (negative values are
// normalized by DoLighting exactly like the C++ original).
//
// Mirrors src/decoders/level_light.cpp. Do not "improve" the math — keep it a
// faithful transcription so regression stays at 100%.

const kNumLightRadiuses = 16;

const kRotationSines = [0, 1, 0, -1];

function rotateX(x, y, quadrants) {
  const q = ((quadrants % 4) + 4) % 4;
  const sine = kRotationSines[q];
  const cosine = kRotationSines[(q + 1) % 4];
  return x * cosine - y * sine;
}
function rotateY(x, y, quadrants) {
  const q = ((quadrants % 4) + 4) % 4;
  const sine = kRotationSines[q];
  const cosine = kRotationSines[(q + 1) % 4];
  return x * sine + y * cosine;
}

// LightFalloffs[radius][distance] -> darkness contribution (0..15).
function makeLightFalloffs(softFalloff) {
  const out = new Uint8Array(kNumLightRadiuses * 128);
  const maxDarkness = 15.0;
  for (let radius = 0; radius < kNumLightRadiuses; ++radius) {
    const maxDistance = (radius + 1) * 8;
    for (let distance = 0; distance < 128; ++distance) {
      let value;
      if (distance > maxDistance) {
        value = 15;
      } else {
        const factor = distance / maxDistance;
        let scaled;
        if (softFalloff) {
          const brightness = radius * 1.25;
          scaled = factor * factor * brightness + (maxDarkness - brightness);
          scaled = Math.max(0.0, scaled);
        } else {
          scaled = factor * maxDarkness;
        }
        scaled += 0.5; // round up
        value = scaled | 0;
      }
      out[radius * 128 + distance] = value;
    }
  }
  return out;
}

// Cone[offsetX][offsetY][x][y] -> linear (Euclidean) distance, precomputed.
function makeLightConeInterpolations() {
  const out = new Uint8Array(8 * 8 * 16 * 16);
  for (let offsetY = 0; offsetY < 8; ++offsetY) {
    for (let offsetX = 0; offsetX < 8; ++offsetX) {
      for (let y = 0; y < 16; ++y) {
        for (let x = 0; x < 16; ++x) {
          const a = 8 * x - offsetX;
          const b = 8 * y - offsetY;
          const d = Math.sqrt(a * a + b * b) | 0;
          out[(offsetX * 8 + offsetY) * 16 * 16 + y * 16 + x] = d;
        }
      }
    }
  }
  return out;
}

/**
 * Compute per-cell dLight levels.
 * @param {number} gridW
 * @param {number} gridH
 * @param {number} ambient         ambient floor (0..15)
 * @param {Array<{x:number,y:number,radius:number,offset_x:number,offset_y:number}>} lights
 * @param {boolean} softFalloff    true for light_style == 2 (Nest/Crypt)
 * @returns {Uint8Array} length gridW*gridH, row-major (y*gridW + x), values 0..15
 */
export function buildLightMap(gridW, gridH, ambient, lights, softFalloff) {
  const cells = gridW * gridH;
  const levels = new Uint8Array(cells);
  levels.fill(Math.min(15, ambient | 0));

  const falloffs = makeLightFalloffs(softFalloff);
  const cone = makeLightConeInterpolations();

  const get = (x, y) => levels[y * gridW + x];
  const set = (x, y, v) => { levels[y * gridW + x] = v; };
  const inBounds = (x, y) => x >= 0 && x < gridW && y >= 0 && y < gridH;

  for (const light of lights) {
    let px = light.x | 0;
    let py = light.y | 0;
    if (!inBounds(px, py)) continue;
    const radius = Math.max(0, Math.min(kNumLightRadiuses - 1, light.radius | 0));
    let offsetX = light.offset_x | 0;
    let offsetY = light.offset_y | 0;
    if (offsetX < 0) { offsetX += 8; px -= 1; }
    if (offsetY < 0) { offsetY += 8; py -= 1; }
    offsetX &= 7;
    offsetY &= 7;

    let minX = 15;
    if (px - 15 < 0) minX = px + 1;
    let maxX = 15;
    if (px + 15 > gridW) maxX = gridW - px;
    let minY = 15;
    if (py - 15 < 0) minY = py + 1;
    let maxY = 15;
    if (py + 15 > gridH) maxY = gridH - py;
    minX = Math.max(0, minX);
    minY = Math.max(0, minY);
    maxX = Math.min(16, maxX);
    maxY = Math.min(16, maxY);

    if (softFalloff) {
      if (inBounds(px, py) && get(px, py) > falloffs[radius * 128 + 0])
        set(px, py, falloffs[radius * 128 + 0]);
    } else if (inBounds(px, py)) {
      set(px, py, 0);
    }

    let distX = offsetX, distY = offsetY;
    let lightX = 0, lightY = 0;
    let blockX = 0, blockY = 0;
    let coneOffX = offsetX, coneOffY = offsetY;

    for (let i = 0; i < 4; ++i) {
      const yBound = (i > 0 && i < 3) ? maxY : minY;
      const xBound = (i < 2) ? maxX : minX;
      for (let y = 0; y < yBound; ++y) {
        for (let x = 1; x < xBound; ++x) {
          const linearDistance =
            cone[(coneOffX * 8 + coneOffY) * 16 * 16 + (y + blockY) * 16 + (x + blockX)];
          if (linearDistance >= 128) continue;
          const tx = px + rotateX(x, y, -i);
          const ty = py + rotateY(x, y, -i);
          if (!inBounds(tx, ty)) continue;
          const v = falloffs[radius * 128 + linearDistance];
          if (v < get(tx, ty)) set(tx, ty, v);
        }
      }

      // RotateRadius() — `offset` is reassigned each quarter-turn.
      const ndistX = 7 - distY;
      const ndistY = distX;
      const nlightX = 7 - lightY;
      const nlightY = lightX;
      distX = ndistX;
      distY = ndistY;
      lightX = nlightX;
      lightY = nlightY;
      let offX = distX - lightX;
      let offY = distY - lightY;
      blockX = 0;
      if (offX < 0) { offX += 8; blockX = 1; }
      blockY = 0;
      if (offY < 0) { offY += 8; blockY = 1; }
      coneOffX = offX;
      coneOffY = offY;
    }
  }

  return levels;
}
