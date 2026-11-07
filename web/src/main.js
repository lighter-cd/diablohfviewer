// web/src/main.js
//
// Glue: connect to the mcp-server WS bridge, load the sprite catalog via
// sprite.list, and decode sprites via sprite.decode for Canvas rendering.
//
// Two browse modes share the canvas / palette / direction / playback controls:
//   - Catalog (目录浏览): pick any sprite from the recovered-name tree.
//   - Character (角色查看器): assemble a player sprite by Class -> Action ->
//     Armor/Weapon via the plrgfx filename formula, then decode + play it with
//     8-direction animation.
//
// Player CL2 frames are stored direction-major: 8 directions x N frames, so
// frameIndex = dir * framesPerDir + frameInDir (framesPerDir = total/8).

import { McpClient } from "./client.js";
import { buildTree, renderCatalog, setAllExpanded } from "./catalog.js";
import { drawFrame, fitCanvas } from "./viewer.js";

const $ = (id) => document.getElementById(id);

// Extensions the backend can actually decode. sprite.list only enumerates CL2/CEL,
// but we still guard so an unexpected entry never fails silently.
const DECODABLE = new Set(["cl2", "cel", "clx", "pcx"]);

// Friendly labels for plrgfx type codes / armor / weapon chars.
const TYPE_NAMES = {
  as: "Stand", st: "Stand", aw: "Walk", wl: "Walk",
  at: "Attack", ht: "Hit", lm: "Lightning", fm: "Fire",
  qm: "Magic", dt: "Death", bl: "Block",
};
const ARMOR_NAMES = { l: "Light", m: "Medium", h: "Heavy" };
const WEAPON_NAMES = {
  n: "None", u: "Unarmed", s: "Sword", d: "Dagger",
  b: "Bow", a: "Axe", m: "Mace", h: "H2H", t: "H2H",
};
function optLabel(code, map) {
  const n = map[code];
  return n ? `${n} (${code})` : code;
}

const client = new McpClient();
let currentFrames = null;   // last decode result frames, for the frame slider
let lastRes = null;         // last decode result (format/width/height) for meta rebuild
let lastSprite = null;      // last selected sprite (re-decode when palette changes)
let lastSprites = [];       // last sprite.list payload (re-group without re-fetch)
let currentMode = "family"; // catalog grouping: "family" (combined) | "mpq" (by package)

// Archive paths discovered from the catalog (each sprite carries archive_path).
// The monster/NPC viewer lets the user pick which MPQ to decode from, since
// monster/towner names are synthesized from data tables and carry no archive.
let allArchives = [];

// Monster catalog grouped by class: class -> [{ name, anim, fpd, type }].
let monsterMap = null;

// 8-direction animation state.
let currentDirections = 1;  // number of directions the current sprite is split into
let framesPerDir = 1;       // frames per direction (total / directions)
let currentDir = 0;         // currently shown direction
let currentFin = 0;         // currently shown frame within the direction
let isPlaying = false;
let playTimer = null;

// Character-viewer index: classPath -> { classChar, archivePath, types:Set, combos:Map<type,Set<"aw">> }.
let charIndex = null;

// Map palette name -> owning MPQ path (from sprite.list kind:"palette"). Used to
// tell sprite.decode which extra MPQ holds the palette, since a palette often
// lives in a different MPQ than the sprite (e.g. Hellfire palette on a DIABDAT sprite).
const paletteArchives = new Map();

// Full palette list fetched from sprite.list(kind:"palette"), kept so the dropdown
// can be filtered (e.g. character mode shows only level palettes).
let allPalettes = []; // [{ name, archive_path, isLevel }]

// Level palettes (levels\ / nlevels\) carry the general colour ramp a character
// sprite needs; everything else (gendata\cut*.pal, ui_art\diablo.pal, …) is
// cutscene/UI-specific and mis-colours player sprites.
function isLevelPalette(name) {
  const n = (name || "").toLowerCase();
  return n.includes("levels\\") || n.includes("levels/") ||
         n.includes("nlevels\\") || n.includes("nlevels/");
}

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

function isCharMode() {
  const r = document.querySelector('input[name="mode"]:checked');
  return !!r && r.value === "character";
}

async function loadCatalog() {
  setMeta("loading catalog…");
  try {
    const res = await client.callTool("sprite.list", {});
    const sprites = (res && res.sprites) || [];
    lastSprites = sprites;
    renderCurrentCatalog();
    setMeta(`catalog: ${res.count ?? sprites.length} sprites`);
    // Collect the distinct archive paths so the monster/NPC viewer can offer an
    // MPQ selector (synthesized monster/towner entries have no archive of their own).
    allArchives = [];
    for (const s of sprites) {
      const p = s.archive_path;
      if (p && !allArchives.includes(p)) allArchives.push(p);
    }
    await loadPalettes();
    await loadCharacterData();   // build the class/action/armor/weapon index
  } catch (e) {
    setMeta("sprite.list failed: " + e.message, true);
  }
}

// Rebuild the catalog tree from the cached sprite list using the current
// grouping mode (family-combined or by-MPQ). Called on load and on mode switch.
function renderCurrentCatalog() {
  const tree = buildTree(lastSprites, currentMode);
  renderCatalog($("catalog"), tree, onSelectSprite);
}

// Populate the palette <select> from sprite.list(kind:"palette"): every .pal in
// the archive, so the user picks instead of typing a path by hand.
async function loadPalettes() {
  const sel = $("palette");
  sel.innerHTML = "";
  allPalettes = [];
  try {
    const res = await client.callTool("sprite.list", { kind: "palette" });
    const pals = (res && res.sprites) || [];
    for (const p of pals) {
      paletteArchives.set(p.name, p.archive_path || "");
      allPalettes.push({
        name: p.name,
        archive_path: p.archive_path || "",
        isLevel: isLevelPalette(p.name),
      });
    }
    if (allPalettes.length === 0) {
      addPaletteOption("（无可用调色板）", "");
      return;
    }
    applyPaletteFilter(isCharMode());
  } catch (e) {
    addPaletteOption("（加载调色板失败）", "");
  }
}

function addPaletteOption(text, value) {
  const opt = document.createElement("option");
  opt.value = value;
  opt.textContent = text;
  $("palette").appendChild(opt);
}

// Fill the palette <select>. In character mode show only level palettes
// (levels\ / nlevels\), which is what player sprites need; in catalog mode
// show everything so missiles/UI sprites can still pick a themed palette.
function applyPaletteFilter(levelOnly) {
  const sel = $("palette");
  const prev = sel.value;
  const list = levelOnly ? allPalettes.filter((p) => p.isLevel) : allPalettes;
  sel.innerHTML = "";
  for (const p of list) {
    const opt = document.createElement("option");
    opt.value = p.name;
    opt.textContent = basename(p.name) + (levelOnly ? "（关卡）" : (p.isLevel ? "（关卡）" : "（其它）"));
    opt.title = p.name;
    sel.appendChild(opt);
  }
  // Keep the previous selection if still present; otherwise default to the first.
  if (list.some((p) => p.name === prev)) sel.value = prev;
  else if (list.length > 0) sel.value = list[0].name;
}

// ---- Character index -------------------------------------------------------

async function loadCharacterData() {
  try {
    const res = await client.callTool("sprite.list", { family: "plrgfx" });
    const sprites = (res && res.sprites) || [];
    charIndex = buildCharIndex(sprites);
    populateCharPanel();
  } catch (e) {
    console.warn("character data load failed:", e.message);
  }
}

// From the plrgfx sprite list, build: classPath -> { classChar, archivePath,
// types:Set<typeCode>, combos:Map<typeCode, Set<"aw">> }.
function buildCharIndex(sprites) {
  const idx = new Map();
  for (const s of sprites) {
    const comps = (s.name || "").split(/[\\/]/);
    if (comps.length < 4) continue;          // need plrgfx / class / prefix / file
    const classPath = comps[1];
    const prefix = comps[2];
    if (prefix.length < 3) continue;
    const type = s.type || "";
    if (!type) continue;
    if (!idx.has(classPath))
      idx.set(classPath, {
        classChar: prefix[0],
        archivePath: s.archive_path || "",
        types: new Set(),
        combos: new Map(),
      });
    const e = idx.get(classPath);
    e.types.add(type);
    if (!e.combos.has(type)) e.combos.set(type, new Set());
    e.combos.get(type).add(prefix[1] + prefix[2]); // armor + weapon char
  }
  return idx;
}

function populateCharPanel() {
  const cls = $("charClass");
  cls.innerHTML = "";
  for (const cp of charIndex.keys()) {
    const o = document.createElement("option");
    o.value = cp;
    o.textContent = cp;
    cls.appendChild(o);
  }
  fillCharActions();
  fillCharArmorWeapon();
}

function fillCharActions() {
  const cp = $("charClass").value;
  const e = charIndex.get(cp);
  const sel = $("charAction");
  sel.innerHTML = "";
  if (!e) return;
  for (const t of [...e.types].sort())
    addOption(sel, t, optLabel(t, TYPE_NAMES));
}

function fillCharArmorWeapon() {
  const cp = $("charClass").value;
  const t = $("charAction").value;
  const e = charIndex.get(cp);
  const combos = (e && e.combos.get(t)) || new Set();
  const armors = new Set();
  const weapons = new Set();
  for (const cw of combos) {
    armors.add(cw[0]);
    weapons.add(cw[1]);
  }
  fillSelect($("charArmor"), [...armors].sort(), ARMOR_NAMES);
  fillSelect($("charWeapon"), [...weapons].sort(), WEAPON_NAMES);
}

function addOption(sel, value, text) {
  const o = document.createElement("option");
  o.value = value;
  o.textContent = text;
  sel.appendChild(o);
}
function fillSelect(sel, codes, names) {
  sel.innerHTML = "";
  for (const c of codes) addOption(sel, c, optLabel(c, names));
}

// Assemble the plrgfx path and decode it via the shared onSelectSprite pipeline.
async function showCharacterSprite() {
  if (!charIndex || !isCharMode()) return;
  const cp = $("charClass").value;
  const e = charIndex.get(cp);
  if (!e) return;
  const type = $("charAction").value || "st";
  const armor = $("charArmor").value || "l";
  const weapon = $("charWeapon").value || "n";
  const prefix = e.classChar + armor + weapon;
  const name = `plrgfx\\${cp}\\${prefix}\\${prefix}${type}.cl2`;
  onSelectSprite({ name, archive_path: e.archivePath, family: "plrgfx", class: cp });
}

// ---- Monster / NPC viewer --------------------------------------------------

function populateMonsterArchive() {
  const sel = $("monsterArchive");
  sel.innerHTML = "";
  for (const a of allArchives) {
    const o = document.createElement("option");
    o.value = a;
    o.textContent = basename(a);
    o.title = a;
    sel.appendChild(o);
  }
  if (allArchives.length === 0) {
    const o = document.createElement("option");
    o.value = "";
    o.textContent = "（无可用 MPQ）";
    sel.appendChild(o);
  }
}

// Build the monster dropdown from sprite.list(family:"monsters"): group every
// (monster, animation) entry by class, so the user first picks a monster and then
// a specific animation (Stand/Walk/Attack/...).
async function populateMonsters() {
  const sel = $("monsterSel");
  sel.innerHTML = "";
  monsterMap = new Map();
  try {
    const res = await client.callTool("sprite.list", { family: "monsters" });
    const sprites = (res && res.sprites) || [];
    for (const s of sprites) {
      const c = s.class || "?";
      if (!monsterMap.has(c)) monsterMap.set(c, []);
      monsterMap.get(c).push(s);
    }
  } catch (e) {
    console.warn("monster list failed:", e.message);
  }
  for (const c of [...monsterMap.keys()].sort()) {
    const o = document.createElement("option");
    o.value = c;
    o.textContent = c;
    sel.appendChild(o);
  }
  fillMonsterAnims();
}

// Populate the animation dropdown for the currently selected monster class.
function fillMonsterAnims() {
  const cls = $("monsterSel").value;
  const list = (monsterMap && monsterMap.get(cls)) || [];
  const sel = $("monsterAnim");
  sel.innerHTML = "";
  for (const s of list) {
    const o = document.createElement("option");
    o.value = s.name;
    o.textContent = `${s.anim || s.type} (${s.frames_per_dir || "?"} 帧/方向)`;
    o.title = s.name;
    sel.appendChild(o);
  }
}

// Build the NPC (towner) dropdown from sprite.list(family:"towners").
async function populateNPCs() {
  const sel = $("npcSel");
  sel.innerHTML = "";
  try {
    const res = await client.callTool("sprite.list", { family: "towners" });
    const sprites = (res && res.sprites) || [];
    for (const s of sprites) {
      const o = document.createElement("option");
      o.value = s.name;
      o.textContent = `${s.class || basename(s.name)}  (${basename(s.name)})`;
      o.title = s.name;
      sel.appendChild(o);
    }
  } catch (e) {
    console.warn("towner list failed:", e.message);
  }
}

// Decode the selected monster (family monsters, directions from backend).
function showMonsterSprite() {
  const archive = $("monsterArchive").value;
  const name = $("monsterAnim").value;
  if (!name) return;
  onSelectSprite({ name, archive_path: archive, family: "monsters", class: $("monsterSel").value });
}

// Decode the selected NPC (family towners, single direction).
function showNpcSprite() {
  const archive = $("monsterArchive").value;
  const name = $("npcSel").value;
  if (!name) return;
  onSelectSprite({ name, archive_path: archive, family: "towners", class: "npc" });
}

// Enter the monster/NPC viewer: populate the MPQ + monster/NPC dropdowns, then
// decode the first entry.
async function showMonsterView() {
  if (!allArchives.length) {
    showError("尚无 MPQ 列表（请先在 目录浏览 模式加载目录）。");
    return;
  }
  populateMonsterArchive();
  const wantNpc = $("monsterType").value === "npc";
  $("monsterSelWrap").hidden = wantNpc;
  $("monsterAnimWrap").hidden = wantNpc;
  $("npcSelWrap").hidden = !wantNpc;
  if (wantNpc) {
    await populateNPCs();
    showNpcSprite();
  } else {
    await populateMonsters();
    showMonsterSprite();
  }
}

// ---- Decode + 8-direction playback -----------------------------------------

async function onSelectSprite(sprite) {
  lastSprite = sprite;
  hideError();
  $("currentName").textContent = sprite.name;

  const ext = extOf(sprite.name);
  if (!DECODABLE.has(ext)) {
    showError(`暂不支持的格式：.${ext || "?"}（当前仅支持 .cl2 / .cel / .clx / .pcx）`);
    stopPlay();
    return;
  }

  const archive = (sprite.archive_path || "").trim();
  if (!archive) {
    showError("该精灵缺少 archive_path，无法定位 MPQ，无法解码。");
    stopPlay();
    return;
  }
  const palette = $("palette").value;
  if (!palette) {
    showError("请先在上方选择一个调色板。");
    stopPlay();
    return;
  }

  const paletteArchive = paletteArchives.get(palette) || "";
  const archives = [archive];
  if (paletteArchive && !archives.includes(paletteArchive))
    archives.push(paletteArchive);

  setMeta("decoding " + sprite.name + " …");
  stopPlay();
  try {
    const args = {
      archive,
      archives,
      name: sprite.name,
      width: "auto",
      palette_name: palette,
    };
    // TRN toggle (off by default = palette-only mode). When on, the backend
    // applies a 256-byte palette remap; default class TRN is plrgfx\trn<class>.trn
    // (not shipped for base classes, so no-op), or an explicit preset if chosen.
    if ($("applyTrn").checked) {
      args.apply_trn = true;
      const preset = $("trnPreset").value;
      if (preset === "infra") args.trn_name = "plrgfx\\infra.trn";
      else if (preset === "stone") args.trn_name = "plrgfx\\stone.trn";
      // preset "class" -> let the backend compute plrgfx\trn<class>.trn
    }
    const res = await client.callTool("sprite.decode", args);
    if (!res || !Array.isArray(res.frames) || res.frames.length === 0) {
      showError("解码失败：" + sprite.name + " 未返回任何帧。");
      return;
    }
    hideError();
    applyDecoded(res, sprite);
  } catch (e) {
    showError("解码失败：" + (e.message || e));
  }
}

// Store a decode result and (re)initialize the direction / frame controls.
function applyDecoded(res, sprite) {
  currentFrames = res.frames;
  lastRes = res;
  // Prefer the direction count the backend computed (monsters report their real
  // direction count from monstdat; player CL2 are fixed 8-direction). Fall back to
  // the historical heuristic when the field is absent.
  const dirs = (res && res.directions && res.directions > 0)
    ? res.directions
    : ((sprite && sprite.family === "plrgfx") ? 8 : 1);
  currentDir = 0;
  currentFin = 0;
  setDirections(dirs);
  showFrame(0, 0);
}

function setDirections(dirs) {
  currentDirections = dirs;
  framesPerDir = Math.max(1, Math.floor((currentFrames ? currentFrames.length : 1) / dirs));
  $("directions").value = String(dirs);
  $("dir").max = String(Math.max(0, dirs - 1));
  $("dir").value = "0";
  $("frame").max = String(framesPerDir - 1);
  $("frame").value = "0";
  currentDir = 0;
  currentFin = 0;
}

// Draw frame (dir, fin) where absolute index = dir * framesPerDir + fin.
function showFrame(dir, fin) {
  if (!currentFrames) return;
  const idx = dir * framesPerDir + fin;
  const f = currentFrames[idx];
  if (!f) return;
  currentDir = dir;
  currentFin = fin;
  drawFrame($("canvas"), f.rgba_base64, f.width, f.height);
  fitCanvas($("canvas"), 384);
  $("dirNo").textContent = String(dir);
  $("frameNo").textContent = `dir ${dir} / ${currentDirections - 1}  · 帧 ${fin} / ${framesPerDir - 1}`;
  updateMeta(idx, dir, fin);
}

// Meta line reflects the CURRENT frame's size (not the whole sheet), plus the
// direction / frame-in-direction it came from.
function updateMeta(idx, dir, fin) {
  if (!lastRes || !currentFrames || !lastSprite) return;
  const f = currentFrames[idx];
  if (!f) return;
  let s = `${lastSprite.name}  ${lastRes.format}  帧 ${f.width}×${f.height}  ` +
         `dir ${dir} 帧内 ${fin}  ${currentFrames.length} frames`;
  if (lastRes.trn_applied)
    s += `  · TRN: ${lastRes.trn_name || "on"}`;
  setMeta(s);
}

function startPlay() {
  if (!currentFrames || framesPerDir <= 1) return;
  isPlaying = true;
  const fps = Math.min(60, Math.max(1, Number($("fps").value) || 12));
  playTimer = setInterval(() => {
    currentFin = (currentFin + 1) % framesPerDir;
    showFrame(currentDir, currentFin);
    $("frame").value = String(currentFin);
  }, 1000 / fps);
  $("playBtn").textContent = "⏸ 暂停";
}
function stopPlay() {
  isPlaying = false;
  if (playTimer) clearInterval(playTimer);
  playTimer = null;
  $("playBtn").textContent = "▶ 播放";
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

  // Browse-mode toggle: Catalog vs Character vs Monster/NPC panes.
  document.querySelectorAll('input[name="mode"]').forEach((r) => {
    r.addEventListener("change", (e) => {
      if (!e.target.checked) return;
      const character = e.target.value === "character";
      const monster = e.target.value === "monster";
      $("catalogPane").hidden = character || monster;
      $("charPane").hidden = !character;
      $("monsterPane").hidden = !monster;
      // Both character and monster modes want level palettes (correct colours).
      applyPaletteFilter(character || monster);
      if (character) showCharacterSprite();
      if (monster) showMonsterView();
    });
  });

  // Catalog grouping mode switch (combined family/class/type <-> by MPQ package).
  document.querySelectorAll('input[name="groupMode"]').forEach((r) => {
    r.addEventListener("change", (e) => {
      if (!e.target.checked) return;
      currentMode = e.target.value;
      renderCurrentCatalog();
    });
  });
  $("expandAll").addEventListener("click", () => setAllExpanded($("catalog"), true));
  $("collapseAll").addEventListener("click", () => setAllExpanded($("catalog"), false));

  // Frame slider (frame within the current direction).
  $("frame").addEventListener("input", (e) => {
    stopPlay();
    showFrame(currentDir, Number(e.target.value));
  });
  // Direction slider.
  $("dir").addEventListener("input", (e) => {
    stopPlay();
    showFrame(Number(e.target.value), currentFin);
  });
  // Directions selector (re-splits the flat frame list).
  $("directions").addEventListener("change", (e) => {
    stopPlay();
    setDirections(Number(e.target.value));
    currentFin = Math.min(currentFin, framesPerDir - 1);
    $("frame").max = String(framesPerDir - 1);
    $("frame").value = String(currentFin);
    showFrame(currentDir, currentFin);
  });
  $("playBtn").addEventListener("click", () => (isPlaying ? stopPlay() : startPlay()));

  // Re-decode the current sprite when the user changes the palette.
  $("palette").addEventListener("change", () => {
    if (lastSprite) onSelectSprite(lastSprite);
  });

  // TRN toggle / preset: re-decode so the colour change takes effect.
  $("applyTrn").addEventListener("change", () => {
    if (lastSprite) onSelectSprite(lastSprite);
  });
  $("trnPreset").addEventListener("change", () => {
    if ($("applyTrn").checked && lastSprite) onSelectSprite(lastSprite);
  });

  // Character panel selectors.
  $("charClass").addEventListener("change", () => {
    fillCharActions();
    fillCharArmorWeapon();
    showCharacterSprite();
  });
  ["charAction", "charArmor", "charWeapon"].forEach((id) => {
    $(id).addEventListener("change", () => {
      if (id === "charAction") fillCharArmorWeapon();
      showCharacterSprite();
    });
  });

  // Monster / NPC viewer controls.
  $("monsterType").addEventListener("change", () => showMonsterView());
  $("monsterArchive").addEventListener("change", () => {
    if ($("monsterType").value === "npc") showNpcSprite();
    else showMonsterSprite();
  });
  $("monsterSel").addEventListener("change", () => {
    fillMonsterAnims();
    showMonsterSprite();
  });
  $("monsterAnim").addEventListener("change", () => showMonsterSprite());
  $("npcSel").addEventListener("change", () => showNpcSprite());
}

wireControls();
