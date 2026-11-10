// web/src/level.js
//
// Level Tile Viewer: calls mcp-server `level.inspect` and renders the returned
// base64 PNGs (assembly / micro pieces / CEL frames) onto canvases. The
// backend does all MPQ/TIL/MIN/CEL work; this page never reads files.

import { McpClient } from "./client.js?v=20261111b";

const $ = (id) => document.getElementById(id);
const client = new McpClient();
let hasLevelTool = false;
let maxMegatile = 0;

const TILESETS = [
  { id: "tristram", label: "Tristram 城镇", mpq: "DIABDAT.MPQ",
    base: "levels/towndata/town", palette: "levels/towndata/town.pal", blocks: 16, micro: 16 },
  { id: "l1", label: "Cathedral L1", mpq: "DIABDAT.MPQ",
    base: "levels/l1data/l1", palette: "levels/l1data/l1_2.pal", blocks: 10, micro: 10 },
  { id: "l2", label: "Catacombs L2", mpq: "DIABDAT.MPQ",
    base: "levels/l2data/l2", palette: "levels/l2data/l2_2.pal", blocks: 10, micro: 10 },
  { id: "l3", label: "Caves L3", mpq: "DIABDAT.MPQ",
    base: "levels/l3data/l3", palette: "levels/l3data/l3pfoul.pal", blocks: 10, micro: 10 },
  { id: "l4", label: "Hell L4", mpq: "DIABDAT.MPQ",
    base: "levels/l4data/l4", palette: "levels/l4data/l4_1.pal", blocks: 16, micro: 12 },
  { id: "l5", label: "Hellfire Crypt L5", mpq: "hellfire.mpq",
    base: "nlevels/l5data/l5", palette: "nlevels/l5data/l5base.pal", blocks: 10, micro: 10 },
  { id: "l6", label: "Hellfire Nest L6", mpq: "hellfire.mpq",
    base: "nlevels/l6data/l6", palette: "nlevels/l6data/l6base1.pal", blocks: 10, micro: 10 },
];

function mpqDir() {
  const p = ($("archive").value || "").replace(/\\/g, "/");
  const slash = p.lastIndexOf("/");
  return slash >= 0 ? p.slice(0, slash + 1) : "D:/work/xt/diablo/DevilutionX-Hell Fire/";
}

function applyTileset(id, runInspect) {
  const t = TILESETS.find((x) => x.id === id) || TILESETS[0];
  $("archive").value = mpqDir() + t.mpq;
  $("cel").value = t.base + ".cel";
  $("til").value = t.base + ".til";
  $("min").value = t.base + ".min";
  $("sol").value = t.base + ".sol";
  $("palette").value = t.palette;
  $("blocks").value = String(t.blocks);
  $("microLen").value = String(t.micro);
  $("megatile").value = "1";
  maxMegatile = 0;
  if (runInspect && hasLevelTool && client.ws && client.ws.readyState === WebSocket.OPEN) inspect();
}

function initTilesets() {
  const sel = $("preset");
  for (const t of TILESETS) {
    const opt = document.createElement("option");
    opt.value = t.id;
    opt.textContent = t.label;
    sel.appendChild(opt);
  }
  sel.value = "tristram";
  sel.addEventListener("change", () => applyTileset(sel.value, true));
}

function setStatus(text, isError = false) {
  const el = $("status");
  el.textContent = text;
  el.classList.toggle("err", isError);
}

// The backend searches `archives` in order. Build the common DevilutionX MPQ
// stack next to the selected archive so town/hellfire assets resolve even when
// the primary archive is not DIABDAT.MPQ.
function archiveStack(primary) {
  const p = primary.replace(/\\/g, "/");
  const slash = p.lastIndexOf("/");
  const dir = slash >= 0 ? p.slice(0, slash + 1) : "";
  const names = ["DIABDAT.MPQ", "hellfire.mpq", "devilutionx.mpq", "hfmonk.mpq", "hfopt1.mpq", "hfopt2.mpq"];
  const out = [];
  for (const n of names) {
    const candidate = dir + n;
    if (candidate.toLowerCase() !== p.toLowerCase() && !out.some((x) => x.toLowerCase() === candidate.toLowerCase()))
      out.push(candidate);
  }
  return out;
}

function bytesFromBase64(b64) {
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return bytes;
}

async function decodePng(b64) {
  const blob = new Blob([bytesFromBase64(b64)], { type: "image/png" });
  if (typeof createImageBitmap === "function") return createImageBitmap(blob);
  const url = URL.createObjectURL(blob);
  try {
    return await new Promise((resolve, reject) => {
      const img = new Image();
      img.onload = () => resolve(img);
      img.onerror = () => reject(new Error("PNG 解码失败"));
      img.src = url;
    });
  } finally {
    // The Image keeps its decoded bitmap; the URL can be released.
    setTimeout(() => URL.revokeObjectURL(url), 0);
  }
}

async function drawPng(canvas, b64, scale = 1) {
  if (!b64) {
    canvas.width = canvas.height = 0;
    return;
  }
  const img = await decodePng(b64);
  const w = img.width || img.naturalWidth;
  const h = img.height || img.naturalHeight;
  canvas.width = Math.max(1, w * scale);
  canvas.height = Math.max(1, h * scale);
  const ctx = canvas.getContext("2d");
  ctx.imageSmoothingEnabled = false;
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
  if (typeof img.close === "function") img.close();
  canvas.dataset.scale = String(scale);
  canvas.dataset.w = String(w);
  canvas.dataset.h = String(h);
  canvas.dataset.b64 = b64;
}

function makeCanvas(cls = "tile-canvas") {
  const c = document.createElement("canvas");
  c.className = cls;
  return c;
}

function blockTable(blocks) {
  const table = document.createElement("table");
  table.innerHTML = "<thead><tr><th>mt</th><th>type</th><th>frame</th><th>data</th></tr></thead>";
  const tbody = document.createElement("tbody");
  for (const b of blocks) {
    const tr = document.createElement("tr");
    const nonzero = b.has_value ? "hl" : "";
    tr.innerHTML = `<td>${b.mt}</td><td class="${nonzero}">${b.type_name}</td>` +
                   `<td class="${nonzero}">${b.frame || ""}</td>` +
                   `<td>${b.data ? "0x" + b.data.toString(16) : ""}</td>`;
    tbody.appendChild(tr);
  }
  table.appendChild(tbody);
  return table;
}

async function inspect() {
  const archive = $("archive").value.trim();
  if (!archive) {
    setStatus("请先填写 MPQ 路径", true);
    return;
  }
  const args = {
    archive,
    archives: archiveStack(archive),
    cel: $("cel").value.trim(),
    til: $("til").value.trim(),
    min: $("min").value.trim(),
    sol: $("sol").value.trim(),
    palette: $("palette").value.trim(),
    blocks: parseInt($("blocks").value, 10) || 16,
    micro_len: parseInt($("microLen").value, 10) || 16,
    megatile: parseInt($("megatile").value, 10) || 1,
  };
  setStatus(`inspect ${args.megatile} …`);
  $("inspectBtn").disabled = true;
  try {
    const res = await client.callTool("level.inspect", args);
    maxMegatile = res.megatile_count || res.megatile || 0;
    await render(res);
    setStatus(`megatile ${res.megatile}/${maxMegatile || "?"} · micros ${res.micros.map((m) => m.id).join(", ")} · ` +
              `frames ${res.frames.length} · 组装 ${res.assembly.width}×${res.assembly.height}`);
  } catch (e) {
    setStatus("level.inspect 失败: " + (e.message || e), true);
  } finally {
    $("inspectBtn").disabled = false;
  }
}

async function render(res) {
  // Assembly
  const canvas = $("assembly");
  await drawPng(canvas, res.assembly.png_base64, 4);
  canvas.onclick = () => {
    const next = canvas.dataset.scale === "4" ? 1 : 4;
    drawPng(canvas, canvas.dataset.b64, next);
  };
  $("meta").textContent =
    `native ${res.assembly.native_width}×${res.assembly.native_height} · ` +
    `crop ${res.assembly.width}×${res.assembly.height} at (${res.assembly.left},${res.assembly.top}) · ` +
    `grid ${res.grid_width}×${res.grid_height} · blocks ${res.blocks} · micro_len ${res.micro_len}`;

  // Frames
  const frames = $("frames");
  frames.innerHTML = "";
  for (const f of res.frames) {
    const card = document.createElement("div");
    card.className = "frame-card";
    const c = makeCanvas();
    await drawPng(c, f.png_base64, 4);
    c.onclick = () => drawPng(c, c.dataset.b64, c.dataset.scale === "4" ? 1 : 4);
    card.appendChild(c);
    const label = document.createElement("div");
    label.className = "label";
    label.textContent = `frame ${f.frame}`;
    card.appendChild(label);
    const sub = document.createElement("div");
    sub.className = "muted";
    sub.textContent = `${f.type_name} · ${f.width}×${f.height}`;
    card.appendChild(sub);
    frames.appendChild(card);
  }

  // Micro pieces
  const micros = $("micros");
  micros.innerHTML = "";
  for (const m of res.micros) {
    const card = document.createElement("div");
    card.className = "micro-card";
    const head = document.createElement("div");
    head.className = "label";
    head.textContent = `micro ${m.id} · ${m.width}×${m.height}`;
    card.appendChild(head);
    const c = makeCanvas();
    await drawPng(c, m.png_base64, 3);
    c.onclick = () => drawPng(c, c.dataset.b64, c.dataset.scale === "3" ? 1 : 3);
    card.appendChild(c);
    card.appendChild(blockTable(m.blocks));
    micros.appendChild(card);
  }
}

initTilesets();

$("connectBtn").addEventListener("click", async () => {
  $("connectBtn").disabled = true;
  $("inspectBtn").disabled = true;
  try {
    await client.connect($("wsUrl").value.trim());
    const listing = await client.listTools();
    const tools = Array.isArray(listing) ? listing : (listing.tools || []);
    hasLevelTool = tools.some((t) => t.name === "level.inspect");
    if (!hasLevelTool) {
      setStatus("后端没有 level.inspect：请停止旧 mcp-server、重新构建并重启 --ws", true);
      return;
    }
    $("inspectBtn").disabled = false;
    setStatus(`已连接（${tools.length} tools）`);
    // Auto-run the default inspection so the page is never an empty shell.
    if ($("archive").value.trim()) await inspect();
  } catch (e) {
    setStatus("连接失败: " + (e.message || e), true);
  } finally {
    $("connectBtn").disabled = false;
  }
});

function stepMegatile(delta) {
  const input = $("megatile");
  let v = (parseInt(input.value, 10) || 1) + delta;
  if (v < 1) v = 1;
  if (maxMegatile > 0 && v > maxMegatile) v = maxMegatile;
  input.value = String(v);
  inspect();
}

$("prevBtn").addEventListener("click", () => stepMegatile(-1));
$("nextBtn").addEventListener("click", () => stepMegatile(1));
$("inspectBtn").addEventListener("click", inspect);
