// web/src/viewer.js
//
// Canvas 2D rendering of decoded sprite frames. The backend returns each frame
// as `rgba_base64`: raw RGBA8 bytes (length === width*height*4), NOT a PNG.
// We decode each frame into an offscreen canvas, then blit it onto the FIXED main
// canvas with `drawImage(img, dx, dy, dw, dh)` where dw/dh are the zoomed size.
// That is Canvas' built-in draw-time scaling mode; `imageSmoothingEnabled=false`
// makes it nearest-neighbour (crisp pixel art). The canvas element itself stays a
// constant 1280x720 (720p) viewport — we never resize it when zooming.

export const VIEW_W = 1280; // fixed logical viewport width  (720p)
export const VIEW_H = 720;  // fixed logical viewport height

function base64ToBytes(b64) {
  const bin = atob(b64);
  const len = bin.length;
  const bytes = new Uint8Array(len);
  for (let i = 0; i < len; i++) bytes[i] = bin.charCodeAt(i);
  return bytes;
}

// Decode one RGBA frame into an offscreen canvas at its native pixel size.
function rgbaToCanvas(rgbaBase64, w, h) {
  const off = document.createElement("canvas");
  off.width = w;
  off.height = h;
  const octx = off.getContext("2d");
  const img = octx.createImageData(w, h);
  img.data.set(base64ToBytes(rgbaBase64).subarray(0, w * h * 4));
  octx.putImageData(img, 0, 0);
  return off;
}

// Reset the fixed viewport: canvas is always VIEW_W x VIEW_H, cleared, and set to
// nearest-neighbour so draw-time scaling stays crisp.
function setupView(canvas) {
  if (canvas.width !== VIEW_W) canvas.width = VIEW_W;
  if (canvas.height !== VIEW_H) canvas.height = VIEW_H;
  const ctx = canvas.getContext("2d");
  ctx.imageSmoothingEnabled = false;
  ctx.clearRect(0, 0, VIEW_W, VIEW_H);
  return ctx;
}

// Clamp/normalise a zoom factor to a finite integer in 1..8 (default 2).
export function validZoom(zoom) {
  const z = Math.round(Number(zoom));
  return Number.isFinite(z) && z >= 1 && z <= 8 ? z : 2;
}

// Draw one offscreen bitmap scaled to (w,h) at (x,y). This is the draw-time
// magnification: the source is native-sized, the destination is native × zoom.
function blitScaled(ctx, img, x, y, w, h, alpha) {
  if (!img || w <= 0 || h <= 0) return;
  if (typeof alpha === "number" && alpha < 1) {
    ctx.save();
    ctx.globalAlpha = Math.max(0, alpha);
    ctx.drawImage(img, x, y, w, h);
    ctx.restore();
  } else {
    ctx.drawImage(img, x, y, w, h);
  }
}

// Single frame magnified `zoom`x and centred.
export function drawComposite(canvas, baseRgba, baseW, baseH, zoom) {
  const z = validZoom(zoom);
  const ctx = setupView(canvas);
  const sw = baseW * z, sh = baseH * z;
  const ox = Math.round((VIEW_W - sw) / 2);
  const oy = Math.round((VIEW_H - sh) / 2);
  blitScaled(ctx, rgbaToCanvas(baseRgba, baseW, baseH), ox, oy, sw, sh, 1);
}

// Whole scene: actors at native scene coordinates, magnified `zoom`x and centred.
// Sprite positions AND sizes scale together, so sprite-to-sprite spacing grows
// with the same factor.
export function drawScene(canvas, sceneW, sceneH, actors, zoom) {
  const z = validZoom(zoom);
  const ctx = setupView(canvas);
  const sw = sceneW * z, sh = sceneH * z;
  const ox = Math.round((VIEW_W - sw) / 2);
  const oy = Math.round((VIEW_H - sh) / 2);
  const sorted = (actors || [])
    .filter((a) => a && a.rgba)
    .slice()
    .sort((a, b) => (a.z || 0) - (b.z || 0));
  for (const a of sorted) {
    const img = rgbaToCanvas(a.rgba, a.w, a.h);
    blitScaled(ctx, img,
      ox + (a.x || 0) * z, oy + (a.y || 0) * z,
      a.w * z, a.h * z, a.alpha);
  }
}

