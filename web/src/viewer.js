// web/src/viewer.js
//
// Canvas 2D rendering of one decoded sprite frame. The backend returns each
// frame as `rgba_base64`: raw RGBA8 bytes (length === width*height*4), NOT a
// PNG data URL. We decode to an ImageData and blit it directly.

function base64ToBytes(b64) {
  const bin = atob(b64);
  const len = bin.length;
  const bytes = new Uint8Array(len);
  for (let i = 0; i < len; i++) bytes[i] = bin.charCodeAt(i);
  return bytes;
}

// Draw a single frame into `canvas`, magnified `zoom`×. The magnification is
// baked into the canvas backing store (canvas pixels = native × zoom) and the 2D
// context is scaled, so nearest-neighbour keeps it crisp.
export function drawFrame(canvas, rgbaBase64, w, h, zoom) {
  const z = validZoom(zoom);
  const bytes = base64ToBytes(rgbaBase64);
  canvas.width = w * z;
  canvas.height = h * z;
  const ctx = canvas.getContext("2d");
  ctx.imageSmoothingEnabled = false;
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  const off = document.createElement("canvas");
  off.width = w;
  off.height = h;
  const octx = off.getContext("2d");
  const img = octx.createImageData(w, h);
  const n = Math.min(bytes.length, w * h * 4);
  img.data.set(bytes.subarray(0, n));
  octx.putImageData(img, 0, 0); // Anything beyond the bytes stays transparent.
  ctx.save();
  ctx.scale(z, z);
  ctx.drawImage(off, 0, 0);
  ctx.restore();
}

// Clamp/normalise a zoom factor to a finite integer in 1..8 (default 4).
export function validZoom(zoom) {
  const z = Math.round(Number(zoom));
  return Number.isFinite(z) && z >= 1 && z <= 8 ? z : 4;
}

// Blit one decoded RGBA frame at absolute scene coordinates (under an already
// scaled context). Shared by drawComposite (overlay offsets) and drawScene
// (independently placed actors). Nearest-neighbour keeps the upscale crisp.
function blitRgba(ctx, rgba, w, h, x, y, alpha) {
  if (!rgba || w <= 0 || h <= 0) return;
  ctx.imageSmoothingEnabled = false;
  const off = document.createElement("canvas");
  off.width = w;
  off.height = h;
  const octx = off.getContext("2d");
  const img = octx.createImageData(w, h);
  img.data.set(base64ToBytes(rgba).subarray(0, w * h * 4));
  octx.putImageData(img, 0, 0);
  if (typeof alpha === "number" && alpha < 1) {
    ctx.save();
    ctx.globalAlpha = Math.max(0, alpha);
    ctx.drawImage(off, x || 0, y || 0);
    ctx.restore();
  } else {
    ctx.drawImage(off, x || 0, y || 0);
  }
}

// Draw a base frame with zero or more overlay layers alpha-composited on top,
// magnified `zoom`×. Each overlay is { rgba, w, h, dx, dy, enabled }; overlays
// are blended with normal source-over alpha. Used for effect overlays (player
// lm/fm/qm, monster special, or any sprite the user picks) — separate sprite
// files in Diablo, not baked in.
export function drawComposite(canvas, baseRgba, baseW, baseH, overlays, zoom) {
  const z = validZoom(zoom);
  const ctx = canvas.getContext("2d");
  canvas.width = baseW * z;
  canvas.height = baseH * z;
  ctx.imageSmoothingEnabled = false;
  ctx.clearRect(0, 0, canvas.width, canvas.height);

  const off = document.createElement("canvas");
  off.width = baseW;
  off.height = baseH;
  const octx = off.getContext("2d");
  const baseBytes = base64ToBytes(baseRgba);
  const baseImg = octx.createImageData(baseW, baseH);
  baseImg.data.set(baseBytes.subarray(0, baseW * baseH * 4));
  octx.putImageData(baseImg, 0, 0);

  ctx.save();
  ctx.scale(z, z);
  ctx.drawImage(off, 0, 0);
  for (const ov of overlays || []) {
    if (!ov || !ov.enabled) continue;
    blitRgba(ctx, ov.rgba, ov.w, ov.h, ov.dx || 0, ov.dy || 0);
  }
  ctx.restore();
}

// Draw a whole scene: several decoded sprites at independent absolute positions,
// magnified `zoom`×. Each actor is { rgba, w, h, x, y, z, alpha }; lower z is
// drawn first. Actor positions AND sizes are in native scene pixels, so the
// context scale applies the zoom uniformly — sprite-to-sprite spacing scales
// with the same factor. Used by the skill / combat viewer (player + monster +
// effects).
export function drawScene(canvas, sceneW, sceneH, actors, zoom) {
  const z = validZoom(zoom);
  const ctx = canvas.getContext("2d");
  canvas.width = sceneW * z;
  canvas.height = sceneH * z;
  ctx.imageSmoothingEnabled = false;
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  const sorted = (actors || [])
    .filter((a) => a && a.rgba)
    .slice()
    .sort((a, b) => (a.z || 0) - (b.z || 0));
  ctx.save();
  ctx.scale(z, z);
  for (const a of sorted)
    blitRgba(ctx, a.rgba, a.w, a.h, a.x || 0, a.y || 0, a.alpha);
  ctx.restore();
}
