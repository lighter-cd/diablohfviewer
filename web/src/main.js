// web/src/main.js
//
// Glue: connect to the mcp-server WS bridge, load the sprite catalog via
// sprite.list, and on sprite click decode one frame via sprite.decode and draw
// it on the canvas. First-cut "select sprite -> view frame" (no animation /
// 8-direction playback yet).

import { McpClient } from "./client.js";
import { buildTree, renderCatalog } from "./catalog.js";
import { drawFrame, fitCanvas } from "./viewer.js";

const $ = (id) => document.getElementById(id);

const client = new McpClient();
let currentFrames = null;   // last decode result frames, for the frame slider

client.onStatus = (connected) => {
  const s = $("status");
  s.textContent = connected ? "connected" : "disconnected";
  s.className = "status " + (connected ? "ok" : "bad");
};

function setMeta(text, isError = false) {
  const m = $("meta");
  m.textContent = text;
  m.className = isError ? "meta err" : "meta";
}

async function loadCatalog() {
  setMeta("loading catalog…");
  try {
    const res = await client.callTool("sprite.list", {});
    const sprites = (res && res.sprites) || [];
    const tree = buildTree(sprites);
    renderCatalog($("catalog"), tree, onSelectSprite);
    setMeta(`catalog: ${res.count ?? sprites.length} sprites`);
  } catch (e) {
    setMeta("sprite.list failed: " + e.message, true);
  }
}

async function onSelectSprite(sprite) {
  const archive = $("archive").value.trim();
  const palette = $("palette").value.trim();
  setMeta("decoding " + sprite.name + " …");
  try {
    const res = await client.callTool("sprite.decode", {
      archive,
      name: sprite.name,
      width: "auto",
      palette_name: palette,
    });
    if (!res || !Array.isArray(res.frames) || res.frames.length === 0) {
      setMeta("no frames returned for " + sprite.name, true);
      return;
    }
    currentFrames = res.frames;
    const max = currentFrames.length - 1;
    const slider = $("frame");
    slider.max = String(max);
    slider.value = "0";
    $("frameNo").textContent = "0 / " + max;
    showFrame(0);
    setMeta(`${sprite.name}  ${res.format}  ${res.width}×${res.height}  ${currentFrames.length} frames`);
  } catch (e) {
    setMeta("sprite.decode failed: " + e.message, true);
  }
}

function showFrame(idx) {
  if (!currentFrames) return;
  const f = currentFrames[idx];
  if (!f) return;
  drawFrame($("canvas"), f.rgba_base64, f.width, f.height);
  fitCanvas($("canvas"), 384);
  $("frameNo").textContent = idx + " / " + (currentFrames.length - 1);
}

function wireControls() {
  $("connectBtn").addEventListener("click", async () => {
    try {
      await client.connect($("wsUrl").value.trim());
      await loadCatalog();
    } catch (e) {
      setMeta("connect failed: " + (e.message || e), true);
    }
  });
  $("frame").addEventListener("input", (e) => showFrame(Number(e.target.value)));
}

wireControls();
