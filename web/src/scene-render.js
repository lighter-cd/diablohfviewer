// web/src/scene-render.js
//
// Front-end isometric scene renderer for the index atlas produced by the backend
// `scene.tiles` tool. It replicates decoders::renderMap (level_render.cpp) in
// pure JS so the map can be assembled in the browser: no CEL decoding, no PNG
// compositing on the server. The atlas stores PALETTE INDICES (R channel) with
// A=255 for drawn pixels / A=0 for transparent, so per-cell dLight shading is
// applied locally from the shipped `light_tables`.
//
// The module is environment-agnostic: it operates on plain pixel buffers
// ({ width, height, data: Uint8ClampedArray }) so it runs identically in a
// browser (ImageData from a decoded <img>) and in Node (regression tests).

export const TILE = 32;

// Isometric bounding box (mirrors decoders::sceneProjection).
export function sceneProjection(gw, gh, microLen) {
  const Hm = (microLen / 2) * 32;
  const minX = -(gh - 1) * 32;
  const minY = -Hm + 1;
  const maxX = (gw - 1) * 32 + 64;
  const maxY = (gw + gh - 2) * 16;
  return { minX, minY, width: maxX - minX + 1, height: maxY - minY + 1 };
}

// Read an axis-aligned region of an atlas pixel buffer into a fresh buffer.
// `rect` may be [x, y] (assumed TILE x TILE) or [x, y, w, h].
function readRegion(atlas, rect) {
  const [x, y, w = TILE, h = TILE] = rect;
  const out = new Uint8ClampedArray(w * h * 4);
  const srcPitch = atlas.width * 4;
  const dstPitch = w * 4;
  for (let row = 0; row < h; ++row) {
    let so = ((y + row) * atlas.width + x) * 4;
    let doff = row * dstPitch;
    for (let i = 0; i < dstPitch; ++i) out[doff + i] = atlas.data[so + i];
  }
  return { width: w, height: h, data: out };
}

// Blit a 32x32 (or w x h) index frame into `dst` at (dx, dy). Each drawn source
// pixel (A != 0) is shaded through `shade(index)` -> palette rgb, then written.
function blitFrame(dst, dstW, dstH, frame, dx, dy, shade, palette) {
  const fw = frame.width, fh = frame.height;
  const fd = frame.data;
  for (let fy = 0; fy < fh; ++fy) {
    const dr = dy + fy;
    if (dr < 0 || dr >= dstH) continue;
    for (let fx = 0; fx < fw; ++fx) {
      const px = dx + fx;
      if (px < 0 || px >= dstW) continue;
      const si = (fy * fw + fx) * 4;
      if (fd[si + 3] === 0) continue; // transparent
      const idx = shade(fd[si]); // -> shaded palette index
      const di = (dr * dstW + px) * 4;
      const pi = idx * 4;
      dst[di] = palette[pi];
      dst[di + 1] = palette[pi + 1];
      dst[di + 2] = palette[pi + 2];
      dst[di + 3] = 255;
    }
  }
}

// Render the whole scene. `tileAtlas`/`specialAtlas` are {width,height,data}
// with R=palette index, A=drawn/0. `layout` is the scene.tiles layout.json.
// Returns { width, height, data } RGBA.
export function renderScene(layout, tileAtlas, specialAtlas, opts = {}) {
  const {
    micro_len,
    grid_width: gw,
    grid_height: gh,
    projection,
    micro_grid,
    atlas,
    palette,
    light_tables,
    prefill_micro,
    solid,
    block_missile,
    special,
    special_atlas,
  } = layout;

  const nRows = micro_len / 2;
  const Hm = nRows * 32;
  const W = projection.width;
  const H = projection.height;
  const out = new Uint8ClampedArray(W * H * 4);

  // Phase 3 (door local toggle): optionally override a cell's micro tile so the
  // front-end can swap a closed door leaf for the open doorway (or any other
  // local edit) with zero server round-trip. `microOverride` is a Map or an
  // indexable { idx -> microId }. Cells absent from it keep their layout micro.
  const microOverride = opts.microOverride || null;
  const effMicro = (idx) => {
    if (microOverride) {
      const v = (microOverride instanceof Map) ? microOverride.get(idx) : microOverride[idx];
      if (v !== undefined && v !== null) return v;
    }
    return micro_grid[idx];
  };

  // Same override mechanism for the dSpecial overlay id: an open door swaps its
  // leaf micro for the doorway hole AND draws the special-CEL door frame
  // (l1s.cel 7/8 cathedral, l2s.cel 5/6 catacombs). Cells absent from the
  // override keep their layout special id.
  const specialOverride = opts.specialOverride || null;
  const effSpecial = (idx) => {
    if (specialOverride) {
      const v = (specialOverride instanceof Map) ? specialOverride.get(idx) : specialOverride[idx];
      if (v !== undefined && v !== null) return v;
    }
    return special[idx];
  };

  // Micro overrides change a cell's micro, so its SOL flags (solid /
  // blockLight / blockMissile) MUST be re-derived from the NEW micro. They are
  // baked per cell from the generated grid, and a closed Catacombs door leaf
  // (micro 537/539, SOL Solid|BlockLight|BlockMissile) would otherwise keep
  // flagging the OPENED doorway (micro 12/16, a floor piece) as solid: the floor
  // pass would skip the cell entirely and the open door would not be drawn at
  // all (just a hole in the ground). `micro_flags` is the shipped per-micro SOL
  // byte; -1 means "no override, use the layout's per-cell arrays".
  const microFlags = layout.micro_flags || null;
  const overrideSol = (idx) => {
    if (!microOverride || !microFlags) return -1;
    const mi = resolveMi(effMicro(idx));
    if (typeof mi !== "number" || mi === 65535 || mi < 0 || mi >= microFlags.length) return -1;
    return microFlags[mi] | 0;
  };
  const isSolidAt = (idx) => {
    const f = overrideSol(idx);
    return f < 0 ? solid[idx] === 1 : (f & 1) !== 0;
  };
  const blocksMissileAt = (idx) => {
    const f = overrideSol(idx);
    return f < 0 ? block_missile[idx] === 1 : (f & 4) !== 0;
  };

  // Opaque background (matches renderMap filling every pixel, no black holes).
  const bg = layout.background || [0, 0, 0, 255];
  for (let i = 0; i < out.length; i += 4) {
    out[i] = bg[0]; out[i + 1] = bg[1]; out[i + 2] = bg[2]; out[i + 3] = bg[3];
  }

  // Lighting: with a precomputed `light` array we shade via LightTables; without
  // it (FullyLit) we use the plain palette, exactly like scene.build with
  // lighting=false. An explicit opts.light (e.g. recomputed by the front-end's
  // ambient slider / draggable torch) takes priority over the shipped one.
  const light = opts.light ? opts.light
              : (layout.lighting && layout.light ? layout.light : null);
  const shadeFor = (idx, cellIdx) => {
    if (!light) return idx; // FullyLit: identity through the palette
    const level = light[cellIdx] & 0x0f;
    return light_tables[level][idx];
  };

  const frames = atlas.frames;
  const frameCache = new Map();
  const getFrame = (key) => {
    if (frameCache.has(key)) return frameCache.get(key);
    const rect = frames[key];
    if (!rect) { frameCache.set(key, null); return null; }
    const f = readRegion(tileAtlas, rect);
    frameCache.set(key, f);
    return f;
  };

  const cellX = (i, j) => (i - j) * 32 - projection.minX;
  const colTop = (i, j) => (i + j) * 16 - projection.minY - Hm + 1;
  const slotTop = (ct, b) => ct + (Hm - (b + 1) * 32);

  const isFloor = (idx) => !isSolidAt(idx) && !blocksMissileAt(idx);
  const resolveMi = (mi) =>
    (mi === 65535 || mi >= 1e9) ? prefill_micro : mi; // kNoMicro sentinel

  const maxSum = (gw - 1) + (gh - 1);

  // ---- Pass 1: floor diamonds for every floor cell (complete ground plane) --
  for (let s = 0; s <= maxSum; ++s) {
    for (let i = 0; i < gw; ++i) {
      const j = s - i;
      if (j < 0 || j >= gh) continue;
      const idx = j * gw + i;
      if (!isFloor(idx)) continue;
      const mi = resolveMi(effMicro(idx));
      const x = cellX(i, j);
      const slot = slotTop(colTop(i, j), 0);
      // Forced Left/Right triangle (with the TransparentSquare exception already
      // baked into the atlas frame by scene.tiles). Fall back to the prefill
      // floor micro when the cell's own block is missing (renderMap behaviour).
      let fl = getFrame(mi + ":0:L") || getFrame(prefill_micro + ":0:L");
      let fr = getFrame(mi + ":1:R") || getFrame(prefill_micro + ":1:R");
      if (fl) blitFrame(out, W, H, fl, x, slot, (k) => shadeFor(k, idx), palette);
      if (fr) blitFrame(out, W, H, fr, x + 32, slot, (k) => shadeFor(k, idx), palette);
    }
  }

  // ---- Pass 2: walls / objects back-to-front, then dSpecial overlay ---------
  const spFrames = special_atlas ? special_atlas.frames : null;
  const spCache = new Map();
  const getSpecial = (id) => {
    if (!spFrames || !specialAtlas) return null;
    const key = String(id);
    if (spCache.has(key)) return spCache.get(key);
    const rect = spFrames[key];
    if (!rect) { spCache.set(key, null); return null; }
    const f = readRegion(specialAtlas, rect);
    spCache.set(key, f);
    return f;
  };

  for (let s = 0; s <= maxSum; ++s) {
    for (let i = 0; i < gw; ++i) {
      const j = s - i;
      if (j < 0 || j >= gh) continue;
      const idx = j * gw + i;
      const mi = resolveMi(effMicro(idx));
      const x = cellX(i, j);
      const ct = colTop(i, j);

      // Wall content: floor cells skip blocks 0/1 (already drawn as diamonds);
      // non-floor cells draw blocks 0..microLen-1.
      const kStart = isFloor(idx) ? 2 : 0;
      for (let k = kStart; k < micro_len; ++k) {
        const f = getFrame(mi + ":" + k);
        if (!f) continue;
        const b = k < 2 ? 0 : Math.floor(k / 2);
        const side = k % 2;
        blitFrame(out, W, H, f, x + side * 32, slotTop(ct, b),
                  (kk) => shadeFor(kk, idx), palette);
      }

      // dSpecial overlay (town trees; dungeon arches / doors), bottom-anchored at
      // the cell base like DrawDungeon's pSpecialCels pass.
      const spId = effSpecial(idx);
      if (spId) {
        const spr = getSpecial(spId);
        if (spr) {
          const w = spr.width, h = spr.height;
          const sx = x + (64 - w) / 2;
          const baseY = ct + Hm - 1;
          const fd = spr.data;
          for (let fy = 0; fy < h; ++fy) {
            const dr = baseY - (h - 1) + fy;
            if (dr < 0 || dr >= H) continue;
            for (let fx = 0; fx < w; ++fx) {
              const px = sx + fx;
              if (px < 0 || px >= W) continue;
              const si = (fy * w + fx) * 4;
              if (fd[si + 3] === 0) continue;
              const idx2 = shadeFor(fd[si], idx);
              const di = (dr * W + px) * 4;
              const pi = idx2 * 4;
              out[di] = palette[pi];
              out[di + 1] = palette[pi + 1];
              out[di + 2] = palette[pi + 2];
              out[di + 3] = 255;
            }
          }
        }
      }
    }
  }

  return { width: W, height: H, data: out };
}

// ---- Phase 4: front-end overlay (grid + SOL tint) ---------------------------
// The backend's `scene.build` used to ship a separate overlay PNG aligned to the
// (downscaled) preview. With the viewer now fully tiles-driven, this port draws
// the SAME overlay on the client at NATIVE resolution — identical projection /
// slot math to renderScene, so it lands pixel-perfectly on the base. Fully
// transparent except for the translucent SOL tints and the coarse reference
// grid, so the underlying tile art still shows through.
//
// SOL tint priority (matches decoders::buildOverlay): solid (red) > transparent
// flag (blue) > blockLight (yellow). Major grid lines every 8th cell.

function overlayPutPixel(out, w, h, x, y, r, g, b, a) {
  if (x < 0 || x >= w || y < 0 || y >= h) return;
  const q = (y * w + x) * 4;
  out[q] = r; out[q + 1] = g; out[q + 2] = b; out[q + 3] = a;
}

function overlayLine(out, w, h, x0, y0, x1, y1, r, g, b, a) {
  let dx = Math.abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  let dy = -Math.abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  let err = dx + dy;
  for (;;) {
    overlayPutPixel(out, w, h, x0, y0, r, g, b, a);
    if (x0 === x1 && y0 === y1) break;
    const e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

function overlayFillDiamond(out, w, h, cx, cy, halfW, halfH, r, g, b, a) {
  const deny = Math.max(1, halfH);
  for (let y = -halfH; y <= halfH; ++y) {
    const span = (halfW * (halfH - Math.abs(y))) / deny;
    for (let x = -span; x <= span; ++x)
      overlayPutPixel(out, w, h, cx + x, cy + y, r, g, b, a);
  }
}

export function renderSceneOverlay(layout, opts = {}) {
  const {
    micro_len,
    grid_width: gw,
    grid_height: gh,
    projection,
  } = layout;
  const Hm = (micro_len / 2) * 32;
  const W = projection.width, H = projection.height;
  // All-zero RGBA => fully transparent; only tints/grid are painted on top.
  const out = new Uint8ClampedArray(W * H * 4);
  const solid = layout.solid || [];
  const transparent = layout.transparent || [];
  const blockLight = layout.block_light || [];
  const { minX, minY } = projection;

  // A micro override (door toggle) changes the cell's SOL flags, so the tint
  // must follow the NEW micro's flags too — otherwise an opened doorway keeps
  // the closed leaf's red "Solid" tint while the base shows the open floor.
  const microOverride = opts.microOverride || null;
  const microFlags = layout.micro_flags || null;
  const microGrid = layout.micro_grid || [];
  const prefillMicro = layout.prefill_micro;
  const overrideSol = (idx) => {
    if (!microOverride || !microFlags) return -1;
    let mi = microGrid[idx];
    const v = (microOverride instanceof Map) ? microOverride.get(idx) : microOverride[idx];
    if (v !== undefined && v !== null) mi = v;
    if (mi === 65535 || mi >= 1e9) mi = prefillMicro; // kNoMicro sentinel
    if (typeof mi !== "number" || mi < 0 || mi >= microFlags.length) return -1;
    return microFlags[mi] | 0;
  };

  for (let j = 0; j < gh; ++j) {
    for (let i = 0; i < gw; ++i) {
      const idx = j * gw + i;
      // Diamond corners in native coords (step = 1). `slot` is the floor
      // diamond top, identical to renderScene's slotTop(colTop(i,j), 0).
      const baseX = (i - j) * 32 - minX;
      const baseY = (i + j) * 16 - minY;
      const slot = baseY - 31;
      const topX = baseX + 32, topY = slot;
      const rightX = baseX + 64, rightY = slot + 16;
      const botX = baseX + 32, botY = slot + 32;
      const leftX = baseX, leftY = slot + 16;
      const cx = (leftX + rightX) / 2;
      const cy = (topY + botY) / 2;
      const halfW = Math.max(1, (rightX - leftX) / 2);
      const halfH = Math.max(1, (botY - topY) / 2);

      const sol = overrideSol(idx);
      const isSolid = sol < 0 ? !!solid[idx] : (sol & 1) !== 0;
      const isBlockLight = sol < 0
        ? (idx < blockLight.length && !!blockLight[idx]) : (sol & 2) !== 0;

      let r = 0, g = 0, b = 0, a = 0;
      if (isSolid) { r = 255; g = 60; b = 60; a = 55; }
      else if (idx < transparent.length && transparent[idx]) { r = 60; g = 160; b = 255; a = 55; }
      else if (isBlockLight) { r = 255; g = 200; b = 60; a = 45; }
      if (a > 0) overlayFillDiamond(out, W, H, cx, cy, halfW, halfH, r, g, b, a);

      // Major reference grid (every 8th cell) — keeps the coarse grid legible
      // while the tint carries per-cell data.
      if (i % 8 === 0 && j % 8 === 0) {
        overlayLine(out, W, H, topX, topY, rightX, rightY, 255, 255, 255, 140);
        overlayLine(out, W, H, rightX, rightY, botX, botY, 255, 255, 255, 140);
        overlayLine(out, W, H, botX, botY, leftX, leftY, 255, 255, 255, 140);
        overlayLine(out, W, H, leftX, leftY, topX, topY, 255, 255, 255, 140);
      }
    }
  }
  return { width: W, height: H, data: out };
}
