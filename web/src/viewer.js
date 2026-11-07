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

// Draw a single frame into `canvas`. Allows up to (w*h*4) bytes; shorter input
// is padded (transparent) so a partial buffer never throws.
export function drawFrame(canvas, rgbaBase64, w, h) {
  const bytes = base64ToBytes(rgbaBase64);
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext("2d");
  const img = ctx.createImageData(w, h);
  const n = Math.min(bytes.length, w * h * 4);
  img.data.set(bytes.subarray(0, n));
  // Anything beyond the provided bytes stays fully transparent (alpha = 0).
  ctx.putImageData(img, 0, 0);
}

// Fit a frame into the visible canvas area while keeping pixel aspect (1:1).
// Diablo sprites are tiny (e.g. 96x96); we scale up with nearest-neighbour.
export function fitCanvas(canvas, displaySize) {
  const cw = canvas.width || 1;
  const ch = canvas.height || 1;
  const scale = Math.max(1, Math.floor(displaySize / Math.max(cw, ch)));
  canvas.style.width = cw * scale + "px";
  canvas.style.height = ch * scale + "px";
  canvas.style.imageRendering = "pixelated";
}

// Draw a base frame with zero or more overlay layers alpha-composited on top.
// Each overlay is { rgba, w, h, dx, dy, enabled }; overlays are blended with
// normal source-over alpha (their transparent pixels leave the base intact).
// Used for effect overlays (player lm/fm/qm, monster special, or any sprite the
// user picks) — these are separate sprite files in Diablo, not baked in.
export function drawComposite(canvas, baseRgba, baseW, baseH, overlays) {
  const ctx = canvas.getContext("2d");
  canvas.width = baseW;
  canvas.height = baseH;
  ctx.clearRect(0, 0, baseW, baseH);

  const baseBytes = base64ToBytes(baseRgba);
  const baseImg = ctx.createImageData(baseW, baseH);
  baseImg.data.set(baseBytes.subarray(0, baseW * baseH * 4));
  ctx.putImageData(baseImg, 0, 0);

  for (const ov of overlays || []) {
    if (!ov || !ov.enabled) continue;
    const w = ov.w, h = ov.h;
    const off = document.createElement("canvas");
    off.width = w;
    off.height = h;
    const octx = off.getContext("2d");
    const img = octx.createImageData(w, h);
    img.data.set(base64ToBytes(ov.rgba).subarray(0, w * h * 4));
    octx.putImageData(img, 0, 0);
    ctx.drawImage(off, ov.dx || 0, ov.dy || 0);
  }
}
