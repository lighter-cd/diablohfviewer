// web/src/level.js
//
// Level Tile Viewer: calls mcp-server `level.inspect` and renders the returned
// base64 PNGs (assembly / micro pieces / CEL frames) onto canvases. The
// backend does all MPQ/TIL/MIN/CEL work; this page never reads files.

import { McpClient } from "./client.js?v=20261109c";

const $ = (id) => document.getElementById(id);
const client = new McpClient();

function setStatus(text, isError = false) {
  const el = $("status");
  el.textContent = text;
  el.classList.toggle("err", isError);
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
    await render(res);
    setStatus(`megatile ${res.megatile} · micros ${res.micros.map((m) => m.id).join(", ")} · ` +
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

$("connectBtn").addEventListener("click", async () => {
  $("connectBtn").disabled = true;
  try {
    await client.connect($("wsUrl").value.trim());
    $("inspectBtn").disabled = false;
    setStatus("已连接");
  } catch (e) {
    setStatus("连接失败: " + (e.message || e), true);
  } finally {
    $("connectBtn").disabled = false;
  }
});

$("inspectBtn").addEventListener("click", inspect);
