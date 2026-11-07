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

// Extensions the backend can actually decode. sprite.list only enumerates CL2/CEL,
// but we still guard so an unexpected entry never fails silently.
const DECODABLE = new Set(["cl2", "cel", "clx", "pcx"]);

const client = new McpClient();
let currentFrames = null;   // last decode result frames, for the frame slider
let lastSprite = null;      // last selected sprite (re-decode when palette changes)
// Map palette name -> owning MPQ path (from sprite.list kind:"palette"). Used to
// tell sprite.decode which extra MPQ holds the palette, since a palette often
// lives in a different MPQ than the sprite (e.g. Hellfire palette on a DIABDAT sprite).
const paletteArchives = new Map();

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

function showError(msg) {
  const b = $("errorBanner");
  b.textContent = msg;
  b.hidden = false;
}
function hideError() {
  $("errorBanner").hidden = true;
}

function basename(p) {
  const i = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/"));
  return i >= 0 ? p.slice(i + 1) : p;
}
function extOf(p) {
  const i = p.lastIndexOf(".");
  return i >= 0 ? p.slice(i + 1).toLowerCase() : "";
}

async function loadCatalog() {
  setMeta("loading catalog…");
  try {
    const res = await client.callTool("sprite.list", {});
    const sprites = (res && res.sprites) || [];
    const tree = buildTree(sprites);
    renderCatalog($("catalog"), tree, onSelectSprite);
    setMeta(`catalog: ${res.count ?? sprites.length} sprites`);
    await loadPalettes();
  } catch (e) {
    setMeta("sprite.list failed: " + e.message, true);
  }
}

// Populate the palette <select> from sprite.list(kind:"palette"): every .pal in
// the archive, so the user picks instead of typing a path by hand.
async function loadPalettes() {
  const sel = $("palette");
  sel.innerHTML = "";
  try {
    const res = await client.callTool("sprite.list", { kind: "palette" });
    const pals = (res && res.sprites) || [];
    if (pals.length === 0) {
      const opt = document.createElement("option");
      opt.textContent = "（无可用调色板）";
      sel.appendChild(opt);
      return;
    }
    for (const p of pals) {
      const opt = document.createElement("option");
      opt.value = p.name;
      opt.textContent = basename(p.name);
      opt.title = p.name;
      paletteArchives.set(p.name, p.archive_path || "");
      sel.appendChild(opt);
    }
  } catch (e) {
    const opt = document.createElement("option");
    opt.textContent = "（加载调色板失败）";
    sel.appendChild(opt);
  }
}

async function onSelectSprite(sprite) {
  lastSprite = sprite;
  hideError();
  $("currentName").textContent = sprite.name;   // prominent current-resource path

  const ext = extOf(sprite.name);
  if (!DECODABLE.has(ext)) {
    showError(`暂不支持的格式：.${ext || "?"}（当前仅支持 .cl2 / .cel / .clx / .pcx）`);
    $("frame").max = "0";
    return;
  }

  // sprite.list already returns the MPQ path per sprite (archive_path).
  const archive = (sprite.archive_path || "").trim();
  if (!archive) {
    showError("该精灵缺少 archive_path，无法定位 MPQ，无法解码。");
    return;
  }
  const palette = $("palette").value;
  if (!palette) {
    showError("请先在上方选择一个调色板。");
    return;
  }

  // The palette may live in a different MPQ than the sprite (e.g. Hellfire
  // palette on a DIABDAT sprite). Pass the sprite's archive plus the palette's
  // owning archive so sprite.decode can resolve it across the MPQ stack.
  const paletteArchive = paletteArchives.get(palette) || "";
  const archives = [archive];
  if (paletteArchive && !archives.includes(paletteArchive))
    archives.push(paletteArchive);

  setMeta("decoding " + sprite.name + " …");
  try {
    const res = await client.callTool("sprite.decode", {
      archive,
      archives,
      name: sprite.name,
      width: "auto",
      palette_name: palette,
    });
    if (!res || !Array.isArray(res.frames) || res.frames.length === 0) {
      showError("解码失败：" + sprite.name + " 未返回任何帧。");
      return;
    }
    hideError();
    currentFrames = res.frames;
    const max = currentFrames.length - 1;
    const slider = $("frame");
    slider.max = String(max);
    slider.value = "0";
    $("frameNo").textContent = "0 / " + max;
    showFrame(0);
    setMeta(`${sprite.name}  ${res.format}  ${res.width}×${res.height}  ${currentFrames.length} frames`);
  } catch (e) {
    // Surface the real reason instead of failing silently (e.g. width auto failed,
    // extract failed, palette not found…).
    showError("解码失败：" + (e.message || e));
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
  // Re-decode the current sprite when the user changes the palette.
  $("palette").addEventListener("change", () => {
    if (lastSprite) onSelectSprite(lastSprite);
  });
}

wireControls();
